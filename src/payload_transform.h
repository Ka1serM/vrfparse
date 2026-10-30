#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace vrfparse {

// Valorant's obfuscation of the content block payloads: each game release has its own.
class PayloadTransform {
public:
    using Decode = void(std::span<std::uint8_t> payload, std::int64_t bits, std::uint32_t seed);

    // The transform of the release that the replay header names, such as "++Ares-Core+release-13.08".
    explicit PayloadTransform(std::string_view branch);

    // False for a release whose transform is not known: its payloads cannot be read.
    bool isKnown() const { return apply != nullptr; }
    // Decodes the payload of a content block of the actor's channel in place. The bits after the payload's bit count
    // stay as they are.
    void decode(std::span<std::uint8_t> payload, std::int64_t bits, std::uint32_t actorGuid) const;

private:
    Decode* apply = nullptr;
};

}
