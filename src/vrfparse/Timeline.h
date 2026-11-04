#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vrfparse/Match.h"

namespace vrfparse {

//! The WinningTeam of a round result names "Blue" and "Red".
enum class Side { Blue, Red };

enum class TeamRole : std::uint8_t { None, Attacker, Defender, FreeForAll, Any };

enum class RoundOutcome : std::uint8_t { Elimination, Defuse, Detonate, TimeExpired, Cheat, Surrendered };

enum class SpikeAction : std::uint8_t { Plant, Defuse };

struct TimelineRound {
    int number = 0;
    float start = 0;
    std::optional<float> combatStart;
    std::optional<float> end;
    float close = 0;
    std::optional<Side> winner;
    TeamRole winnerRole = TeamRole::None;
    std::optional<RoundOutcome> outcome;
};

struct Kill {
    float time = 0;
    std::uint32_t killer = 0;
    std::uint32_t victim = 0;
    std::string_view weaponClass;
    bool headshot = false;
    bool wallbang = false;
    std::vector<std::uint32_t> assisters;
};

struct Spike {
    float time = 0;
    SpikeAction action = SpikeAction::Plant;
    std::uint32_t character = 0;
    //! 0 for A, 1 for B, 2 for C, and -1 for a defuse.
    int site = -1;
    float x = 0;
    float y = 0;
};

struct Pose {
    float time = 0;
    float x = 0;
    float y = 0;
    float yaw = 0;
};

//! Its texts are views into the match: the match must outlive it.
class Timeline {
public:
    explicit Timeline(const Match& match);

    std::string_view getGameStateClass() const { return gameStateClass; }
    std::size_t getPlayerCount() const { return playerCount; }
    std::span<const TimelineRound> getRounds() const { return rounds; }
    std::span<const Kill> getKills() const { return kills; }
    std::span<const Spike> getSpikes() const { return spikes; }
    int getWins(Side side) const { return side == Side::Blue ? blueWins : redWins; }
    std::span<const std::string> getProblems() const { return problems; }

    std::optional<Pose> findPose(std::uint32_t character, float time) const;
    float getHealth(std::uint32_t character, float time) const;
    float getShield(std::uint32_t character, float time) const;

private:
    struct Track {
        std::vector<float> moveTimes;
        std::vector<Pose> poses;
        std::vector<float> healthTimes;
        std::vector<float> health;
        std::vector<float> shieldTimes;
        std::vector<float> shield;
    };

    const Track* findTrack(std::uint32_t character) const;

    std::string_view gameStateClass;
    std::size_t playerCount = 0;
    std::vector<TimelineRound> rounds;
    std::vector<Kill> kills;
    std::vector<Spike> spikes;
    int blueWins = 0;
    int redWins = 0;
    std::vector<std::string> problems;
    std::unordered_map<std::uint32_t, Track> tracks;
};

}
