#pragma once

#include <optional>

#include "vrfparse/Event.h"

namespace vrfparse {

struct CharacterMove {
    Vector location;
    float yaw = 0;
};

//! The bytes hold Valorant's movement component: the 13.08 natives FUN_146d9dea0 and FUN_147428850.
std::optional<CharacterMove> readLatestMove(BitReader& payload);

}
