#include "ValueFormatter.h"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <limits>

namespace ValueFormatter {

namespace {
bool parseUnsignedStrict(const std::string& input, int base, uint64_t maxValue, uint64_t& value) {
    size_t begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos || input[begin] == '-') {
        return false;
    }

    const char* str = input.c_str() + begin;
    char* end = nullptr;
    errno = 0;
    value = std::strtoull(str, &end, base);
    if (end == str || errno == ERANGE || value > maxValue) {
        return false;
    }

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    return *end == '\0';
}

bool parseFloatStrict(const std::string& input, float& value) {
    const char* str = input.c_str();
    char* end = nullptr;
    errno = 0;
    value = std::strtof(str, &end);
    if (end == str || errno == ERANGE) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    return *end == '\0';
}

bool parseDoubleStrict(const std::string& input, double& value) {
    const char* str = input.c_str();
    char* end = nullptr;
    errno = 0;
    value = std::strtod(str, &end);
    if (end == str || errno == ERANGE) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    return *end == '\0';
}
}

std::vector<unsigned char> parseInput(const std::string& input, int valueType, bool hexInput) {
    std::vector<unsigned char> result;
    try {
        switch (valueType) {
            case Byte1: {
                uint64_t parsed = 0;
                if (!parseUnsignedStrict(input, hexInput ? 16 : 10, 0xFF, parsed)) break;
                uint8_t v = static_cast<uint8_t>(parsed);
                result.resize(1);
                result[0] = v;
                break;
            }
            case Byte2: {
                uint64_t parsed = 0;
                if (!parseUnsignedStrict(input, hexInput ? 16 : 10, 0xFFFF, parsed)) break;
                uint16_t v = static_cast<uint16_t>(parsed);
                result.resize(2);
                std::memcpy(result.data(), &v, 2);
                break;
            }
            case Byte4: {
                uint64_t parsed = 0;
                if (!parseUnsignedStrict(input, hexInput ? 16 : 10, 0xFFFFFFFFULL, parsed)) break;
                uint32_t v = static_cast<uint32_t>(parsed);
                result.resize(4);
                std::memcpy(result.data(), &v, 4);
                break;
            }
            case Byte8: {
                uint64_t v = 0;
                if (!parseUnsignedStrict(input, hexInput ? 16 : 10, UINT64_MAX, v)) break;
                result.resize(8);
                std::memcpy(result.data(), &v, 8);
                break;
            }
            case Float: {
                float v = 0.0f;
                if (!parseFloatStrict(input, v)) break;
                result.resize(4);
                std::memcpy(result.data(), &v, 4);
                break;
            }
            case Double: {
                double v = 0.0;
                if (!parseDoubleStrict(input, v)) break;
                result.resize(8);
                std::memcpy(result.data(), &v, 8);
                break;
            }
        }
    } catch (...) {
        result.clear();
    }
    return result;
}

std::string formatValue(const std::vector<unsigned char>& data, int valueType) {
    if (data.empty()) return "无效";
    return formatValue(data.data(), valueType);
}

std::string formatValue(const unsigned char* data, int valueType) {
    if (!data) return "无效";
    std::ostringstream oss;
    switch (valueType) {
        case Byte1: oss << (int)data[0]; break;
        case Byte2: { uint16_t v; std::memcpy(&v, data, 2); oss << v; break; }
        case Byte4: { uint32_t v; std::memcpy(&v, data, 4); oss << v; break; }
        case Byte8: { uint64_t v; std::memcpy(&v, data, 8); oss << v; break; }
        case Float: {
            float v; std::memcpy(&v, data, 4);
            oss << std::fixed << std::setprecision(6) << v;
            break;
        }
        case Double: {
            double v; std::memcpy(&v, data, 8);
            oss << std::fixed << std::setprecision(10) << v;
            break;
        }
    }
    return oss.str();
}

std::string formatFromRaw(uint64_t value, int valueType) {
    std::ostringstream oss;
    switch (valueType) {
        case Byte1: oss << (int)(uint8_t)(value & 0xFF); break;
        case Byte2: oss << (uint16_t)(value & 0xFFFF); break;
        case Byte4: oss << (uint32_t)(value & 0xFFFFFFFF); break;
        case Byte8: oss << value; break;
        case Float: {
            uint32_t dw = (uint32_t)(value & 0xFFFFFFFF);
            float fv; std::memcpy(&fv, &dw, sizeof(float));
            if (std::isfinite(fv)) {
                if (std::abs(fv) >= 1e6 || (std::abs(fv) < 1e-3 && fv != 0.0f))
                    oss << std::scientific << std::setprecision(3) << fv;
                else
                    oss << std::fixed << std::setprecision(6) << fv;
            } else if (std::isnan(fv)) oss << "NaN";
            else if (std::isinf(fv)) oss << (fv > 0 ? "+∞" : "-∞");
            else oss << "无效浮点数";
            break;
        }
        case Double: {
            double dv; std::memcpy(&dv, &value, sizeof(double));
            if (std::isfinite(dv)) {
                if (std::abs(dv) >= 1e12 || (std::abs(dv) < 1e-6 && dv != 0.0))
                    oss << std::scientific << std::setprecision(6) << dv;
                else
                    oss << std::fixed << std::setprecision(10) << dv;
            } else if (std::isnan(dv)) oss << "NaN";
            else if (std::isinf(dv)) oss << (dv > 0 ? "+∞" : "-∞");
            else oss << "无效浮点数";
            break;
        }
        default: oss << "未知类型: " << value; break;
    }
    return oss.str();
}

std::string formatFromRawHex(uint64_t value, int valueType) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase;
    switch (valueType) {
        case Byte1: oss << "0x" << std::setfill('0') << std::setw(2) << (int)(uint8_t)(value & 0xFF); break;
        case Byte2: oss << "0x" << std::setfill('0') << std::setw(4) << (uint16_t)(value & 0xFFFF); break;
        case Byte4: oss << "0x" << std::setfill('0') << std::setw(8) << (uint32_t)(value & 0xFFFFFFFF); break;
        case Byte8: oss << "0x" << std::setfill('0') << std::setw(16) << value; break;
        default: oss << "0x" << value; break;
    }
    return oss.str();
}

std::string formatAddress(uint64_t address) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << address;
    return oss.str();
}

std::string formatHex(uint64_t value, int byteWidth) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(byteWidth * 2) << value;
    return oss.str();
}

uint32_t getTypeSize(int valueType) {
    switch (valueType) {
        case Byte1: return 1;
        case Byte2: return 2;
        case Byte4: return 4;
        case Byte8: return 8;
        case Float: return 4;
        case Double: return 8;
        default: return 4;
    }
}

} // namespace ValueFormatter
