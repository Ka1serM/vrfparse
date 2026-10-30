#include "replay/byte_reader.h"

#include <fmt/format.h>

#include "net/bit_reader.h"

namespace replay {

std::span<const std::uint8_t> ByteReader::readBytes(const std::size_t count) {
    if (count > data.size() - position) fail(fmt::format("{} bytes do not fit", count));
    const std::span<const std::uint8_t> bytes = data.subspan(position, count);
    position += count;
    return bytes;
}

std::uint32_t ByteReader::readIntPacked() {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        const std::uint8_t next = readUInt8();
        value |= static_cast<std::uint32_t>(next >> 1) << shift;
        if ((next & 1) == 0) return value;
    }
    fail("a packed integer does not end within five bytes");
}

std::string ByteReader::readString() {
    net::BitReader reader(data.subspan(position), static_cast<std::int64_t>(data.size() - position) * 8);
    std::string text = reader.readString();
    if (reader.isError()) fail("a string does not fit");
    position += static_cast<std::size_t>(reader.getPosition() / 8);
    return text;
}

void ByteReader::fail(const std::string_view message) const {
    throw Error("replay.parse", fmt::format("The {} is damaged at byte {}: {}.", context, position, message));
}

void ByteReader::skip(const std::uint64_t count) {
    if (count > data.size() - position) fail(fmt::format("{} bytes do not fit", count));
    position += static_cast<std::size_t>(count);
}

std::span<const std::uint8_t> ByteReader::readByteArray() {
    const std::int32_t count = readInt32();
    if (count < 0) fail(fmt::format("the byte count {} is negative", count));
    return readBytes(static_cast<std::size_t>(count));
}

}
