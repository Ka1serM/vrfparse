#include "replay/local_file_replay.h"

#include <fmt/format.h>

#include <algorithm>
#include <fstream>
#include <iterator>

#include "replay/byte_reader.h"

// ooz: kraken.cpp. Returns the decoded size, or -1.
int Kraken_Decompress(const std::uint8_t* source, std::size_t sourceSize, std::uint8_t* destination, std::size_t destinationSize);

namespace replay {

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

// ELocalFileChunkType.
enum class ChunkType : std::uint32_t { Header, ReplayData, Checkpoint, Event };

std::vector<std::uint8_t> readFile(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw Error("replay.open", fmt::format("'{}' cannot be opened.", file.string()));
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad()) throw Error("replay.open", fmt::format("'{}' cannot be read.", file.string()));
    return bytes;
}

}

// FLocalFileNetworkReplayStreamer::ReadReplayInfo.
LocalFileReplay::LocalFileReplay(const std::filesystem::path& file) : bytes(readFile(file)) {
    const std::size_t fileSize = bytes.size();
    // Room for what the Oodle decoder reads past the last chunk.
    bytes.resize(fileSize + oodleSlack);
    ByteReader reader{std::span(bytes).first(fileSize), "replay info"};
    if (reader.readUInt32() != fileMagic) throw Error("replay.parse", "The file is not a Valorant replay.");
    const std::uint32_t version = reader.readUInt32();
    if (version != fileVersion) throw Error("replay.version", fmt::format("The replay file version {} is not supported.", version));
    const std::int32_t customVersionCount = reader.readInt32();
    if (customVersionCount < 0) reader.fail("the custom version count is negative");
    // FCustomVersion: a GUID and the version.
    reader.skip(static_cast<std::uint64_t>(customVersionCount) * 20);
    lengthMs = reader.readInt32();
    // NetworkVersion, Changelist, FriendlyName, bIsLive, and Timestamp.
    reader.skip(8);
    reader.readString();
    reader.skip(12);
    compressed = reader.readUInt32() != 0;
    if (reader.readUInt32() != 0) throw Error("replay.encrypted", "The replay is encrypted.");
    reader.readByteArray();

    while (!reader.atEnd()) {
        const auto type = static_cast<ChunkType>(reader.readUInt32());
        const std::int32_t size = reader.readInt32();
        if (size < 0) reader.fail("a chunk size is negative");
        ByteReader chunk(reader.readBytes(static_cast<std::size_t>(size)), "replay chunk");
        switch (type) {
        case ChunkType::Header:
            if (!header.empty()) throw Error("replay.parse", "The replay has two header chunks.");
            header = chunk.readBytes(static_cast<std::size_t>(size));
            break;
        case ChunkType::ReplayData: {
            if (header.empty()) throw Error("replay.parse", "Replay data comes before the header.");
            // Time1, Time2.
            chunk.skip(8);
            const std::int32_t dataSize = chunk.readInt32();
            const std::int32_t memorySize = chunk.readInt32();
            if (memorySize < 0) chunk.fail("the memory size is negative");
            dataChunks.push_back({.data = chunk.readBytes(static_cast<std::size_t>(std::max(dataSize, 0))), .memorySize = memorySize});
            break;
        }
        case ChunkType::Event: {
            Marker& marker = markers.emplace_back();
            marker.id = chunk.readString();
            marker.group = chunk.readString();
            marker.metadata = chunk.readString();
            marker.time = static_cast<float>(chunk.readUInt32()) / 1000.0f;
            marker.endTime = static_cast<float>(chunk.readUInt32()) / 1000.0f;
            break;
        }
        case ChunkType::Checkpoint:
            break;
        }
    }
    if (header.empty()) throw Error("replay.parse", "The replay has no header chunk.");
}

// FLocalFileNetworkReplayStreamer::DecompressBuffer: the sizes, then the Oodle data.
std::vector<std::uint8_t> LocalFileReplay::readDataChunk(const std::size_t index) const {
    const DataChunk& chunk = dataChunks[index];
    if (!compressed) return {chunk.data.begin(), chunk.data.end()};
    ByteReader reader(chunk.data, "replay data chunk");
    const std::int32_t decompressedSize = reader.readInt32();
    const std::int32_t compressedSize = reader.readInt32();
    if (decompressedSize != chunk.memorySize || compressedSize < 0) reader.fail("the compressed sizes do not match the chunk");
    const std::span<const std::uint8_t> input = reader.readBytes(static_cast<std::size_t>(compressedSize));
    // The input is copied so that ooz's reads past its end stay in the buffer.
    std::vector<std::uint8_t> padded(input.size() + oodleSlack);
    std::copy(input.begin(), input.end(), padded.begin());
    std::vector<std::uint8_t> output(static_cast<std::size_t>(decompressedSize) + oodleSlack);
    const int size = Kraken_Decompress(padded.data(), input.size(), output.data(), static_cast<std::size_t>(decompressedSize));
    if (size != decompressedSize) throw Error("replay.parse", fmt::format("Replay data chunk {} does not decompress.", index));
    output.resize(static_cast<std::size_t>(decompressedSize));
    return output;
}

// FNetworkDemoHeader's operator<<, with Valorant's block of its own after the engine version.
NetworkDemoHeader::NetworkDemoHeader(const std::span<const std::uint8_t> data) {
    ByteReader reader(data, "replay header");
    if (reader.readUInt32() != networkMagic) reader.fail("the network magic does not match");
    const std::uint32_t version = reader.readUInt32();
    if (version != networkVersion)
        throw Error("replay.version", fmt::format("The replay network version {} is not supported; {} is.", version, networkVersion));
    const std::int32_t customVersionCount = reader.readInt32();
    if (customVersionCount < 0) reader.fail("the custom version count is negative");
    reader.skip(static_cast<std::uint64_t>(customVersionCount) * 20);
    // NetworkChecksum.
    reader.skip(4);
    const std::uint32_t protocolVersion = reader.readUInt32();
    if (protocolVersion != engineNetworkProtocolVersion)
        throw Error("replay.version", fmt::format("The replay engine network protocol {} is not supported; {} is.", protocolVersion,
                                                  engineNetworkProtocolVersion));
    // GameNetworkProtocolVersion, Guid.
    reader.skip(20);
    const std::uint16_t major = reader.readUInt16();
    const std::uint16_t minor = reader.readUInt16();
    // Patch.
    reader.skip(2);
    changelist = reader.readUInt32();
    branch = reader.readString();
    if (major != engineMajor || minor != engineMinor)
        throw Error("replay.version", fmt::format("The replay was recorded with Unreal {}.{} ({}); only {}.{} is supported.", major, minor,
                                                  branch, engineMajor, engineMinor));
    reader.skip(reader.readUInt32());
    // FPackageFileVersion and the licensee version.
    reader.skip(12);
    const std::int32_t levelCount = reader.readInt32();
    if (levelCount < 0) reader.fail("the level count is negative");
    for (std::int32_t i = 0; i < levelCount; ++i) {
        levels.push_back(reader.readString());
        // The time of the level.
        reader.skip(4);
    }
    flags = reader.readUInt32();
}

}
