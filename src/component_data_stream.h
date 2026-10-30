#pragma once

#include <optional>

#include "vrfparse/event.h"

namespace vrfparse {

// A move of a remote character: where it is and where it looks.
struct CharacterMove {
    Vector location;
    // Degrees.
    float yaw = 0;
};

// The latest move of a FComponentDataStream payload (a remote character update of
// ReplaysClientReceiveRemoteCharacterUpdatesSingleArrayNoAutonomous): the byte count, the bytes, and the count of unused
// bits in the last byte. The bytes hold Valorant's movement component (13.08 natives FUN_146d9dea0 and FUN_147428850),
// then the input and discrepancy components, which replays leave empty. Empty when the stream has no move or does not
// decode.
std::optional<CharacterMove> readLatestMove(BitReader& payload);

}
