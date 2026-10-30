#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vrfparse/error.h"
#include "vrfparse/match.h"

namespace vrfparse {

// FLocalFileNetworkReplayStreamer's view of a .vrf file: the replay info, the header chunk, the event chunks, the replay
// data chunks, which hold the demo frames, and the checkpoints. It keeps the file open and reads a data chunk or a
// checkpoint when it is asked for.
class LocalFileReplay {
public:
    // A checkpoint chunk: the state of the replay at its time. The frames after it start at the data chunk, which is the
    // data chunk count when no frames follow.
    struct Checkpoint {
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
        std::size_t dataChunk = 0;
    };

    // Reads the replay info and the chunk headers.
    [[nodiscard]] Result<> open(const std::filesystem::path& path);

    std::int32_t getLengthMs() const { return lengthMs; }
    std::span<const std::uint8_t> getHeader() const { return header; }
    // In file order.
    std::vector<Marker>& getMarkers() { return markers; }
    std::size_t getDataChunkCount() const { return dataChunks.size(); }
    // Decompresses the demo frames of a replay data chunk into the buffer.
    [[nodiscard]] Result<> readDataChunk(std::size_t index, std::vector<std::uint8_t>& frames);
    // In file order.
    std::span<const Checkpoint> getCheckpoints() const { return checkpoints; }
    // Decompresses the checkpoint into the buffer.
    [[nodiscard]] Result<> readCheckpoint(const Checkpoint& checkpoint, std::vector<std::uint8_t>& data);

private:
    // The file, read at offsets. As ByteReader, a read that does not fit sets the error, which stays.
    class File {
    public:
        bool open(const std::filesystem::path& path);

        template <class Value>
        Value read() {
            Value value{};
            read(std::span(reinterpret_cast<std::uint8_t*>(&value), sizeof(Value)));
            return value;
        }
        void read(std::span<std::uint8_t> bytes);
        // Resizes the buffer to the bytes and the zeroed slack bytes after them, and reads the bytes.
        void read(std::vector<std::uint8_t>& buffer, std::size_t count, std::size_t slack);
        // FString, as ByteReader::readString reads it.
        std::string readString();
        void skip(std::uint64_t count);
        void seek(std::uint64_t to);
        std::uint64_t getOffset() const { return offset; }
        bool atEnd() const { return offset == size; }
        bool isError() const { return reason != nullptr; }
        void setError(const char* why) {
            if (!reason) reason = why;
        }
        // The error as a damaged file at the offset.
        std::unexpected<Error> fail() const;

    private:
        std::ifstream stream;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
        const char* reason = nullptr;
    };

    // A ReplayData chunk: where its data is in the file, and its size once decompressed.
    struct DataChunk {
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
        std::uint32_t memorySize = 0;
    };

    // FLocalFileNetworkReplayStreamer::DecompressBuffer of the chunk data at the offset, or the data as it is when the
    // replay is not compressed. A compressed size other than the memory size, when it is known, is damage.
    [[nodiscard]] Result<> readChunkData(std::uint64_t offset, std::uint32_t size, std::optional<std::uint32_t> memorySize,
                                         std::vector<std::uint8_t>& data);

    File file;
    std::int32_t lengthMs = 0;
    bool compressed = false;
    std::vector<std::uint8_t> header;
    std::vector<Marker> markers;
    std::vector<DataChunk> dataChunks;
    std::vector<Checkpoint> checkpoints;
    std::vector<std::uint8_t> compressedData;
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
    // Reads the header chunk. A replay of a network version other than Unreal 5.3's is not supported.
    [[nodiscard]] Result<> read(std::span<const std::uint8_t> data);

    bool hasFlag(ReplayHeaderFlags flag) const { return (flags & static_cast<std::uint32_t>(flag)) != 0; }

    // The branch of the build, such as "++Ares-Core+release-13.08".
    std::string branch;
    std::uint32_t changelist = 0;
    // The level names: the map, such as "/Game/Maps/Ascent/Ascent".
    std::vector<std::string> levels;
    std::uint32_t flags = 0;
};

}
