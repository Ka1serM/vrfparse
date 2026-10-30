#include "replay/demo_reader.h"

#include <fmt/format.h>

#include <algorithm>
#include <bit>

#include "net/bit_reader.h"
#include "replay/byte_reader.h"

namespace replay {

namespace {

// MAX_REPLAY_PACKET: the largest packet of a replay connection, which also bounds a bunch's payload.
constexpr std::uint32_t maxPacketBytes = 2 * 1024;
constexpr std::uint32_t maxPacketBits = maxPacketBytes * 8;
constexpr std::string_view actorChannelName = "Actor";
// The player controllers of Valorant's replays. APlayerController::OnActorChannelOpen reads a NetPlayerIndex from the
// open bunch of one.
constexpr std::string_view playerControllerClasses[] = {"/Game/Characters/_Core/BaseReplayController.BaseReplayController_C"};

// EChannelCloseReason::MAX.
constexpr std::uint32_t closeReasonCount = 15;

bool isDynamic(const std::uint32_t guid) { return guid > 0 && (guid & 1) == 0; }

// FVector_NetQuantize10 and the other quantized vectors of UE::Net::ReadQuantizedVector: the component bits and whether
// the components are scaled, then the components. Without component bits, the components are floats or doubles.
Vector readQuantizedVector(net::BitReader& reader, const float scale) {
    const std::uint32_t info = reader.serializeInt(1 << 7);
    const int componentBits = static_cast<int>(info & 63);
    const bool scaled = (info >> 6) != 0;
    if (componentBits == 0) {
        if (!scaled) return {reader.readFloat(), reader.readFloat(), reader.readFloat()};
        return {static_cast<float>(reader.readDouble()), static_cast<float>(reader.readDouble()), static_cast<float>(reader.readDouble())};
    }
    const auto component = [&] {
        const std::uint64_t sign = std::uint64_t{1} << (componentBits - 1);
        const auto value = static_cast<double>(static_cast<std::int64_t>(reader.readBits(componentBits) ^ sign) - static_cast<std::int64_t>(sign));
        return static_cast<float>(scaled ? value / scale : value);
    };
    const float x = component();
    const float y = component();
    return {x, y, component()};
}

// The spawn vectors of UPackageMapClient::SerializeNewActor: whether it is sent, whether it is quantized, then the value.
Vector readSpawnVector(net::BitReader& reader, const Vector fallback) {
    if (!reader.readBit()) return fallback;
    if (reader.readBit()) return readQuantizedVector(reader, 10);
    return {static_cast<float>(reader.readDouble()), static_cast<float>(reader.readDouble()), static_cast<float>(reader.readDouble())};
}

// FRotator::SerializeCompressedShort.
Rotator readRotatorShort(net::BitReader& reader) {
    const auto component = [&] { return reader.readBit() ? static_cast<float>(reader.readUInt16()) * (360.0f / 65536.0f) : 0.0f; };
    const float pitch = component();
    const float yaw = component();
    return {pitch, yaw, component()};
}

}

DemoReader::DemoReader(const NetworkDemoHeader& header, EventLog& log, std::vector<std::string>& problems)
    : log(log), problems(problems), transform(header.branch), hasStreamingFixes(header.hasFlag(ReplayHeaderFlags::HasStreamingFixes)),
      hasGameSpecificFrameData(header.hasFlag(ReplayHeaderFlags::GameSpecificFrameData)), packageMap(log) {
    if (!transform.isKnown())
        throw Error("replay.version", fmt::format("The game release {} is not supported: its payload transform is not known.", header.branch));
}

void DemoReader::readFrames(const std::span<const std::uint8_t> data) {
    ByteReader reader(data, "replay data");
    while (!reader.atEnd()) readFrame(reader);
}

// FReplayHelper::ReadDemoFrame.
void DemoReader::readFrame(ByteReader& reader) {
    // The current level index.
    reader.skip(4);
    frameTime = reader.readFloat();
    log.setTime(frameTime);
    packageMap.receiveExportData(reader);
    // The streaming levels.
    const std::uint32_t levelCount = reader.readIntPacked();
    for (std::uint32_t i = 0; i < levelCount; ++i) {
        reader.readString();
        if (hasStreamingFixes) continue;
        reader.readString();
        // The level's FTransform: a quaternion and two vectors of doubles.
        reader.skip(80);
    }
    // SkipExternalOffset.
    if (hasStreamingFixes) reader.skip(8);
    // LoadExternalData: the bits, the NetGUID, and the data of each object.
    while (const std::uint32_t bits = reader.readIntPacked()) {
        reader.readIntPacked();
        reader.skip((static_cast<std::uint64_t>(bits) + 7) / 8);
    }
    if (hasGameSpecificFrameData) reader.skip(reader.readUInt64());
    // A frame at or before the latest one: UE reads its packets only to skip them.
    const bool appendPackets = latestFrameTime < frameTime;
    if (!appendPackets) report(fmt::format("replay.frame_order: The frame at {:.3f} s is not after the frame before; its packets are skipped.", frameTime));
    latestFrameTime = std::max(latestFrameTime, frameTime);
    while (true) {
        // The seen level index.
        if (hasStreamingFixes) reader.readIntPacked();
        const std::int32_t size = reader.readInt32();
        if (size == 0) return;
        if (size < 0 || static_cast<std::uint32_t>(size) > maxPacketBytes) reader.fail(fmt::format("the packet size {} is not valid", size));
        const std::span<const std::uint8_t> packet = reader.readBytes(static_cast<std::size_t>(size));
        if (appendPackets) receivedPacket(packet);
    }
}

// UNetConnection::ReceivedRawPacket, ReceivedPacket, and DispatchPacket of a replay connection: no packet header, and
// every bunch arrives in order.
void DemoReader::receivedPacket(const std::span<const std::uint8_t> packet) {
    ++packetId;
    if (packet.empty()) return;
    // The packet ends with a 1 bit after the last bunch.
    const std::uint8_t last = packet.back();
    if (last == 0) throw Error("replay.parse", fmt::format("Packet {} at {:.3f} s has no end bit.", packetId, frameTime));
    const std::int64_t bits = static_cast<std::int64_t>(packet.size()) * 8 - 1 - std::countl_zero(last);
    net::BitReader reader(packet, bits);
    while (!reader.atEnd()) {
        const BunchHeader header = readBunchHeader(reader, packetId);
        const std::uint32_t payloadBits = reader.serializeInt(maxPacketBits);
        net::BitReader payload = reader.readSubReader(payloadBits);
        check(reader, "bunch header", header);
        if (header.hasPackageMapExports) {
            packageMap.receiveNetGuidBunch(payload);
            check(payload, "NetGUID exports", header);
        }
        auto found = channels.find(header.channel);
        // An unreliable bunch for a channel that is not open opens it, or it is skipped.
        if (found == channels.end() && !header.reliable && !(header.open && (header.close || header.partial))) continue;
        if (found == channels.end()) found = channels.emplace(header.channel, Channel{.isActorChannel = header.channelName == actorChannelName}).first;
        receivedRawBunch(found->second, header, payload);
    }
}

DemoReader::BunchHeader DemoReader::readBunchHeader(net::BitReader& reader, const std::uint32_t packet) {
    BunchHeader header;
    const bool control = reader.readBit();
    header.open = control && reader.readBit();
    header.close = control && reader.readBit();
    if (header.close) header.closeReason = static_cast<ChannelCloseReason>(reader.serializeInt(closeReasonCount));
    // bIsReplicationPaused.
    reader.readBit();
    header.reliable = reader.readBit();
    header.channel = reader.serializeIntPacked();
    header.hasPackageMapExports = reader.readBit();
    header.hasMustBeMappedGuids = reader.readBit();
    header.partial = reader.readBit();
    if (header.reliable) {
        const auto found = channels.find(header.channel);
        header.sequence = (found == channels.end() ? 0 : found->second.reliableSequence) + 1;
    } else if (header.partial) {
        // An unreliable partial bunch takes the packet's sequence.
        header.sequence = static_cast<std::int32_t>(packet);
    }
    // Valorant's own bit, which Unreal 5.3 does not have.
    reader.readBit();
    header.partialInitial = header.partial && reader.readBit();
    header.partialFinal = header.partial && reader.readBit();
    if (header.reliable || header.open) header.channelName = PackageMap::readName(reader);
    return header;
}

// UChannel::ReceivedRawBunch and ReceivedNextBunch: a partial bunch waits for the rest of its payload.
void DemoReader::receivedRawBunch(Channel& channel, const BunchHeader& header, net::BitReader& payload) {
    if (header.reliable) channel.reliableSequence = header.sequence;
    if (!header.partial) {
        receivedSequencedBunch(header, payload);
        return;
    }
    PartialBunch& partial = channel.partial;
    const std::int64_t bits = header.hasPackageMapExports ? 0 : payload.getBitsLeft();
    if (header.partialInitial) {
        if (channel.hasPartial && !partial.header.partialFinal)
            report(fmt::format("replay.partial_bunch: A partial bunch of channel {} at {:.3f} s did not complete.", header.channel, frameTime));
        channel.hasPartial = true;
        partial.header = header;
        partial.header.partialFinal = false;
        partial.bits = 0;
        partial.data.clear();
    } else {
        const bool sequenceMatches = partial.header.reliable ? header.sequence == partial.header.sequence + 1
                                                             : header.sequence == partial.header.sequence ||
                                                                   header.sequence == partial.header.sequence + 1;
        if (!channel.hasPartial || partial.header.partialFinal || !sequenceMatches || partial.header.reliable != header.reliable) {
            report(fmt::format("replay.partial_bunch: A partial bunch of channel {} at {:.3f} s does not continue a partial bunch.",
                               header.channel, frameTime));
            channel.hasPartial = false;
            return;
        }
        partial.header.sequence = header.sequence;
    }
    // Only the final part may end within a byte.
    if (partial.bits % 8 != 0 && bits > 0) throw Error("replay.parse", fmt::format("A partial bunch of channel {} at {:.3f} s is not byte aligned.", header.channel, frameTime));
    partial.data.resize(static_cast<std::size_t>((partial.bits + bits + 7) / 8));
    payload.readBits(std::span(partial.data).subspan(static_cast<std::size_t>(partial.bits / 8)), bits);
    partial.bits += bits;
    if (header.partialInitial || !header.partialFinal) return;
    // The joined bunch has the open of the first part and the close of the last.
    channel.hasPartial = false;
    BunchHeader joined = partial.header;
    joined.partialFinal = true;
    joined.close = header.close;
    joined.closeReason = header.closeReason;
    joined.hasMustBeMappedGuids = header.hasMustBeMappedGuids;
    net::BitReader joinedPayload{std::span(partial.data), partial.bits};
    receivedSequencedBunch(joined, joinedPayload);
}

// UChannel::ReceivedSequencedBunch: the bunch, then the close, which destroys the channel.
void DemoReader::receivedSequencedBunch(const BunchHeader& header, net::BitReader& payload) {
    Channel& channel = channels.at(header.channel);
    if (channel.isActorChannel) {
        // UActorChannel::ReceivedBunch.
        if (header.hasMustBeMappedGuids) {
            const std::uint16_t count = payload.readUInt16();
            for (std::uint16_t i = 0; i < count; ++i) payload.serializeIntPacked();
            check(payload, "must be mapped NetGUIDs", header);
        }
        processBunch(channel, header, payload);
    }
    if (!header.close) return;
    if (channel.hasActor) log.add({.kind = EventKind::ActorClose, .closeReason = header.closeReason, .channel = header.channel, .actor = channel.actor});
    channels.erase(header.channel);
}

// UActorChannel::ProcessBunch.
void DemoReader::processBunch(Channel& channel, const BunchHeader& header, net::BitReader& payload) {
    if (!channel.hasActor) {
        // A channel without its actor ignores the bunches until the open one.
        if (!header.open) return;
        serializeNewActor(channel, header, payload);
    }
    while (!payload.atEnd()) {
        readContentBlock(channel, header.channel, payload);
        check(payload, "content block", header);
    }
    log.add({.kind = EventKind::BunchEnd, .channel = header.channel, .actor = channel.actor});
}

void DemoReader::serializeNewActor(Channel& channel, const BunchHeader& header, net::BitReader& payload) {
    channel.hasActor = true;
    channel.actor = packageMap.loadObject(payload, false);
    Event open{.kind = EventKind::ActorOpen, .channel = header.channel, .actor = channel.actor, .name = packageMap.getName(channel.actor)};
    // A dynamic actor's spawn data, unless a closing bunch leaves it out.
    if (isDynamic(channel.actor) && !(payload.atEnd() && header.close)) {
        channel.archetype = packageMap.loadObject(payload, false);
        channel.classPath = packageMap.getArchetypeClassPath(channel.archetype);
        open.classGuid = channel.archetype;
        open.outerGuid = packageMap.loadObject(payload, false);
        open.path = packageMap.getName(channel.archetype);
        ActorSpawn spawn;
        spawn.location = readSpawnVector(payload, {});
        if (payload.readBit()) spawn.rotation = readRotatorShort(payload);
        spawn.scale = readSpawnVector(payload, {1, 1, 1});
        spawn.velocity = readSpawnVector(payload, {});
        open.spawn = log.addSpawn(spawn);
    }
    check(payload, "SerializeNewActor", header);
    log.add(open);
    // APlayerController::OnActorChannelOpen.
    if (isDynamic(channel.actor) && std::ranges::find(playerControllerClasses, channel.classPath) != std::ranges::end(playerControllerClasses)) payload.readByte();
}

// UActorChannel::ReadContentBlockPayload: the header, then the payload, which Valorant obfuscates.
void DemoReader::readContentBlock(const Channel& channel, const std::uint32_t index, net::BitReader& payload) {
    Event block{.kind = EventKind::ContentBlock, .channel = index, .actor = channel.actor};
    block.hasRepLayout = payload.readBit();
    block.isActorBlock = payload.readBit();
    block.outerGuid = channel.actor;
    if (block.isActorBlock) {
        block.object = channel.actor;
        block.path = channel.classPath;
    } else {
        block.object = packageMap.loadObject(payload, false);
        // A stably named subobject is found by its name; another one comes with its class, unless it is destroyed.
        if (!payload.readBit()) {
            if (payload.readBit()) {
                block.deleted = true;
                block.deleteFlags = payload.readByte();
            } else {
                block.classGuid = packageMap.loadObject(payload, false);
                block.deleted = block.classGuid == 0;
                if (!block.deleted && !payload.readBit()) block.outerGuid = packageMap.loadObject(payload, false);
            }
        }
        block.path = block.deleted ? packageMap.getName(block.object) : block.classGuid != 0 ? packageMap.getClassPath(block.classGuid) : std::string_view();
    }
    if (block.deleted) {
        block.classGuid = 0;
        log.add(block);
        return;
    }
    const std::uint32_t bits = payload.serializeIntPacked();
    if (payload.isError() || bits > payload.getBitsLeft()) return payload.setError();
    content.assign(static_cast<std::size_t>((bits + 7) / 8), 0);
    payload.readBits(content, bits);
    if (channel.actor != 0) transform.decode(content, bits, channel.actor);
    log.add(block);
    net::BitReader decoded{std::span(content), bits};
    if (block.hasRepLayout) readFields(block, decoded);
    // FObjectReplicator::ReceivedBunch reads the class net cache fields (custom delta properties and RPCs) after the
    // RepLayout properties. Their handles index the export group "<class>_ClassNetCache".
    const std::int64_t rest = decoded.getBitsLeft();
    if (rest > 0)
        log.add({.kind = EventKind::ClassNetCache, .hasRepLayout = block.hasRepLayout, .channel = block.channel, .actor = block.actor,
                 .object = block.object, .path = block.path, .payload = copyPayload(decoded, rest), .payloadBits = rest});
    if (decoded.isError()) payload.setError();
}

// FRepLayout::ReceiveProperties_BackwardsCompatible: the checksum bit, then each property by its handle + 1, until 0.
void DemoReader::readFields(const Event& block, net::BitReader& content) {
    const ExportGroup* group = packageMap.findGroup(block.path);
    content.readBit();
    while (!content.atEnd()) {
        const std::uint32_t handle = content.serializeIntPacked();
        if (handle == 0 || content.isError()) return;
        const std::uint32_t bits = content.serializeIntPacked();
        if (content.isError() || bits > content.getBitsLeft()) return content.setError();
        const std::string_view name = group && handle - 1 < group->names.size() ? group->names[handle - 1] : std::string_view();
        log.add({.kind = EventKind::Field, .channel = block.channel, .actor = block.actor, .object = block.object,
                 .handle = static_cast<std::int32_t>(handle - 1), .path = block.path, .name = name, .payload = copyPayload(content, bits),
                 .payloadBits = bits});
    }
}

std::span<const std::uint8_t> DemoReader::copyPayload(net::BitReader& reader, const std::int64_t bits) {
    const std::span<std::uint8_t> payload = log.allocatePayload(bits);
    reader.readBits(payload, bits);
    return payload;
}

void DemoReader::check(const net::BitReader& reader, const std::string_view what, const BunchHeader& header) const {
    if (!reader.isError()) return;
    throw Error("replay.parse", fmt::format("The {} of a bunch of channel {} at {:.3f} s (packet {}) does not fit the bunch.", what,
                                            header.channel, frameTime, packetId));
}

void DemoReader::finish() {
    for (const auto& [index, channel] : channels)
        if (channel.hasPartial) report(fmt::format("replay.partial_bunch: A partial bunch of channel {} did not complete before the end.", index));
}

void DemoReader::report(std::string problem) { problems.push_back(std::move(problem)); }

}
