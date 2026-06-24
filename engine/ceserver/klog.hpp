#pragma once
#include <sys/klog.h>
#include <vector>
#include <string>
#include <map>
#include <sstream>
#include <algorithm>
#include <cctype>



static void ClearKernelLog() {
    klogctl(KLOG_CLEAR, nullptr, 0);
}


struct KernelLogMatches {
    std::vector<std::string> mem_logs;
    std::vector<std::string> net_verify_logs;
};

// 通用的内核日志提取器结构
struct KernelLogExtractor {
    std::map<std::string, std::vector<std::string>> tag_logs;  // tag -> 匹配的日志列表
    
    // 清空所有日志
    void clear() {
        tag_logs.clear();
    }
    
    // 获取指定tag的日志数量
    size_t getLogCount(const std::string& tag) const {
        auto it = tag_logs.find(tag);
        return (it != tag_logs.end()) ? it->second.size() : 0;
    }
    
    // 获取指定tag的日志
    std::vector<std::string> getLogs(const std::string& tag) const {
        auto it = tag_logs.find(tag);
        return (it != tag_logs.end()) ? it->second : std::vector<std::string>();
    }
    
    // 获取所有日志的总数
    size_t getTotalLogCount() const {
        size_t total = 0;
        for (const auto& pair : tag_logs) {
            total += pair.second.size();
        }
        return total;
    }
};

/**
 * 通用的内核日志提取函数 - 根据多个tag进行提取
 * @param tags 要匹配的tag列表，支持正则表达式
 * @param out_extractor 输出的日志提取器，包含按tag分类的日志
 * @param out_info 错误信息或状态信息
 * @param clear_after_extract 提取后是否清空内核日志缓冲区
 * @param case_sensitive 是否区分大小写匹配
 * @return 成功返回true，失败返回false
 */
static bool ExtractKernelLogsByTags(const std::vector<std::string>& tags, 
                                   KernelLogExtractor& out_extractor, 
                                   std::string& out_info,
                                   bool clear_after_extract = false,
                                   bool case_sensitive = true) {
    // 清空输出结果
    out_extractor.clear();
    
    // 获取内核日志缓冲区大小
    int buf_size = klogctl(KLOG_SIZE_BUFFER, nullptr, 0);
    if (buf_size <= 0) {
        buf_size = klogctl(KLOG_SIZE_UNREAD, nullptr, 0);
        if (buf_size <= 0) {
            out_info = "无法获取内核日志大小";
            return false;
        }
    }

    // 读取内核日志
    std::string buffer;
    buffer.resize(static_cast<size_t>(buf_size) + 1);
    int read_len = klogctl(KLOG_READ_ALL, buffer.data(), static_cast<int>(buffer.size() - 1));
    if (read_len < 0) {
        out_info = std::string("读取内核日志失败: ") + strerror(errno);
        return false;
    }
    buffer[static_cast<size_t>(read_len)] = '\0';

    // 为每个tag使用 string::find 进行匹配（比 regex 快得多）
    // 按行分割缓冲区
    std::istringstream stream(std::string(buffer.data(), static_cast<size_t>(read_len)));
    std::string line;

    while (std::getline(stream, line)) {
        // 去除行尾的换行符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;

        for (const std::string& tag : tags) {
            bool found = false;
            if (case_sensitive) {
                found = (line.find(tag) != std::string::npos);
            } else {
                // 不区分大小写的查找
                std::string lower_line = line;
                std::string lower_tag = tag;
                std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);
                std::transform(lower_tag.begin(), lower_tag.end(), lower_tag.begin(), ::tolower);
                found = (lower_line.find(lower_tag) != std::string::npos);
            }
            if (found) {
                out_extractor.tag_logs[tag].push_back(line);
            }
        }
    }

    // 检查是否找到任何日志
    if (out_extractor.getTotalLogCount() == 0) {
        std::string tag_list;
        for (size_t i = 0; i < tags.size(); ++i) {
            if (i > 0) tag_list += ", ";
            tag_list += tags[i];
        }
        out_info = "未在内核日志中找到匹配的标签: " + tag_list;
        return false;
    }

    // 如果需要，清空内核日志
    if (clear_after_extract) {
        if (klogctl(KLOG_CLEAR, nullptr, 0) < 0) {
            out_info = std::string("提取成功，但清空内核日志失败: ") + strerror(errno);
            // 仍然返回成功，因为日志已提取
        }
    }

    // 构建成功信息
    std::ostringstream success_info;
    success_info << "成功提取 " << out_extractor.getTotalLogCount() << " 条日志";
    for (const std::string& tag : tags) {
        size_t count = out_extractor.getLogCount(tag);
        if (count > 0) {
            success_info << ", " << tag << ": " << count << " 条";
        }
    }
    out_info = success_info.str();

    return true;
}

// 重构后的函数 - 使用通用提取器
static bool ExtractMemAndNetVerifyLogsFromKernelLog(KernelLogMatches &out_logs, std::string &out_info) {
    // 使用通用提取器
    std::vector<std::string> tags = {"Mem", "NET_VERIFY"};
    KernelLogExtractor extractor;
    
    bool result = ExtractKernelLogsByTags(tags, extractor, out_info, false, true);
    if (!result) {
        return false;
    }
    
    // 转换为原有格式
    out_logs.mem_logs = extractor.getLogs("Mem");
    out_logs.net_verify_logs = extractor.getLogs("NET_VERIFY");
    
    return true;
}

/**
 * 便利函数：提取单个tag的日志
 * @param tag 要匹配的标签
 * @param out_logs 输出的日志列表
 * @param out_info 错误信息或状态信息
 * @param clear_after_extract 提取后是否清空内核日志缓冲区
 * @param case_sensitive 是否区分大小写匹配
 * @return 成功返回true，失败返回false
 */
static bool ExtractKernelLogsByTag(const std::string& tag, 
                                  std::vector<std::string>& out_logs, 
                                  std::string& out_info,
                                  bool clear_after_extract = false,
                                  bool case_sensitive = true) {
    std::vector<std::string> tags = {tag};
    KernelLogExtractor extractor;
    
    bool result = ExtractKernelLogsByTags(tags, extractor, out_info, clear_after_extract, case_sensitive);
    if (!result) {
        return false;
    }
    
    out_logs = extractor.getLogs(tag);
    return true;
}

/**
 * 便利函数：提取多个tag的日志并合并到一个列表中
 * @param tags 要匹配的标签列表
 * @param out_logs 输出的合并日志列表
 * @param out_info 错误信息或状态信息
 * @param clear_after_extract 提取后是否清空内核日志缓冲区
 * @param case_sensitive 是否区分大小写匹配
 * @return 成功返回true，失败返回false
 */
static bool ExtractKernelLogsMerged(const std::vector<std::string>& tags, 
                                   std::vector<std::string>& out_logs, 
                                   std::string& out_info,
                                   bool clear_after_extract = false,
                                   bool case_sensitive = true) {
    KernelLogExtractor extractor;
    
    bool result = ExtractKernelLogsByTags(tags, extractor, out_info, clear_after_extract, case_sensitive);
    if (!result) {
        return false;
    }
    
    // 合并所有tag的日志
    out_logs.clear();
    for (const std::string& tag : tags) {
        std::vector<std::string> tag_logs = extractor.getLogs(tag);
        out_logs.insert(out_logs.end(), tag_logs.begin(), tag_logs.end());
    }
    
    return true;
}

/**
 * 使用示例：
 * 
 * // 示例1：提取单个tag的日志
 * std::vector<std::string> error_logs;
 * std::string info;
 * if (ExtractKernelLogsByTag("ERROR", error_logs, info)) {
 *     for (const auto& log : error_logs) {
 *         printf("Error log: %s\n", log.c_str());
 *     }
 * }
 * 
 * // 示例2：提取多个tag的日志（分类存储）
 * KernelLogExtractor extractor;
 * std::vector<std::string> tags = {"DEBUG", "INFO", "WARN", "ERROR"};
 * if (ExtractKernelLogsByTags(tags, extractor, info)) {
 *     printf("DEBUG logs: %zu\n", extractor.getLogCount("DEBUG"));
 *     printf("ERROR logs: %zu\n", extractor.getLogCount("ERROR"));
 *     
 *     auto debug_logs = extractor.getLogs("DEBUG");
 *     for (const auto& log : debug_logs) {
 *         printf("Debug: %s\n", log.c_str());
 *     }
 * }
 * 
 * // 示例3：提取多个tag的日志（合并存储）
 * std::vector<std::string> all_logs;
 * std::vector<std::string> search_tags = {"kernel", "driver", "module"};
 * if (ExtractKernelLogsMerged(search_tags, all_logs, info, true)) { // 提取后清空日志
 *     printf("Total logs found: %zu\n", all_logs.size());
 *     for (const auto& log : all_logs) {
 *         printf("Log: %s\n", log.c_str());
 *     }
 * }
 * 
 * // 示例4：使用正则表达式匹配
 * std::vector<std::string> regex_tags = {".*[Ee]rror.*", ".*[Ww]arn.*"};
 * KernelLogExtractor regex_extractor;
 * if (ExtractKernelLogsByTags(regex_tags, regex_extractor, info, false, false)) { // 不区分大小写
 *     printf("Found %zu total logs\n", regex_extractor.getTotalLogCount());
 * }
 */