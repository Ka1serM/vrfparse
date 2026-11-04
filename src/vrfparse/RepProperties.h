#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

#include "vrfparse/BitReader.h"

namespace vrfparse {


template <class Visit>
bool readProperties(BitReader& stream, const std::span<const std::string_view> names, Visit&& visit) {
    while (!stream.atEnd()) {
        const std::uint32_t handle = stream.serializeIntPacked();
        if (stream.isError())
            return false;
        if (handle == 0)
            return true;
        const std::uint32_t bits = stream.serializeIntPacked();
        if (stream.isError() || bits > stream.getBitsLeft())
            return false;
        BitReader payload = stream.readSubReader(bits);
        visit(handle - 1 < names.size() ? names[handle - 1] : std::string_view(), payload);
    }
    return true;
}

template <class Visit>
bool readParameters(BitReader& stream, const std::span<const std::string_view> names, Visit&& visit) {
    stream.readBit();
    return readProperties(stream, names, visit);
}

template <class Visit>
bool readArray(BitReader& payload, Visit&& visitElement) {
    payload.serializeIntPacked();
    while (true) {
        const std::uint32_t index = payload.serializeIntPacked();
        if (payload.isError())
            return false;
        if (index == 0)
            return true;
        if (!visitElement(index - 1, payload))
            return false;
    }
}

inline std::uint32_t readObject(BitReader& payload) {
    const std::uint32_t guid = payload.serializeIntPacked();
    if (guid != 1)
        return guid;
    const std::uint8_t flags = payload.readByte();
    if ((flags & 1) == 0)
        return guid;
    readObject(payload);
    payload.readString();
    if (flags & 4)
        payload.readUInt32();
    return guid;
}

inline std::uint32_t readEnum(BitReader& payload) {
    return static_cast<std::uint32_t>(payload.readBits(static_cast<int>(std::min<std::int64_t>(payload.getBitsLeft(), 32))));
}

}
