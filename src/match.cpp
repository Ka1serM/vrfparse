#include "vrfparse/match.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <limits>

#include <tbb/parallel_invoke.h>

#include "demo_reader.h"

namespace vrfparse {

namespace {

// MSVC _wtoi, which the game parses the round number with: leading white space, an optional sign, then the digits up
// to the first other character. Text without digits is 0; a number outside int is INT_MAX or INT_MIN.
int parseInt(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || (text.front() >= '\t' && text.front() <= '\r')))
        text.remove_prefix(1);
    const bool negative = !text.empty() && text.front() == '-';
    if (!text.empty() && (text.front() == '-' || text.front() == '+'))
        text.remove_prefix(1);
    long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            break;
        value = std::min(value * 10 + (c - '0'), static_cast<long long>(std::numeric_limits<int>::max()) + 1);
    }
    if (negative)
        return static_cast<int>(std::max(-value, static_cast<long long>(std::numeric_limits<int>::min())));
    return static_cast<int>(std::min(value, static_cast<long long>(std::numeric_limits<int>::max())));
}

char toLower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

}

std::expected<void, Error> Match::read(const std::filesystem::path& file, const Interval range, const std::stop_token stop) {
    LocalFileReplay replay;
    if (std::expected<void, Error> opened = replay.open(file); !opened)
        return opened;
    NetworkDemoHeader header;
    if (std::expected<void, Error> read = header.read(replay.getHeader()); !read)
        return read;
    const PayloadTransform transform(header.branch);
    if (!transform.isKnown())
        return std::unexpected(Error{ErrorCode::Version, std::format("The game release {} is not supported: its payload transform is not known.", header.branch)});
    length = static_cast<float>(replay.getLengthMs()) / 1000.0f;
    changelist = header.changelist;
    markers = std::move(replay.getMarkers());
    readRounds();
    DemoReader reader(header, transform, log, problems);
    std::vector<std::uint8_t> frames;
    const std::span<const LocalFileReplay::DataChunk> chunks = replay.getDataChunks();
    std::size_t firstChunk = 0;
    span.start = 0;
    const std::span<const LocalFileReplay::Checkpoint> checkpoints = replay.getCheckpoints();
    const auto after = std::ranges::upper_bound(checkpoints, range.start, {}, &LocalFileReplay::Checkpoint::time);
    if (after != checkpoints.begin()) {
        const LocalFileReplay::Checkpoint& checkpoint = *std::prev(after);
        if (checkpoint.dataChunk > chunks.size())
            return std::unexpected(Error{ErrorCode::Parse, std::format("A checkpoint names data chunk {}; the replay has {}.", checkpoint.dataChunk, chunks.size())});
        if (std::expected<void, Error> read = replay.readCheckpoint(checkpoint, frames); !read)
            return read;
        if (std::expected<void, Error> read = reader.readCheckpoint(frames); !read)
            return read;
        firstChunk = checkpoint.dataChunk;
        span.start = checkpoint.time;
    }
    // The chunks up to the one that holds the range's end: the first chunk that starts after it is not read.
    const std::span<const LocalFileReplay::DataChunk> following = chunks.subspan(firstChunk);
    const std::size_t lastChunk =
        firstChunk + static_cast<std::size_t>(std::distance(following.begin(), std::ranges::upper_bound(following, range.end, {}, &LocalFileReplay::DataChunk::startTime)));
    span.end = lastChunk == chunks.size() ? length : chunks[lastChunk - 1].endTime;
    // The next chunk decompresses while the reader reads the frames of the chunk.
    std::vector<std::uint8_t> nextFrames;
    std::expected<void, Error> decompressed = firstChunk == lastChunk ? std::expected<void, Error>() : replay.readDataChunk(firstChunk, frames);
    for (std::size_t chunk = firstChunk; chunk < lastChunk; ++chunk) {
        if (!decompressed)
            return decompressed;
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::Cancelled, "The replay read was cancelled."});
        std::expected<void, Error> played;
        tbb::parallel_invoke([&] { played = reader.readFrames(frames); },
                             [&] { if (chunk + 1 < lastChunk) decompressed = replay.readDataChunk(chunk + 1, nextFrames); });
        if (!played)
            return played;
        std::swap(frames, nextFrames);
    }
    reader.finish();
    branch = std::move(header.branch);
    levels = std::move(header.levels);
    for (const Event& event : getEvents())
        if (event.kind == EventKind::ContentBlock && event.isActorBlock && !event.path.empty())
            classPaths.try_emplace(event.actor, event.path);
    return {};
}

std::optional<Round> Match::findRound(const int number) const {
    const auto found = std::ranges::find(rounds, number, &Round::number);
    if (found == rounds.end())
        return std::nullopt;
    return *found;
}

// UReplayBlueprintLibrary::GetStartTimeForRound and GetRoundIndexFromTime (13.08 exe 0x14748a100, 0x147467480): a round
// starts at the Time1 of an event whose group equals "roundStarted" without regard to case; the event's metadata is the
// zero-based round number.
void Match::readRounds() {
    constexpr std::string_view roundStarted = "roundStarted";
    for (const Marker& marker : markers) {
        if (!std::ranges::equal(marker.group, roundStarted, {}, toLower, toLower))
            continue;
        const int roundNumber = parseInt(marker.metadata);
        if (!rounds.empty())
            rounds.back().time.end = marker.time;
        rounds.push_back({.number = roundNumber + 1, .time = {marker.time, std::max(marker.time, length)}});
    }
}

std::string_view Match::getClassPath(const std::uint32_t actor) const {
    const auto found = classPaths.find(actor);
    return found == classPaths.end() ? std::string_view() : found->second;
}

}
