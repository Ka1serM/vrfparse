#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace replay {

// A replay that cannot be read. The code names the step, such as "replay.open", "replay.parse", or "cancelled".
class Error : public std::runtime_error {
public:
    Error(std::string code, const std::string& message) : std::runtime_error(message), code(std::move(code)) {}
    const std::string& getCode() const noexcept { return code; }

private:
    std::string code;
};

}
