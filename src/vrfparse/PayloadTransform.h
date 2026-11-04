#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace vrfparse {

class PayloadTransform {
public:
    using Decode = void(std::span<std::uint8_t> payload, std::int64_t bits, std::uint32_t seed);

    explicit PayloadTransform(std::string_view branch);

    bool isKnown() const { return apply != nullptr; }
    void decode(std::span<std::uint8_t> payload, std::int64_t bits, std::uint32_t actorGuid) const;

private:
    Decode* apply = nullptr;
};

}
