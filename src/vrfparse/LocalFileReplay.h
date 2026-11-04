#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vrfparse/Error.h"
#include "vrfparse/Match.h"

namespace vrfparse {

class LocalFileReplay {
public:
    struct Checkpoint {
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
        std::size_t dataChunk = 0;
        float time = 0;
    };

    struct DataChunk {
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
        std::uint32_t memorySize = 0;
        float startTime = 0;
        float endTime = 0;
    };

    [[nodiscard]] std::expected<void, Error> open(const std::filesystem::path& path);

    std::int32_t getLengthMs() const { return lengthMs; }
    const std::string& getMatchId() const { return matchId; }
    std::span<const std::uint8_t> getHeader() const { return header; }
    std::vector<Marker>& getMarkers() { return markers; }
    std::span<const DataChunk> getDataChunks() const { return dataChunks; }
    [[nodiscard]] std::expected<void, Error> readDataChunk(std::size_t index, std::vector<std::uint8_t>& frames);
    std::span<const Checkpoint> getCheckpoints() const { return checkpoints; }
    [[nodiscard]] std::expected<void, Error> readCheckpoint(const Checkpoint& checkpoint, std::vector<std::uint8_t>& data);

private:
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
        void read(std::vector<std::uint8_t>& buffer, std::size_t count, std::size_t slack);
        std::string readString();
        void skip(std::uint64_t count);
        void seek(std::uint64_t to);
        std::uint64_t getOffset() const { return offset; }
        bool atEnd() const { return offset == size; }
        bool isError() const { return reason != nullptr; }
        void setError(const char* why) {
            if (!reason)
                reason = why;
        }
        std::unexpected<Error> fail() const;

    private:
        //! In the browser, each file read has a high fixed cost.
        std::vector<char> buffer = std::vector<char>(1 << 20);
        std::ifstream stream;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
        const char* reason = nullptr;
    };

    [[nodiscard]] std::expected<void, Error> readChunkData(std::uint64_t offset, std::uint32_t size, std::optional<std::uint32_t> memorySize,
                                         std::vector<std::uint8_t>& data);

    File file;
    std::int32_t lengthMs = 0;
    std::string matchId;
    bool compressed = false;
    std::vector<std::uint8_t> header;
    std::vector<Marker> markers;
    std::vector<DataChunk> dataChunks;
    std::vector<Checkpoint> checkpoints;
    std::vector<std::uint8_t> compressedData;
};

enum class ReplayHeaderFlags : std::uint32_t {
    ClientRecorded = 1 << 0,
    HasStreamingFixes = 1 << 1,
    DeltaCheckpoints = 1 << 2,
    GameSpecificFrameData = 1 << 3,
};

struct NetworkDemoHeader {
    [[nodiscard]] std::expected<void, Error> read(std::span<const std::uint8_t> data);

    bool hasFlag(ReplayHeaderFlags flag) const { return (flags & static_cast<std::uint32_t>(flag)) != 0; }

    std::string branch;
    std::uint32_t changelist = 0;
    std::vector<std::string> levels;
    std::uint32_t flags = 0;
};

}
