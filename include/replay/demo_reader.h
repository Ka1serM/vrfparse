#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "replay/event.h"
#include "replay/local_file_replay.h"
#include "replay/package_map.h"
#include "replay/payload_transform.h"

namespace net {
class BitReader;
}

namespace replay {

class ByteReader;

// UDemoNetDriver's playback of the demo frames, as far as reading goes: UNetConnection::ReceivedPacket splits the packets
// into bunches, UChannel joins partial bunches, and UActorChannel::ProcessBunch reads the actor and its content blocks.
// What they carry goes to the event log; the payloads stay serialized.
class DemoReader {
public:
    // Throws replay::Error for a game release whose payload transform is not known.
    DemoReader(const NetworkDemoHeader& header, EventLog& log, std::vector<std::string>& problems);
    DemoReader(const DemoReader&) = delete;
    DemoReader& operator=(const DemoReader&) = delete;

    // UDemoNetDriver::ReadDemoFrame for each frame of a replay data chunk. Throws replay::Error.
    void readFrames(std::span<const std::uint8_t> data);
    // The end of the replay: a partial bunch that did not complete is a problem.
    void finish();

private:
    struct BunchHeader {
        bool open = false;
        bool close = false;
        ChannelCloseReason closeReason = ChannelCloseReason::Destroyed;
        bool reliable = false;
        bool hasPackageMapExports = false;
        bool hasMustBeMappedGuids = false;
        bool partial = false;
        bool partialInitial = false;
        bool partialFinal = false;
        std::uint32_t channel = 0;
        std::int32_t sequence = 0;
        std::string channelName;
    };

    // UChannel::InPartialBunch: the payloads of the bunches so far.
    struct PartialBunch {
        BunchHeader header;
        std::vector<std::uint8_t> data;
        std::int64_t bits = 0;
    };

    // UActorChannel, or another channel, whose bunches the replay does not need.
    struct Channel {
        bool isActorChannel = false;
        // UChannel::InReliable.
        std::int32_t reliableSequence = 0;
        bool hasPartial = false;
        PartialBunch partial;
        // UActorChannel::Actor: set once the open bunch read the actor.
        bool hasActor = false;
        std::uint32_t actor = 0;
        std::uint32_t archetype = 0;
        std::string_view classPath;
    };

    void readFrame(ByteReader& reader);
    void receivedPacket(std::span<const std::uint8_t> packet);
    BunchHeader readBunchHeader(net::BitReader& reader, std::uint32_t packetId);
    void receivedRawBunch(Channel& channel, const BunchHeader& header, net::BitReader& payload);
    void receivedSequencedBunch(const BunchHeader& header, net::BitReader& payload);
    void processBunch(Channel& channel, const BunchHeader& header, net::BitReader& payload);
    void serializeNewActor(Channel& channel, const BunchHeader& header, net::BitReader& payload);
    void readContentBlock(const Channel& channel, std::uint32_t index, net::BitReader& payload);
    void readFields(const Event& block, net::BitReader& content);
    std::span<const std::uint8_t> copyPayload(net::BitReader& reader, std::int64_t bits);
    // Throws "replay.parse" for a read past the end of the bunch.
    void check(const net::BitReader& reader, std::string_view what, const BunchHeader& header) const;
    void report(std::string problem);

    EventLog& log;
    std::vector<std::string>& problems;
    const PayloadTransform transform;
    const bool hasStreamingFixes;
    const bool hasGameSpecificFrameData;
    PackageMap packageMap;
    std::unordered_map<std::uint32_t, Channel> channels;
    std::uint32_t packetId = 0;
    float frameTime = 0;
    float latestFrameTime = -1;
    // The decoded payload of the content block that is read.
    std::vector<std::uint8_t> content;
};

}
