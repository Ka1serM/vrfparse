#include "demo_reader.h"

#include <algorithm>
#include <bit>
#include <format>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "byte_reader.h"
#include "net_quantize.h"
#include "vrfparse/bit_reader.h"

namespace vrfparse {

namespace {

// MAX_REPLAY_PACKET: the largest packet of a replay connection, which also bounds a bunch's payload.
constexpr std::uint32_t maxPacketBytes = 2 * 1024;
constexpr std::uint32_t maxPacketBits = maxPacketBytes * 8;
constexpr std::string_view actorChannelName = "Actor";
// The player controllers of Valorant's replays. APlayerController::OnActorChannelOpen reads a NetPlayerIndex from the
// open bunch of one.
constexpr std::string_view playerControllerClasses[] = {"/Game/Characters/_Core/BaseReplayController.BaseReplayController_C"};

// About 15 KB of payloads at the mean content block size: a task that outweighs its scheduling.
constexpr std::size_t blocksPerTask = 128;
// About 1 MB of events.
constexpr std::size_t eventsPerTask = 8192;
// DEFAULT_MAX_CHANNEL_SIZE: UNetConnection's channel count.
constexpr std::uint32_t maxChannels = 32767;
// EChannelCloseReason::MAX.
constexpr std::uint32_t closeReasonCount = 15;

// The spawn vectors of UPackageMapClient::SerializeNewActor: whether it is sent, whether it is quantized, then the value.
Vector readSpawnVector(BitReader& reader, const Vector fallback) {
    if (!reader.readBit()) return fallback;
    if (reader.readBit()) return readQuantizedVector(reader, 10);
    return readVector(reader);
}

// FRotator::SerializeCompressedShort.
Rotator readRotatorShort(BitReader& reader) {
    const auto component = [&] { return reader.readBit() ? decompressAxisFromShort(reader.readUInt16()) : 0.0f; };
    const float pitch = component();
    const float yaw = component();
    return {pitch, yaw, component()};
}

// FObjectReplicator::ReceivedBunch's parts of a content block's payload: the RepLayout properties
// (FRepLayout::ReceiveProperties_BackwardsCompatible: the checksum bit, then each property by its handle + 1, until 0),
// then the class net cache fields (custom delta properties and RPCs), unframed, whose handles index the export group
// "<class>_ClassNetCache". The visitor gets each part's handle, -1 for the class net cache fields, and its bits in the
// payload. False when the payload is damaged.
template <class Visit>
bool splitContentBlock(const Event& block, Visit&& visit) {
    BitReader content = block.readPayload();
    if (block.hasRepLayout) {
        content.readBit();
        while (!content.atEnd()) {
            const std::uint32_t handle = content.serializeIntPacked();
            if (handle == 0 || content.isError()) break;
            const std::uint32_t bits = content.serializeIntPacked();
            if (content.isError() || bits > content.getBitsLeft()) return false;
            visit(static_cast<std::int32_t>(handle - 1), content.getPosition(), std::int64_t{bits});
            content.readSubReader(bits);
        }
    }
    if (const std::int64_t rest = content.getBitsLeft(); rest > 0) visit(-1, content.getPosition(), rest);
    return !content.isError();
}

// The part of the block's payload from the bit on.
Event partOf(const Event& block, Event part, const std::int64_t start, const std::int64_t bits) {
    part.time = block.time;
    part.channel = block.channel;
    part.actor = block.actor;
    part.object = block.object;
    part.path = block.path;
    part.payload = block.payload.subspan(static_cast<std::size_t>(start / 8), static_cast<std::size_t>((start % 8 + bits + 7) / 8));
    part.payloadOffset = static_cast<std::uint8_t>(start % 8);
    part.payloadBits = bits;
    return part;
}

}

DemoReader::DemoReader(const NetworkDemoHeader& header, const PayloadTransform transform, EventLog& log, std::vector<std::string>& problems)
    : log(log), problems(problems), transform(transform), hasStreamingFixes(header.hasFlag(ReplayHeaderFlags::HasStreamingFixes)),
      hasDeltaCheckpoints(header.hasFlag(ReplayHeaderFlags::DeltaCheckpoints)), hasGameSpecificFrameData(header.hasFlag(ReplayHeaderFlags::GameSpecificFrameData)), packageMap(log) {}

// The frames, then their content blocks: a payload does not change how the bunches are read, so the blocks of the chunk
// are decoded at once, in parallel. finish puts them in the log with their parts.
Result<> DemoReader::readFrames(const std::span<const std::uint8_t> data) {
    beginChunk();
    ByteReader reader(data);
    while (!reader.atEnd() && !reader.isError() && !failure) readFrame(reader);
    return endChunk(reader);
}

Result<> DemoReader::readCheckpoint(const std::span<const std::uint8_t> data) {
    if (hasDeltaCheckpoints) return vrfparse::fail(ErrorCode::Version, "The replay has delta checkpoints, which are not supported.");
    beginChunk();
    ByteReader reader(data);
    while (!reader.atEnd() && !reader.isError() && !failure) {
        // PacketOffset, then the level index.
        if (hasStreamingFixes) reader.skip(8);
        reader.skip(4);
        // The paths of the startup actors that were deleted before the checkpoint: the log has no event for them.
        const std::int32_t deletedCount = reader.readInt32();
        if (deletedCount < 0) reader.setError("the deleted startup actor count is negative");
        for (std::int32_t i = 0; i < deletedCount && !reader.isError(); ++i) reader.readString();
        packageMap.receiveCheckpoint(reader);
        readFrame(reader);
    }
    return endChunk(reader);
}

void DemoReader::beginChunk() {
    // A chunk has about as many events as the one before.
    log.addPendingChunk(log.getPending().empty() ? 0 : log.getPending().back().size());
    chunkBlocks.emplace_back().reserve(chunkBlocks.size() > 1 ? chunkBlocks[chunkBlocks.size() - 2].size() : 0);
}

Result<> DemoReader::endChunk(const ByteReader& reader) {
    if (reader.isError() && !failure)
        fail(std::format("The replay data is damaged at byte {} of the frame at {:.3f} s: {}.", reader.getPosition(), frameTime, reader.getReason()));
    if (!failure) decodeContentBlocks();
    if (failure) return std::unexpected(*failure);
    return {};
}

void DemoReader::decodeContentBlocks() {
    const std::span<const Event> pending = log.getPending().back();
    const std::span<ContentBlock> blocks = chunkBlocks.back();
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, blocks.size(), blocksPerTask), [&](const tbb::blocked_range<std::size_t>& range) {
        for (std::size_t i = range.begin(); i != range.end(); ++i) {
            ContentBlock& block = blocks[i];
            const Event& event = pending[block.event];
            if (event.actor != 0) transform.decode(block.payload, event.payloadBits, event.actor);
            block.readable = splitContentBlock(event, [&](std::int32_t, std::int64_t, std::int64_t) { ++block.parts; });
        }
    });
    const auto damaged = std::ranges::find(blocks, false, &ContentBlock::readable);
    if (damaged == blocks.end()) return;
    const Event& event = pending[damaged->event];
    fail(std::format("The content block of a bunch of channel {} at {:.3f} s cannot be read.", event.channel, event.time));
}

// The pending events, each content block with its parts after it, into storage of their exact count: one pass names the
// parts as the export groups come and places them, and then the events are copied to their places in parallel.
void DemoReader::commitEvents() {
    const std::span<const std::vector<Event>> pending = log.getPending();
    std::unordered_map<const char*, std::span<const std::string_view>> fieldNames;
    std::vector<std::size_t> chunkStarts;
    std::size_t place = 0;
    for (std::size_t chunk = 0; chunk < pending.size(); ++chunk) {
        chunkStarts.push_back(place);
        std::size_t parts = 0;
        auto block = chunkBlocks[chunk].begin();
        for (std::size_t i = 0; i < pending[chunk].size(); ++i) {
            const Event& event = pending[chunk][i];
            // An export group's path and a block's are interned: the same text is the same view.
            if (event.kind == EventKind::ExportGroup) fieldNames[event.path.data()] = event.fieldNames;
            if (block == chunkBlocks[chunk].end() || block->event != i) continue;
            if (const auto found = fieldNames.find(event.path.data()); found != fieldNames.end()) block->names = found->second;
            block->partsBefore = parts;
            parts += block->parts;
            ++block;
        }
        place += pending[chunk].size() + parts;
    }
    const std::span<Event> events = log.allocateEvents(place);
    tbb::parallel_for(std::size_t{0}, pending.size(), [&](const std::size_t chunk) {
        const std::span<const Event> chunkEvents = pending[chunk];
        const std::span<const ContentBlock> blocks = chunkBlocks[chunk];
        const std::size_t partsTotal = blocks.empty() ? 0 : blocks.back().partsBefore + blocks.back().parts;
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, chunkEvents.size(), eventsPerTask), [&](const tbb::blocked_range<std::size_t>& range) {
            auto next = std::ranges::lower_bound(blocks, range.begin(), {}, &ContentBlock::event);
            std::size_t at = chunkStarts[chunk] + range.begin() + (next == blocks.end() ? partsTotal : next->partsBefore);
            for (std::size_t i = range.begin(); i != range.end(); ++i) {
                const Event& event = chunkEvents[i];
                events[at++] = event;
                if (next == blocks.end() || next->event != i) continue;
                const std::span<const std::string_view> names = next->names;
                splitContentBlock(event, [&](const std::int32_t handle, const std::int64_t start, const std::int64_t bits) {
                    if (handle < 0) {
                        events[at++] = partOf(event, {.kind = EventKind::ClassNetCache, .hasRepLayout = event.hasRepLayout}, start, bits);
                        return;
                    }
                    const auto index = static_cast<std::size_t>(handle);
                    events[at++] = partOf(event, {.kind = EventKind::Field, .handle = handle, .name = index < names.size() ? names[index] : std::string_view()}, start, bits);
                });
                ++next;
            }
        });
    });
    log.clearPending();
    chunkBlocks = {};
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
    if (!appendPackets) report(std::format("replay.frame_order: The frame at {:.3f} s is not after the frame before; its packets are skipped.", frameTime));
    latestFrameTime = std::max(latestFrameTime, frameTime);
    while (!reader.isError() && !failure) {
        // The seen level index.
        if (hasStreamingFixes) reader.readIntPacked();
        const std::int32_t size = reader.readInt32();
        if (size == 0) return;
        if (size < 0 || static_cast<std::uint32_t>(size) > maxPacketBytes) return reader.setError("a packet size is not valid");
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
    if (last == 0) return fail(std::format("Packet {} at {:.3f} s has no end bit.", packetId, frameTime));
    const std::int64_t bits = static_cast<std::int64_t>(packet.size()) * 8 - 1 - std::countl_zero(last);
    BitReader reader(packet, bits);
    while (!reader.atEnd() && !failure) {
        const BunchHeader header = readBunchHeader(reader, packetId);
        const std::uint32_t payloadBits = reader.serializeInt(maxPacketBits);
        BitReader payload = reader.readSubReader(payloadBits);
        if (!check(reader, "bunch header", header)) return;
        if (header.channel >= maxChannels) return fail(std::format("Packet {} at {:.3f} s names channel {}, past the channel limit.", packetId, frameTime, header.channel));
        if (header.hasPackageMapExports) {
            packageMap.receiveNetGuidBunch(payload);
            if (!check(payload, "NetGUID exports", header)) return;
        }
        if (header.channel >= channels.size()) channels.resize(header.channel + 1);
        Channel& channel = channels[header.channel];
        if (!channel.isOpen) {
            // An unreliable bunch for a channel that is not open opens it, or it is skipped.
            if (!header.reliable && !(header.open && (header.close || header.partial))) continue;
            channel.isOpen = true;
            channel.isActorChannel = header.isActorChannel;
        }
        receivedRawBunch(channel, header, payload);
    }
}

DemoReader::BunchHeader DemoReader::readBunchHeader(BitReader& reader, const std::uint32_t packet) {
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
        header.sequence = (header.channel < channels.size() ? channels[header.channel].reliableSequence : 0) + 1;
    } else if (header.partial) {
        // An unreliable partial bunch takes the packet's sequence.
        header.sequence = static_cast<std::int32_t>(packet);
    }
    // Valorant's own bit, which Unreal 5.3 does not have.
    reader.readBit();
    header.partialInitial = header.partial && reader.readBit();
    header.partialFinal = header.partial && reader.readBit();
    if (header.reliable || header.open) header.isActorChannel = PackageMap::readName(reader, nameText) == actorChannelName;
    return header;
}

// UChannel::ReceivedRawBunch and ReceivedNextBunch: a partial bunch waits for the rest of its payload.
void DemoReader::receivedRawBunch(Channel& channel, const BunchHeader& header, BitReader& payload) {
    if (header.reliable) channel.reliableSequence = header.sequence;
    if (!header.partial) {
        receivedSequencedBunch(channel, header, payload);
        return;
    }
    PartialBunch& partial = channel.partial;
    const std::int64_t bits = header.hasPackageMapExports ? 0 : payload.getBitsLeft();
    if (header.partialInitial) {
        if (channel.hasPartial && !partial.header.partialFinal)
            report(std::format("replay.partial_bunch: A partial bunch of channel {} at {:.3f} s did not complete.", header.channel, frameTime));
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
            report(std::format("replay.partial_bunch: A partial bunch of channel {} at {:.3f} s does not continue a partial bunch.",
                               header.channel, frameTime));
            channel.hasPartial = false;
            return;
        }
        partial.header.sequence = header.sequence;
    }
    // Only the final part may end within a byte.
    if (partial.bits % 8 != 0 && bits > 0) return fail(std::format("A partial bunch of channel {} at {:.3f} s is not byte aligned.", header.channel, frameTime));
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
    BitReader joinedPayload{std::span(partial.data), partial.bits};
    receivedSequencedBunch(channel, joined, joinedPayload);
}

// UChannel::ReceivedSequencedBunch: the bunch, then the close, which destroys the channel.
void DemoReader::receivedSequencedBunch(Channel& channel, const BunchHeader& header, BitReader& payload) {
    if (channel.isActorChannel) {
        // UActorChannel::ReceivedBunch.
        if (header.hasMustBeMappedGuids) {
            const std::uint16_t count = payload.readUInt16();
            for (std::uint16_t i = 0; i < count; ++i) payload.serializeIntPacked();
            if (!check(payload, "must be mapped NetGUIDs", header)) return;
        }
        processBunch(channel, header, payload);
    }
    if (!header.close || failure) return;
    if (channel.hasActor) log.add({.kind = EventKind::ActorClose, .closeReason = header.closeReason, .channel = header.channel, .actor = channel.actor});
    channel = Channel();
}

// UActorChannel::ProcessBunch.
void DemoReader::processBunch(Channel& channel, const BunchHeader& header, BitReader& payload) {
    if (!channel.hasActor) {
        // A channel without its actor ignores the bunches until the open one.
        if (!header.open || !serializeNewActor(channel, header, payload)) return;
    }
    while (!payload.atEnd()) {
        readContentBlock(channel, header.channel, payload);
        if (!check(payload, "content block", header)) return;
    }
    log.add({.kind = EventKind::BunchEnd, .channel = header.channel, .actor = channel.actor});
}

bool DemoReader::serializeNewActor(Channel& channel, const BunchHeader& header, BitReader& payload) {
    channel.hasActor = true;
    channel.actor = packageMap.loadObject(payload, false);
    Event open{.kind = EventKind::ActorOpen, .channel = header.channel, .actor = channel.actor, .name = packageMap.getName(channel.actor)};
    // A dynamic actor's spawn data, unless a closing bunch leaves it out.
    if (isDynamicGuid(channel.actor) && !(payload.atEnd() && header.close)) {
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
    if (!check(payload, "SerializeNewActor", header)) return false;
    log.add(open);
    // APlayerController::OnActorChannelOpen.
    if (isDynamicGuid(channel.actor) && std::ranges::find(playerControllerClasses, channel.classPath) != std::ranges::end(playerControllerClasses)) payload.readByte();
    return true;
}

// UActorChannel::ReadContentBlockPayload: the header, then the payload, which Valorant obfuscates; it is decoded with the
// other blocks of the data chunk.
void DemoReader::readContentBlock(const Channel& channel, const std::uint32_t index, BitReader& payload) {
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
    const std::span<std::uint8_t> content = log.allocatePayload(bits);
    payload.readBits(content, bits);
    chunkBlocks.back().push_back({.event = log.getPending().back().size(), .payload = content});
    block.payload = content;
    block.payloadBits = bits;
    log.add(block);
}

bool DemoReader::check(const BitReader& reader, const std::string_view what, const BunchHeader& header) {
    if (!reader.isError()) return true;
    fail(std::format("The {} of a bunch of channel {} at {:.3f} s (packet {}) cannot be read.", what, header.channel, frameTime, packetId));
    return false;
}

void DemoReader::fail(std::string message) {
    if (!failure) failure = Error{ErrorCode::Parse, std::move(message)};
}

void DemoReader::finish() {
    commitEvents();
    for (std::size_t index = 0; index < channels.size(); ++index)
        if (channels[index].hasPartial) report(std::format("replay.partial_bunch: A partial bunch of channel {} did not complete before the end.", index));
}

void DemoReader::report(std::string problem) { problems.push_back(std::move(problem)); }

}
