#include "vrfparse/match.h"

#include <format>

#include <tbb/parallel_invoke.h>

#include "demo_reader.h"

namespace vrfparse {

Result<> Match::read(const std::filesystem::path& file, const std::stop_token stop) { return read(file, Start::FirstFrame, stop); }

Result<> Match::readFromLastCheckpoint(const std::filesystem::path& file, const std::stop_token stop) {
    return read(file, Start::LastCheckpoint, stop);
}

Result<> Match::read(const std::filesystem::path& file, const Start start, const std::stop_token stop) {
    LocalFileReplay replay;
    if (Result<> opened = replay.open(file); !opened) return opened;
    NetworkDemoHeader header;
    if (Result<> read = header.read(replay.getHeader()); !read) return read;
    const PayloadTransform transform(header.branch);
    if (!transform.isKnown())
        return fail(ErrorCode::Version, std::format("The game release {} is not supported: its payload transform is not known.", header.branch));
    length = static_cast<float>(replay.getLengthMs()) / 1000.0f;
    changelist = header.changelist;
    markers = std::move(replay.getMarkers());
    DemoReader reader(header, transform, log, problems);
    std::vector<std::uint8_t> frames;
    std::size_t firstChunk = 0;
    if (start == Start::LastCheckpoint && !replay.getCheckpoints().empty()) {
        const LocalFileReplay::Checkpoint& checkpoint = replay.getCheckpoints().back();
        if (checkpoint.dataChunk > replay.getDataChunkCount())
            return fail(ErrorCode::Parse, std::format("The last checkpoint names data chunk {}; the replay has {}.", checkpoint.dataChunk,
                                                      replay.getDataChunkCount()));
        if (Result<> read = replay.readCheckpoint(checkpoint, frames); !read) return read;
        if (Result<> read = reader.readCheckpoint(frames); !read) return read;
        firstChunk = checkpoint.dataChunk;
    }
    // The next chunk decompresses while the reader reads the frames of the chunk.
    std::vector<std::uint8_t> nextFrames;
    Result<> decompressed = firstChunk == replay.getDataChunkCount() ? Result<>() : replay.readDataChunk(firstChunk, frames);
    for (std::size_t chunk = firstChunk; chunk < replay.getDataChunkCount(); ++chunk) {
        if (!decompressed) return decompressed;
        if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "The replay read was cancelled.");
        Result<> played;
        tbb::parallel_invoke([&] { played = reader.readFrames(frames); },
                             [&] { if (chunk + 1 < replay.getDataChunkCount()) decompressed = replay.readDataChunk(chunk + 1, nextFrames); });
        if (!played) return played;
        std::swap(frames, nextFrames);
    }
    reader.finish();
    branch = std::move(header.branch);
    levels = std::move(header.levels);
    for (const Event& event : getEvents())
        if (event.kind == EventKind::ContentBlock && event.isActorBlock && !event.path.empty()) classPaths.try_emplace(event.actor, event.path);
    return {};
}

std::string_view Match::getClassPath(const std::uint32_t actor) const {
    const auto found = classPaths.find(actor);
    return found == classPaths.end() ? std::string_view() : found->second;
}

}
