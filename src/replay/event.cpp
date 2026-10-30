#include "replay/event.h"

#include <algorithm>

namespace replay {

namespace {

// The payloads go into blocks of this size; a larger payload gets its own block.
constexpr std::size_t payloadBlockSize = std::size_t{1} << 20;

}

std::string_view EventLog::intern(const std::string_view text) {
    if (text.empty()) return {};
    return *strings.emplace(text).first;
}

std::span<const std::string_view> EventLog::addNames(std::vector<std::string_view> names) {
    return nameLists.emplace_back(std::move(names));
}

std::span<std::uint8_t> EventLog::allocatePayload(const std::int64_t bits) {
    const auto size = static_cast<std::size_t>((bits + 7) / 8);
    if (size == 0) return {};
    if (blocks.empty() || blockUsed + size > blockSize) {
        blockSize = std::max(payloadBlockSize, size);
        blocks.push_back(std::make_unique<std::uint8_t[]>(blockSize));
        blockUsed = 0;
    }
    const std::span<std::uint8_t> payload(blocks.back().get() + blockUsed, size);
    blockUsed += size;
    return payload;
}

}
