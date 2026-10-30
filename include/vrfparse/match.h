#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vrfparse/error.h"
#include "vrfparse/event.h"

namespace vrfparse {

// FLocalFileEventInfo: an event that the recording game added with AddEvent, such as the group "roundStarted" with the
// zero-based round number as metadata. Times are replay seconds.
struct Marker {
    std::string id;
    std::string group;
    std::string metadata;
    float time = 0;
    float endTime = 0;
};

// Replay time in seconds, from start to end.
struct Interval {
    float start = 0;
    float end = 0;
};

// The time from a "roundStarted" replay event to the next one, or to the replay's end.
struct Round {
    // The event's zero-based round number + 1.
    int number = 0;
    Interval time;
};

// A read of a replay: its header, its rounds, and the events of a time range, which the net driver plays.
class Match {
public:
    Match() = default;
    Match(const Match&) = delete;
    Match& operator=(const Match&) = delete;

    // UDemoNetDriver::GotoTime's read into the empty match: the last checkpoint at or before the range's start, which
    // opens each actor with its state at the checkpoint's time, then the frames after it through the data chunk that holds
    // the range's end. Without such a checkpoint, the events start at the first frame. A range that ends after the replay
    // reads to its end: {0, infinity} reads the whole replay, {infinity, infinity} the last checkpoint on. The header,
    // the markers and the rounds are the whole replay's. Fails with ErrorCode::Cancelled when stop is requested.
    [[nodiscard]] std::expected<void, Error> read(const std::filesystem::path& file, Interval range, std::stop_token stop);

    // The replay time that the events cover: from the checkpoint, or the first frame, to the end of the last data chunk.
    Interval getSpan() const { return span; }

    std::string_view getBranch() const { return branch; }
    std::uint32_t getChangelist() const { return changelist; }
    float getLength() const { return length; }
    // The levels of the header: the map, such as "/Game/Maps/Ascent/Ascent".
    std::span<const std::string> getLevels() const { return levels; }
    // The first level: the map. Empty when the header names none.
    std::string_view getLevel() const { return levels.empty() ? std::string_view() : std::string_view(levels.front()); }
    // What the reader skipped, as "<code>: <message>".
    std::span<const std::string> getProblems() const { return problems; }
    std::span<const Event> getEvents() const { return log.getEvents(); }
    // The event chunks of the file, in file order.
    std::span<const Marker> getMarkers() const { return markers; }
    // UReplayBlueprintLibrary::GetStartTimeForRound: in time order.
    std::span<const Round> getRounds() const { return rounds; }
    // Empty when the replay has no round of the number.
    std::optional<Round> findRound(int number) const;

    // The class of the actor, from the content blocks of the actor's channel. Empty when the replay names none.
    std::string_view getClassPath(std::uint32_t actor) const;

private:
    void readRounds();

    std::string branch;
    std::uint32_t changelist = 0;
    float length = 0;
    Interval span;
    std::vector<std::string> levels;
    std::vector<std::string> problems;
    std::vector<Marker> markers;
    std::vector<Round> rounds;
    EventLog log;
    std::unordered_map<std::uint32_t, std::string_view> classPaths;
};

}
