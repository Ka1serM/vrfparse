// The payload transform of each game release against the output of ValorantReplayParser's transforms: payloads of 0 to
// 200 bits, which reach every word width and every count of last bits.
#include <cstdint>
#include <print>
#include <string>
#include <vector>

#include "payload_transform.h"

namespace {

struct Expected {
    const char* release;
    std::uint64_t hash;
};

constexpr Expected releases[] = {
    {"11.06", 0x59b792203a79aa23ULL},
    {"11.07", 0x89c4d9e5e72e2e8dULL},
    {"11.08", 0x068eb43bf2f9e8bfULL},
    {"11.09", 0x13ba3f8f0b3e3a2fULL},
    {"11.10", 0xeb2d06c3949e56deULL},
    {"11.11", 0xd27ffbcd4da7a714ULL},
    {"12.00", 0x108bb0b52d7ecfefULL},
    {"12.01", 0x10de105e8cf06ffcULL},
    {"12.02", 0xa78846c7fd138b8fULL},
    {"12.03", 0xc06c36bcda16d7ebULL},
    {"12.04", 0x7f4b77e131c35f79ULL},
    {"12.05", 0xd5ef74ac69020d49ULL},
    {"12.06", 0x6fc12d758d3a745dULL},
    {"12.07", 0x16d33f8741da969fULL},
    {"12.08", 0xddaf50b84e3fff6dULL},
    {"12.09", 0xda012a2163ac5908ULL},
    {"12.10", 0xfb4a82b3b2519844ULL},
    {"12.11", 0x1a80b9da2aa33aa0ULL},
    {"13.00", 0x079fbc3331935409ULL},
    {"13.01", 0x2797e862a4163172ULL},
    {"13.02", 0x26734bbca764dc2aULL},
    {"13.04", 0x28a65420e7466520ULL},
    {"13.05", 0x42c97c1cc7ac312eULL},
    {"13.06", 0xbd1aaaa7b814081cULL},
    {"13.08", 0x8d6898a89089d783ULL},
    {"13.09", 0xee76ea9baf54d318ULL},
    {"china-13.05", 0x28c74dbab115021aULL},
};

// FNV-1a.
std::uint64_t hashOf(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : bytes)
        hash = (hash ^ byte) * 0x100000001b3ULL;
    return hash;
}

}

int main() {
    int failures = 0;
    for (const Expected& expected : releases) {
        const vrfparse::PayloadTransform transform(std::string("++Ares-Core+release-") + expected.release);
        std::vector<std::uint8_t> decoded;
        for (std::int64_t bits = 0; bits <= 200; ++bits) {
            std::vector<std::uint8_t> payload(static_cast<std::size_t>((bits + 7) / 8));
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<std::uint8_t>(i * 151 + static_cast<std::size_t>(bits) * 7 + 3);
            if (transform.isKnown())
                transform.decode(payload, bits, 0x2468ACE1u);
            decoded.insert(decoded.end(), payload.begin(), payload.end());
        }
        if (transform.isKnown() && hashOf(decoded) == expected.hash)
            continue;
        std::println(stderr, "The payload transform of release {} does not match.", expected.release);
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
