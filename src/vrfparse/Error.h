#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vrfparse {

enum class ErrorCode : std::uint8_t {
    Open,
    Parse,
    Version,
    Encrypted,
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

struct Error {
    ErrorCode code = ErrorCode::Parse;
    std::string message;
};

}
