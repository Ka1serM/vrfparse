#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vrfparse/match.h"

namespace vrfparse {

// The two teams of a round result: the WinningTeam names "Blue" and "Red".
enum class Side { Blue, Red };

// EAresTeamRole.
enum class TeamRole : std::uint8_t { None, Attacker, Defender, FreeForAll, Any };

// EAresRoundOutcome.
enum class RoundOutcome : std::uint8_t { Elimination, Defuse, Detonate, TimeExpired, Cheat, Surrendered };

enum class SpikeAction : std::uint8_t { Plant, Defuse };

// A round of the replay (Round): from the start of its buy phase to the next round's start, or to the end of the replay.
struct TimelineRound {
    // From 1.
    int number = 0;
    float start = 0;
    // InRound and RoundEnding. Empty when the replay has no such phase in the round.
    std::optional<float> combatStart;
    std::optional<float> end;
    float close = 0;
    // The round's result (FAresRoundResult). Empty: the replay names none yet.
    std::optional<Side> winner;
    TeamRole winnerRole = TeamRole::None;
    std::optional<RoundOutcome> outcome;
};

// A MulticastNotifyKilledEnemy, with the damage notification that killed the victim.
struct Kill {
    float time = 0;
    std::uint32_t killer = 0;
    std::uint32_t victim = 0;
    // The class of the weapon, such as "/Game/Equippables/Guns/Sidearms/Luger/LugerPistol.LugerPistol_C". Empty when the
    // killing damage names none or the replay has no killing damage for the kill.
    std::string_view weaponClass;
    bool headshot = false;
    bool wallbang = false;
    // Player states.
    std::vector<std::uint32_t> assisters;
};

// A BombPlantedRPC or BombDefusedRPC.
struct Spike {
    float time = 0;
    SpikeAction action = SpikeAction::Plant;
    // The planter or the defuser.
    std::uint32_t character = 0;
    // 0 for A, 1 for B, 2 for C. -1 for a defuse.
    int site = -1;
    float x = 0;
    float y = 0;
};

// Where a character is: world units and the yaw in degrees, from the move at the time.
struct Pose {
    float time = 0;
    float x = 0;
    float y = 0;
    float yaw = 0;
};

// The match facts of a replay in the form the replay view reads them: rounds, kills, spike events, and a track of each
// character's poses and life by time. They come from the replicated game state and the RPCs of the replay's events.
// Every query is a binary search on a track. Its texts are views into the match, which must outlive it.
class Timeline {
public:
    explicit Timeline(const Match& match);

    // The class of the game state actor, such as "/Game/GameModes/Bomb/BombGameState.BombGameState_C". Empty when the
    // replay replicates none.
    std::string_view getGameStateClass() const { return gameStateClass; }
    // The players that the replay names by their account id (a player state's Subject).
    std::size_t getPlayerCount() const { return playerCount; }
    std::span<const TimelineRound> getRounds() const { return rounds; }
    std::span<const Kill> getKills() const { return kills; }
    std::span<const Spike> getSpikes() const { return spikes; }
    // The rounds that each team won, over every round result of the replay.
    int getWins(Side side) const { return side == Side::Blue ? blueWins : redWins; }
    // What could not be read, as "<code>: <message>".
    std::span<const std::string> getProblems() const { return problems; }

    // The last pose at or before the time. Empty before the character's first move.
    std::optional<Pose> findPose(std::uint32_t character, float time) const;
    // The health and the shield at the time: the last value at or before it, 0 before the first.
    float getHealth(std::uint32_t character, float time) const;
    float getShield(std::uint32_t character, float time) const;

private:
    // A track: times in order and the values at them, in parallel arrays.
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
