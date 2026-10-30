#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vrfparse {

// The step at which a replay cannot be read.
enum class ErrorCode : std::uint8_t {
    // The file cannot be opened or read.
    Open,
    // The replay's data is damaged.
    Parse,
    // The replay's format or game release is not supported.
    Version,
    Encrypted,
    // The caller requested the stop.
    Cancelled,
};

constexpr std::string_view getErrorName(const ErrorCode code) {
    switch (code) {
    case ErrorCode::Open: return "replay.open";
    case ErrorCode::Parse: return "replay.parse";
    case ErrorCode::Version: return "replay.version";
    case ErrorCode::Encrypted: return "replay.encrypted";
    case ErrorCode::Cancelled: return "cancelled";
    }
    return {};
}

// A replay that cannot be read: the step, and a message for the user.
struct Error {
    ErrorCode code = ErrorCode::Parse;
    std::string message;
};

}
