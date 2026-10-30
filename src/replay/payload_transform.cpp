#include "replay/payload_transform.h"

#include <bit>
#include <cstring>
#include <iterator>

namespace replay {

namespace {

// Riot's obfuscation of the content block payloads, one per game release. Each was recovered from that release's
// executable (the 13.08 one is FUN_144539860) by the authors of ValorantReplayParser; the releases below are its
// ValorantSeededTransform classes, translated by a script. A transform turns the payload in place: 64-bit words, then
// 32-bit words, then bytes, each with the state of a xorshift generator, and the last bits with a mask.

constexpr std::uint64_t multiplier = 0x2545f4914f6cdd1dULL;

std::uint64_t rotateLeft(const std::uint64_t value, const int count) { return std::rotl(value, count); }
std::uint32_t rotateLeft(const std::uint32_t value, const int count) { return std::rotl(value, count); }
std::uint8_t rotateLeft(const std::uint8_t value, const int count) { return static_cast<std::uint8_t>((value << count) | (value >> (8 - count))); }
std::uint64_t rotateRight(const std::uint64_t value, const int count) { return std::rotr(value, count); }
std::uint32_t rotateRight(const std::uint32_t value, const int count) { return std::rotr(value, count); }
std::uint8_t rotateRight(const std::uint8_t value, const int count) { return static_cast<std::uint8_t>((value >> count) | (value << (8 - count))); }

std::uint64_t swapAdjacentBits(const std::uint64_t value) { return ((value & 0x5555555555555555ULL) << 1) | ((value >> 1) & 0x5555555555555555ULL); }
std::uint32_t swapAdjacentBits(const std::uint32_t value) { return ((value & 0x55555555u) << 1) | ((value >> 1) & 0x55555555u); }
std::uint8_t swapAdjacentBits(const std::uint8_t value) { return static_cast<std::uint8_t>(((value & 0x55) << 1) | ((value >> 1) & 0x55)); }

// Reverses the bits within each 32-bit half and swaps the halves; the halves' own 16-bit halves stay in place.
std::uint64_t reverseBits64WithoutFinal16BitSwap(std::uint64_t value) {
    value = ((value & 0x5555555555555555ULL) << 1) | ((value >> 1) & 0x5555555555555555ULL);
    value = ((value & 0x3333333333333333ULL) << 2) | ((value >> 2) & 0x3333333333333333ULL);
    value = ((value & 0x0F0F0F0F0F0F0F0FULL) << 4) | ((value >> 4) & 0x0F0F0F0F0F0F0F0FULL);
    value = ((value & 0x00FF00FF00FF00FFULL) << 8) | ((value >> 8) & 0x00FF00FF00FF00FFULL);
    return (value << 32) | (value >> 32);
}

std::uint32_t reverseBits32(std::uint32_t value) {
    value = ((value & 0x55555555u) << 1) | ((value >> 1) & 0x55555555u);
    value = ((value & 0x33333333u) << 2) | ((value >> 2) & 0x33333333u);
    value = ((value & 0x0F0F0F0Fu) << 4) | ((value >> 4) & 0x0F0F0F0Fu);
    value = ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
    return (value << 16) | (value >> 16);
}

std::uint8_t reverseBits8(std::uint8_t value) {
    value = static_cast<std::uint8_t>(((value & 0x55) << 1) | ((value >> 1) & 0x55));
    value = static_cast<std::uint8_t>(((value & 0x33) << 2) | ((value >> 2) & 0x33));
    return static_cast<std::uint8_t>(((value & 0x0F) << 4) | ((value >> 4) & 0x0F));
}

std::uint64_t substituteBytes(const std::uint64_t value, const std::uint8_t (&table)[256]) {
    std::uint64_t result = 0;
    for (int shift = 0; shift < 64; shift += 8) result |= static_cast<std::uint64_t>(table[(value >> shift) & 0xFF]) << shift;
    return result;
}

std::uint32_t substituteBytes(const std::uint32_t value, const std::uint8_t (&table)[256]) {
    std::uint32_t result = 0;
    for (int shift = 0; shift < 32; shift += 8) result |= static_cast<std::uint32_t>(table[(value >> shift) & 0xFF]) << shift;
    return result;
}

// Little endian, as the game reads the words.
std::uint64_t readUInt64(const std::uint8_t* data, const int offset) {
    std::uint64_t value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

std::uint32_t readUInt32(const std::uint8_t* data, const int offset) {
    std::uint32_t value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

void writeUInt64(std::uint8_t* data, const int offset, const std::uint64_t value) { std::memcpy(data + offset, &value, sizeof(value)); }
void writeUInt32(std::uint8_t* data, const int offset, const std::uint32_t value) { std::memcpy(data + offset, &value, sizeof(value)); }

// One step of the generator (xoroshiro128+): the next state is the high half of the sum.
void advanceTransformState(std::uint32_t& state, std::uint64_t& prngA, std::uint64_t& prngB, std::uint8_t& streamByte) {
    const std::uint64_t sum = prngB + prngA;
    prngB ^= prngA;
    prngA = rotateRight(prngA, 9) ^ (prngB << 14) ^ prngB;
    prngB = rotateLeft(prngB, 36);
    state = static_cast<std::uint32_t>(sum >> 32);
    streamByte = static_cast<std::uint8_t>(state);
}

std::uint64_t initialPrngA(const std::uint32_t seed, const std::uint32_t seedAddend, const int seedOffset) {
    const std::uint32_t seedPlus = seed + seedAddend;
    const std::uint32_t mixed = (((seedPlus >> 15) ^ seedPlus) >> 12) ^ ((seed + static_cast<std::uint32_t>(seedOffset)) << 25) ^ seedPlus;
    return mixed * multiplier;
}

std::uint64_t initialPrngB(const std::uint32_t seed) {
    const std::uint32_t mixed = (((seed >> 15) ^ seed) >> 12) ^ (seed << 25) ^ seed;
    return mixed * multiplier;
}

constexpr std::uint8_t substituteTable64[256] = {
    0x77, 0xb9, 0x04, 0x2f, 0xeb, 0x7d, 0x27, 0xc9, 0x44, 0x73, 0x9a, 0x3f, 0x36, 0xf5, 0x65, 0xdd,
    0xf7, 0xe0, 0x30, 0x2d, 0xa9, 0x98, 0x5d, 0xde, 0x69, 0xa3, 0x94, 0xa0, 0x5e, 0x17, 0x06, 0x78,
    0xa4, 0xf6, 0xab, 0x03, 0x43, 0xc8, 0x28, 0xe5, 0x6a, 0x8e, 0x1c, 0xf2, 0x70, 0xcf, 0x53, 0x05,
    0xd3, 0x0d, 0xff, 0xa7, 0xa2, 0x3a, 0x32, 0x25, 0x5a, 0x1f, 0x48, 0xc1, 0xb7, 0xe1, 0x6e, 0x85,
    0x99, 0x60, 0x47, 0xbb, 0xe4, 0x8a, 0xcb, 0xc0, 0x1b, 0xea, 0x61, 0x64, 0xf0, 0xc2, 0xd8, 0x8b,
    0xcd, 0xfd, 0xad, 0xb8, 0x19, 0xb5, 0xbf, 0x0e, 0x91, 0x81, 0x83, 0x9d, 0x45, 0xd2, 0x49, 0xe9,
    0xc7, 0x31, 0xbd, 0x20, 0xbe, 0xc6, 0x66, 0x80, 0xd1, 0x79, 0xd7, 0xe6, 0xfc, 0xa1, 0x5b, 0x5f,
    0xdf, 0xf1, 0xd0, 0x50, 0x67, 0x52, 0xfe, 0x7b, 0x35, 0x13, 0xf8, 0x46, 0xb3, 0x75, 0x8d, 0xe3,
    0x3e, 0x2e, 0xf4, 0xdc, 0x34, 0x2a, 0x08, 0x23, 0xe2, 0x0c, 0x09, 0x4b, 0xee, 0xc3, 0x0f, 0x24,
    0x8f, 0x54, 0x4c, 0x55, 0x39, 0xcc, 0x1d, 0x1e, 0x3b, 0x22, 0x72, 0xda, 0x29, 0x6b, 0x41, 0xaa,
    0xa6, 0x12, 0x2c, 0x93, 0xca, 0x9c, 0x97, 0x0a, 0x56, 0xa8, 0x7a, 0x9e, 0xb4, 0x62, 0x92, 0x3d,
    0x9f, 0x38, 0xf3, 0x40, 0x84, 0x37, 0xb2, 0xd4, 0xaf, 0x76, 0x33, 0xfa, 0x21, 0xef, 0xfb, 0x71,
    0x6f, 0x90, 0x82, 0x51, 0x1a, 0xc5, 0x74, 0xf9, 0x59, 0x07, 0xba, 0x11, 0xb1, 0xac, 0xd6, 0xed,
    0xe7, 0x02, 0xae, 0x96, 0x10, 0x16, 0x7c, 0x4f, 0x88, 0x14, 0x26, 0xbc, 0x15, 0x01, 0x68, 0x4a,
    0x2b, 0x0b, 0x7f, 0xa5, 0x4e, 0xe8, 0x6d, 0xec, 0x4d, 0xb0, 0x5c, 0xc4, 0x00, 0x95, 0x58, 0xb6,
    0xd5, 0x7e, 0x42, 0xdb, 0x57, 0x18, 0x86, 0x6c, 0xce, 0xd9, 0x9b, 0x89, 0x87, 0x3c, 0x8c, 0x63,
};

constexpr std::uint8_t substituteTable32[256] = {
    0x21, 0x67, 0xb3, 0x96, 0x31, 0x3f, 0xba, 0xd3, 0xd5, 0x06, 0x2b, 0x16, 0xf1, 0xb6, 0x51, 0xa7,
    0x9c, 0x7b, 0x41, 0x95, 0x84, 0x25, 0x15, 0x36, 0xa4, 0x70, 0x35, 0x46, 0xb0, 0x5f, 0xa6, 0xc3,
    0xbb, 0x86, 0x38, 0xf6, 0x2e, 0xa2, 0xa9, 0x94, 0x83, 0x1b, 0x62, 0x39, 0xf3, 0xd2, 0x28, 0x14,
    0x9e, 0x9a, 0xf2, 0xc9, 0xde, 0xcc, 0x26, 0xa1, 0xd8, 0xd0, 0x74, 0x8d, 0x69, 0x12, 0x71, 0x89,
    0xf7, 0x58, 0xcd, 0x4d, 0xb7, 0x11, 0x48, 0x09, 0xb9, 0x68, 0xc7, 0x7c, 0xf4, 0x20, 0x42, 0xf5,
    0x6b, 0x54, 0x75, 0x6d, 0xa8, 0x1d, 0x6a, 0x07, 0xd7, 0xc5, 0x0e, 0xa0, 0x66, 0xdb, 0xf8, 0x99,
    0xad, 0x10, 0x04, 0xff, 0x8f, 0xb1, 0xef, 0x98, 0x6c, 0x29, 0xe2, 0x01, 0x18, 0x3d, 0x37, 0x1e,
    0x65, 0x4b, 0x4a, 0x6e, 0x24, 0xd9, 0xbd, 0x90, 0xfe, 0x13, 0x56, 0x93, 0x34, 0xaa, 0x8b, 0x0d,
    0x79, 0xe7, 0x49, 0x92, 0xf9, 0x8e, 0xca, 0x43, 0xcb, 0xc6, 0xda, 0x02, 0x2d, 0x8c, 0x0f, 0xb2,
    0xc0, 0x8a, 0x47, 0x85, 0xae, 0xe0, 0xd4, 0x77, 0xc4, 0x0b, 0x5c, 0x61, 0x7e, 0x33, 0x57, 0x45,
    0xe6, 0x2f, 0xfd, 0x6f, 0x91, 0x5b, 0x9f, 0xcf, 0x3c, 0x4f, 0xe3, 0x3a, 0xed, 0xe4, 0x80, 0x08,
    0x73, 0x72, 0xea, 0x63, 0xfb, 0xfc, 0xb8, 0x7a, 0x23, 0xa5, 0x1f, 0x81, 0x59, 0x52, 0x87, 0x5d,
    0xfa, 0x78, 0xc1, 0xb5, 0xbe, 0xb4, 0xa3, 0x64, 0x1c, 0x32, 0x53, 0xf0, 0x7f, 0xdc, 0x3b, 0x76,
    0x40, 0xec, 0x30, 0x97, 0x55, 0x4c, 0x00, 0xbc, 0x88, 0x0c, 0x05, 0xe1, 0xdf, 0x19, 0x7d, 0x22,
    0xc2, 0x5a, 0x9b, 0xe5, 0x2a, 0x50, 0xbf, 0x1a, 0xc8, 0x03, 0x5e, 0x2c, 0xd1, 0xab, 0xdd, 0x44,
    0xee, 0x82, 0xce, 0x27, 0xaf, 0xeb, 0xd6, 0x4e, 0x0a, 0xe9, 0x17, 0x3e, 0x9d, 0xe8, 0xac, 0x60,
};

constexpr std::uint8_t substituteTable8[256] = {
    0x0a, 0x6c, 0x69, 0x96, 0xca, 0xdc, 0x5a, 0x08, 0xb3, 0x83, 0x39, 0xa0, 0xf9, 0xad, 0xf4, 0x56,
    0x0e, 0x6e, 0x4c, 0x85, 0x64, 0x99, 0x82, 0xd4, 0x88, 0x5c, 0x87, 0x36, 0x23, 0x9a, 0x11, 0x2d,
    0xb8, 0xc4, 0x34, 0x18, 0x66, 0x13, 0x6f, 0x59, 0xe0, 0x74, 0x22, 0xfa, 0xa6, 0x65, 0xe2, 0xd7,
    0x95, 0x4e, 0x94, 0xb0, 0x77, 0x9e, 0x1a, 0xee, 0xe7, 0x05, 0xa2, 0xc8, 0x30, 0x90, 0x0d, 0x9b,
    0xd2, 0x19, 0xc9, 0x3a, 0x47, 0x15, 0x12, 0xa9, 0x29, 0x1f, 0x53, 0xac, 0xaf, 0x43, 0x52, 0xae,
    0xf5, 0x4d, 0xbf, 0xbe, 0xe3, 0x4a, 0x06, 0xd5, 0xd0, 0xa3, 0x78, 0xa7, 0xd6, 0x1c, 0x7a, 0x6b,
    0x81, 0xd8, 0xde, 0xe5, 0x68, 0xfb, 0x26, 0x7e, 0xbc, 0xba, 0xe8, 0xcc, 0xe4, 0x72, 0x7f, 0x2c,
    0xfc, 0xf0, 0xec, 0x28, 0x71, 0x60, 0x48, 0xef, 0x3e, 0x03, 0x8f, 0x1e, 0xf1, 0x6a, 0x8d, 0xf2,
    0x46, 0x1b, 0x9c, 0x86, 0xf7, 0xb4, 0x76, 0x62, 0x8a, 0x10, 0xfd, 0x6d, 0x0b, 0x3f, 0x9f, 0x2f,
    0x55, 0x5f, 0xc3, 0xc6, 0x92, 0x16, 0x27, 0xd3, 0x44, 0x84, 0x0f, 0xe1, 0x80, 0x8c, 0xb7, 0x73,
    0x89, 0x45, 0xdb, 0x33, 0x25, 0x50, 0xea, 0x04, 0x14, 0xc5, 0x0c, 0x32, 0x41, 0x5e, 0x79, 0xa4,
    0x1d, 0x3d, 0x5b, 0x40, 0x37, 0xc1, 0xcf, 0xfe, 0x2b, 0x54, 0xeb, 0x9d, 0x49, 0x91, 0xf3, 0x07,
    0x17, 0x3c, 0xda, 0x57, 0x8b, 0xcd, 0x61, 0xf6, 0xce, 0x70, 0x2e, 0xff, 0x21, 0x93, 0x97, 0x2a,
    0x7d, 0x67, 0xab, 0xb5, 0x7c, 0x5d, 0x00, 0x42, 0xa5, 0xd9, 0x20, 0x51, 0xed, 0xdd, 0x02, 0x09,
    0xc2, 0xd1, 0xf8, 0xbd, 0xbb, 0xe9, 0x35, 0x24, 0x98, 0x58, 0x38, 0xaa, 0xb9, 0xa8, 0xb2, 0x75,
    0x01, 0xcb, 0xc0, 0x63, 0xdf, 0x3b, 0x8e, 0xc7, 0x31, 0xb1, 0xa1, 0xb6, 0xe6, 0x7b, 0x4b, 0x4f,
};

struct Release {
    std::string_view branch;
    void (*transform)(std::uint8_t* output, int bitCount, std::uint32_t seed);
};

// ++Ares-Core+release-11.06: ValorantSeededTransform11_06.cs.
struct Release11_06 {
    static constexpr std::uint32_t SeedAddend = 0x3325e3bdu;
    static constexpr int SeedOffset = 0x3d;
    static constexpr std::uint8_t TailXor = 0xbd;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value += rotateRight(state, 8);
            value = substituteBytes(value, substituteTable64);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 6));
            value = substituteBytes(value, substituteTable64);
            value = substituteBytes(value, substituteTable64);
            value = reverseBits64WithoutFinal16BitSwap(value);
            value = swapAdjacentBits(value);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value += rotateLeft(state, 8);
            value = substituteBytes(value, substituteTable32);
            value ^= rotateLeft(state, 6);
            value = substituteBytes(value, substituteTable32);
            value = substituteBytes(value, substituteTable32);
            value = reverseBits32(value);
            value = swapAdjacentBits(value);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value += static_cast<std::uint8_t>(state * 0xcc6db61u);
            value = substituteTable8[value];
            value ^= static_cast<std::uint8_t>(state * 0x1b0829u);
            value = substituteTable8[value];
            value = substituteTable8[value];
            value = reverseBits8(value);
            value = swapAdjacentBits(value);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-11.07: ValorantSeededTransform11_07.cs.
struct Release11_07 {
    static constexpr std::uint32_t SeedAddend = 0x17b077d3u;
    static constexpr int SeedOffset = -0x2d;
    static constexpr std::uint8_t TailXor = 0xd3;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 8));
            value = substituteBytes(value, substituteTable64);
            value = reverseBits64WithoutFinal16BitSwap(value);
            value = ~value;
            value = swapAdjacentBits(value);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 1));

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value ^= rotateLeft(state, 8);
            value = substituteBytes(value, substituteTable32);
            value = reverseBits32(value);
            value = ~value;
            value = swapAdjacentBits(value);
            value ^= rotateLeft(state, 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value ^= static_cast<std::uint8_t>(state * 0xcc6db61u);
            value = substituteTable8[value];
            value = reverseBits8(value);
            value = static_cast<std::uint8_t>(~value);
            value = swapAdjacentBits(value);
            value ^= static_cast<std::uint8_t>(state * 0xbu);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-11.08: ValorantSeededTransform11_08.cs.
struct Release11_08 {
    static constexpr std::uint32_t SeedAddend = 0xacf2cdffu;
    static constexpr int SeedOffset = -0x01;
    static constexpr std::uint8_t TailXor = 0xff;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = ~value;
            value = rotateRight(value, static_cast<int>(rotateRight(state, 7) % 63) + 1);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 6) % 63) + 1);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 5) % 63) + 1);
            value -= rotateRight(state, 2);
            value = swapAdjacentBits(value);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = ~value;
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 7) % 31) + 1);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 6) % 31) + 1);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 5) % 31) + 1);
            value -= rotateLeft(state, 2);
            value = swapAdjacentBits(value);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = static_cast<std::uint8_t>(~value);
            value = rotateRight(value, static_cast<int>(state * 0x12959c3u % 7) + 1);
            value = rotateLeft(value, static_cast<int>(state * 0x1b0829u % 7) + 1);
            value = rotateLeft(value, static_cast<int>(state * 0x2751bu % 7) + 1);
            value -= static_cast<std::uint8_t>(state * 0x79u);
            value = swapAdjacentBits(value);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-11.09: ValorantSeededTransform11_09.cs.
struct Release11_09 {
    static constexpr std::uint32_t SeedAddend = 0x12cf14e5u;
    static constexpr int SeedOffset = -0x1b;
    static constexpr std::uint8_t TailXor = 0xe5;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = substituteBytes(value, substituteTable64);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 7));
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 6));
            value = swapAdjacentBits(value);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 3) % 63) + 1);
            value = swapAdjacentBits(value);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 1));

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = substituteBytes(value, substituteTable32);
            value ^= rotateLeft(state, 7);
            value ^= rotateLeft(state, 6);
            value = swapAdjacentBits(value);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 3) % 31) + 1);
            value = swapAdjacentBits(value);
            value ^= rotateLeft(state, 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = substituteTable8[value];
            value ^= static_cast<std::uint8_t>(state * 0x12959c3u);
            value ^= static_cast<std::uint8_t>(state * 0x1b0829u);
            value = swapAdjacentBits(value);
            value = rotateLeft(value, static_cast<int>(state * 0x533u % 7) + 1);
            value = swapAdjacentBits(value);
            value ^= static_cast<std::uint8_t>(state * 0xbu);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-11.10: ValorantSeededTransform11_10.cs.
struct Release11_10 {
    static constexpr std::uint32_t SeedAddend = 0x34e9d3ecu;
    static constexpr int SeedOffset = -0x14;
    static constexpr std::uint8_t TailXor = 0xec;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value -= rotateRight(state, 8);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 7));
            value -= rotateRight(state, 6);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 5) % 63) + 1);
            value -= rotateRight(state, 4);
            value = reverseBits64WithoutFinal16BitSwap(value);
            value = substituteBytes(value, substituteTable64);
            value = ~value;

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value -= rotateLeft(state, 8);
            value ^= rotateLeft(state, 7);
            value -= rotateLeft(state, 6);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 5) % 31) + 1);
            value -= rotateLeft(state, 4);
            value = reverseBits32(value);
            value = substituteBytes(value, substituteTable32);
            value = ~value;

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value -= static_cast<std::uint8_t>(state * 0xcc6db61u);
            value ^= static_cast<std::uint8_t>(state * 0x12959c3u);
            value -= static_cast<std::uint8_t>(state * 0x1b0829u);
            value = rotateLeft(value, static_cast<int>(state * 0x2751bu % 7) + 1);
            value -= static_cast<std::uint8_t>(state * 0x3931u);
            value = reverseBits8(value);
            value = substituteTable8[value];
            value = static_cast<std::uint8_t>(~value);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-11.11: ValorantSeededTransform11_11.cs.
struct Release11_11 {
    static constexpr std::uint32_t SeedAddend = 0xc4445c41u;
    static constexpr int SeedOffset = -0x3f;
    static constexpr std::uint8_t TailXor = 0x41;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 8));
            value = substituteBytes(value, substituteTable64);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 6) % 63) + 1);
            value = swapAdjacentBits(value);
            value = substituteBytes(value, substituteTable64);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 1) % 63) + 1);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value ^= rotateLeft(state, 8);
            value = substituteBytes(value, substituteTable32);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 6) % 31) + 1);
            value = swapAdjacentBits(value);
            value = substituteBytes(value, substituteTable32);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 1) % 31) + 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value ^= static_cast<std::uint8_t>(state * 0xcc6db61u);
            value = substituteTable8[value];
            value = rotateLeft(value, static_cast<int>(state * 0x1b0829u % 7) + 1);
            value = swapAdjacentBits(value);
            value = substituteTable8[value];
            value = rotateLeft(value, static_cast<int>(state * 0xbu % 7) + 1);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.00: ValorantSeededTransform12_00.cs.
struct Release12_00 {
    static constexpr std::uint32_t SeedAddend = 0x70876679u;
    static constexpr int SeedOffset = -0x07;
    static constexpr std::uint8_t TailXor = 0x79;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = rotateRight(value, static_cast<int>(rotateRight(state, 8) % 63) + 1);
            value = reverseBits64WithoutFinal16BitSwap(value);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 6));
            value -= rotateRight(state, 5);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 4));
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 3));
            value = ~value;

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 8) % 31) + 1);
            value = reverseBits32(value);
            value ^= rotateLeft(state, 6);
            value -= rotateLeft(state, 5);
            value ^= rotateLeft(state, 4);
            value ^= rotateLeft(state, 3);
            value = ~value;

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = rotateRight(value, static_cast<int>(state * 0xcc6db61u % 7) + 1);
            value = reverseBits8(value);
            value ^= static_cast<std::uint8_t>(state * 0x1b0829u);
            value -= static_cast<std::uint8_t>(state * 0x2751bu);
            value ^= static_cast<std::uint8_t>(state * 0x3931u);
            value ^= static_cast<std::uint8_t>(state * 0x533u);
            value = static_cast<std::uint8_t>(~value);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.01: ValorantSeededTransform12_01.cs.
struct Release12_01 {
    static constexpr std::uint32_t SeedAddend = 0x13fdd831u;
    static constexpr int SeedOffset = 0x31;
    static constexpr std::uint8_t TailXor = 0x31;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = substituteBytes(value, substituteTable64);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 7));
            value -= rotateRight(state, 6);
            value = swapAdjacentBits(value);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 4) % 63) + 1);
            value = substituteBytes(value, substituteTable64);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 2));
            value += rotateRight(state, 1);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = substituteBytes(value, substituteTable32);
            value ^= rotateLeft(state, 7);
            value -= rotateLeft(state, 6);
            value = swapAdjacentBits(value);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 4) % 31) + 1);
            value = substituteBytes(value, substituteTable32);
            value ^= rotateLeft(state, 2);
            value += rotateLeft(state, 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = substituteTable8[value];
            value ^= static_cast<std::uint8_t>(state * 0x12959c3u);
            value -= static_cast<std::uint8_t>(state * 0x1b0829u);
            value = swapAdjacentBits(value);
            value = rotateLeft(value, static_cast<int>(state * 0x3931u % 7) + 1);
            value = substituteTable8[value];
            value ^= static_cast<std::uint8_t>(state * 0x79u);
            value += static_cast<std::uint8_t>(state * 0xbu);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.02: ValorantSeededTransform12_02.cs.
struct Release12_02 {
    static constexpr std::uint32_t SeedAddend = 0x9830d09du;
    static constexpr int SeedOffset = 0x1d;
    static constexpr std::uint8_t TailXor = 0x9d;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 8) % 63) + 1);
            value = rotateRight(value, static_cast<int>(rotateRight(state, 7) % 63) + 1);
            value = reverseBits64WithoutFinal16BitSwap(value);
            value = swapAdjacentBits(value);
            value = substituteBytes(value, substituteTable64);
            value = reverseBits64WithoutFinal16BitSwap(value);
            value -= rotateRight(state, 2);
            value = reverseBits64WithoutFinal16BitSwap(value);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 8) % 31) + 1);
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 7) % 31) + 1);
            value = reverseBits32(value);
            value = swapAdjacentBits(value);
            value = substituteBytes(value, substituteTable32);
            value = reverseBits32(value);
            value -= rotateLeft(state, 2);
            value = reverseBits32(value);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = rotateLeft(value, static_cast<int>(state * 0xcc6db61u % 7) + 1);
            value = rotateRight(value, static_cast<int>(state * 0x12959c3u % 7) + 1);
            value = reverseBits8(value);
            value = swapAdjacentBits(value);
            value = substituteTable8[value];
            value = reverseBits8(value);
            value -= static_cast<std::uint8_t>(state * 0x79u);
            value = reverseBits8(value);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.03: ValorantSeededTransform12_03.cs.
struct Release12_03 {
    static constexpr std::uint32_t SeedAddend = 0x33d59dffu;
    static constexpr int SeedOffset = -0x01;
    static constexpr std::uint8_t TailXor = 0xff;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = swapAdjacentBits(value);
            value -= rotateRight(state, 7);
            value += rotateRight(state, 6);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 5) % 63) + 1);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 4) % 63) + 1);
            value = substituteBytes(value, substituteTable64);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 2) % 63) + 1);
            value += rotateRight(state, 1);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = swapAdjacentBits(value);
            value -= rotateLeft(state, 7);
            value += rotateLeft(state, 6);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 5) % 31) + 1);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 4) % 31) + 1);
            value = substituteBytes(value, substituteTable32);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 2) % 31) + 1);
            value += rotateLeft(state, 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = swapAdjacentBits(value);
            value -= static_cast<std::uint8_t>(state * 0x12959c3u);
            value += static_cast<std::uint8_t>(state * 0x1b0829u);
            value = rotateLeft(value, static_cast<int>(state * 0x2751bu % 7) + 1);
            value = rotateLeft(value, static_cast<int>(state * 0x3931u % 7) + 1);
            value = substituteTable8[value];
            value = rotateLeft(value, static_cast<int>(state * 0x79u % 7) + 1);
            value += static_cast<std::uint8_t>(state * 0xbu);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.04: ValorantSeededTransform12_04.cs.
struct Release12_04 {
    static constexpr std::uint32_t SeedAddend = 0xa5684b42u;
    static constexpr int SeedOffset = -0x3e;
    static constexpr std::uint8_t TailXor = 0x42;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = rotateRight(value, static_cast<int>(rotateRight(state, 8) % 63) + 1);
            value = rotateRight(value, static_cast<int>(rotateRight(state, 7) % 63) + 1);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 6) % 63) + 1);
            value = rotateRight(value, static_cast<int>(rotateRight(state, 5) % 63) + 1);
            value += rotateRight(state, 4);
            value = substituteBytes(value, substituteTable64);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 1) % 63) + 1);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 8) % 31) + 1);
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 7) % 31) + 1);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 6) % 31) + 1);
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 5) % 31) + 1);
            value += rotateLeft(state, 4);
            value = substituteBytes(value, substituteTable32);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 1) % 31) + 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = rotateRight(value, static_cast<int>(state * 0xcc6db61u % 7) + 1);
            value = rotateRight(value, static_cast<int>(state * 0x12959c3u % 7) + 1);
            value = rotateLeft(value, static_cast<int>(state * 0x1b0829u % 7) + 1);
            value = rotateRight(value, static_cast<int>(state * 0x2751bu % 7) + 1);
            value += static_cast<std::uint8_t>(state * 0x3931u);
            value = substituteTable8[value];
            value = rotateLeft(value, static_cast<int>(state * 0xbu % 7) + 1);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.05: ValorantSeededTransform12_05.cs.
struct Release12_05 {
    static constexpr std::uint32_t SeedAddend = 0xc21d548cu;
    static constexpr int SeedOffset = 0x0c;
    static constexpr std::uint8_t TailXor = 0x8c;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = rotateRight(value, static_cast<int>(rotateRight(state, 8) % 63) + 1);
            value = ~value;
            value = reverseBits64WithoutFinal16BitSwap(value);
            value -= rotateRight(state, 4);
            value = rotateRight(value, static_cast<int>(rotateRight(state, 3) % 63) + 1);
            value = substituteBytes(value, substituteTable64);
            value = ~value;

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 8) % 31) + 1);
            value = ~value;
            value = reverseBits32(value);
            value -= rotateLeft(state, 4);
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 3) % 31) + 1);
            value = substituteBytes(value, substituteTable32);
            value = ~value;

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = rotateRight(value, static_cast<int>(state * 0xcc6db61u % 7) + 1);
            value = static_cast<std::uint8_t>(~value);
            value = reverseBits8(value);
            value -= static_cast<std::uint8_t>(state * 0x3931u);
            value = rotateRight(value, static_cast<int>(state * 0x533u % 7) + 1);
            value = substituteTable8[value];
            value = static_cast<std::uint8_t>(~value);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.06: ValorantSeededTransform12_06.cs.
struct Release12_06 {
    static constexpr std::uint32_t SeedAddend = 0x8d686ca6u;
    static constexpr int SeedOffset = 0x26;
    static constexpr std::uint8_t TailXor = 0xa6;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 8));
            value = rotateRight(value, static_cast<int>(rotateRight(state, 7) % 63) + 1);
            value -= rotateRight(state, 6);
            value = reverseBits64WithoutFinal16BitSwap(value);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 3) % 63) + 1);
            value = reverseBits64WithoutFinal16BitSwap(value);
            value -= rotateRight(state, 1);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value ^= rotateLeft(state, 8);
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 7) % 31) + 1);
            value -= rotateLeft(state, 6);
            value = reverseBits32(value);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 3) % 31) + 1);
            value = reverseBits32(value);
            value -= rotateLeft(state, 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value ^= static_cast<std::uint8_t>(state * 0xcc6db61u);
            value = rotateRight(value, static_cast<int>(state * 0x12959c3u % 7) + 1);
            value -= static_cast<std::uint8_t>(state * 0x1b0829u);
            value = reverseBits8(value);
            value = rotateLeft(value, static_cast<int>(state * 0x533u % 7) + 1);
            value = reverseBits8(value);
            value -= static_cast<std::uint8_t>(state * 0xbu);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.07: ValorantSeededTransform12_07.cs.
struct Release12_07 {
    static constexpr std::uint32_t SeedAddend = 0x2d21d7c3u;
    static constexpr int SeedOffset = -0x3d;
    static constexpr std::uint8_t TailXor = 0xc3;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 8));
            value += rotateRight(state, 7);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 6));
            value = swapAdjacentBits(value);
            value = substituteBytes(value, substituteTable64);
            value -= rotateRight(state, 3);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value ^= rotateLeft(state, 8);
            value += rotateLeft(state, 7);
            value ^= rotateLeft(state, 6);
            value = swapAdjacentBits(value);
            value = substituteBytes(value, substituteTable32);
            value -= rotateLeft(state, 3);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value ^= static_cast<std::uint8_t>(state * 0xcc6db61u);
            value += static_cast<std::uint8_t>(state * 0x12959c3u);
            value ^= static_cast<std::uint8_t>(state * 0x1b0829u);
            value = swapAdjacentBits(value);
            value = substituteTable8[value];
            value -= static_cast<std::uint8_t>(state * 0x533u);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.08: ValorantSeededTransform12_08.cs.
struct Release12_08 {
    static constexpr std::uint32_t SeedAddend = 0xce2e33e5u;
    static constexpr int SeedOffset = -0x1b;
    static constexpr std::uint8_t TailXor = 0xe5;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 7) % 63) + 1);
            value = substituteBytes(value, substituteTable64);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 4));
            value = reverseBits64WithoutFinal16BitSwap(value);
            value += rotateRight(state, 2);
            value = rotateRight(value, static_cast<int>(rotateRight(state, 1) % 63) + 1);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 7) % 31) + 1);
            value = substituteBytes(value, substituteTable32);
            value ^= rotateLeft(state, 4);
            value = reverseBits32(value);
            value += rotateLeft(state, 2);
            value = rotateRight(value, static_cast<int>(rotateLeft(state, 1) % 31) + 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = rotateLeft(value, static_cast<int>(state * 0x12959c3u % 7) + 1);
            value = substituteTable8[value];
            value ^= static_cast<std::uint8_t>(state * 0x3931u);
            value = reverseBits8(value);
            value += static_cast<std::uint8_t>(state * 0x79u);
            value = rotateRight(value, static_cast<int>(state * 0xbu % 7) + 1);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.09: ValorantSeededTransform12_09.cs.
struct Release12_09 {
    static constexpr std::uint32_t SeedAddend = 0x7ff2feecu;
    static constexpr int SeedOffset = -0x14;
    static constexpr std::uint8_t TailXor = 0xec;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, SeedOffset);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 8) % 63) + 1);
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 7));
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 6) % 63) + 1);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 5) % 63) + 1);
            value = ~value;
            value ^= ~static_cast<std::uint64_t>(rotateRight(state, 3));
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 2) % 63) + 1);
            value = rotateLeft(value, static_cast<int>(rotateRight(state, 1) % 63) + 1);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 8) % 31) + 1);
            value ^= rotateLeft(state, 7);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 6) % 31) + 1);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 5) % 31) + 1);
            value = ~value;
            value ^= rotateLeft(state, 3);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 2) % 31) + 1);
            value = rotateLeft(value, static_cast<int>(rotateLeft(state, 1) % 31) + 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            value = rotateLeft(value, static_cast<int>(state * 0xcc6db61u % 7) + 1);
            value ^= static_cast<std::uint8_t>(state * 0x12959c3u);
            value = rotateLeft(value, static_cast<int>(state * 0x1b0829u % 7) + 1);
            value = rotateLeft(value, static_cast<int>(state * 0x2751bu % 7) + 1);
            value = static_cast<std::uint8_t>(~value);
            value ^= static_cast<std::uint8_t>(state * 0x533u);
            value = rotateLeft(value, static_cast<int>(state * 0x79u % 7) + 1);
            value = rotateLeft(value, static_cast<int>(state * 0xbu % 7) + 1);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }
};

// ++Ares-Core+release-12.10: ValorantSeededTransform12_10.cs.
struct Release12_10 {
    static constexpr std::uint32_t SeedAddend = 0x12fd0ee5u;
    static constexpr std::uint32_t InitAOffset = 0x1bu;
    static constexpr std::uint8_t TailXor = 0xe5;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            auto ror4 = rotateRight(state, 4);
            auto ror5 = rotateRight(state, 5);
            auto ror6 = rotateRight(state, 6);
            auto ror8 = rotateRight(state, 8);

            value = rotateRight(value, static_cast<int>(ror8 % 63) + 1);
            value = swapAdjacentBits(value);
            value -= ror6;
            value = rotateRight(value, static_cast<int>(ror5 % 63) + 1);
            value = swapAdjacentBits(value ^ ~static_cast<std::uint64_t>(ror4));
            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            auto rot4 = rotateLeft(state, 4);
            auto rot5 = rotateLeft(state, 5);
            auto rot6 = rotateLeft(state, 6);
            auto rot8 = rotateLeft(state, 8);

            value = rotateRight(value, static_cast<int>(rot8 % 31) + 1);
            value = swapAdjacentBits(value);
            value -= rot6;
            value = rotateRight(value, static_cast<int>(rot5 % 31) + 1);
            value = swapAdjacentBits(value ^ rot4);
            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            auto addend1 = static_cast<std::uint8_t>(state * 0x31);
            auto addend2 = static_cast<std::uint8_t>(state * 0x29);

            value = rotateRight(value, static_cast<int>(state * 0xcc6db61 % 7) + 1);
            value = swapAdjacentBits(value);
            value -= addend2;
            value = rotateRight(value, static_cast<int>(state * 0x2751b % 7) + 1);
            value = swapAdjacentBits(static_cast<std::uint8_t>(value ^ addend1));
            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t initialPrngA(std::uint32_t seed) {
        auto seedPlus = seed + SeedAddend;
        auto mixed = ((seedPlus >> 15) ^ seedPlus) >> 12 ^ ((seed - InitAOffset) * 0x02000000u) ^ seedPlus;
        return mixed * multiplier;
    }
};

// ++Ares-Core+release-12.11: ValorantSeededTransform12_11.cs.
struct Release12_11 {
    static constexpr std::uint32_t SeedAddend = 0x409d36a3u;
    static constexpr std::uint32_t InitAOffset = 0x23u;
    static constexpr std::uint8_t TailXor = 0xa3;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            auto ror2 = rotateRight(state, 2);
            auto ror3 = rotateRight(state, 3);
            auto ror4 = rotateRight(state, 4);
            auto ror6 = rotateRight(state, 6);
            auto ror8 = rotateRight(state, 8);

            value = rotateRight(value, static_cast<int>(ror8 % 63) + 1);
            value = swapAdjacentBits(value);
            value += ror6;
            value = reverseBits64WithoutFinal16BitSwap(value);
            value -= ror4;
            value -= ror3;
            value -= ror2;
            value = swapAdjacentBits(value);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            auto rol2 = rotateLeft(state, 2);
            auto rol3 = rotateLeft(state, 3);
            auto rol4 = rotateLeft(state, 4);
            auto rol6 = rotateLeft(state, 6);
            auto rol8 = rotateLeft(state, 8);

            value = rotateRight(value, static_cast<int>(rol8 % 31) + 1);
            value = swapAdjacentBits(value);
            value += rol6;
            value = reverseBits32(value);
            value -= rol4;
            value -= rol3;
            value -= rol2;
            value = swapAdjacentBits(value);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            auto stateByte = static_cast<std::uint8_t>(state);
            auto rotate2Input = state * 0x0cc6db61u;

            value = rotateRight(value, static_cast<int>(rotate2Input % 7) + 1);
            value = swapAdjacentBits(value);
            value += static_cast<std::uint8_t>(stateByte * 0x29);
            value = reverseBits8(value);
            value += static_cast<std::uint8_t>(stateByte * 0x23);
            value = swapAdjacentBits(value);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t initialPrngA(std::uint32_t seed) {
        auto seedPlus = seed + SeedAddend;
        auto mixed = ((seedPlus >> 15) ^ seedPlus) >> 12 ^ ((seed + InitAOffset) * 0x02000000u) ^ seedPlus;
        return mixed * multiplier;
    }
};

// ++Ares-Core+release-13.00: ValorantSeededTransform13_00.cs.
struct Release13_00 {
    static constexpr std::uint32_t SeedAddend = 0x2949b6efu;
    static constexpr std::uint32_t InitAOffset = 0x11u;
    static constexpr std::uint8_t TailXor = 0xef;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            auto ror1 = rotateRight(state, 1);
            auto ror3 = rotateRight(state, 3);
            auto ror6 = rotateRight(state, 6);
            auto ror8 = rotateRight(state, 8);

            value += ror8;
            value = reverseBits64WithoutFinal16BitSwap(value);
            value = (value + ror6) ^ ror3;
            value = substituteBytes(value, substituteTable64);
            value = rotateRight(value, static_cast<int>(ror1 % 63) + 1);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            auto rol1 = rotateLeft(state, 1);
            auto rol3 = rotateLeft(state, 3);
            auto rol6 = rotateLeft(state, 6);
            auto rol8 = rotateLeft(state, 8);

            value += rol8;
            value = reverseBits32(value);
            value = ~(value + rol6) ^ rol3;
            value = substituteBytes(value, substituteTable32);
            value = rotateRight(value, static_cast<int>(rol1 % 31) + 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto value = output[byteOffset];
            auto mix = state * 0x533u;

            value = static_cast<std::uint8_t>(value + static_cast<std::uint8_t>(mix) * 0x1b);
            value = reverseBits8(value);
            value = static_cast<std::uint8_t>(~(value + static_cast<std::uint8_t>(mix) * 0x33) ^ static_cast<std::uint8_t>(mix));
            value = substituteTable8[value];
            value = rotateRight(value, static_cast<int>(state * 0x0bu % 7) + 1);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t initialPrngA(std::uint32_t seed) {
        auto seedPlus = seed + SeedAddend;
        auto mixed = ((seedPlus >> 15) ^ seedPlus) >> 12 ^ ((seed - InitAOffset) * 0x02000000u) ^ seedPlus;
        return mixed * multiplier;
    }
};

// ++Ares-Core+release-13.01: ValorantSeededTransform13_01.cs.
struct Release13_01 {
    static constexpr std::uint32_t SeedAddend = 0xe62fcd5cu;
    static constexpr std::uint32_t InitAOffset = 0x24u;
    static constexpr std::uint8_t TailXor = 0x5c;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = transformUInt64(readUInt64(output, byteOffset), state);
            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = transformUInt32(readUInt32(output, byteOffset), state);
            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            output[byteOffset] = transformByte(output[byteOffset], state);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t transformUInt64(std::uint64_t value, std::uint32_t state) {
        value = swapAdjacentBits(~value) ^ ~static_cast<std::uint64_t>(rotateRight(state, 5));
        value = ~rotateRight(value, static_cast<int>(rotateRight(state, 4) % 63) + 1);
        return value + rotateRight(state, 1);
    }

    static std::uint32_t transformUInt32(std::uint32_t value, std::uint32_t state) {
        value = swapAdjacentBits(~value) ^ rotateLeft(state, 5);
        value = ~rotateRight(value, static_cast<int>(rotateLeft(state, 4) % 31) + 1);
        return value + rotateLeft(state, 1);
    }

    static std::uint8_t transformByte(std::uint8_t value, std::uint32_t state) {
        auto state11 = state * 0x0bu;
        auto mix = state11 * 0x533u;
        value = static_cast<std::uint8_t>(swapAdjacentBits(static_cast<std::uint8_t>(~value)) ^ static_cast<std::uint8_t>(mix * 0x0bu));
        value = static_cast<std::uint8_t>(~rotateRight(value, static_cast<int>(mix % 7) + 1));
        return static_cast<std::uint8_t>(value + static_cast<std::uint8_t>(state11));
    }

    static std::uint64_t initialPrngA(std::uint32_t seed) {
        auto seedPlus = seed + SeedAddend;
        auto mixed = ((seedPlus >> 15) ^ seedPlus) >> 12 ^
                    ((seed - InitAOffset) * 0x02000000u) ^
                    seedPlus;
        return mixed * multiplier;
    }
};

// ++Ares-Core+release-13.02: ValorantSeededTransform13_02.cs.
struct Release13_02 {
    static constexpr std::uint32_t SeedAddend = 0x9e81a37cu;
    static constexpr std::uint32_t InitAOffset = 0x04u;
    static constexpr std::uint8_t TailXor = 0x7c;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = substituteBytes(readUInt64(output, byteOffset), substituteTable64);
            auto ror2 = rotateRight(state, 2);
            auto ror3 = rotateRight(state, 3);
            auto ror6 = rotateRight(state, 6);

            value = reverseBits64WithoutFinal16BitSwap(value);
            value = ~(value - ror6);
            value = reverseBits64WithoutFinal16BitSwap(value);
            value = rotateLeft(value, static_cast<int>(ror3 % 63) + 1);
            value = rotateRight(value, static_cast<int>(ror2 % 63) + 1);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = substituteBytes(readUInt32(output, byteOffset), substituteTable32);
            auto rol2 = rotateLeft(state, 2);
            auto rol3 = rotateLeft(state, 3);
            auto rol6 = rotateLeft(state, 6);

            value = reverseBits32(value);
            value = ~(value - rol6);
            value = reverseBits32(value);
            value = rotateLeft(value, static_cast<int>(rol3 % 31) + 1);
            value = rotateRight(value, static_cast<int>(rol2 % 31) + 1);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto mixA = state * 0x79u;
            auto mixB = mixA * 0x0bu;
            auto value = substituteTable8[output[byteOffset]];

            value = reverseBits8(value);
            value = static_cast<std::uint8_t>(~(value - static_cast<std::uint8_t>(mixB * 0x33u)));
            value = reverseBits8(value);
            value = rotateLeft(value, static_cast<int>(mixB % 7) + 1);
            value = rotateRight(value, static_cast<int>(mixA % 7) + 1);

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t initialPrngA(std::uint32_t seed) {
        auto seedPlus = seed + SeedAddend;
        auto mixed = ((seedPlus >> 15) ^ seedPlus) >> 12 ^ ((seed - InitAOffset) * 0x02000000u) ^ seedPlus;
        return mixed * multiplier;
    }
};

// ++Ares-Core+release-13.04: ValorantSeededTransform13_04.cs.
struct Release13_04 {
    static constexpr std::uint32_t SeedAddend = 0x076dc658u;
    static constexpr std::uint32_t InitAOffset = 0x28u;
    static constexpr std::uint8_t TailXor = 0x58;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = transformUInt64(readUInt64(output, byteOffset), state);
            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = transformUInt32(readUInt32(output, byteOffset), state);
            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            output[byteOffset] = transformByte(output[byteOffset], state);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t transformUInt64(std::uint64_t value, std::uint32_t state) {
        auto ror1 = rotateRight(state, 1);
        auto ror2 = rotateRight(state, 2);
        auto ror3 = rotateRight(state, 3);
        auto ror5 = rotateRight(state, 5);
        auto ror6 = rotateRight(state, 6);
        auto ror7 = rotateRight(state, 7);

        value = rotateRight(value, static_cast<int>(ror7 % 63) + 1);
        value ^= ~static_cast<std::uint64_t>(ror6);
        value -= ror5;
        value = swapAdjacentBits(value);
        value += ror3;
        value += ror2;
        return rotateLeft(value, static_cast<int>(ror1 % 63) + 1);
    }

    static std::uint32_t transformUInt32(std::uint32_t value, std::uint32_t state) {
        auto rol1 = rotateLeft(state, 1);
        auto rol2 = rotateLeft(state, 2);
        auto rol3 = rotateLeft(state, 3);
        auto rol5 = rotateLeft(state, 5);
        auto rol6 = rotateLeft(state, 6);
        auto rol7 = rotateLeft(state, 7);

        value = rotateRight(value, static_cast<int>(rol7 % 31) + 1);
        value ^= rol6;
        value -= rol5;
        value = swapAdjacentBits(value);
        value += rol3;
        value += rol2;
        return rotateLeft(value, static_cast<int>(rol1 % 31) + 1);
    }

    static std::uint8_t transformByte(std::uint8_t value, std::uint32_t state) {
        auto mixA = state * 0x0bu;
        auto mixB = mixA * 0x0bu;
        auto mixC = mixB * 0x0bu;
        auto mixD = mixC * 0x79u;
        auto mixE = mixD * 0x0bu;
        auto mixF = mixE * 0x0bu;

        value = rotateRight(value, static_cast<int>(mixF % 7) + 1);
        value = static_cast<std::uint8_t>((value ^ static_cast<std::uint8_t>(mixE)) - static_cast<std::uint8_t>(mixD));
        value = swapAdjacentBits(value);
        value = static_cast<std::uint8_t>(value + static_cast<std::uint8_t>(mixB) + static_cast<std::uint8_t>(mixC));
        return rotateLeft(value, static_cast<int>(mixA % 7) + 1);
    }

    static std::uint64_t initialPrngA(std::uint32_t seed) {
        auto seedPlus = seed + SeedAddend;
        auto mixed = ((seedPlus >> 15) ^ seedPlus) >> 12 ^
                    ((seed - InitAOffset) * 0x02000000u) ^
                    seedPlus;
        return mixed * multiplier;
    }
};

// ++Ares-Core+release-13.05: ValorantSeededTransform13_05.cs.
struct Release13_05 {
    static constexpr std::uint32_t SeedAddend = 0x48c26613u;
    static constexpr std::uint32_t InitASeedAddend = 0x13u;
    static constexpr std::uint8_t TailXor = 0x13;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = transformUInt64(readUInt64(output, byteOffset), state);
            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = transformUInt32(readUInt32(output, byteOffset), state);
            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            output[byteOffset] = transformByte(output[byteOffset], state);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t transformUInt64(std::uint64_t value, std::uint32_t state) {
        auto ror1 = rotateRight(state, 1);
        auto ror2 = rotateRight(state, 2);
        auto ror3 = rotateRight(state, 3);
        auto ror4 = rotateRight(state, 4);
        auto ror5 = rotateRight(state, 5);
        auto ror6 = rotateRight(state, 6);
        auto ror7 = rotateRight(state, 7);
        auto ror8 = rotateRight(state, 8);

        value = (value ^ ~static_cast<std::uint64_t>(ror8)) - ror7;
        value = rotateLeft(value, static_cast<int>(ror6 % 63) + 1);
        value = (value - ror5) ^ ~static_cast<std::uint64_t>(ror4) ^ ~static_cast<std::uint64_t>(ror3);
        value = rotateLeft(value, static_cast<int>(ror2 % 63) + 1);
        return rotateLeft(value, static_cast<int>(ror1 % 63) + 1);
    }

    static std::uint32_t transformUInt32(std::uint32_t value, std::uint32_t state) {
        auto rol1 = rotateLeft(state, 1);
        auto rol2 = rotateLeft(state, 2);
        auto rol3 = rotateLeft(state, 3);
        auto rol4 = rotateLeft(state, 4);
        auto rol5 = rotateLeft(state, 5);
        auto rol6 = rotateLeft(state, 6);
        auto rol7 = rotateLeft(state, 7);
        auto rol8 = rotateLeft(state, 8);

        value = (value ^ rol8) - rol7;
        value = rotateLeft(value, static_cast<int>(rol6 % 31) + 1);
        value = (value - rol5) ^ rol4 ^ rol3;
        value = rotateLeft(value, static_cast<int>(rol2 % 31) + 1);
        return rotateLeft(value, static_cast<int>(rol1 % 31) + 1);
    }

    static std::uint8_t transformByte(std::uint8_t value, std::uint32_t state) {
        auto stateByte = static_cast<std::uint8_t>(state);
        auto mixA = state * 0x1b0829u;

        value = static_cast<std::uint8_t>((static_cast<std::uint8_t>(mixA * 0x79u) ^ value) - static_cast<std::uint8_t>(mixA * 0x0bu));
        value = rotateLeft(value, static_cast<int>(mixA % 7) + 1);
        value = static_cast<std::uint8_t>((value - static_cast<std::uint8_t>(stateByte * 0x1bu)) ^ static_cast<std::uint8_t>(stateByte * 0x33u) ^
                       static_cast<std::uint8_t>(stateByte * 0x31u));
        value = rotateLeft(value, static_cast<int>(state * 0x79u % 7) + 1);
        return rotateLeft(value, static_cast<int>(state * 0x0bu % 7) + 1);
    }

    static std::uint64_t initialPrngA(std::uint32_t seed) {
        auto seedPlus = seed + SeedAddend;
        auto mixed = ((seedPlus >> 15) ^ seedPlus) >> 12 ^
                    ((seed + InitASeedAddend) * 0x02000000u) ^
                    seedPlus;
        return mixed * multiplier;
    }
};

// ++Ares-Core+release-13.06: ValorantSeededTransform13_06.cs.
struct Release13_06 {
    static constexpr std::uint32_t SeedAddend = 0xe974593cu;
    static constexpr std::uint32_t InitASeedAddend = 0x3cu;
    static constexpr std::uint8_t TailXor = 0x3c;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = readUInt64(output, byteOffset);
            value = swapAdjacentBits(value) ^ ~static_cast<std::uint64_t>(rotateRight(state, 7));
            value = reverseBits64WithoutFinal16BitSwap(swapAdjacentBits(value));
            value = substituteBytes(value, substituteTable64);
            value = rotateRight(state, 2) + ~value;
            value = substituteBytes(value, substituteTable64);

            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = readUInt32(output, byteOffset);
            value = rotateLeft(state, 7) ^ swapAdjacentBits(value);
            value = reverseBits32(swapAdjacentBits(value));
            value = substituteBytes(value, substituteTable32);
            value = ~value + rotateLeft(state, 2);
            value = substituteBytes(value, substituteTable32);

            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            auto stateByte = static_cast<std::uint8_t>(static_cast<std::int8_t>(state) * 0x79);
            auto value = static_cast<std::uint8_t>(swapAdjacentBits(output[byteOffset]) ^ static_cast<std::uint8_t>(stateByte * 0x1b));
            value = reverseBits8(value);
            value = substituteTable8[swapAdjacentBits(value)];
            value = substituteTable8[static_cast<std::uint8_t>(~value + stateByte)];

            output[byteOffset] = value;
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t initialPrngA(std::uint32_t seed) {
        auto seedPlus = seed + SeedAddend;
        auto mixed = ((seedPlus >> 15) ^ seedPlus) >> 12 ^
                    ((seed + InitASeedAddend) * 0x02000000u) ^
                    seedPlus;
        return mixed * multiplier;
    }
};

// ++Ares-Core+release-13.08: ValorantSeededTransform13_08.cs.
struct Release13_08 {
    static constexpr std::uint32_t SeedAddend = 0x3c17a9fau;
    static constexpr std::uint8_t TailXor = 0xfa;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed, SeedAddend, -6);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            auto value = transformUInt64(readUInt64(output, byteOffset), state);
            writeUInt64(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            auto value = transformUInt32(readUInt32(output, byteOffset), state);
            writeUInt32(output, byteOffset, value);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            output[byteOffset] = transformByte(output[byteOffset], state);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t transformUInt64(std::uint64_t value, std::uint32_t state) {
        value = reverseBits64WithoutFinal16BitSwap(value + rotateRight(state, 8));
        value = rotateLeft(value, static_cast<int>(rotateRight(state, 6) % 63) + 1);
        value = substituteBytes(value, substituteTable64);
        value = rotateRight(value, static_cast<int>(rotateRight(state, 4) % 63) + 1);
        value = substituteBytes(value, substituteTable64);
        value = substituteBytes(value, substituteTable64);
        return reverseBits64WithoutFinal16BitSwap(value);
    }

    static std::uint32_t transformUInt32(std::uint32_t value, std::uint32_t state) {
        value = reverseBits32(value + rotateLeft(state, 8));
        value = rotateLeft(value, static_cast<int>(rotateLeft(state, 6) % 31) + 1);
        value = substituteBytes(value, substituteTable32);
        value = rotateRight(value, static_cast<int>(rotateLeft(state, 4) % 31) + 1);
        value = substituteBytes(value, substituteTable32);
        value = substituteBytes(value, substituteTable32);
        return reverseBits32(value);
    }

    static std::uint8_t transformByte(std::uint8_t value, std::uint32_t state) {
        auto mix6 = state * 0x1b0829u;
        auto mix4 = state * 0x3931u;

        value = reverseBits8(static_cast<std::uint8_t>(value + static_cast<std::uint8_t>(mix6 * 0x79u)));
        value = rotateLeft(value, static_cast<int>(mix6 % 7) + 1);
        value = substituteTable8[value];
        value = rotateRight(value, static_cast<int>(mix4 % 7) + 1);
        value = substituteTable8[value];
        value = substituteTable8[value];
        return reverseBits8(value);
    }
};

// ++Ares-Core+release-china-13.05: ValorantSeededTransformChina13_05.cs.
struct ReleaseChina13_05 {
    static constexpr std::uint32_t SeedAddend = 0xf67761c9u;
    static constexpr std::uint32_t InitASeedOffset = 0xc9u;
    static constexpr std::uint8_t TailXor = 0xc9;

    static void transform(std::uint8_t* output, int bitCount, std::uint32_t seed) {
        if (bitCount == 0) return;

        auto state = seed;
        auto streamByte = static_cast<std::uint8_t>(seed);
        auto prngA = initialPrngA(seed);
        auto prngB = initialPrngB(seed);
        auto byteOffset = 0;
        auto bitsRemaining = bitCount;

        while (bitsRemaining > 63) {
            writeUInt64(output, byteOffset, transformUInt64(readUInt64(output, byteOffset), state));
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 8;
            bitsRemaining -= 64;
        }

        while (bitsRemaining > 31) {
            writeUInt32(output, byteOffset, transformUInt32(readUInt32(output, byteOffset), state));
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset += 4;
            bitsRemaining -= 32;
        }

        while (bitsRemaining > 7) {
            output[byteOffset] = transformByte(output[byteOffset], state);
            advanceTransformState(state, prngA, prngB, streamByte);
            byteOffset++;
            bitsRemaining -= 8;
        }

        if (bitsRemaining != 0) {
            auto mask = static_cast<std::uint8_t>(0xff >> (7 - ((bitCount - 1) & 7)));
            output[byteOffset] ^= static_cast<std::uint8_t>(mask & (streamByte ^ TailXor));
        }
    }

    static std::uint64_t transformUInt64(std::uint64_t value, std::uint32_t state) {
        auto ror1 = rotateRight(state, 1);
        auto ror2 = rotateRight(state, 2);
        auto ror4 = rotateRight(state, 4);
        auto ror6 = rotateRight(state, 6);
        auto ror7 = rotateRight(state, 7);
        auto ror8 = rotateRight(state, 8);
        value += ror8;
        value = rotateLeft(value, static_cast<int>(ror6 % 63) + 1);
        value = rotateRight(value, static_cast<int>(ror7 % 63) + 1);
        value = rotateLeft(value, static_cast<int>(ror4 % 63) + 1);
        value = rotateRight(value, static_cast<int>(ror2 % 63) + 1);
        return value - ror1;
    }

    static std::uint32_t transformUInt32(std::uint32_t value, std::uint32_t state) {
        auto rol1 = rotateLeft(state, 1);
        auto rol2 = rotateLeft(state, 2);
        auto rol4 = rotateLeft(state, 4);
        auto rol6 = rotateLeft(state, 6);
        auto rol7 = rotateLeft(state, 7);
        auto rol8 = rotateLeft(state, 8);
        value += rol8;
        value = rotateLeft(value, static_cast<int>(rol6 % 31) + 1);
        value = rotateRight(value, static_cast<int>(rol7 % 31) + 1);
        value = rotateLeft(value, static_cast<int>(rol4 % 31) + 1);
        value = rotateRight(value, static_cast<int>(rol2 % 31) + 1);
        return value - rol1;
    }

    static std::uint8_t transformByte(std::uint8_t value, std::uint32_t state) {
        auto m1 = state * 0x0000000bu;
        auto m2 = state * 0x00000079u;
        auto m4 = state * 0x00003931u;
        auto m6 = state * 0x001b0829u;
        auto m7 = state * 0x012959c3u;
        auto m8 = state * 0x0cc6db61u;
        value += static_cast<std::uint8_t>(m8);
        value = rotateLeft(value, static_cast<int>(m6 % 7) + 1);
        value = rotateRight(value, static_cast<int>(m7 % 7) + 1);
        value = rotateLeft(value, static_cast<int>(m4 % 7) + 1);
        value = rotateRight(value, static_cast<int>(m2 % 7) + 1);
        return static_cast<std::uint8_t>(value - static_cast<std::uint8_t>(m1));
    }

    static std::uint64_t initialPrngA(std::uint32_t seed) {
        auto seedPlus = seed + SeedAddend;
        auto mixed = ((seedPlus >> 15) ^ seedPlus) >> 12 ^
                    ((seed + InitASeedOffset) * 0x02000000u) ^
                    seedPlus;
        return mixed * multiplier;
    }
};

constexpr Release releases[] = {
    {"++Ares-Core+release-11.06", Release11_06::transform},
    {"++Ares-Core+release-11.07", Release11_07::transform},
    {"++Ares-Core+release-11.08", Release11_08::transform},
    {"++Ares-Core+release-11.09", Release11_09::transform},
    {"++Ares-Core+release-11.10", Release11_10::transform},
    {"++Ares-Core+release-11.11", Release11_11::transform},
    {"++Ares-Core+release-12.00", Release12_00::transform},
    {"++Ares-Core+release-12.01", Release12_01::transform},
    {"++Ares-Core+release-12.02", Release12_02::transform},
    {"++Ares-Core+release-12.03", Release12_03::transform},
    {"++Ares-Core+release-12.04", Release12_04::transform},
    {"++Ares-Core+release-12.05", Release12_05::transform},
    {"++Ares-Core+release-12.06", Release12_06::transform},
    {"++Ares-Core+release-12.07", Release12_07::transform},
    {"++Ares-Core+release-12.08", Release12_08::transform},
    {"++Ares-Core+release-12.09", Release12_09::transform},
    {"++Ares-Core+release-12.10", Release12_10::transform},
    {"++Ares-Core+release-12.11", Release12_11::transform},
    {"++Ares-Core+release-13.00", Release13_00::transform},
    {"++Ares-Core+release-13.01", Release13_01::transform},
    {"++Ares-Core+release-13.02", Release13_02::transform},
    {"++Ares-Core+release-13.04", Release13_04::transform},
    {"++Ares-Core+release-13.05", Release13_05::transform},
    {"++Ares-Core+release-13.06", Release13_06::transform},
    {"++Ares-Core+release-13.08", Release13_08::transform},
    {"++Ares-Core+release-china-13.05", ReleaseChina13_05::transform},
};

}

PayloadTransform::PayloadTransform(const std::string_view branch) {
    const auto found = std::ranges::find(releases, branch, &Release::branch);
    if (found != std::end(releases)) apply = found->transform;
}

void PayloadTransform::decode(const std::span<std::uint8_t> payload, const std::int64_t bits, const std::uint32_t actorGuid) const {
    // The seed is the payload's size in bits with the NetGUID of the channel's actor.
    apply(payload.data(), static_cast<int>(bits), static_cast<std::uint32_t>(bits) ^ actorGuid);
}

}
