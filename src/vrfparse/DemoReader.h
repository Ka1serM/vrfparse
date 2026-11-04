#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include "vrfparse/LocalFileReplay.h"
#include "vrfparse/PackageMap.h"
#include "vrfparse/PayloadTransform.h"
#include "vrfparse/Error.h"
#include "vrfparse/Event.h"

namespace vrfparse {

class ByteReader;

class DemoReader {
public:
    DemoReader(const NetworkDemoHeader& header, PayloadTransform transform, EventLog& log, std::vector<std::string>& problems);
    DemoReader(const DemoReader&) = delete;
    DemoReader& operator=(const DemoReader&) = delete;

    [[nodiscard]] std::expected<void, Error> readFrames(std::span<const std::uint8_t> data);
    [[nodiscard]] std::expected<void, Error> readCheckpoint(std::span<const std::uint8_t> data);
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
        bool isActorChannel = false;
    };

    struct PartialBunch {
        BunchHeader header;
        std::vector<std::uint8_t> data;
        std::int64_t bits = 0;
    };

    struct Channel {
        bool isOpen = false;
        bool isActorChannel = false;
        std::int32_t reliableSequence = 0;
        bool hasPartial = false;
        PartialBunch partial;
        bool hasActor = false;
        std::uint32_t actor = 0;
        std::uint32_t archetype = 0;
        std::string_view classPath;
    };

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
    bool check(const BitReader& reader, std::string_view what, const BunchHeader& header);
    void fail(std::string message);
    void report(std::string problem);

    EventLog& log;
    std::vector<std::string>& problems;
    const PayloadTransform transform;
    const bool hasStreamingFixes;
    const bool hasDeltaCheckpoints;
    const bool hasGameSpecificFrameData;
    PackageMap packageMap;
    std::vector<Channel> channels;
    std::optional<Error> failure;
    std::string nameText;
    std::uint32_t packetId = 0;
    float frameTime = 0;
    float latestFrameTime = -1;
    struct ContentBlock {
        std::size_t event = 0;
        std::span<std::uint8_t> payload;
        std::size_t parts = 0;
        bool readable = true;
        std::span<const std::string_view> names;
        std::size_t partsBefore = 0;
    };
    std::vector<std::vector<ContentBlock>> chunkBlocks;
};

}
