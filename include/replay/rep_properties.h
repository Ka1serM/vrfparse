#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

#include "net/bit_reader.h"

namespace replay {

// The property streams of a replay, which name each property by its export in an export group
// (FRepLayout::ReceiveProperties_BackwardsCompatible_r). The visitors get the export name and the property's payload;
// a handle that the group does not name has an empty name. They return false when the stream is damaged.

// The properties until the 0 handle: each by its handle + 1, with the payload's bit count.
template <class Visit>
bool readProperties(net::BitReader& stream, const std::span<const std::string_view> names, Visit&& visit) {
    while (!stream.atEnd()) {
        const std::uint32_t handle = stream.serializeIntPacked();
        if (stream.isError()) return false;
        if (handle == 0) return true;
        const std::uint32_t bits = stream.serializeIntPacked();
        if (stream.isError() || bits > stream.getBitsLeft()) return false;
        net::BitReader payload = stream.readSubReader(bits);
        visit(handle - 1 < names.size() ? names[handle - 1] : std::string_view(), payload);
    }
    return true;
}

// The payload of an RPC: the checksum bit, then its parameters.
template <class Visit>
bool readParameters(net::BitReader& stream, const std::span<const std::string_view> names, Visit&& visit) {
    stream.readBit();
    return readProperties(stream, names, visit);
}

// The payload of a dynamic array property: the element count, then each changed element by its index + 1, with its
// properties, until 0. The visitor gets the index and reads the element's properties from the stream.
template <class Visit>
bool readArray(net::BitReader& payload, Visit&& visitElement) {
    payload.serializeIntPacked();
    while (true) {
        const std::uint32_t index = payload.serializeIntPacked();
        if (payload.isError()) return false;
        if (index == 0) return true;
        if (!visitElement(index - 1, payload)) return false;
    }
}

// FObjectPropertyBase::NetSerializeItem: the NetGUID. A default NetGUID (1) brings its export, which is skipped.
inline std::uint32_t readObject(net::BitReader& payload) {
    const std::uint32_t guid = payload.serializeIntPacked();
    if (guid != 1) return guid;
    // EGuidPackageMapExportFlags::bHasPath: the outer, the path, and with bHasNetworkChecksum the checksum.
    const std::uint8_t flags = payload.readByte();
    if ((flags & 1) == 0) return guid;
    readObject(payload);
    payload.readString();
    if (flags & 4) payload.readUInt32();
    return guid;
}

// An enum property: its value in the payload's bits (FByteProperty::NetSerializeItem with GetMaxNetSerializeBits).
inline std::uint32_t readEnum(net::BitReader& payload) {
    return static_cast<std::uint32_t>(payload.readBits(static_cast<int>(std::min<std::int64_t>(payload.getBitsLeft(), 32))));
}

}
