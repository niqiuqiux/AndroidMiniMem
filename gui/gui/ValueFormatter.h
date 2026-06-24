#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace ValueFormatter {
    // 值类型常量
    enum ValueType : int {
        Byte1 = 0,
        Byte2 = 1,
        Byte4 = 2,
        Byte8 = 3,
        Float = 4,
        Double = 5
    };

    // 解析用户输入为字节数组
    std::vector<unsigned char> parseInput(const std::string& input, int valueType, bool hexInput = false);

    // 从字节数组格式化为可读字符串
    std::string formatValue(const std::vector<unsigned char>& data, int valueType);
    std::string formatValue(const unsigned char* data, int valueType);

    // 从 uint64_t 原始值格式化（扫描结果用）
    std::string formatFromRaw(uint64_t value, int valueType);
    std::string formatFromRawHex(uint64_t value, int valueType);

    // 地址格式化
    std::string formatAddress(uint64_t address);
    std::string formatHex(uint64_t value, int byteWidth);

    // 获取值类型的字节大小
    uint32_t getTypeSize(int valueType);
}
