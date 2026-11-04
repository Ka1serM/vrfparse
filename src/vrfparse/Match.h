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

#include "vrfparse/Error.h"
#include "vrfparse/Event.h"

namespace vrfparse {

struct Marker {
    std::string id;
    std::string group;
    std::string metadata;
    float time = 0;
    float endTime = 0;
};

struct Interval {
    float start = 0;
    float end = 0;
};

struct Round {
    int number = 0;
    Interval time;
};

class Match {
public:
    Match() = default;
    Match(const Match&) = delete;
    Match& operator=(const Match&) = delete;

    [[nodiscard]] std::expected<void, Error> read(const std::filesystem::path& file, Interval range, std::stop_token stop);

    Interval getSpan() const { return span; }

    std::string_view getMatchId() const { return matchId; }
    std::string_view getBranch() const { return branch; }
    std::uint32_t getChangelist() const { return changelist; }
    float getLength() const { return length; }
    std::span<const std::string> getLevels() const { return levels; }
    std::string_view getLevel() const { return levels.empty() ? std::string_view() : std::string_view(levels.front()); }
    std::span<const std::string> getProblems() const { return problems; }
    std::span<const Event> getEvents() const { return log.getEvents(); }
    std::span<const Marker> getMarkers() const { return markers; }
    std::span<const Round> getRounds() const { return rounds; }
    std::optional<Round> findRound(int number) const;

    std::string_view getClassPath(std::uint32_t actor) const;

private:
    void readRounds();

    std::string matchId;
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
