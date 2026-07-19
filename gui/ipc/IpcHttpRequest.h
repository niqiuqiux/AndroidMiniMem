#pragma once

#include <cstddef>
#include <string>

namespace IpcHttp {

constexpr size_t kMaxRequestBytes = 1024u * 1024u;

struct RequestHead {
    size_t contentLength = 0;
};

struct ValidationResult {
    int statusCode = 0;
    std::string error;
    RequestHead request;

    bool accepted() const { return statusCode == 0; }
};

// headers 不包含结尾的 CRLFCRLF。
ValidationResult ValidateRequestHead(
    const std::string& headers,
    size_t maxRequestBytes = kMaxRequestBytes);

} // namespace IpcHttp
