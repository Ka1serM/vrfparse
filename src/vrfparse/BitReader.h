#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace vrfparse {

//! A view: the data must outlive the reader.
class BitReader {
public:
    BitReader(std::span<const std::uint8_t> data, std::int64_t bits);

    bool readBit();
    std::uint64_t readBits(int count);
    std::uint32_t serializeIntPacked();
    std::uint32_t serializeInt(std::uint32_t valueMax);
    std::uint8_t readByte() { return static_cast<std::uint8_t>(readBits(8)); }
    std::uint16_t readUInt16() { return static_cast<std::uint16_t>(readBits(16)); }
    std::uint32_t readUInt32() { return static_cast<std::uint32_t>(readBits(32)); }
    std::int32_t readInt32() { return static_cast<std::int32_t>(readUInt32()); }
    std::uint64_t readUInt64() { return readBits(64); }
    float readFloat();
    double readDouble();
    std::string readString();
    void readBits(std::span<std::uint8_t> destination, std::int64_t count);
    BitReader readSubReader(std::int64_t bits);

    std::int64_t getBitsLeft() const { return end - position; }
    std::int64_t getPosition() const { return position - start; }
    void seek(const std::int64_t bit) {
        position = start + bit;
        error = false;
    }
    bool atEnd() const { return position >= end; }
    bool isError() const { return error; }
    void setError() { error = true; }

private:
    BitReader(std::span<const std::uint8_t> data, std::int64_t start, std::int64_t end);

    bool reserve(std::int64_t count);
    std::uint64_t peek(std::int64_t bit) const;

    std::span<const std::uint8_t> data;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::int64_t position = 0;
    bool error = false;
};

}
