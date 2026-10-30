#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace net {

// FBitReader: reads a bit stream, least significant bit first. A read past the end sets the error and gives zeros, as
// FBitReader::SetOverflowed does; the caller checks isError at the boundary of what it reads.
class BitReader {
public:
    BitReader(std::span<const std::uint8_t> data, std::int64_t bits);
    BitReader(const BitReader&) = delete;
    BitReader& operator=(const BitReader&) = delete;
    BitReader(BitReader&&) = default;
    BitReader& operator=(BitReader&&) = default;

    bool readBit();
    // Up to 64 bits.
    std::uint64_t readBits(int count);
    // FBitReader::SerializeIntPacked: 7 bits per byte, the low bit says that another byte follows.
    std::uint32_t serializeIntPacked();
    // FBitReader::SerializeInt: a value below valueMax in as many bits as valueMax needs.
    std::uint32_t serializeInt(std::uint32_t valueMax);
    std::uint8_t readByte() { return static_cast<std::uint8_t>(readBits(8)); }
    std::uint16_t readUInt16() { return static_cast<std::uint16_t>(readBits(16)); }
    std::uint32_t readUInt32() { return static_cast<std::uint32_t>(readBits(32)); }
    std::int32_t readInt32() { return static_cast<std::int32_t>(readUInt32()); }
    std::uint64_t readUInt64() { return readBits(64); }
    float readFloat();
    double readDouble();
    // FString: an int32 length with the terminator, negative for UTF-16 characters.
    std::string readString();
    // Copies the next bits to the destination, from its first bit; the bits after them in its last byte stay.
    void readBits(std::span<std::uint8_t> destination, std::int64_t count);
    // The next bits as their own reader (FBitReader::ResetData), and moves past them.
    BitReader readSubReader(std::int64_t bits);

    std::int64_t getBitsLeft() const { return bits - position; }
    std::int64_t getPosition() const { return position; }
    // FBitReader::SetData at a position: reads again from the bit, without the error.
    void seek(const std::int64_t bit) {
        position = bit;
        error = false;
    }
    bool atEnd() const { return position >= bits; }
    bool isError() const { return error; }
    void setError() { error = true; }

private:
    explicit BitReader(std::vector<std::uint8_t> owned, std::int64_t bits);

    std::vector<std::uint8_t> owned;
    std::span<const std::uint8_t> data;
    std::int64_t bits = 0;
    std::int64_t position = 0;
    bool error = false;
};

}
