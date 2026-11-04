#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vrfparse/Match.h"

namespace vrfparse {

struct Player {
    std::uint32_t playerState = 0;
    std::string subject;
    std::vector<std::uint32_t> characters;
    std::uint32_t teamState = 0;
};

struct Team {
    std::uint32_t teamState = 0;
    std::string_view name;
};

//! The match must outlive the roster.
class Roster {
public:
    explicit Roster(const Match& match);

    std::span<const Player> getPlayers() const { return players; }
    std::span<const Team> getTeams() const { return teams; }

private:
    std::vector<Player> players;
    std::vector<Team> teams;
};

}
