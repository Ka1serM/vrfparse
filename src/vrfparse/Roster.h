#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vrfparse/Match.h"
#include "vrfparse/Timeline.h"

namespace vrfparse {

struct CharacterChange {
    float time = 0;
    std::uint32_t character = 0;
};

struct TeamChange {
    float time = 0;
    std::optional<Side> side;
};

struct Player {
    std::uint32_t playerState = 0;
    std::string subject;
    std::vector<std::uint32_t> characters;
    //! The SpawnedCharacter and AssignedTeamState of the player state, oldest first: custom games change both after the start.
    std::vector<CharacterChange> spawnedCharacters;
    std::vector<TeamChange> teams;

    std::uint32_t findSpawnedCharacter(float time) const;
    std::optional<Side> findSide(float time) const;
};

class Roster {
public:
    explicit Roster(const Match& match);

    std::span<const Player> getPlayers() const { return players; }

private:
    std::vector<Player> players;
};

}
