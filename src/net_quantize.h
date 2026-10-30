#pragma once

#include <cstdint>

#include "vrfparse/bit_reader.h"
#include "vrfparse/event.h"

namespace vrfparse {

// FVector's NetSerialize: three doubles.
inline Vector readVector(BitReader& reader) {
    const auto component = [&] { return static_cast<float>(reader.readDouble()); };
    const float x = component();
    const float y = component();
    return {x, y, component()};
}

// UE::Net::ReadQuantizedVector, of FVector_NetQuantize10 and the other quantized vectors: 6 bits of component size and
// a bit that says whether the components are scaled, then the components. Without a size, the components are floats,
// or doubles when scaled.
inline Vector readQuantizedVector(BitReader& reader, const double scale) {
    const auto info = static_cast<std::uint32_t>(reader.readBits(7));
    const int componentBits = static_cast<int>(info & 63);
    const bool scaled = (info >> 6) != 0;
    if (componentBits == 0) {
        if (scaled)
            return readVector(reader);
        const float x = reader.readFloat();
        const float y = reader.readFloat();
        return {x, y, reader.readFloat()};
    }
    // Each component is offset by its sign bit.
    const std::uint64_t bias = std::uint64_t{1} << (componentBits - 1);
    const auto component = [&] {
        const auto value = static_cast<double>(static_cast<std::int64_t>(reader.readBits(componentBits) ^ bias) - static_cast<std::int64_t>(bias));
        return static_cast<float>(scaled ? value / scale : value);
    };
    const float x = component();
    const float y = component();
    return {x, y, component()};
}

// FRotator::DecompressAxisFromShort: degrees.
inline float decompressAxisFromShort(const std::uint16_t angle) { return static_cast<float>(angle * (360.0 / 65536.0)); }

}
