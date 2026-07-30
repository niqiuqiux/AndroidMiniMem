#pragma once

#include <optional>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace Mem {

enum class ErrorCode {
    InvalidArgument,
    CancelRequested,
    Timeout,
    ConnectionChanged,
    ConnectionPoisoned,
    NotConnected,
    NoTarget,
    TargetChanged,
    ProtocolError,
    PermissionDenied,
    PartialWrite,
    CompletionUnknown,
    SymbolSessionChanged,
    InternalError,
};

inline const char* errorCodeName(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::CancelRequested: return "cancel_requested";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::ConnectionChanged: return "connection_changed";
    case ErrorCode::ConnectionPoisoned: return "connection_poisoned";
    case ErrorCode::NotConnected: return "not_connected";
    case ErrorCode::NoTarget: return "no_target";
    case ErrorCode::TargetChanged: return "target_changed";
    case ErrorCode::ProtocolError: return "protocol_error";
    case ErrorCode::PermissionDenied: return "permission_denied";
    case ErrorCode::PartialWrite: return "partial_write";
    case ErrorCode::CompletionUnknown: return "completion_unknown";
    case ErrorCode::SymbolSessionChanged: return "symbol_session_changed";
    case ErrorCode::InternalError: return "internal_error";
    }
    return "internal_error";
}

struct Error {
    ErrorCode code = ErrorCode::InternalError;
    std::string message;
    bool retryable = false;
    std::optional<uint64_t> affectedBytes;
    std::optional<int32_t> nativeCode;
};

template<typename T>
class Result {
public:
    static Result success(T value) {
        Result result;
        result.value_.emplace(std::move(value));
        return result;
    }

    static Result failure(ErrorCode code,
                          std::string message,
                          bool retryable = false,
                          std::optional<uint64_t> affectedBytes = std::nullopt,
                          std::optional<int32_t> nativeCode = std::nullopt) {
        Result result;
        result.error_.emplace(
            Error{code, std::move(message), retryable, affectedBytes,
                  nativeCode});
        return result;
    }

    bool ok() const { return value_.has_value(); }

    const T& value() const {
        if (!value_) {
            throw std::logic_error("Mem::Result has no value");
        }
        return *value_;
    }

    T& value() {
        if (!value_) {
            throw std::logic_error("Mem::Result has no value");
        }
        return *value_;
    }

    const Error& error() const {
        if (!error_) {
            throw std::logic_error("Mem::Result has no error");
        }
        return *error_;
    }

private:
    std::optional<T> value_;
    std::optional<Error> error_;
};

} // namespace Mem
