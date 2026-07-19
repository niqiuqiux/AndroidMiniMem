#include "IpcHttpRequest.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace IpcHttp {
namespace {

struct RequestLine {
    std::string method;
    std::string target;
};

using HeaderMap = std::unordered_map<std::string, std::string>;

ValidationResult reject(int statusCode, std::string error) {
    ValidationResult result;
    result.statusCode = statusCode;
    result.error = std::move(error);
    return result;
}

bool isTokenCharacter(unsigned char value) {
    return std::isalnum(value) != 0 ||
           std::string_view("!#$%&'*+-.^_`|~").find(
               static_cast<char>(value)) != std::string_view::npos;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

std::string trimOptionalWhitespace(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool parseRequestLine(const std::string& line, RequestLine& output) {
    const size_t methodEnd = line.find(' ');
    if (methodEnd == std::string::npos || methodEnd == 0)
        return false;
    const size_t targetEnd = line.find(' ', methodEnd + 1);
    if (targetEnd == std::string::npos || targetEnd == methodEnd + 1)
        return false;
    if (line.find(' ', targetEnd + 1) != std::string::npos)
        return false;

    const std::string version = line.substr(targetEnd + 1);
    if (version != "HTTP/1.0" && version != "HTTP/1.1")
        return false;

    output.method = line.substr(0, methodEnd);
    output.target = line.substr(methodEnd + 1,
                                targetEnd - methodEnd - 1);
    return true;
}

bool parseHeaders(const std::string& input,
                  size_t firstLineEnd,
                  HeaderMap& output,
                  std::string& error) {
    size_t lineStart = firstLineEnd + 2;
    while (lineStart < input.size()) {
        size_t lineEnd = input.find("\r\n", lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = input.size();
        if (lineEnd == lineStart) {
            error = "Unexpected empty HTTP header";
            return false;
        }
        if (input[lineStart] == ' ' || input[lineStart] == '\t') {
            error = "Folded HTTP headers are not supported";
            return false;
        }

        const size_t colon = input.find(':', lineStart);
        if (colon == std::string::npos || colon >= lineEnd ||
            colon == lineStart) {
            error = "Malformed HTTP header";
            return false;
        }
        for (size_t i = lineStart; i < colon; ++i) {
            if (!isTokenCharacter(static_cast<unsigned char>(input[i]))) {
                error = "Invalid HTTP header name";
                return false;
            }
        }
        for (size_t i = colon + 1; i < lineEnd; ++i) {
            const unsigned char ch = static_cast<unsigned char>(input[i]);
            if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
                error = "Invalid HTTP header value";
                return false;
            }
        }

        std::string name = lowerAscii(
            input.substr(lineStart, colon - lineStart));
        std::string value = trimOptionalWhitespace(
            input.substr(colon + 1, lineEnd - colon - 1));
        if (!output.emplace(std::move(name), std::move(value)).second) {
            error = "Duplicate HTTP header";
            return false;
        }

        if (lineEnd == input.size())
            break;
        lineStart = lineEnd + 2;
    }
    return true;
}

bool parseContentLength(const std::string& value, size_t& output) {
    if (value.empty())
        return false;
    size_t parsed = 0;
    for (unsigned char ch : value) {
        if (std::isdigit(ch) == 0)
            return false;
        const size_t digit = static_cast<size_t>(ch - '0');
        if (parsed > ((std::numeric_limits<size_t>::max)() - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }
    output = parsed;
    return true;
}

bool equalsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

bool isJsonContentType(const std::string& value) {
    const size_t separator = value.find(';');
    const std::string mediaType = trimOptionalWhitespace(
        value.substr(0, separator));
    return equalsIgnoreCase(mediaType, "application/json");
}

std::string pathWithoutQuery(const std::string& target) {
    const size_t query = target.find('?');
    return query == std::string::npos ? target : target.substr(0, query);
}

} // namespace

ValidationResult ValidateRequestHead(const std::string& headers,
                                     size_t maxRequestBytes) {
    if (headers.size() > maxRequestBytes || headers.size() + 4u < headers.size())
        return reject(413, "HTTP request too large");

    const size_t requestLineEnd = headers.find("\r\n");
    if (requestLineEnd == std::string::npos)
        return reject(400, "Invalid HTTP request line");

    RequestLine requestLine;
    if (!parseRequestLine(headers.substr(0, requestLineEnd), requestLine))
        return reject(400, "Invalid HTTP request line");
    if (pathWithoutQuery(requestLine.target) != "/")
        return reject(404, "Unknown IPC endpoint");
    if (requestLine.method != "POST")
        return reject(405, "Unsupported HTTP method");

    HeaderMap parsedHeaders;
    std::string headerError;
    if (!parseHeaders(headers, requestLineEnd,
                      parsedHeaders, headerError)) {
        return reject(400, std::move(headerError));
    }

    if (parsedHeaders.find("transfer-encoding") != parsedHeaders.end())
        return reject(400, "Transfer-Encoding is not supported");

    const auto contentLength = parsedHeaders.find("content-length");
    if (contentLength == parsedHeaders.end())
        return reject(400, "Missing Content-Length");

    size_t bodyLength = 0;
    if (!parseContentLength(contentLength->second, bodyLength))
        return reject(400, "Invalid Content-Length");

    const auto contentType = parsedHeaders.find("content-type");
    if (contentType == parsedHeaders.end() ||
        !isJsonContentType(contentType->second)) {
        return reject(415, "Content-Type must be application/json");
    }

    const auto origin = parsedHeaders.find("origin");
    if (origin != parsedHeaders.end() && !origin->second.empty()) {
        return reject(403,
                      "Browser-originated IPC requests are not allowed");
    }

    const size_t framedHeaderBytes = headers.size() + 4u;
    if (bodyLength > maxRequestBytes ||
        framedHeaderBytes > maxRequestBytes - bodyLength) {
        return reject(413, "HTTP request too large");
    }

    ValidationResult result;
    result.request.contentLength = bodyLength;
    return result;
}

} // namespace IpcHttp
