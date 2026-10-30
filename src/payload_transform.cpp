#include "payload_transform.h"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstring>
#include <iterator>
#include <limits>

namespace vrfparse {

namespace {

// Riot's obfuscation of the content block payloads, one per game release. The authors of ValorantReplayParser
// recovered each from that release's executable (the 13.08 one is FUN_144539860); the steps below are its
// ValorantSeededTransform classes. A transform turns the payload in place: the 64-bit words, then the 32-bit words,
// then the bytes, each with the state of a xorshift generator, and the last bits with the generator's byte.
//
// The game has the same step for the three widths. Only the keys that it takes from the state differ: a 64-bit word
// rotates the state right, a 32-bit word rotates it left, and a byte multiplies it by 11^n.

// A word of the payload, whose operators wrap at its width.
template <std::unsigned_integral T>
struct Word {
    static constexpr int bits = std::numeric_limits<T>::digits;
    T value;

    friend Word operator+(const Word a, const Word b) { return {static_cast<T>(a.value + b.value)}; }
    friend Word operator-(const Word a, const Word b) { return {static_cast<T>(a.value - b.value)}; }
    friend Word operator^(const Word a, const Word b) { return {static_cast<T>(a.value ^ b.value)}; }
    Word operator~() const { return {static_cast<T>(~value)}; }
};

template <class T>
Word<T> rotl(const Word<T> word, const int count) { return {std::rotl(word.value, count)}; }
template <class T>
Word<T> rotr(const Word<T> word, const int count) { return {std::rotr(word.value, count)}; }

// Swaps each group of the size with its neighbour.
template <class T>
Word<T> swapGroups(const Word<T> word, const int size) {
    const auto low = static_cast<T>(static_cast<T>(~T{}) / static_cast<T>((T{1} << size) + 1));
    return {static_cast<T>(((word.value & low) << size) | ((word.value >> size) & low))};
}

template <class T>
Word<T> swapAdjacentBits(const Word<T> word) { return swapGroups(word, 1); }

// The game's bit reversal: it swaps the groups of 1, 2, 4, and 8 bits that are smaller than half the word, then the
// halves. That reverses a byte and a 32-bit word; a 64-bit word keeps the order of the bits' 16-bit groups.
template <class T>
Word<T> reverse(Word<T> word) {
    for (const int size : {1, 2, 4, 8})
        if (size < Word<T>::bits / 2) word = swapGroups(word, size);
    return rotl(word, Word<T>::bits / 2);
}

constexpr std::uint8_t substitutionTable64[256] = {
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

constexpr std::uint8_t substitutionTable32[256] = {
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

constexpr std::uint8_t substitutionTable8[256] = {
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

// Substitutes each byte of the word with the table of its width.
template <class T>
Word<T> substitute(const Word<T> word) {
    constexpr const std::uint8_t* table = sizeof(T) == 8 ? substitutionTable64 : sizeof(T) == 4 ? substitutionTable32 : substitutionTable8;
    T result = 0;
    for (int shift = 0; shift < Word<T>::bits; shift += 8) result = static_cast<T>(result | (T{table[(word.value >> shift) & 0xFF]} << shift));
    return {result};
}

// The keys of a word's step, from the generator's state: key n is the state rotated by n, or multiplied by 11^n for a
// byte.
template <class T>
class Key {
public:
    explicit Key(const std::uint32_t state) : state(state) {}

    Word<T> add(const int n) const { return {static_cast<T>(source(n))}; }
    // A 64-bit word's mask has the high 32 bits set.
    Word<T> mask(const int n) const { return sizeof(T) == 8 ? ~add(n) : add(n); }
    // A rotation by 1 to the word's width - 1.
    int turns(const int n) const { return static_cast<int>(source(n) % (Word<T>::bits - 1)) + 1; }

private:
    static constexpr std::array<std::uint32_t, 9> powersOf11 = [] {
        std::array<std::uint32_t, 9> powers{1};
        for (std::size_t n = 1; n < powers.size(); ++n) powers[n] = powers[n - 1] * 11;
        return powers;
    }();

    std::uint32_t source(const int n) const {
        if constexpr (sizeof(T) == 8) return std::rotr(state, n);
        if constexpr (sizeof(T) == 4) return std::rotl(state, n);
        return state * powersOf11[static_cast<std::size_t>(n)];
    }

    std::uint32_t state;
};

constexpr std::uint64_t seedMultiplier = 0x2545f4914f6cdd1dULL;

// The start of a generator half: the seed's xorshift, times the multiplier.
std::uint64_t mixSeed(const std::uint32_t seed) { return std::uint64_t{(((seed >> 15) ^ seed) >> 12) ^ (seed << 25) ^ seed} * seedMultiplier; }

// The generator (xoroshiro128+): two 64-bit halves, whose sum's high half is the state of the next word.
class Generator {
public:
    Generator(const std::uint32_t seed, const std::uint32_t seedAddend) : a(mixSeed(seed + seedAddend)), b(mixSeed(seed)), state(seed) {}

    std::uint32_t getState() const { return state; }
    void next() {
        const std::uint64_t sum = a + b;
        b ^= a;
        a = std::rotr(a, 9) ^ (b << 14) ^ b;
        b = std::rotl(b, 36);
        state = static_cast<std::uint32_t>(sum >> 32);
    }

private:
    std::uint64_t a;
    std::uint64_t b;
    std::uint32_t state;
};

// A release: the seed addend of its generator, which also masks the last bits, and the step of its words.
template <class Release>
void decodeRelease(const std::span<std::uint8_t> payload, const std::int64_t bits, const std::uint32_t seed) {
    Generator generator(seed, Release::seedAddend);
    std::size_t offset = 0;
    std::int64_t bitsLeft = bits;
    const auto decodeWords = [&]<class T>(const T) {
        for (; bitsLeft >= Word<T>::bits; bitsLeft -= Word<T>::bits, offset += sizeof(T)) {
            Word<T> word;
            std::memcpy(&word.value, payload.data() + offset, sizeof(T));
            word = Release::step(word, Key<T>(generator.getState()));
            std::memcpy(payload.data() + offset, &word.value, sizeof(T));
            generator.next();
        }
    };
    decodeWords(std::uint64_t{});
    decodeWords(std::uint32_t{});
    decodeWords(std::uint8_t{});
    if (bitsLeft == 0) return;
    const auto lastBits = static_cast<std::uint8_t>((1u << bitsLeft) - 1);
    payload[offset] ^= static_cast<std::uint8_t>(lastBits & (generator.getState() ^ Release::seedAddend));
}

struct Release11_06 {
    static constexpr std::uint32_t seedAddend = 0x3325e3bd;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = substitute(v + k.add(8));
        v = substitute(substitute(v ^ k.mask(6)));
        return swapAdjacentBits(reverse(v));
    }
};

struct Release11_07 {
    static constexpr std::uint32_t seedAddend = 0x17b077d3;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = reverse(substitute(v ^ k.mask(8)));
        return swapAdjacentBits(~v) ^ k.mask(1);
    }
};

struct Release11_08 {
    static constexpr std::uint32_t seedAddend = 0xacf2cdff;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = rotl(rotl(rotr(~v, k.turns(7)), k.turns(6)), k.turns(5));
        return swapAdjacentBits(v - k.add(2));
    }
};

struct Release11_09 {
    static constexpr std::uint32_t seedAddend = 0x12cf14e5;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = swapAdjacentBits(substitute(v) ^ k.mask(7) ^ k.mask(6));
        return swapAdjacentBits(rotl(v, k.turns(3))) ^ k.mask(1);
    }
};

struct Release11_10 {
    static constexpr std::uint32_t seedAddend = 0x34e9d3ec;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = ((v - k.add(8)) ^ k.mask(7)) - k.add(6);
        v = rotl(v, k.turns(5)) - k.add(4);
        return ~substitute(reverse(v));
    }
};

struct Release11_11 {
    static constexpr std::uint32_t seedAddend = 0xc4445c41;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = swapAdjacentBits(rotl(substitute(v ^ k.mask(8)), k.turns(6)));
        return rotl(substitute(v), k.turns(1));
    }
};

struct Release12_00 {
    static constexpr std::uint32_t seedAddend = 0x70876679;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = reverse(rotr(v, k.turns(8)));
        return ~(((v ^ k.mask(6)) - k.add(5)) ^ k.mask(4) ^ k.mask(3));
    }
};

struct Release12_01 {
    static constexpr std::uint32_t seedAddend = 0x13fdd831;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = (substitute(v) ^ k.mask(7)) - k.add(6);
        v = substitute(rotl(swapAdjacentBits(v), k.turns(4)));
        return (v ^ k.mask(2)) + k.add(1);
    }
};

struct Release12_02 {
    static constexpr std::uint32_t seedAddend = 0x9830d09d;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = reverse(rotr(rotl(v, k.turns(8)), k.turns(7)));
        v = reverse(substitute(swapAdjacentBits(v)));
        return reverse(v - k.add(2));
    }
};

struct Release12_03 {
    static constexpr std::uint32_t seedAddend = 0x33d59dff;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = swapAdjacentBits(v) - k.add(7) + k.add(6);
        v = substitute(rotl(rotl(v, k.turns(5)), k.turns(4)));
        return rotl(v, k.turns(2)) + k.add(1);
    }
};

struct Release12_04 {
    static constexpr std::uint32_t seedAddend = 0xa5684b42;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = rotr(rotl(rotr(rotr(v, k.turns(8)), k.turns(7)), k.turns(6)), k.turns(5));
        return rotl(substitute(v + k.add(4)), k.turns(1));
    }
};

struct Release12_05 {
    static constexpr std::uint32_t seedAddend = 0xc21d548c;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = reverse(~rotr(v, k.turns(8))) - k.add(4);
        return ~substitute(rotr(v, k.turns(3)));
    }
};

struct Release12_06 {
    static constexpr std::uint32_t seedAddend = 0x8d686ca6;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = reverse(rotr(v ^ k.mask(8), k.turns(7)) - k.add(6));
        return reverse(rotl(v, k.turns(3))) - k.add(1);
    }
};

struct Release12_07 {
    static constexpr std::uint32_t seedAddend = 0x2d21d7c3;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = ((v ^ k.mask(8)) + k.add(7)) ^ k.mask(6);
        return substitute(swapAdjacentBits(v)) - k.add(3);
    }
};

struct Release12_08 {
    static constexpr std::uint32_t seedAddend = 0xce2e33e5;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = reverse(substitute(rotl(v, k.turns(7))) ^ k.mask(4));
        return rotr(v + k.add(2), k.turns(1));
    }
};

struct Release12_09 {
    static constexpr std::uint32_t seedAddend = 0x7ff2feec;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = rotl(rotl(rotl(v, k.turns(8)) ^ k.mask(7), k.turns(6)), k.turns(5));
        return rotl(rotl(~v ^ k.mask(3), k.turns(2)), k.turns(1));
    }
};

struct Release12_10 {
    static constexpr std::uint32_t seedAddend = 0x12fd0ee5;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = swapAdjacentBits(rotr(v, k.turns(8))) - k.add(6);
        return swapAdjacentBits(rotr(v, k.turns(5)) ^ k.mask(4));
    }
};

struct Release12_11 {
    static constexpr std::uint32_t seedAddend = 0x409d36a3;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = reverse(swapAdjacentBits(rotr(v, k.turns(8))) + k.add(6));
        return swapAdjacentBits(v - k.add(4) - k.add(3) - k.add(2));
    }
};

struct Release13_00 {
    static constexpr std::uint32_t seedAddend = 0x2949b6ef;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = ~((reverse(v + k.add(8)) + k.add(6)) ^ k.mask(3));
        return rotr(substitute(v), k.turns(1));
    }
};

struct Release13_01 {
    static constexpr std::uint32_t seedAddend = 0xe62fcd5c;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = swapAdjacentBits(~v) ^ k.mask(5);
        return ~rotr(v, k.turns(4)) + k.add(1);
    }
};

struct Release13_02 {
    static constexpr std::uint32_t seedAddend = 0x9e81a37c;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = reverse(~(reverse(substitute(v)) - k.add(6)));
        return rotr(rotl(v, k.turns(3)), k.turns(2));
    }
};

struct Release13_04 {
    static constexpr std::uint32_t seedAddend = 0x076dc658;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = (rotr(v, k.turns(7)) ^ k.mask(6)) - k.add(5);
        return rotl(swapAdjacentBits(v) + k.add(3) + k.add(2), k.turns(1));
    }
};

struct Release13_05 {
    static constexpr std::uint32_t seedAddend = 0x48c26613;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = rotl((v ^ k.mask(8)) - k.add(7), k.turns(6));
        v = (v - k.add(5)) ^ k.mask(4) ^ k.mask(3);
        return rotl(rotl(v, k.turns(2)), k.turns(1));
    }
};

struct Release13_06 {
    static constexpr std::uint32_t seedAddend = 0xe974593c;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = reverse(swapAdjacentBits(swapAdjacentBits(v) ^ k.mask(7)));
        return substitute(~substitute(v) + k.add(2));
    }
};

struct Release13_08 {
    static constexpr std::uint32_t seedAddend = 0x3c17a9fa;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = rotl(reverse(v + k.add(8)), k.turns(6));
        v = rotr(substitute(v), k.turns(4));
        return reverse(substitute(substitute(v)));
    }
};

struct ReleaseChina13_05 {
    static constexpr std::uint32_t seedAddend = 0xf67761c9;
    template <class T>
    static Word<T> step(Word<T> v, const Key<T> k) {
        v = rotr(rotl(v + k.add(8), k.turns(6)), k.turns(7));
        return rotr(rotl(v, k.turns(4)), k.turns(2)) - k.add(1);
    }
};

struct Release {
    std::string_view branch;
    PayloadTransform::Decode* decode;
};

constexpr Release releases[] = {
    {"++Ares-Core+release-11.06", decodeRelease<Release11_06>},
    {"++Ares-Core+release-11.07", decodeRelease<Release11_07>},
    {"++Ares-Core+release-11.08", decodeRelease<Release11_08>},
    {"++Ares-Core+release-11.09", decodeRelease<Release11_09>},
    {"++Ares-Core+release-11.10", decodeRelease<Release11_10>},
    {"++Ares-Core+release-11.11", decodeRelease<Release11_11>},
    {"++Ares-Core+release-12.00", decodeRelease<Release12_00>},
    {"++Ares-Core+release-12.01", decodeRelease<Release12_01>},
    {"++Ares-Core+release-12.02", decodeRelease<Release12_02>},
    {"++Ares-Core+release-12.03", decodeRelease<Release12_03>},
    {"++Ares-Core+release-12.04", decodeRelease<Release12_04>},
    {"++Ares-Core+release-12.05", decodeRelease<Release12_05>},
    {"++Ares-Core+release-12.06", decodeRelease<Release12_06>},
    {"++Ares-Core+release-12.07", decodeRelease<Release12_07>},
    {"++Ares-Core+release-12.08", decodeRelease<Release12_08>},
    {"++Ares-Core+release-12.09", decodeRelease<Release12_09>},
    {"++Ares-Core+release-12.10", decodeRelease<Release12_10>},
    {"++Ares-Core+release-12.11", decodeRelease<Release12_11>},
    {"++Ares-Core+release-13.00", decodeRelease<Release13_00>},
    {"++Ares-Core+release-13.01", decodeRelease<Release13_01>},
    {"++Ares-Core+release-13.02", decodeRelease<Release13_02>},
    {"++Ares-Core+release-13.04", decodeRelease<Release13_04>},
    {"++Ares-Core+release-13.05", decodeRelease<Release13_05>},
    {"++Ares-Core+release-13.06", decodeRelease<Release13_06>},
    {"++Ares-Core+release-13.08", decodeRelease<Release13_08>},
    {"++Ares-Core+release-china-13.05", decodeRelease<ReleaseChina13_05>},
};

}

PayloadTransform::PayloadTransform(const std::string_view branch) {
    const auto found = std::ranges::find(releases, branch, &Release::branch);
    if (found != std::end(releases)) apply = found->decode;
}

void PayloadTransform::decode(const std::span<std::uint8_t> payload, const std::int64_t bits, const std::uint32_t actorGuid) const {
    // The seed is the payload's size in bits with the NetGUID of the channel's actor.
    apply(payload, bits, static_cast<std::uint32_t>(bits) ^ actorGuid);
}

}
