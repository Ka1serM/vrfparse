#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vrfparse {

// The name of a hardcoded FName (EName), as UPackageMap::StaticSerializeName writes it by index. Empty for an index
// that Unreal does not register.
std::optional<std::string_view> findHardcodedName(std::uint32_t index);

}
