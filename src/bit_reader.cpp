#include "vrfparse/bit_reader.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace vrfparse {

namespace {

static_assert(std::endian::native == std::endian::little, "The reader loads the stream's words as little endian.");

// Longer strings do not occur in replays; a longer length is a misread.
constexpr std::int32_t maxStringLength = 1 << 16;

std::uint64_t lowBits(const int count) { return count >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << count) - 1; }

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

BitReader::BitReader(const std::span<const std::uint8_t> data, const std::int64_t bits)
    : BitReader(data, 0, std::min(bits, static_cast<std::int64_t>(data.size()) * 8)) {
    if (end < bits) error = true;
}

BitReader::BitReader(const std::span<const std::uint8_t> data, const std::int64_t start, const std::int64_t end)
    : data(data), start(start), end(end), position(start) {}

bool BitReader::reserve(const std::int64_t count) {
    if (count >= 0 && count <= end - position) return true;
    error = true;
    position = end;
    return false;
}

std::uint64_t BitReader::peek(const std::int64_t bit) const {
    const auto byte = static_cast<std::size_t>(bit >> 3);
    const int shift = static_cast<int>(bit & 7);
    std::uint64_t low = 0;
    std::memcpy(&low, data.data() + byte, std::min<std::size_t>(8, data.size() - byte));
    if (shift == 0) return low;
    const std::uint64_t high = byte + 8 < data.size() ? data[byte + 8] : 0;
    return (low >> shift) | (high << (64 - shift));
}

bool BitReader::readBit() {
    if (!reserve(1)) return false;
    const bool bit = (data[static_cast<std::size_t>(position >> 3)] >> (position & 7)) & 1;
    ++position;
    return bit;
}

std::uint64_t BitReader::readBits(const int count) {
    if (count <= 0 || !reserve(count)) return 0;
    const std::uint64_t value = peek(position) & lowBits(count);
    position += count;
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
        if (atEnd()) {
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
        text.resize(static_cast<std::size_t>(length));
        readBits(std::span(reinterpret_cast<std::uint8_t*>(text.data()), text.size()), std::int64_t{length} * 8);
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
    if (error) return {};
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
}

void BitReader::readBits(const std::span<std::uint8_t> destination, const std::int64_t count) {
    if ((count + 7) / 8 > static_cast<std::int64_t>(destination.size()) || !reserve(count)) {
        error = true;
        position = end;
        return;
    }
    const auto whole = static_cast<std::size_t>(count / 8);
    if ((position & 7) == 0) {
        std::memcpy(destination.data(), data.data() + (position >> 3), whole);
    } else {
        std::size_t byte = 0;
        for (; byte + 8 <= whole; byte += 8) {
            const std::uint64_t word = peek(position + static_cast<std::int64_t>(byte) * 8);
            std::memcpy(destination.data() + byte, &word, 8);
        }
        for (; byte < whole; ++byte) destination[byte] = static_cast<std::uint8_t>(peek(position + static_cast<std::int64_t>(byte) * 8));
    }
    position += static_cast<std::int64_t>(whole) * 8;
    if (const int rest = static_cast<int>(count & 7)) {
        const auto mask = static_cast<std::uint8_t>((1u << rest) - 1u);
        destination[whole] = static_cast<std::uint8_t>((destination[whole] & ~mask) | (peek(position) & mask));
        position += rest;
    }
}

BitReader BitReader::readSubReader(const std::int64_t count) {
    if (!reserve(count)) return BitReader(data, end, end);
    BitReader sub(data, position, position + count);
    position += count;
    return sub;
}

}
