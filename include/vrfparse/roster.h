#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vrfparse/match.h"

namespace vrfparse {

struct Player {
    std::uint32_t playerState = 0;
    // The account id. Empty when the replay does not name it.
    std::string subject;
    // The character actors whose PlayerState is this player, by the start of their first lifetime.
    std::vector<std::uint32_t> characters;
    // The NetGUID of the player state's AssignedTeamState. 0 when the replay assigns none.
    std::uint32_t teamState = 0;
};

// A team state actor (ABaseTeamState).
struct Team {
    std::uint32_t teamState = 0;
    // The path of its TeamComponent's NetGUID export, such as a team's name. Empty when the replay names none.
    std::string_view name;
};

// The players and teams of a match. Keeps a reference to the match.
class Roster {
public:
    explicit Roster(const Match& match);

    // By the NetGUID of the player state.
    std::span<const Player> getPlayers() const { return players; }
    // By the NetGUID of the team state.
    std::span<const Team> getTeams() const { return teams; }

private:
    std::vector<Player> players;
    std::vector<Team> teams;
};

}
