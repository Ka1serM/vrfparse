#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace replay {

// FLocalFileEventInfo: an event that the recording game added with AddEvent, such as the group "roundStarted" with the
// zero-based round number as metadata. Times are replay seconds.
struct Marker {
    std::string id;
    std::string group;
    std::string metadata;
    float time = 0;
    float endTime = 0;
};

// FLocalFileNetworkReplayStreamer's view of a .vrf file: the replay info, the header chunk, the event chunks, and the
// replay data chunks, which hold the demo frames.
class LocalFileReplay {
public:
    // Reads the file. Throws replay::Error.
    explicit LocalFileReplay(const std::filesystem::path& file);
    LocalFileReplay(const LocalFileReplay&) = delete;
    LocalFileReplay& operator=(const LocalFileReplay&) = delete;

    std::int32_t getLengthMs() const { return lengthMs; }
    std::span<const std::uint8_t> getHeader() const { return header; }
    // In file order.
    std::span<const Marker> getMarkers() const { return markers; }
    std::size_t getDataChunkCount() const { return dataChunks.size(); }
    // The demo frames of a replay data chunk, decompressed. Throws replay::Error.
    std::vector<std::uint8_t> readDataChunk(std::size_t index) const;

private:
    // A ReplayData chunk: its bytes in the file, and their size once decompressed.
    struct DataChunk {
        std::span<const std::uint8_t> data;
        std::int32_t memorySize = 0;
    };

    std::vector<std::uint8_t> bytes;
    std::int32_t lengthMs = 0;
    bool compressed = false;
    std::span<const std::uint8_t> header;
    std::vector<Marker> markers;
    std::vector<DataChunk> dataChunks;
};

// EReplayHeaderFlags.
enum class ReplayHeaderFlags : std::uint32_t {
    ClientRecorded = 1 << 0,
    HasStreamingFixes = 1 << 1,
    DeltaCheckpoints = 1 << 2,
    GameSpecificFrameData = 1 << 3,
};

// FNetworkDemoHeader of the header chunk, with the fields that the playback needs.
struct NetworkDemoHeader {
    // Reads the header chunk. Throws replay::Error for a replay that this reader does not support: a network version
    // other than Unreal 5.3's.
    explicit NetworkDemoHeader(std::span<const std::uint8_t> data);

    bool hasFlag(ReplayHeaderFlags flag) const { return (flags & static_cast<std::uint32_t>(flag)) != 0; }

    // The branch of the build, such as "++Ares-Core+release-13.08".
    std::string branch;
    std::uint32_t changelist = 0;
    // The level names: the map, such as "/Game/Maps/Ascent/Ascent".
    std::vector<std::string> levels;
    std::uint32_t flags = 0;
};

}
