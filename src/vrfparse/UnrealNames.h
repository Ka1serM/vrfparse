#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vrfparse {

std::optional<std::string_view> findHardcodedName(std::uint32_t index);

}
