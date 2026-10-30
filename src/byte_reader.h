#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace vrfparse {

// FArchive over bytes in memory, little endian. As FArchive::SetError, a read that does not fit sets the error, which
// stays, and gives zeros; the caller checks isError at the boundary of what it reads.
class ByteReader {
public:
    explicit ByteReader(const std::span<const std::uint8_t> data) : data(data) {}

    std::span<const std::uint8_t> readBytes(std::size_t count);
    void skip(std::uint64_t count);

    std::uint8_t readUInt8() { return read<std::uint8_t>(); }
    std::uint16_t readUInt16() { return read<std::uint16_t>(); }
    std::uint32_t readUInt32() { return read<std::uint32_t>(); }
    std::int32_t readInt32() { return read<std::int32_t>(); }
    std::uint64_t readUInt64() { return read<std::uint64_t>(); }
    float readFloat() { return read<float>(); }
    // FArchive::SerializeIntPacked: 7 bits per byte, the low bit says that another byte follows.
    std::uint32_t readIntPacked();
    // FString, as BitReader::readString reads it.
    std::string readString();
    // An int32 count and the bytes.
    std::span<const std::uint8_t> readByteArray();

    bool atEnd() const { return position == data.size(); }
    std::size_t getPosition() const { return position; }
    bool isError() const { return reason != nullptr; }
    // What failed first, such as "a string does not fit". Null without the error.
    const char* getReason() const { return reason; }
    // Sets the error with its reason, a string literal; the first reason stays.
    void setError(const char* why) {
        if (!reason) reason = why;
    }

private:
    template <class Value>
    Value read() {
        Value value{};
        const std::span<const std::uint8_t> bytes = readBytes(sizeof(Value));
        if (!bytes.empty()) std::memcpy(&value, bytes.data(), sizeof(Value));
        return value;
    }

    std::span<const std::uint8_t> data;
    std::size_t position = 0;
    const char* reason = nullptr;
};

}
