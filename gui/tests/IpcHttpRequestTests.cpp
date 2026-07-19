#include "../ipc/IpcHttpRequest.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::string request(const std::string& method,
                    const std::string& extraHeaders,
                    const std::string& contentLength = "2") {
    return method + " / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: " + contentLength + "\r\n" + extraHeaders;
}

} // namespace

int main() {
    const auto valid = IpcHttp::ValidateRequestHead(request("POST", ""));
    check(valid.accepted() && valid.request.contentLength == 2,
          "normal JSON POST should be accepted");

    const auto missingType = IpcHttp::ValidateRequestHead(
        "POST / HTTP/1.1\r\nContent-Length: 2");
    check(!missingType.accepted() && missingType.statusCode == 415,
          "missing Content-Type should be rejected");

    const auto wrongType = IpcHttp::ValidateRequestHead(
        "POST / HTTP/1.1\r\nContent-Type: text/plain\r\n"
        "Content-Length: 2");
    check(!wrongType.accepted() && wrongType.statusCode == 415,
          "non-JSON Content-Type should be rejected");

    const auto browserOrigin = IpcHttp::ValidateRequestHead(
        request("POST", "Origin: https://example.test"));
    check(!browserOrigin.accepted() && browserOrigin.statusCode == 403,
          "non-empty Origin should be rejected");

    const auto emptyOrigin = IpcHttp::ValidateRequestHead(
        request("POST", "Origin:\r\n"));
    check(emptyOrigin.accepted(), "empty Origin should remain allowed");

    const auto options = IpcHttp::ValidateRequestHead(
        request("OPTIONS", ""));
    check(!options.accepted() && options.statusCode == 405,
          "OPTIONS should not expose a browser preflight path");

    const auto duplicateLength = IpcHttp::ValidateRequestHead(
        request("POST", "Content-Length: 2\r\n"));
    check(!duplicateLength.accepted() &&
              duplicateLength.statusCode == 400,
          "duplicate Content-Length should be rejected");

    const auto duplicateOrigin = IpcHttp::ValidateRequestHead(
        request("POST", "Origin:\r\nOrigin:\r\n"));
    check(!duplicateOrigin.accepted() && duplicateOrigin.statusCode == 400,
          "duplicate Origin should be rejected");

    const auto transferEncoding = IpcHttp::ValidateRequestHead(
        request("POST", "Transfer-Encoding: chunked\r\n"));
    check(!transferEncoding.accepted() &&
              transferEncoding.statusCode == 400,
          "Transfer-Encoding should be rejected to avoid framing ambiguity");

    const auto oversized = IpcHttp::ValidateRequestHead(
        request("POST", "", std::to_string(IpcHttp::kMaxRequestBytes)));
    check(!oversized.accepted() && oversized.statusCode == 413,
          "body that pushes the framed request over the limit should fail");

    const auto missingLength = IpcHttp::ValidateRequestHead(
        "POST / HTTP/1.1\r\nContent-Type: application/json");
    check(!missingLength.accepted() && missingLength.statusCode == 400,
          "missing Content-Length should be rejected");

    const auto malformedHeader = IpcHttp::ValidateRequestHead(
        request("POST", " malformed-fold\r\n"));
    check(!malformedHeader.accepted() && malformedHeader.statusCode == 400,
          "folded or malformed headers should be rejected");

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "IPC HTTP request tests passed\n";
    return 0;
}
