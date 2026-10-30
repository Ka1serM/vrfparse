#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "replay/event.h"
#include "replay/local_file_replay.h"

namespace replay {

// Replay time in seconds, from start to end.
struct Interval {
    float start = 0;
    float end = 0;
};

// One read replay: its header and its events, which the net driver plays.
class Match {
public:
    // Reads the file. Throws replay::Error, with the code "cancelled" when stop is requested.
    Match(const std::filesystem::path& file, std::stop_token stop);
    Match(const Match&) = delete;
    Match& operator=(const Match&) = delete;

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

    // The class of the actor, from the content blocks of the actor's channel. Empty when the replay names none.
    std::string_view getClassPath(std::uint32_t actor) const;

private:
    std::string branch;
    std::uint32_t changelist = 0;
    float length = 0;
    std::vector<std::string> levels;
    std::vector<std::string> problems;
    std::vector<Marker> markers;
    EventLog log;
    std::unordered_map<std::uint32_t, std::string_view> classPaths;
};

}
