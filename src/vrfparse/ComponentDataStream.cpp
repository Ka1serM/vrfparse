#include "vrfparse/ComponentDataStream.h"

#include "vrfparse/NetQuantize.h"
#include "vrfparse/BitReader.h"

namespace vrfparse {

namespace {

constexpr std::uint8_t movementMagic = 0x52;
constexpr int maxMoves = 16;
constexpr std::int32_t maxBytes = 8192;

void skipVarInt(BitReader& reader) {
    for (int shift = 0; shift < 32 && (reader.readByte() & 1) && !reader.isError(); shift += 7) {}
}

std::optional<CharacterMove> readMove(BitReader& reader) {
    //! The 25 bits end with a byte that the game does not use.
    const bool moveType = (reader.readBits(25) & 1) != 0;
    reader.readBits(48);
    skipVarInt(reader);
    const Vector location = readQuantizedVector(reader, 100);
    if (reader.readBit())
        reader.readByte();
    const std::uint64_t angles = reader.readBits(33);
    if (moveType) {
        reader.readBit();
        readQuantizedVector(reader, 10);
    } else if (reader.readBits(33) & 1) {
        //! A reference to another character, which no replay has shown yet.
        return std::nullopt;
    }
    const bool errorSentinel = reader.readBit();
    if (reader.isError() || errorSentinel)
        return std::nullopt;
    return CharacterMove{.location = location, .yaw = decompressAxisFromShort(static_cast<std::uint16_t>(angles >> 17))};
}

}

std::optional<CharacterMove> readLatestMove(BitReader& payload) {
    const std::int32_t byteCount = payload.readInt32();
    if (byteCount <= 0 || byteCount > maxBytes || payload.getBitsLeft() < std::int64_t{byteCount} * 8 + 8)
        return std::nullopt;
    BitReader bytes = payload.readSubReader(std::int64_t{byteCount} * 8);
    const std::uint8_t unusedBits = payload.readByte();
    if (unusedBits > 7)
        return std::nullopt;
    BitReader reader = bytes.readSubReader(std::int64_t{byteCount} * 8 - unusedBits);
    if (reader.readByte() != movementMagic)
        return std::nullopt;
    //! Each record has a 3-bit marker: 1 to 7, then 2 to 7 again, and 0 after the last one.
    std::optional<CharacterMove> latest;
    int expected = 1;
    for (int moves = 0;; ++moves) {
        const auto marker = static_cast<int>(reader.readBits(3));
        if (reader.isError())
            return std::nullopt;
        if (marker == 0)
            return latest;
        if (moves == maxMoves || marker != expected)
            return std::nullopt;
        latest = readMove(reader);
        if (!latest)
            return std::nullopt;
        expected = (expected + 1) & 7;
        if (expected < 2)
            expected = 1;
    }
}

}
