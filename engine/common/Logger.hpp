#pragma once
#include <iostream>
#include <sstream>
#include <string>
#include <chrono>
#include <iomanip>
#include <thread>
#include <mutex>
#include <fstream>
#include <atomic>
#include <syslog.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cstdio>

// 日志级别定义
enum class LogLevel {
    VERBOSE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    FATAL = 5
};

// 日志输出模式
enum class LogMode {
    CONSOLE = 1,    // 控制台输出
    SYSLOG = 2,     // 系统日志
    FILE = 4,       // 文件输出
    ALL = 7         // 所有模式
};

class Logger {
private:
    static inline LogLevel currentLevel_ = LogLevel::VERBOSE;
    static inline LogMode currentMode_ = LogMode::CONSOLE;
    static inline std::string logFile_;
    static inline std::mutex logMutex_;
    static inline std::atomic<bool> initialized_{false};
    static inline std::ofstream logFileStream_;
    
    static std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        std::stringstream ss;
        struct tm tm_buf;
        localtime_r(&time_t, &tm_buf);
        ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        ss << "." << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }
    
    static std::string GetThreadId() {
        std::stringstream ss;
        ss << std::this_thread::get_id();
        return ss.str();
    }
    
    static pid_t GetTid() {
        return static_cast<pid_t>(syscall(SYS_gettid));
    }
    
    static std::string LevelToString(LogLevel level) {
        switch (level) {
            case LogLevel::VERBOSE: return "V";
            case LogLevel::DEBUG:   return "D";
            case LogLevel::INFO:    return "I";
            case LogLevel::WARN:    return "W";
            case LogLevel::ERROR:   return "E";
            case LogLevel::FATAL:   return "F";
            default: return "U";
        }
    }
    
    static int LevelToSyslog(LogLevel level) {
        switch (level) {
            case LogLevel::VERBOSE: return LOG_DEBUG;
            case LogLevel::DEBUG:   return LOG_DEBUG;
            case LogLevel::INFO:    return LOG_INFO;
            case LogLevel::WARN:    return LOG_WARNING;
            case LogLevel::ERROR:   return LOG_ERR;
            case LogLevel::FATAL:   return LOG_CRIT;
            default: return LOG_INFO;
        }
    }
    
public:
    static void Initialize(LogLevel level = LogLevel::INFO, 
                          LogMode mode = LogMode::CONSOLE,
                          const std::string& logFile = "") {
        std::lock_guard<std::mutex> lock(logMutex_);
        currentLevel_ = level;
        currentMode_ = mode;
        logFile_ = logFile;
        
        // 若先前有打开的文件流，先关闭
        if (logFileStream_.is_open()) {
            logFileStream_.close();
        }

        // 文件日志备份与清理：
        // 1) 若同目录下存在 log.bak，则先删除
        // 2) 若已存在当前日志文件，则将其重命名为 log.bak
        if ((static_cast<int>(mode) & static_cast<int>(LogMode::FILE)) && !logFile_.empty()) {
            const std::string& targetLog = logFile_;
            size_t pos = targetLog.find_last_of("/");
            std::string directory = (pos == std::string::npos) ? std::string() : targetLog.substr(0, pos + 1);
            std::string bakPath = directory + "log.bak";

            if (access(bakPath.c_str(), F_OK) == 0) {
                unlink(bakPath.c_str());
            }
            if (access(targetLog.c_str(), F_OK) == 0) {
                rename(targetLog.c_str(), bakPath.c_str());
            }

            // 初始化时打开日志文件流，后续直接写入
            logFileStream_.open(logFile_, std::ios::out | std::ios::trunc);
        }
        
        if (static_cast<int>(mode) & static_cast<int>(LogMode::SYSLOG)) {
            openlog("memory_server", LOG_PID | LOG_CONS, LOG_DAEMON);
        }
        
        initialized_ = true;
    }
    
    static void Cleanup() {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (static_cast<int>(currentMode_) & static_cast<int>(LogMode::SYSLOG)) {
            closelog();
        }
        if (logFileStream_.is_open()) {
            logFileStream_.close();
        }

        initialized_ = false;
    }
    
    static void SetLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(logMutex_);
        currentLevel_ = level;
    }
    
    static void SetMode(LogMode mode) {
        std::lock_guard<std::mutex> lock(logMutex_);
        currentMode_ = mode;
    }
    
    static void Log(LogLevel level, const char* file, int line, 
                   const char* func, const std::string& message) {
        if (!initialized_ || level < currentLevel_) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(logMutex_);
        
        // 提取文件名（兼容 Windows 和 Unix 路径分隔符）
        const char* p1 = strrchr(file, '/');
        const char* p2 = strrchr(file, '\\');
        const char* filename = file;
        if (p1 && p2) {
            filename = (p1 > p2) ? (p1 + 1) : (p2 + 1);
        } else if (p1) {
            filename = p1 + 1;
        } else if (p2) {
            filename = p2 + 1;
        }
        
        // 构建日志消息
        std::stringstream ss;
        ss << "[" << GetTimestamp() << "] "
           << "[" << LevelToString(level) << "] "
           << "[" << getpid() << ":" << GetTid() << "] "
           << "[" << filename << ":" << line << "] "
           << "[" << func << "] "
           << message;
        
        std::string logMessage = ss.str();
        
        // 控制台输出
        if (static_cast<int>(currentMode_) & static_cast<int>(LogMode::CONSOLE)) {
            if (level >= LogLevel::ERROR) {
                std::cerr << logMessage << std::endl;
            } else {
                std::cout << logMessage << std::endl;
            }
        }
        
        // 系统日志输出
        if (static_cast<int>(currentMode_) & static_cast<int>(LogMode::SYSLOG)) {
            syslog(LevelToSyslog(level), "%s", message.c_str());
        }
        
        // 文件输出（使用已打开的持久化文件流）
        if ((static_cast<int>(currentMode_) & static_cast<int>(LogMode::FILE))) {
            if (logFileStream_.is_open()) {
                logFileStream_ << logMessage << std::endl;
            }
        }
    }
    
    template<typename... Args>
    static void LogFormat(LogLevel level, const char* file, int line,
                         const char* func, const char* format, Args... args) {
        if (!initialized_ || level < currentLevel_) {
            return;
        }
        
        char buffer[2048];
        snprintf(buffer, sizeof(buffer), format, args...);
        Log(level, file, line, func, std::string(buffer));
    }
};

// 日志宏定义
#define LOG_INIT(level, mode, file) Logger::Initialize(level, mode, file)
#define LOG_CLEANUP() Logger::Cleanup()
#define LOG_SET_LEVEL(level) Logger::SetLevel(level)
#define LOG_SET_MODE(mode) Logger::SetMode(mode)

#define LOGV(msg) Logger::Log(LogLevel::VERBOSE, __FILE__, __LINE__, __FUNCTION__, msg)
#define LOGD(msg) Logger::Log(LogLevel::DEBUG, __FILE__, __LINE__, __FUNCTION__, msg)
#define LOGI(msg) Logger::Log(LogLevel::INFO, __FILE__, __LINE__, __FUNCTION__, msg)
#define LOGW(msg) Logger::Log(LogLevel::WARN, __FILE__, __LINE__, __FUNCTION__, msg)
#define LOGE(msg) Logger::Log(LogLevel::ERROR, __FILE__, __LINE__, __FUNCTION__, msg)
#define LOGF(msg) Logger::Log(LogLevel::FATAL, __FILE__, __LINE__, __FUNCTION__, msg)

// 格式化日志宏
#define LOGVF(fmt, ...) Logger::LogFormat(LogLevel::VERBOSE, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOGDF(fmt, ...) Logger::LogFormat(LogLevel::DEBUG, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOGIF(fmt, ...) Logger::LogFormat(LogLevel::INFO, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOGWF(fmt, ...) Logger::LogFormat(LogLevel::WARN, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOGEF(fmt, ...) Logger::LogFormat(LogLevel::ERROR, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOGFF(fmt, ...) Logger::LogFormat(LogLevel::FATAL, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

// 条件日志宏
#define LOGV_IF(cond, msg) do { if (cond) LOGV(msg); } while(0)
#define LOGD_IF(cond, msg) do { if (cond) LOGD(msg); } while(0)
#define LOGI_IF(cond, msg) do { if (cond) LOGI(msg); } while(0)
#define LOGW_IF(cond, msg) do { if (cond) LOGW(msg); } while(0)
#define LOGE_IF(cond, msg) do { if (cond) LOGE(msg); } while(0)
#define LOGF_IF(cond, msg) do { if (cond) LOGF(msg); } while(0)

// 断言宏
#define LOG_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        LOGF("ASSERTION FAILED: " #cond " - " msg); \
        abort(); \
    } \
} while(0)

// 性能计时宏
#define LOG_TIMER_START(name) \
    auto __timer_start_##name = std::chrono::high_resolution_clock::now()

#define LOG_TIMER_END(name) do { \
    auto __timer_end_##name = std::chrono::high_resolution_clock::now(); \
    auto __duration_##name = std::chrono::duration_cast<std::chrono::microseconds>( \
        __timer_end_##name - __timer_start_##name).count(); \
    LOGDF("TIMER [%s]: %ld μs", #name, __duration_##name); \
} while(0)

// 内存转储宏
#define LOG_HEX_DUMP(data, size, msg) do { \
    std::stringstream __ss; \
    __ss << msg << " (size=" << size << "):\n"; \
    const uint8_t* __bytes = static_cast<const uint8_t*>(data); \
    for (size_t __i = 0; __i < size; __i += 16) { \
        __ss << std::hex << std::setfill('0') << std::setw(8) << __i << ": "; \
        for (size_t __j = 0; __j < 16 && __i + __j < size; ++__j) { \
            __ss << std::hex << std::setfill('0') << std::setw(2) \
                 << static_cast<int>(__bytes[__i + __j]) << " "; \
        } \
        __ss << "\n"; \
    } \
    LOGD(__ss.str()); \
} while(0)