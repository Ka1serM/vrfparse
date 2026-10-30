#include "byte_reader.h"

#include "vrfparse/bit_reader.h"

namespace vrfparse {

std::span<const std::uint8_t> ByteReader::readBytes(const std::size_t count) {
    if (isError() || count > data.size() - position) {
        setError("the data ends");
        return {};
    }
    const std::span<const std::uint8_t> bytes = data.subspan(position, count);
    position += count;
    return bytes;
}

void ByteReader::skip(const std::uint64_t count) {
    if (isError() || count > data.size() - position)
        return setError("the data ends");
    position += static_cast<std::size_t>(count);
}

std::uint32_t ByteReader::readIntPacked() {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        const std::uint8_t next = readUInt8();
        value |= static_cast<std::uint32_t>(next >> 1) << shift;
        if ((next & 1) == 0)
            return value;
    }
    setError("a packed integer does not end within five bytes");
    return value;
}

std::string ByteReader::readString() {
    if (isError())
        return {};
    BitReader reader(data.subspan(position), static_cast<std::int64_t>(data.size() - position) * 8);
    std::string text = reader.readString();
    if (reader.isError()) {
        setError("a string does not fit");
        return {};
    }
    position += static_cast<std::size_t>(reader.getPosition() / 8);
    return text;
}

std::span<const std::uint8_t> ByteReader::readByteArray() {
    const std::int32_t count = readInt32();
    if (count < 0) {
        setError("a byte count is negative");
        return {};
    }
    return readBytes(static_cast<std::size_t>(count));
}

}
