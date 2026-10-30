#include "local_file_replay.h"

#include <charconv>
#include <cstring>
#include <format>

#include "byte_reader.h"

// ooz: kraken.cpp. Returns the decoded size, or -1.
int Kraken_Decompress(const std::uint8_t* source, std::size_t sourceSize, std::uint8_t* destination, std::size_t destinationSize);

namespace vrfparse {

namespace {

// Valorant's file magic; Unreal's FLocalFileNetworkReplayStreamer::FileMagic is 0x1CA2E27F.
constexpr std::uint32_t fileMagic = 0x43F4EFDD;
// FLocalFileReplayCustomVersion::LatestVersion of Unreal 5.3: the replay info has every field below.
constexpr std::uint32_t fileVersion = 7;
constexpr std::uint32_t networkMagic = 0x2CF5A13D;
// ENetworkVersionHistory and FEngineNetworkCustomVersion of Unreal 5.3: the packet and bunch formats that this reader
// reads.
constexpr std::uint32_t networkVersion = 19;
constexpr std::uint32_t engineNetworkProtocolVersion = 32;
// FEngineVersion of the recording build.
constexpr std::uint16_t engineMajor = 5;
constexpr std::uint16_t engineMinor = 3;
// ooz reads and writes up to 64 bytes past its buffers ("Kraken_Decompress").
constexpr std::size_t oodleSlack = 64;
// FCustomVersion: a GUID and the version.
constexpr std::uint64_t customVersionSize = 20;

// ELocalFileChunkType.
enum class ChunkType : std::uint32_t { Header, ReplayData, Checkpoint, Event };

std::unexpected<Error> damaged(const std::string_view what, const ByteReader& reader) {
    return fail(ErrorCode::Parse, std::format("The {} is damaged at byte {}: {}.", what, reader.getPosition(), reader.getReason()));
}

}

bool LocalFileReplay::File::open(const std::filesystem::path& path) {
    stream.open(path, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    size = static_cast<std::uint64_t>(stream.tellg());
    return static_cast<bool>(stream);
}

void LocalFileReplay::File::read(const std::span<std::uint8_t> bytes) {
    if (isError() || bytes.size() > size - offset) return setError("the file ends");
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) return setError("the file cannot be read");
    offset += bytes.size();
}

void LocalFileReplay::File::read(std::vector<std::uint8_t>& buffer, const std::size_t count, const std::size_t slack) {
    if (isError() || count > size - offset) return setError("the file ends");
    buffer.assign(count + slack, 0);
    read(std::span(buffer).first(count));
}

std::string LocalFileReplay::File::readString() {
    const auto length = read<std::int32_t>();
    // A negative length counts UTF-16 characters.
    const std::uint64_t count = length < 0 ? static_cast<std::uint64_t>(-std::int64_t{length}) * 2 : static_cast<std::uint64_t>(length);
    if (isError() || count > size - offset) {
        setError("the file ends");
        return {};
    }
    std::vector<std::uint8_t> bytes(sizeof(length) + count);
    std::memcpy(bytes.data(), &length, sizeof(length));
    read(std::span(bytes).subspan(sizeof(length)));
    ByteReader reader(bytes);
    std::string text = reader.readString();
    if (reader.isError()) setError("a string does not decode");
    return text;
}

void LocalFileReplay::File::skip(const std::uint64_t count) {
    if (isError() || count > size - offset) return setError("the file ends");
    offset += count;
}

void LocalFileReplay::File::seek(const std::uint64_t to) {
    if (to > size) return setError("the file ends");
    offset = to;
}

std::unexpected<Error> LocalFileReplay::File::fail() const {
    return vrfparse::fail(ErrorCode::Parse, std::format("The replay file is damaged at byte {}: {}.", offset, reason));
}

// FLocalFileNetworkReplayStreamer::ReadReplayInfo, then the chunk headers. Only the header and the event chunks are read
// whole; the replay data chunks and the checkpoints are read when they are decompressed.
Result<> LocalFileReplay::open(const std::filesystem::path& path) {
    if (!file.open(path)) return fail(ErrorCode::Open, std::format("'{}' cannot be opened.", path.string()));
    if (file.read<std::uint32_t>() != fileMagic) return fail(ErrorCode::Parse, "The file is not a Valorant replay.");
    const auto version = file.read<std::uint32_t>();
    if (file.isError()) return file.fail();
    if (version != fileVersion) return fail(ErrorCode::Version, std::format("The replay file version {} is not supported.", version));
    const auto customVersionCount = file.read<std::int32_t>();
    if (customVersionCount < 0) file.setError("the custom version count is negative");
    file.skip(static_cast<std::uint64_t>(customVersionCount) * customVersionSize);
    lengthMs = file.read<std::int32_t>();
    // NetworkVersion and Changelist, then FriendlyName: an FString, negative for UTF-16 characters.
    file.skip(8);
    const auto nameLength = file.read<std::int32_t>();
    file.skip(static_cast<std::uint64_t>(nameLength < 0 ? -std::int64_t{nameLength} * 2 : std::int64_t{nameLength}));
    // bIsLive and Timestamp.
    file.skip(12);
    compressed = file.read<std::uint32_t>() != 0;
    if (file.read<std::uint32_t>() != 0 && !file.isError()) return fail(ErrorCode::Encrypted, "The replay is encrypted.");
    const auto keyLength = file.read<std::int32_t>();
    if (keyLength < 0) file.setError("the key length is negative");
    file.skip(static_cast<std::uint64_t>(keyLength));

    std::vector<std::uint8_t> bytes;
    while (!file.atEnd() && !file.isError()) {
        const auto type = static_cast<ChunkType>(file.read<std::uint32_t>());
        const auto size = file.read<std::int32_t>();
        if (size < 0) file.setError("a chunk size is negative");
        if (file.isError()) break;
        const std::uint64_t chunkEnd = file.getOffset() + static_cast<std::uint64_t>(size);
        switch (type) {
        case ChunkType::Header:
            if (!header.empty()) return fail(ErrorCode::Parse, "The replay has two header chunks.");
            file.read(header, static_cast<std::size_t>(size), 0);
            break;
        case ChunkType::ReplayData: {
            if (header.empty()) return fail(ErrorCode::Parse, "Replay data comes before the header.");
            // Time1, Time2.
            file.skip(8);
            const auto dataSize = file.read<std::int32_t>();
            const auto memorySize = file.read<std::int32_t>();
            if (dataSize < 0 || memorySize < 0 || file.getOffset() + static_cast<std::uint64_t>(dataSize) > chunkEnd)
                file.setError("the data sizes do not fit the chunk");
            dataChunks.push_back({.offset = file.getOffset(), .size = static_cast<std::uint32_t>(dataSize), .memorySize = static_cast<std::uint32_t>(memorySize)});
            break;
        }
        case ChunkType::Event: {
            file.read(bytes, static_cast<std::size_t>(size), 0);
            ByteReader chunk(bytes);
            Marker& marker = markers.emplace_back();
            marker.id = chunk.readString();
            marker.group = chunk.readString();
            marker.metadata = chunk.readString();
            marker.time = static_cast<float>(chunk.readUInt32()) / 1000.0f;
            marker.endTime = static_cast<float>(chunk.readUInt32()) / 1000.0f;
            if (chunk.isError()) return damaged("replay event chunk", chunk);
            break;
        }
        case ChunkType::Checkpoint: {
            // Id and Group, then Metadata: the index of the data chunk after the checkpoint.
            file.readString();
            file.readString();
            const std::string metadata = file.readString();
            // Time1, Time2.
            file.skip(8);
            const auto dataSize = file.read<std::int32_t>();
            if (dataSize < 0 || file.getOffset() + static_cast<std::uint64_t>(dataSize) > chunkEnd) file.setError("the checkpoint size does not fit the chunk");
            if (file.isError()) break;
            Checkpoint& checkpoint = checkpoints.emplace_back(Checkpoint{.offset = file.getOffset(), .size = static_cast<std::uint32_t>(dataSize)});
            const char* const metadataEnd = metadata.data() + metadata.size();
            const auto [end, parsed] = std::from_chars(metadata.data(), metadataEnd, checkpoint.dataChunk);
            if (parsed != std::errc() || end != metadataEnd)
                return fail(ErrorCode::Parse, std::format("Checkpoint {} names no data chunk: '{}'.", checkpoints.size() - 1, metadata));
            break;
        }
        }
        file.seek(chunkEnd);
    }
    if (file.isError()) return file.fail();
    if (header.empty()) return fail(ErrorCode::Parse, "The replay has no header chunk.");
    return {};
}

Result<> LocalFileReplay::readDataChunk(const std::size_t index, std::vector<std::uint8_t>& frames) {
    const DataChunk& chunk = dataChunks[index];
    return readChunkData(chunk.offset, chunk.size, chunk.memorySize, frames);
}

Result<> LocalFileReplay::readCheckpoint(const Checkpoint& checkpoint, std::vector<std::uint8_t>& data) {
    return readChunkData(checkpoint.offset, checkpoint.size, std::nullopt, data);
}

// The sizes, then the Oodle data.
Result<> LocalFileReplay::readChunkData(const std::uint64_t offset, const std::uint32_t size, const std::optional<std::uint32_t> memorySize,
                                       std::vector<std::uint8_t>& data) {
    file.seek(offset);
    if (!compressed) {
        file.read(data, size, 0);
        return file.isError() ? file.fail() : Result<>();
    }
    const auto decompressedSize = file.read<std::int32_t>();
    const auto compressedSize = file.read<std::int32_t>();
    if (decompressedSize < 0 || (memorySize && static_cast<std::uint32_t>(decompressedSize) != *memorySize) || compressedSize < 0 ||
        static_cast<std::uint64_t>(compressedSize) + 8 > size)
        file.setError("the compressed sizes do not match the chunk");
    // The slack keeps ooz's reads and writes past the ends in the buffers.
    file.read(compressedData, static_cast<std::size_t>(compressedSize), oodleSlack);
    if (file.isError()) return file.fail();
    data.resize(static_cast<std::size_t>(decompressedSize) + oodleSlack);
    const int decoded = Kraken_Decompress(compressedData.data(), static_cast<std::size_t>(compressedSize), data.data(), static_cast<std::size_t>(decompressedSize));
    if (decoded != decompressedSize) return fail(ErrorCode::Parse, std::format("The chunk at byte {} does not decompress.", offset));
    data.resize(static_cast<std::size_t>(decompressedSize));
    return {};
}

// FNetworkDemoHeader's operator<<, with Valorant's block of its own after the engine version.
Result<> NetworkDemoHeader::read(const std::span<const std::uint8_t> data) {
    ByteReader reader(data);
    if (reader.readUInt32() != networkMagic) return fail(ErrorCode::Parse, "The replay header's network magic does not match.");
    const std::uint32_t version = reader.readUInt32();
    if (version != networkVersion)
        return fail(ErrorCode::Version, std::format("The replay network version {} is not supported; {} is.", version, networkVersion));
    const std::int32_t customVersionCount = reader.readInt32();
    if (customVersionCount < 0) reader.setError("the custom version count is negative");
    reader.skip(static_cast<std::uint64_t>(customVersionCount) * customVersionSize);
    // NetworkChecksum.
    reader.skip(4);
    const std::uint32_t protocolVersion = reader.readUInt32();
    if (!reader.isError() && protocolVersion != engineNetworkProtocolVersion)
        return fail(ErrorCode::Version, std::format("The replay engine network protocol {} is not supported; {} is.", protocolVersion,
                                                    engineNetworkProtocolVersion));
    // GameNetworkProtocolVersion, Guid.
    reader.skip(20);
    const std::uint16_t major = reader.readUInt16();
    const std::uint16_t minor = reader.readUInt16();
    // Patch.
    reader.skip(2);
    changelist = reader.readUInt32();
    branch = reader.readString();
    if (!reader.isError() && (major != engineMajor || minor != engineMinor))
        return fail(ErrorCode::Version, std::format("The replay was recorded with Unreal {}.{} ({}); only {}.{} is supported.", major, minor,
                                                    branch, engineMajor, engineMinor));
    reader.skip(reader.readUInt32());
    // FPackageFileVersion and the licensee version.
    reader.skip(12);
    const std::int32_t levelCount = reader.readInt32();
    if (levelCount < 0) reader.setError("the level count is negative");
    for (std::int32_t i = 0; i < levelCount && !reader.isError(); ++i) {
        levels.push_back(reader.readString());
        // The time of the level.
        reader.skip(4);
    }
    flags = reader.readUInt32();
    if (reader.isError()) return damaged("replay header", reader);
    return {};
}

}
