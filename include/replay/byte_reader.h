#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "replay/error.h"

namespace replay {

// FArchive over bytes in memory, little endian. A read past the end throws "replay.parse".
class ByteReader {
public:
    // The context names what is read, for the error.
    ByteReader(std::span<const std::uint8_t> data, std::string_view context) : data(data), context(context) {}

    std::span<const std::uint8_t> readBytes(std::size_t count);
    void skip(std::uint64_t count);

    std::uint8_t readUInt8() { return readBytes(1)[0]; }
    std::uint16_t readUInt16() { return read<std::uint16_t>(); }
    std::uint32_t readUInt32() { return read<std::uint32_t>(); }
    std::int32_t readInt32() { return read<std::int32_t>(); }
    std::int64_t readInt64() { return read<std::int64_t>(); }
    std::uint64_t readUInt64() { return read<std::uint64_t>(); }
    float readFloat() { return read<float>(); }
    // FArchive::SerializeIntPacked: 7 bits per byte, the low bit says that another byte follows.
    std::uint32_t readIntPacked();
    // FString, as net::BitReader::readString reads it.
    std::string readString();
    // An int32 count and the bytes.
    std::span<const std::uint8_t> readByteArray();

    bool atEnd() const { return position == data.size(); }
    std::size_t getPosition() const { return position; }
    // Throws "replay.parse" with the message and the reader's context and position.
    [[noreturn]] void fail(std::string_view message) const;

private:
    template <class Value>
    Value read() {
        Value value;
        std::memcpy(&value, readBytes(sizeof(Value)).data(), sizeof(Value));
        return value;
    }

    std::span<const std::uint8_t> data;
    std::string_view context;
    std::size_t position = 0;
};

}
