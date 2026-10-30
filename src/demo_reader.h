#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include "local_file_replay.h"
#include "package_map.h"
#include "payload_transform.h"
#include "vrfparse/error.h"
#include "vrfparse/event.h"

namespace vrfparse {

class ByteReader;

// UDemoNetDriver's playback of the demo frames, as far as reading goes: UNetConnection::ReceivedPacket splits the packets
// into bunches, UChannel joins partial bunches, and UActorChannel::ProcessBunch reads the actor and its content blocks.
// What they carry goes to the event log; the payloads stay serialized, but without Valorant's obfuscation.
class DemoReader {
public:
    // The transform is the one of the header's game release.
    DemoReader(const NetworkDemoHeader& header, PayloadTransform transform, EventLog& log, std::vector<std::string>& problems);
    DemoReader(const DemoReader&) = delete;
    DemoReader& operator=(const DemoReader&) = delete;

    // UDemoNetDriver::ReadDemoFrame for each frame of a replay data chunk. After an error, the reader reads no more.
    [[nodiscard]] std::expected<void, Error> readFrames(std::span<const std::uint8_t> data);
    // UDemoNetDriver::LoadCheckpoint of a checkpoint that is not a delta: the NetGUID cache, the export groups, and a frame
    // that opens each channel with its state. Read it first; the frames that follow are the frames after it.
    [[nodiscard]] std::expected<void, Error> readCheckpoint(std::span<const std::uint8_t> data);
    // The end of the replay: commits the events to the log. A partial bunch that did not complete is a problem.
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
        // The channel's name is "Actor".
        bool isActorChannel = false;
    };

    // UChannel::InPartialBunch: the payloads of the bunches so far.
    struct PartialBunch {
        BunchHeader header;
        std::vector<std::uint8_t> data;
        std::int64_t bits = 0;
    };

    // UActorChannel, or another channel, whose bunches the replay does not need.
    // A closed channel is a default Channel.
    struct Channel {
        bool isOpen = false;
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

    // A chunk's events and content blocks start, and end with the content blocks decoded.
    void beginChunk();
    [[nodiscard]] std::expected<void, Error> endChunk(const ByteReader& reader);
    void readFrame(ByteReader& reader);
    void receivedPacket(std::span<const std::uint8_t> packet);
    BunchHeader readBunchHeader(BitReader& reader, std::uint32_t packetId);
    void receivedRawBunch(Channel& channel, const BunchHeader& header, BitReader& payload);
    void receivedSequencedBunch(Channel& channel, const BunchHeader& header, BitReader& payload);
    void processBunch(Channel& channel, const BunchHeader& header, BitReader& payload);
    bool serializeNewActor(Channel& channel, const BunchHeader& header, BitReader& payload);
    void readContentBlock(const Channel& channel, std::uint32_t index, BitReader& payload);
    void decodeContentBlocks();
    void commitEvents();
    // False, with the failure, after a read past the end of the bunch.
    bool check(const BitReader& reader, std::string_view what, const BunchHeader& header);
    // Keeps the first failure.
    void fail(std::string message);
    void report(std::string problem);

    EventLog& log;
    std::vector<std::string>& problems;
    const PayloadTransform transform;
    const bool hasStreamingFixes;
    const bool hasDeltaCheckpoints;
    const bool hasGameSpecificFrameData;
    PackageMap packageMap;
    // UNetConnection::Channels, by index.
    std::vector<Channel> channels;
    std::optional<Error> failure;
    // The text of the name that is read.
    std::string nameText;
    std::uint32_t packetId = 0;
    float frameTime = 0;
    float latestFrameTime = -1;
    // A content block of a chunk's pending events: its payload, the count of the parts that it splits into, and, once the
    // events are committed, the names of its fields and the count of the parts of the blocks before it.
    struct ContentBlock {
        std::size_t event = 0;
        std::span<std::uint8_t> payload;
        std::size_t parts = 0;
        bool readable = true;
        std::span<const std::string_view> names;
        std::size_t partsBefore = 0;
    };
    // The content blocks of each chunk's pending events.
    std::vector<std::vector<ContentBlock>> chunkBlocks;
};

}
