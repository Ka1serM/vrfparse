#include "replay/component_data_stream.h"

#include <vector>

#include "net/bit_reader.h"

namespace replay {

namespace {

constexpr std::uint8_t movementMagic = 0x52;
constexpr int maxMoves = 16;
constexpr std::int32_t maxBytes = 8192;

// A quantized vector of the movement component: 7 bits of component size and scale flag, then the components, or
// floats or doubles without a size.
std::optional<Vector> readMoveVector(net::BitReader& reader, const double scale) {
    const auto info = static_cast<std::uint32_t>(reader.readBits(7));
    const int componentBits = static_cast<int>(info & 63);
    const bool scaled = (info >> 6) != 0;
    if (componentBits == 0) {
        if (!scaled) return Vector{reader.readFloat(), reader.readFloat(), reader.readFloat()};
        return Vector{static_cast<float>(reader.readDouble()), static_cast<float>(reader.readDouble()), static_cast<float>(reader.readDouble())};
    }
    if (componentBits > 62) return std::nullopt;
    const auto component = [&] {
        const std::uint64_t sign = std::uint64_t{1} << (componentBits - 1);
        const auto value = static_cast<double>(static_cast<std::int64_t>(reader.readBits(componentBits) ^ sign) - static_cast<std::int64_t>(sign));
        return static_cast<float>(scaled ? value / scale : value);
    };
    const float x = component();
    const float y = component();
    return Vector{x, y, component()};
}

// A variable-length integer: 7 bits per byte, the low bit says that another byte follows.
void skipVarInt(net::BitReader& reader) {
    for (int shift = 0; shift < 32 && (reader.readByte() & 1) && !reader.isError(); shift += 7) {}
}

// One move record. Empty when its fields do not decode or its error sentinel is set.
std::optional<CharacterMove> readMove(net::BitReader& reader) {
    // Bit 0: the move type; then the yaw multiplier, the movement state, and a byte that the game does not use.
    const bool moveType = (reader.readBits(25) & 1) != 0;
    // The rotation input: a fixed vector of three 16-bit components.
    reader.readBits(48);
    // The timestamp.
    skipVarInt(reader);
    const std::optional<Vector> location = readMoveVector(reader, 100);
    if (!location) return std::nullopt;
    if (reader.readBit()) reader.readByte();
    // A flag, then the pitch and the yaw as 16-bit angles.
    const std::uint64_t angles = reader.readBits(33);
    if (moveType) {
        reader.readBit();
        // The velocity.
        if (!readMoveVector(reader, 10)) return std::nullopt;
    } else if (reader.readBits(33) & 1) {
        // A reference to another character, which no replay has shown yet.
        return std::nullopt;
    }
    const bool errorSentinel = reader.readBit();
    if (reader.isError() || errorSentinel) return std::nullopt;
    return CharacterMove{.location = *location, .yaw = static_cast<float>(static_cast<double>((angles >> 17) & 0xFFFF) * (360.0 / 65536.0))};
}

}

std::optional<CharacterMove> readLatestMove(net::BitReader& payload) {
    const std::int32_t byteCount = payload.readInt32();
    if (byteCount <= 0 || byteCount > maxBytes || payload.getBitsLeft() < std::int64_t{byteCount} * 8 + 8) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byteCount));
    payload.readBits(bytes, std::int64_t{byteCount} * 8);
    const std::uint8_t unusedBits = payload.readByte();
    if (unusedBits > 7) return std::nullopt;
    net::BitReader reader{std::span(bytes), std::int64_t{byteCount} * 8 - unusedBits};
    if (reader.readByte() != movementMagic) return std::nullopt;
    // Each record has a 3-bit marker: 1 to 7, then 2 to 7 again, and 0 after the last one.
    std::optional<CharacterMove> latest;
    int expected = 1;
    for (int moves = 0;; ++moves) {
        const auto marker = static_cast<int>(reader.readBits(3));
        if (reader.isError()) return std::nullopt;
        if (marker == 0) return latest;
        if (moves == maxMoves || marker != expected) return std::nullopt;
        latest = readMove(reader);
        if (!latest) return std::nullopt;
        expected = (expected + 1) & 7;
        if (expected < 2) expected = 1;
    }
}

}
