#include "net/bit_reader.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace net {

namespace {

// Longer strings do not occur in replays; a longer length is a misread.
constexpr std::int32_t maxStringLength = 1 << 16;

void appendUtf8(std::string& text, const char32_t code) {
    if (code < 0x80) {
        text.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (code >> 6)));
        text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        text.push_back(static_cast<char>(0xE0 | (code >> 12)));
        text.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xF0 | (code >> 18)));
        text.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

}

BitReader::BitReader(const std::span<const std::uint8_t> data, const std::int64_t bits) : data(data), bits(bits) {
    if (static_cast<std::int64_t>(data.size()) * 8 < bits) error = true;
}

BitReader::BitReader(std::vector<std::uint8_t> owned, const std::int64_t bits) : owned(std::move(owned)), data(this->owned), bits(bits) {}

bool BitReader::readBit() {
    if (position >= bits) {
        error = true;
        return false;
    }
    const bool bit = (data[static_cast<std::size_t>(position >> 3)] >> (position & 7)) & 1;
    ++position;
    return bit;
}

std::uint64_t BitReader::readBits(const int count) {
    if (count <= 0) return 0;
    if (position + count > bits) {
        error = true;
        position = bits;
        return 0;
    }
    std::uint64_t value = 0;
    for (int bit = 0; bit < count;) {
        const std::size_t byte = static_cast<std::size_t>(position >> 3);
        const int offset = static_cast<int>(position & 7);
        const int take = std::min(8 - offset, count - bit);
        const std::uint64_t chunk = (data[byte] >> offset) & ((1u << take) - 1u);
        value |= chunk << bit;
        bit += take;
        position += take;
    }
    return value;
}

std::uint32_t BitReader::serializeIntPacked() {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        const std::uint8_t next = readByte();
        value |= static_cast<std::uint32_t>(next >> 1) << shift;
        if ((next & 1) == 0) return value;
    }
    error = true;
    return value;
}

std::uint32_t BitReader::serializeInt(const std::uint32_t valueMax) {
    std::uint32_t value = 0;
    for (std::uint32_t mask = 1; value + mask < valueMax && mask; mask *= 2) {
        if (position >= bits) {
            error = true;
            break;
        }
        if (readBit()) value |= mask;
    }
    return value;
}

float BitReader::readFloat() { return std::bit_cast<float>(readUInt32()); }

double BitReader::readDouble() { return std::bit_cast<double>(readUInt64()); }

std::string BitReader::readString() {
    const std::int32_t length = readInt32();
    if (length == 0 || error) return {};
    if (length > maxStringLength || length < -maxStringLength) {
        error = true;
        return {};
    }
    std::string text;
    if (length > 0) {
        text.reserve(static_cast<std::size_t>(length));
        for (std::int32_t i = 0; i < length; ++i) text.push_back(static_cast<char>(readByte()));
    } else {
        for (std::int32_t i = 0; i < -length; ++i) {
            char32_t code = readUInt16();
            if (code >= 0xD800 && code < 0xDC00 && i + 1 < -length) {
                const char32_t low = readUInt16();
                ++i;
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
            appendUtf8(text, code);
        }
    }
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
}

void BitReader::readBits(const std::span<std::uint8_t> destination, const std::int64_t count) {
    if (count < 0 || position + count > bits || (count + 7) / 8 > static_cast<std::int64_t>(destination.size())) {
        error = true;
        position = bits;
        return;
    }
    if ((position & 7) == 0) {
        const auto whole = static_cast<std::size_t>(count / 8);
        std::memcpy(destination.data(), data.data() + (position >> 3), whole);
        position += static_cast<std::int64_t>(whole) * 8;
        if (const int rest = static_cast<int>(count & 7)) {
            const auto mask = static_cast<std::uint8_t>((1u << rest) - 1u);
            destination[whole] = static_cast<std::uint8_t>((destination[whole] & ~mask) | static_cast<std::uint8_t>(readBits(rest)));
        }
        return;
    }
    for (std::int64_t bit = 0; bit < count;) {
        const int take = static_cast<int>(std::min<std::int64_t>(8, count - bit));
        const auto index = static_cast<std::size_t>(bit >> 3);
        const auto mask = static_cast<std::uint8_t>((1u << take) - 1u);
        destination[index] = static_cast<std::uint8_t>((destination[index] & ~mask) | static_cast<std::uint8_t>(readBits(take)));
        bit += take;
    }
}

BitReader BitReader::readSubReader(const std::int64_t count) {
    if (count < 0 || position + count > bits) {
        error = true;
        position = bits;
        return BitReader(std::vector<std::uint8_t>(), 0);
    }
    std::vector<std::uint8_t> copied(static_cast<std::size_t>((count + 7) / 8));
    readBits(copied, count);
    return BitReader(std::move(copied), count);
}

}
