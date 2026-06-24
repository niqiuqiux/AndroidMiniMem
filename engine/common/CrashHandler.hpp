#pragma once

#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <dlfcn.h>
#include <exception>
#include <typeinfo>

// 根据平台选择不同的栈回溯方法
#ifdef __ANDROID__
    #include <unwind.h>
#else
    #include <execinfo.h>
    #include <cxxabi.h>
#endif

#define CRASH_HANDLER_USE_LOGGER
// 可选：与 Logger 集成
// 如果定义了 CRASH_HANDLER_USE_LOGGER，则会尝试将崩溃信息记录到日志系统
#ifdef CRASH_HANDLER_USE_LOGGER
    #include "Logger.hpp"
    #define CRASH_LOG_ENABLED true
#else
    #define CRASH_LOG_ENABLED false
#endif

// 崩溃信息结构
struct CrashInfo {
    int signal_number;                    // 信号编号
    std::string signal_name;              // 信号名称
    std::string signal_description;       // 信号描述
    void* fault_address;                  // 故障地址
    pid_t process_id;                     // 进程ID
    pid_t thread_id;                      // 线程ID
    std::string timestamp;                // 时间戳
    std::string stack_trace;              // 堆栈跟踪
    std::string register_dump;            // 寄存器转储（架构相关）
    std::string exception_type;           // C++ 异常类型
    std::string exception_message;        // C++ 异常消息
};

// 崩溃处理器类
class CrashHandler {
private:
    static std::atomic<bool> handling_crash_;     // 防止递归崩溃处理
    static std::string crash_log_dir_;            // 崩溃日志目录（Initialize 后不再修改）
    static std::atomic<bool> initialized_;        // 是否已初始化
    static std::atomic<bool> use_logger_;         // 是否使用日志系统
    static struct sigaction old_handlers_[32];    // 保存旧的信号处理器
    static std::terminate_handler old_terminate_; // 保存旧的 terminate 处理器
    
    // 安全地记录日志（用于非信号处理器上下文）
    static void SafeLog(const std::string& message, bool is_error = false) {
#ifdef CRASH_HANDLER_USE_LOGGER
        if (use_logger_) {
            try {
                if (is_error) {
                    LOGE(message);
                } else {
                    LOGI(message);
                }
            } catch (...) {
                // 日志系统失败，忽略
            }
        }
#else
        (void)message;
        (void)is_error;
#endif
    }
    
    // 获取信号名称
    static const char* GetSignalName(int sig) {
        switch (sig) {
            case SIGSEGV: return "SIGSEGV";
            case SIGABRT: return "SIGABRT";
            case SIGFPE:  return "SIGFPE";
            case SIGILL:  return "SIGILL";
            case SIGBUS:  return "SIGBUS";
            case SIGTRAP: return "SIGTRAP";
            case SIGSYS:  return "SIGSYS";
            default:      return "UNKNOWN";
        }
    }
    
    // 获取信号描述
    static const char* GetSignalDescription(int sig) {
        switch (sig) {
            case SIGSEGV: return "段错误 (无效内存引用)";
            case SIGABRT: return "异常终止 (通常由 abort() 调用)";
            case SIGFPE:  return "浮点异常 (除零、溢出等)";
            case SIGILL:  return "非法指令";
            case SIGBUS:  return "总线错误 (内存访问对齐错误)";
            case SIGTRAP: return "跟踪/断点陷阱";
            case SIGSYS:  return "非法系统调用";
            default:      return "未知信号";
        }
    }
    
    // 获取当前时间戳
    static std::string GetTimestamp() {
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        char buffer[128];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
        return std::string(buffer);
    }
    
    // 获取线程ID
    static pid_t GetTid() {
        return static_cast<pid_t>(syscall(SYS_gettid));
    }
    
    // 获取当前异常信息
    static void GetCurrentException(std::string& exception_type, std::string& exception_message) {
        exception_type = "Unknown";
        exception_message = "No exception information available";
        
        try {
            // 尝试重新抛出当前异常以获取信息
            std::exception_ptr current_exception = std::current_exception();
            if (current_exception) {
                std::rethrow_exception(current_exception);
            }
        } catch (const std::bad_alloc& e) {
            exception_type = "std::bad_alloc";
            exception_message = e.what() ? e.what() : "Memory allocation failed";
        } catch (const std::runtime_error& e) {
            exception_type = "std::runtime_error";
            exception_message = e.what();
        } catch (const std::logic_error& e) {
            exception_type = "std::logic_error";
            exception_message = e.what();
        } catch (const std::exception& e) {
            exception_type = "std::exception";
            exception_message = e.what();
        } catch (const char* msg) {
            exception_type = "const char*";
            exception_message = msg ? msg : "(null)";
        } catch (const std::string& msg) {
            exception_type = "std::string";
            exception_message = msg;
        } catch (...) {
            // 尝试使用 RTTI 获取类型信息
#ifndef __ANDROID__
            try {
                std::type_info* ti = abi::__cxa_current_exception_type();
                if (ti) {
                    exception_type = DemangleName(ti->name());
                    exception_message = "Unknown exception of this type";
                } else {
                    exception_type = "Unknown exception type";
                    exception_message = "Cannot determine exception information";
                }
            } catch (...) {
                exception_type = "Unknown exception type";
                exception_message = "Cannot determine exception information";
            }
#else
            exception_type = "Unknown exception type";
            exception_message = "Cannot determine exception information (Android platform)";
#endif
        }
    }
    
    // 函数名解码（C++ name demangling）
    static std::string DemangleName(const char* mangled) {
#ifndef __ANDROID__
        int status = 0;
        char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
        
        if (status == 0 && demangled) {
            std::string result(demangled);
            free(demangled);
            return result;
        }
#endif
        return mangled ? mangled : "??";
    }

#ifdef __ANDROID__
    // Android 平台的栈回溯数据结构
    struct BacktraceState {
        void** current;
        void** end;
    };
    
    // Android unwind 回调函数
    static _Unwind_Reason_Code UnwindCallback(struct _Unwind_Context* context, void* arg) {
        BacktraceState* state = static_cast<BacktraceState*>(arg);
        uintptr_t pc = _Unwind_GetIP(context);
        if (pc) {
            if (state->current == state->end) {
                return _URC_END_OF_STACK;
            } else {
                *state->current++ = reinterpret_cast<void*>(pc);
            }
        }
        return _URC_NO_REASON;
    }
    
    // Android 平台的栈回溯实现
    static size_t CaptureBacktrace(void** buffer, size_t max) {
        BacktraceState state = {buffer, buffer + max};
        _Unwind_Backtrace(UnwindCallback, &state);
        return static_cast<size_t>(state.current - buffer);
    }
#endif
    
    // 获取堆栈跟踪
    static std::string GetStackTrace(int skip_frames = 2) {
        const int max_frames = 128;
        void* buffer[max_frames];
        int frame_count = 0;
        
#ifdef __ANDROID__
        frame_count = static_cast<int>(CaptureBacktrace(buffer, max_frames));
#else
        frame_count = backtrace(buffer, max_frames);
#endif
        
        std::stringstream ss;
        ss << "堆栈跟踪 (共 " << (frame_count - skip_frames) << " 帧):\n";
        ss << "========================================\n";
        
        for (int i = skip_frames; i < frame_count; i++) {
            ss << "#" << std::setw(2) << (i - skip_frames) << " ";
            
            // 解析符号信息
            Dl_info info;
            if (dladdr(buffer[i], &info)) {
                // 显示地址
                ss << "0x" << std::hex << std::setfill('0') << std::setw(sizeof(void*) * 2) 
                   << reinterpret_cast<uintptr_t>(buffer[i]) << " ";
                
                // 显示模块名
                if (info.dli_fname) {
                    const char* fname = strrchr(info.dli_fname, '/');
                    ss << "[" << (fname ? fname + 1 : info.dli_fname) << "] ";
                }
                
                // 显示函数名（解码后）
                if (info.dli_sname) {
                    std::string demangled = DemangleName(info.dli_sname);
                    ss << demangled;
                    
                    // 显示偏移量
                    if (info.dli_saddr) {
                        ptrdiff_t offset = static_cast<char*>(buffer[i]) - 
                                          static_cast<char*>(info.dli_saddr);
                        ss << " + 0x" << std::hex << offset;
                    }
                } else {
                    ss << "??";
                }
            } else {
                // 回退到地址显示
                ss << "0x" << std::hex << std::setfill('0') << std::setw(sizeof(void*) * 2)
                   << reinterpret_cast<uintptr_t>(buffer[i]);
            }
            
            ss << std::dec << "\n";
        }
        
        return ss.str();
    }
    
    // 获取寄存器转储（架构相关）
    static std::string GetRegisterDump(const ucontext_t* context) {
        if (!context) {
            return "寄存器信息不可用\n";
        }
        
        std::stringstream ss;
        ss << "寄存器状态:\n";
        ss << "========================================\n";
        
#if defined(__x86_64__)
        ss << std::hex << std::setfill('0');
        ss << "RAX: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_RAX] << "  ";
        ss << "RBX: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_RBX] << "\n";
        ss << "RCX: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_RCX] << "  ";
        ss << "RDX: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_RDX] << "\n";
        ss << "RSI: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_RSI] << "  ";
        ss << "RDI: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_RDI] << "\n";
        ss << "RBP: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_RBP] << "  ";
        ss << "RSP: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_RSP] << "\n";
        ss << "RIP: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_RIP] << "  ";
        ss << "EFL: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_EFL] << "\n";
        ss << "R8:  0x" << std::setw(16) << context->uc_mcontext.gregs[REG_R8]  << "  ";
        ss << "R9:  0x" << std::setw(16) << context->uc_mcontext.gregs[REG_R9]  << "\n";
        ss << "R10: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_R10] << "  ";
        ss << "R11: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_R11] << "\n";
        ss << "R12: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_R12] << "  ";
        ss << "R13: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_R13] << "\n";
        ss << "R14: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_R14] << "  ";
        ss << "R15: 0x" << std::setw(16) << context->uc_mcontext.gregs[REG_R15] << "\n";
#elif defined(__i386__)
        ss << std::hex << std::setfill('0');
        ss << "EAX: 0x" << std::setw(8) << context->uc_mcontext.gregs[REG_EAX] << "  ";
        ss << "EBX: 0x" << std::setw(8) << context->uc_mcontext.gregs[REG_EBX] << "\n";
        ss << "ECX: 0x" << std::setw(8) << context->uc_mcontext.gregs[REG_ECX] << "  ";
        ss << "EDX: 0x" << std::setw(8) << context->uc_mcontext.gregs[REG_EDX] << "\n";
        ss << "ESI: 0x" << std::setw(8) << context->uc_mcontext.gregs[REG_ESI] << "  ";
        ss << "EDI: 0x" << std::setw(8) << context->uc_mcontext.gregs[REG_EDI] << "\n";
        ss << "EBP: 0x" << std::setw(8) << context->uc_mcontext.gregs[REG_EBP] << "  ";
        ss << "ESP: 0x" << std::setw(8) << context->uc_mcontext.gregs[REG_ESP] << "\n";
        ss << "EIP: 0x" << std::setw(8) << context->uc_mcontext.gregs[REG_EIP] << "\n";
#elif defined(__aarch64__)
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 31; i += 2) {
            ss << "X" << std::setw(2) << std::dec << i << ": 0x" 
               << std::hex << std::setw(16) << context->uc_mcontext.regs[i] << "  ";
            if (i + 1 < 31) {
                ss << "X" << std::setw(2) << std::dec << (i + 1) << ": 0x" 
                   << std::hex << std::setw(16) << context->uc_mcontext.regs[i + 1];
            }
            ss << "\n";
        }
        ss << "SP:  0x" << std::setw(16) << context->uc_mcontext.sp << "  ";
        ss << "PC:  0x" << std::setw(16) << context->uc_mcontext.pc << "\n";
        ss << "PSTATE: 0x" << std::setw(16) << context->uc_mcontext.pstate << "\n";
#elif defined(__arm__)
        ss << std::hex << std::setfill('0');
        // ARM32 寄存器在 mcontext 中从 arm_r0 开始连续排列
        const unsigned long* arm_regs = &context->uc_mcontext.arm_r0;
        for (int i = 0; i < 13; i += 2) {
            ss << "R" << std::setw(2) << std::dec << i << ": 0x"
               << std::hex << std::setw(8) << arm_regs[i] << "  ";
            if (i + 1 < 13) {
                ss << "R" << std::setw(2) << std::dec << (i + 1) << ": 0x"
                   << std::hex << std::setw(8) << arm_regs[i + 1];
            }
            ss << "\n";
        }
        ss << "SP:  0x" << std::setw(8) << context->uc_mcontext.arm_sp << "  ";
        ss << "LR:  0x" << std::setw(8) << context->uc_mcontext.arm_lr << "\n";
        ss << "PC:  0x" << std::setw(8) << context->uc_mcontext.arm_pc << "  ";
        ss << "CPSR: 0x" << std::setw(8) << context->uc_mcontext.arm_cpsr << "\n";
#else
        ss << "不支持的架构，无法显示寄存器信息\n";
#endif
        
        ss << std::dec;
        return ss.str();
    }
    
    // 生成崩溃转储文件
    static void WriteCrashDump(const CrashInfo& crash_info) {
        // 立即输出调试信息
        const char* dump_msg = "Writing crash dump...\n";
        write(STDERR_FILENO, dump_msg, strlen(dump_msg));
        
        // 生成文件名
        std::stringstream filename;
        filename << crash_log_dir_ << "/crash_"
                 << crash_info.process_id << "_"
                 << crash_info.thread_id << "_"
                 << time(nullptr) << ".log";
        
        std::string crash_file = filename.str();
        
        // 输出文件路径
        char path_msg[512];
        snprintf(path_msg, sizeof(path_msg), "Crash file: %s\n", crash_file.c_str());
        write(STDERR_FILENO, path_msg, strlen(path_msg));
        
        std::ofstream file(crash_file, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            // 无法打开文件，输出错误信息
            const char* err_msg = "ERROR: Cannot open crash file, trying fallback...\n";
            write(STDERR_FILENO, err_msg, strlen(err_msg));
            
            // 尝试写入当前目录
            crash_file = "crash_dump.log";
            std::ofstream fallback(crash_file, std::ios::out | std::ios::trunc);
            if (fallback.is_open()) {
                WriteCrashDumpContent(fallback, crash_info);
                fallback.close();
                const char* success_msg = "Crash dump written to fallback file\n";
                write(STDERR_FILENO, success_msg, strlen(success_msg));
            } else {
                const char* fail_msg = "ERROR: Cannot write crash dump to any file!\n";
                write(STDERR_FILENO, fail_msg, strlen(fail_msg));
            }
            return;
        }
        
        WriteCrashDumpContent(file, crash_info);
        file.close();
        
        const char* success_msg = "Crash dump written successfully\n";
        write(STDERR_FILENO, success_msg, strlen(success_msg));
        
        // 记录崩溃信息到日志系统（注意：这不在信号处理器中调用，而是在后续处理）
        LogCrashInfo(crash_info, crash_file);
    }
    
    // 将崩溃信息记录到日志系统
    static void LogCrashInfo(const CrashInfo& crash_info, const std::string& crash_file) {
#ifdef CRASH_HANDLER_USE_LOGGER
        if (!use_logger_) return;
        
        try {
            // 注意：由于在信号处理器中，我们不应该调用可能使用互斥锁的函数
            // 但如果日志系统设计得当，简单的写入应该是可以的
            // 为了安全起见，我们在这里捕获所有异常
            
            std::stringstream ss;
            ss << "========== 程序崩溃 ==========";
            LOGF(ss.str());
            
            ss.str("");
            ss << "信号: " << crash_info.signal_name << " (" << crash_info.signal_number << ")";
            LOGF(ss.str());
            
            ss.str("");
            ss << "描述: " << crash_info.signal_description;
            LOGF(ss.str());
            
            if (crash_info.fault_address) {
                ss.str("");
                ss << "故障地址: 0x" << std::hex << crash_info.fault_address;
                LOGF(ss.str());
            }
            
            // 记录 C++ 异常信息
            if (!crash_info.exception_type.empty() && crash_info.exception_type != "Unknown") {
                ss.str("");
                ss << "异常类型: " << crash_info.exception_type;
                LOGF(ss.str());
                
                ss.str("");
                ss << "异常消息: " << crash_info.exception_message;
                LOGF(ss.str());
            }
            
            ss.str("");
            ss << "崩溃文件: " << crash_file;
            LOGF(ss.str());
            
            // 记录堆栈跟踪的前几行
            std::istringstream stack_stream(crash_info.stack_trace);
            std::string line;
            int line_count = 0;
            while (std::getline(stack_stream, line) && line_count < 10) {
                if (!line.empty()) {
                    LOGF(line);
                    line_count++;
                }
            }
            
            LOGF("========== 崩溃信息结束 ==========");
        } catch (...) {
            // 日志系统失败，忽略
        }
#else
        (void)crash_info;
        (void)crash_file;
#endif
    }
    
    // 写入崩溃转储内容
    static void WriteCrashDumpContent(std::ofstream& file, const CrashInfo& crash_info) {
        file << "========================================\n";
        file << "        程序崩溃报告\n";
        file << "========================================\n\n";
        
        file << "时间: " << crash_info.timestamp << "\n";
        file << "进程ID: " << crash_info.process_id << "\n";
        file << "线程ID: " << crash_info.thread_id << "\n\n";
        
        file << "信号信息:\n";
        file << "----------------------------------------\n";
        file << "信号编号: " << crash_info.signal_number << "\n";
        file << "信号名称: " << crash_info.signal_name << "\n";
        file << "信号描述: " << crash_info.signal_description << "\n";
        
        if (crash_info.fault_address) {
            file << "故障地址: 0x" << std::hex << crash_info.fault_address << std::dec << "\n";
        }
        file << "\n";
        
        // C++ 异常信息
        if (!crash_info.exception_type.empty() && crash_info.exception_type != "Unknown") {
            file << "C++ 异常信息:\n";
            file << "----------------------------------------\n";
            file << "异常类型: " << crash_info.exception_type << "\n";
            file << "异常消息: " << crash_info.exception_message << "\n\n";
        }
        
        file << crash_info.register_dump << "\n";
        file << crash_info.stack_trace << "\n";
        
        file << "========================================\n";
        file << "            报告结束\n";
        file << "========================================\n";
    }
    
    // 输出简化的堆栈跟踪到 stderr（不使用 C++ 流）
    static void WriteSimpleStackTrace() {
        void* buffer[64];
        int frame_count = 0;
        
#ifdef __ANDROID__
        BacktraceState state = {buffer, buffer + 64};
        _Unwind_Backtrace(UnwindCallback, &state);
        frame_count = static_cast<int>(state.current - buffer);
#else
        frame_count = backtrace(buffer, 64);
#endif
        
        const char* stack_header = "\nStack trace:\n";
        write(STDERR_FILENO, stack_header, strlen(stack_header));
        
        for (int i = 0; i < frame_count && i < 32; i++) {
            char frame_buf[256];
            Dl_info info;
            if (dladdr(buffer[i], &info) && info.dli_sname) {
                snprintf(frame_buf, sizeof(frame_buf), "  #%-2d 0x%016lx %s+0x%lx\n", 
                    i, 
                    (unsigned long)buffer[i],
                    info.dli_sname,
                    (unsigned long)((char*)buffer[i] - (char*)info.dli_saddr));
            } else {
                snprintf(frame_buf, sizeof(frame_buf), "  #%-2d 0x%016lx\n", 
                    i, (unsigned long)buffer[i]);
            }
            write(STDERR_FILENO, frame_buf, strlen(frame_buf));
        }
        write(STDERR_FILENO, "\n", 1);
    }
    
    // C++ 异常 terminate 处理函数
    static void TerminateHandler() {
        // 立即输出到 stderr，确保至少有些输出
        const char* msg = "\n!!! TERMINATE HANDLER TRIGGERED !!!\n";
        write(STDERR_FILENO, msg, strlen(msg));
        
        // 防止递归崩溃
        bool expected = false;
        if (!handling_crash_.compare_exchange_strong(expected, true)) {
            // 递归崩溃 - 输出简化的信息
            const char* recursive_msg = "!!! RECURSIVE CRASH DETECTED !!!\n";
            write(STDERR_FILENO, recursive_msg, strlen(recursive_msg));
            
            // 仍然尝试输出堆栈
            WriteSimpleStackTrace();
            
            const char* exit_msg = "Aborting due to recursive crash...\n\n";
            write(STDERR_FILENO, exit_msg, strlen(exit_msg));
            _exit(1);
        }
        
        // 先输出基本信息到 stderr（使用安全的 C 函数）
        const char* header = "\n"
            "===========================================\n"
            "    程序遇到未捕获的C++异常并即将终止\n"
            "===========================================\n";
        write(STDERR_FILENO, header, strlen(header));
        
        // 尝试获取异常信息并输出
        try {
            std::string exception_type, exception_message;
            GetCurrentException(exception_type, exception_message);
            
            char exc_buf[512];
            snprintf(exc_buf, sizeof(exc_buf), "异常类型: %s\n异常消息: %s\n\n", 
                exception_type.c_str(), exception_message.c_str());
            write(STDERR_FILENO, exc_buf, strlen(exc_buf));
        } catch (...) {
            const char* err = "无法获取异常信息\n\n";
            write(STDERR_FILENO, err, strlen(err));
        }
        
        // 输出简化的堆栈跟踪
        WriteSimpleStackTrace();
        
        // 尝试收集完整崩溃信息并写入文件（可能失败）
        try {
            CrashInfo crash_info;
            crash_info.signal_number = -1;
            crash_info.signal_name = "CPP_EXCEPTION";
            crash_info.signal_description = "未捕获的 C++ 异常";
            crash_info.fault_address = nullptr;
            crash_info.process_id = getpid();
            crash_info.thread_id = GetTid();
            crash_info.timestamp = GetTimestamp();
            crash_info.stack_trace = GetStackTrace(2);
            crash_info.register_dump = "异常终止，无寄存器信息\n";
            
            GetCurrentException(crash_info.exception_type, crash_info.exception_message);
            
            WriteCrashDump(crash_info);
            
            const char* saved_msg = "崩溃转储已保存\n";
            write(STDERR_FILENO, saved_msg, strlen(saved_msg));
        } catch (...) {
            const char* err = "警告: 无法保存崩溃转储文件\n";
            write(STDERR_FILENO, err, strlen(err));
        }
        
        const char* footer = "\n程序即将终止...\n\n";
        write(STDERR_FILENO, footer, strlen(footer));
        
        // 调用旧的 terminate handler 或 abort
        if (old_terminate_) {
            old_terminate_();
        } else {
            std::abort();
        }
    }
    
    // 信号安全的寄存器转储（仅使用 write/snprintf，不使用 C++ 流）
    static void WriteSignalSafeRegisterDump(int fd, const ucontext_t* context) {
        if (!context) return;
        char buf[128];

#if defined(__aarch64__)
        for (int i = 0; i < 31; i += 2) {
            snprintf(buf, sizeof(buf), "X%-2d: 0x%016lx  ", i,
                     (unsigned long)context->uc_mcontext.regs[i]);
            write(fd, buf, strlen(buf));
            if (i + 1 < 31) {
                snprintf(buf, sizeof(buf), "X%-2d: 0x%016lx",
                         i + 1, (unsigned long)context->uc_mcontext.regs[i + 1]);
                write(fd, buf, strlen(buf));
            }
            write(fd, "\n", 1);
        }
        snprintf(buf, sizeof(buf), "SP:  0x%016lx  PC:  0x%016lx\n",
                 (unsigned long)context->uc_mcontext.sp,
                 (unsigned long)context->uc_mcontext.pc);
        write(fd, buf, strlen(buf));
#elif defined(__arm__)
        const unsigned long* arm_regs = &context->uc_mcontext.arm_r0;
        for (int i = 0; i < 13; i += 2) {
            snprintf(buf, sizeof(buf), "R%-2d: 0x%08lx  ", i, arm_regs[i]);
            write(fd, buf, strlen(buf));
            if (i + 1 < 13) {
                snprintf(buf, sizeof(buf), "R%-2d: 0x%08lx", i + 1, arm_regs[i + 1]);
                write(fd, buf, strlen(buf));
            }
            write(fd, "\n", 1);
        }
        snprintf(buf, sizeof(buf), "SP:  0x%08lx  LR:  0x%08lx\nPC:  0x%08lx  CPSR: 0x%08lx\n",
                 (unsigned long)context->uc_mcontext.arm_sp,
                 (unsigned long)context->uc_mcontext.arm_lr,
                 (unsigned long)context->uc_mcontext.arm_pc,
                 (unsigned long)context->uc_mcontext.arm_cpsr);
        write(fd, buf, strlen(buf));
#elif defined(__x86_64__)
        snprintf(buf, sizeof(buf), "RIP: 0x%016lx  RSP: 0x%016lx\n",
                 (unsigned long)context->uc_mcontext.gregs[REG_RIP],
                 (unsigned long)context->uc_mcontext.gregs[REG_RSP]);
        write(fd, buf, strlen(buf));
        snprintf(buf, sizeof(buf), "RAX: 0x%016lx  RBX: 0x%016lx\n",
                 (unsigned long)context->uc_mcontext.gregs[REG_RAX],
                 (unsigned long)context->uc_mcontext.gregs[REG_RBX]);
        write(fd, buf, strlen(buf));
#endif
    }

    // 信号安全的崩溃转储写入（仅使用 async-signal-safe 函数）
    static void WriteSignalSafeCrashDump(int sig, siginfo_t* info, const ucontext_t* context) {
        // 构造文件路径
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/crash_%d_%d_%ld.log",
                 crash_log_dir_.c_str(), (int)getpid(), (int)GetTid(), (long)time(nullptr));

        int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            // 回退到当前目录
            fd = open("crash_dump.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) return;
        }

        char buf[512];
        write(fd, "========== Crash Report ==========\n", 35);

        snprintf(buf, sizeof(buf), "Signal: %d (%s)\nDescription: %s\nFault addr: %p\nPID: %d  TID: %d\n\n",
                 sig, GetSignalName(sig), GetSignalDescription(sig),
                 info ? info->si_addr : nullptr,
                 (int)getpid(), (int)GetTid());
        write(fd, buf, strlen(buf));

        write(fd, "Registers:\n", 11);
        WriteSignalSafeRegisterDump(fd, context);
        write(fd, "\n", 1);

        // 栈回溯（dladdr 在 Linux/Android 上实际可用于信号处理器）
        void* frames[64];
        int frame_count = 0;
#ifdef __ANDROID__
        BacktraceState state = {frames, frames + 64};
        _Unwind_Backtrace(UnwindCallback, &state);
        frame_count = static_cast<int>(state.current - frames);
#else
        frame_count = backtrace(frames, 64);
#endif
        write(fd, "Stack trace:\n", 13);
        for (int i = 0; i < frame_count && i < 48; i++) {
            Dl_info dlinfo;
            if (dladdr(frames[i], &dlinfo) && dlinfo.dli_sname) {
                snprintf(buf, sizeof(buf), "  #%-2d 0x%016lx %s+0x%lx\n",
                         i, (unsigned long)frames[i], dlinfo.dli_sname,
                         (unsigned long)((char*)frames[i] - (char*)dlinfo.dli_saddr));
            } else {
                snprintf(buf, sizeof(buf), "  #%-2d 0x%016lx\n",
                         i, (unsigned long)frames[i]);
            }
            write(fd, buf, strlen(buf));
        }

        write(fd, "========== End ==========\n", 26);
        close(fd);

        snprintf(buf, sizeof(buf), "Crash dump saved: %s\n", filepath);
        write(STDERR_FILENO, buf, strlen(buf));
    }

    // 信号处理函数
    static void SignalHandler(int sig, siginfo_t* info, void* context) {
        // 立即输出信号信息到 stderr
        char sig_msg[256];
        snprintf(sig_msg, sizeof(sig_msg), "\n!!! SIGNAL %d (%s) CAUGHT !!!\n", sig, GetSignalName(sig));
        write(STDERR_FILENO, sig_msg, strlen(sig_msg));
        
        // 防止递归崩溃
        bool expected = false;
        if (!handling_crash_.compare_exchange_strong(expected, true)) {
            // 递归信号 - 输出简化的信息
            const char* recursive_msg = "!!! RECURSIVE SIGNAL DETECTED !!!\n";
            write(STDERR_FILENO, recursive_msg, strlen(recursive_msg));
            
            // 仍然尝试输出堆栈
            WriteSimpleStackTrace();
            
            const char* exit_msg = "Aborting due to recursive signal...\n\n";
            write(STDERR_FILENO, exit_msg, strlen(exit_msg));
            _exit(128 + sig);
        }
        
        // 先输出基本信息到 stderr
        const char* header = "\n"
            "===========================================\n"
            "    程序遇到致命错误并即将终止\n"
            "===========================================\n";
        write(STDERR_FILENO, header, strlen(header));
        
        char basic_info[512];
        snprintf(basic_info, sizeof(basic_info), 
            "信号: %s (%s)\n故障地址: %p\nPID: %d, TID: %d\n\n",
            GetSignalName(sig), GetSignalDescription(sig),
            info ? info->si_addr : nullptr,
            getpid(), (int)GetTid());
        write(STDERR_FILENO, basic_info, strlen(basic_info));

        // 输出简化的堆栈跟踪
        WriteSimpleStackTrace();

        // 使用信号安全的方式写入崩溃转储文件（不使用 C++ 流/字符串）
        WriteSignalSafeCrashDump(sig, info, static_cast<ucontext_t*>(context));

        const char* footer = "\n程序即将终止...\n\n";
        write(STDERR_FILENO, footer, strlen(footer));
        
        // 恢复默认信号处理并重新触发
        signal(sig, SIG_DFL);
        raise(sig);
    }
    
    // 紧急崩溃处理器（用于在初始化之前就崩溃的情况）
    static void EmergencySignalHandler(int sig) {
        const char* msg = "\n!!! EMERGENCY: CRASH BEFORE HANDLER INITIALIZATION !!!\n";
        write(STDERR_FILENO, msg, strlen(msg));
        
        char sig_msg[128];
        snprintf(sig_msg, sizeof(sig_msg), "Signal: %d\n", sig);
        write(STDERR_FILENO, sig_msg, strlen(sig_msg));
        
        // 尝试输出简单的堆栈跟踪
        const char* stack_msg = "Stack trace:\n";
        write(STDERR_FILENO, stack_msg, strlen(stack_msg));
        
        void* buffer[32];
#ifdef __ANDROID__
        // Android 平台使用 unwind
        BacktraceState state = {buffer, buffer + 32};
        _Unwind_Backtrace(UnwindCallback, &state);
        int frame_count = static_cast<int>(state.current - buffer);
#else
        int frame_count = backtrace(buffer, 32);
#endif
        
        for (int i = 0; i < frame_count; i++) {
            char frame_msg[128];
            snprintf(frame_msg, sizeof(frame_msg), "  #%d: %p\n", i, buffer[i]);
            write(STDERR_FILENO, frame_msg, strlen(frame_msg));
        }
        
        _exit(128 + sig);
    }

public:
    // 初始化崩溃处理器
    static bool Initialize(const std::string& crash_log_dir = "./crash_logs", bool enable_logger = CRASH_LOG_ENABLED) {
        if (initialized_) {
            return true;
        }
        
        crash_log_dir_ = crash_log_dir;
        use_logger_ = enable_logger;
        
        // 首先注册紧急处理器，防止在初始化过程中崩溃
        signal(SIGSEGV, EmergencySignalHandler);
        signal(SIGABRT, EmergencySignalHandler);
        signal(SIGFPE, EmergencySignalHandler);
        signal(SIGILL, EmergencySignalHandler);
        signal(SIGBUS, EmergencySignalHandler);
        
        // 创建崩溃日志目录
        struct stat st;
        if (stat(crash_log_dir_.c_str(), &st) != 0) {
            if (mkdir(crash_log_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
                // 无法创建目录，使用当前目录
                crash_log_dir_ = ".";
            }
        }
        
        // 注册信号处理器
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;  // SA_RESETHAND: 处理后恢复默认
        sa.sa_sigaction = SignalHandler;
        
        // 需要捕获的信号列表
        int signals[] = {
            SIGSEGV,  // 段错误
            SIGABRT,  // 异常终止
            SIGFPE,   // 浮点异常
            SIGILL,   // 非法指令
            SIGBUS,   // 总线错误
            SIGTRAP,  // 跟踪陷阱
            SIGSYS    // 非法系统调用
        };
        
        for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
            int sig = signals[i];
            if (sigaction(sig, &sa, &old_handlers_[sig]) != 0) {
                // 处理失败，继续注册其他信号
                continue;
            }
        }
        
        // 注册 C++ 异常 terminate handler
        old_terminate_ = std::set_terminate(TerminateHandler);
        
        initialized_ = true;
        
        // 输出初始化成功信息到 stderr（方便调试）
        const char* init_msg = "\n*** CrashHandler initialized successfully ***\n";
        write(STDERR_FILENO, init_msg, strlen(init_msg));
        
        // 记录初始化信息
        std::stringstream ss;
        ss << "崩溃处理器已初始化 [日志目录: " << crash_log_dir_ 
           << ", 日志集成: " << (use_logger_ ? "启用" : "禁用") 
           << ", C++异常捕获: 已启用]";
        SafeLog(ss.str(), false);
        
        return true;
    }
    
    // 反初始化（恢复旧的信号处理器）
    static void Cleanup() {
        if (!initialized_) {
            return;
        }
        
        SafeLog("崩溃处理器正在清理", false);
        
        int signals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS, SIGTRAP, SIGSYS};
        
        for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
            int sig = signals[i];
            sigaction(sig, &old_handlers_[sig], nullptr);
        }
        
        // 恢复旧的 terminate handler
        if (old_terminate_) {
            std::set_terminate(old_terminate_);
        }
        
        initialized_ = false;
        use_logger_ = false;
    }
    
    // 手动触发崩溃转储（用于调试）
    static void DumpCurrentState(const std::string& reason = "手动触发") {
        CrashInfo crash_info;
        crash_info.signal_number = 0;
        crash_info.signal_name = "MANUAL_DUMP";
        crash_info.signal_description = reason;
        crash_info.fault_address = nullptr;
        crash_info.process_id = getpid();
        crash_info.thread_id = GetTid();
        crash_info.timestamp = GetTimestamp();
        crash_info.stack_trace = GetStackTrace(1);
        crash_info.register_dump = "手动转储，无寄存器信息\n";
        
        // 记录手动转储信息
        std::stringstream ss;
        ss << "手动触发状态转储: " << reason;
        SafeLog(ss.str(), false);
        
        WriteCrashDump(crash_info);
        
        SafeLog("状态转储已完成", false);
    }
    
    // 设置崩溃日志目录
    static void SetCrashLogDirectory(const std::string& dir) {
        crash_log_dir_ = dir;
    }
    
    // 获取是否已初始化
    static bool IsInitialized() {
        return initialized_;
    }
    
    // 启用/禁用日志集成
    static void EnableLogger(bool enable = true) {
        use_logger_ = enable;
        if (initialized_) {
            std::stringstream ss;
            ss << "崩溃处理器日志集成已" << (enable ? "启用" : "禁用");
            SafeLog(ss.str(), false);
        }
    }
    
    // 获取日志集成状态
    static bool IsLoggerEnabled() {
        return use_logger_;
    }
};

// 静态成员初始化
// 注意：这些变量需要在某个 .cpp 文件中定义，避免重复定义错误
// 如果这是纯头文件实现，可以使用 inline 变量（C++17）
inline std::atomic<bool> CrashHandler::handling_crash_{false};
inline std::string CrashHandler::crash_log_dir_{"./crash_logs"};
inline std::atomic<bool> CrashHandler::initialized_{false};
inline std::atomic<bool> CrashHandler::use_logger_{CRASH_LOG_ENABLED};
inline struct sigaction CrashHandler::old_handlers_[32]{};
inline std::terminate_handler CrashHandler::old_terminate_{nullptr};

// 便捷宏定义
#define CRASH_HANDLER_INIT() CrashHandler::Initialize()
#define CRASH_HANDLER_INIT_WITH_DIR(dir) CrashHandler::Initialize(dir)
#define CRASH_HANDLER_INIT_FULL(dir, logger) CrashHandler::Initialize(dir, logger)
#define CRASH_HANDLER_CLEANUP() CrashHandler::Cleanup()
#define CRASH_HANDLER_DUMP(reason) CrashHandler::DumpCurrentState(reason)
#define CRASH_HANDLER_ENABLE_LOGGER(enable) CrashHandler::EnableLogger(enable)

// RAII 包装器，确保在作用域结束时清理
class CrashHandlerGuard {
public:
    // 基本构造函数
    explicit CrashHandlerGuard(const std::string& crash_log_dir = "./crash_logs", 
                               bool enable_logger = CRASH_LOG_ENABLED) {
        CrashHandler::Initialize(crash_log_dir, enable_logger);
    }
    
    ~CrashHandlerGuard() {
        CrashHandler::Cleanup();
    }
    
    // 启用/禁用日志
    void EnableLogger(bool enable = true) {
        CrashHandler::EnableLogger(enable);
    }
    
    // 手动转储
    void DumpCurrentState(const std::string& reason = "手动触发") {
        CrashHandler::DumpCurrentState(reason);
    }
    
    // 禁止拷贝和移动
    CrashHandlerGuard(const CrashHandlerGuard&) = delete;
    CrashHandlerGuard& operator=(const CrashHandlerGuard&) = delete;
    CrashHandlerGuard(CrashHandlerGuard&&) = delete;
    CrashHandlerGuard& operator=(CrashHandlerGuard&&) = delete;
};

/*
使用示例：

// 方式1：全局初始化（不启用日志集成）
int main() {
    CRASH_HANDLER_INIT_WITH_DIR("/tmp/crash_logs");
    
    // 你的程序代码
    
    CRASH_HANDLER_CLEANUP();
    return 0;
}

// 方式2：使用 RAII 包装器（推荐）
int main() {
    CrashHandlerGuard crash_guard("/tmp/crash_logs");
    
    // 你的程序代码
    // crash_guard 析构时会自动清理
    
    return 0;
}

// 方式3：同时使用 Logger 和 CrashHandler（启用日志集成）
#define CRASH_HANDLER_USE_LOGGER  // 在包含头文件前定义
#include "common/Logger.hpp"
#include "common/CrashHandler.hpp"

int main() {
    // 初始化 Logger
    Logger::Initialize(LogLevel::INFO, LogMode::ALL, "server.log");
    
    // 初始化 CrashHandler，启用日志集成
    CrashHandlerGuard crash_guard("/tmp/crash_logs", true);
    
    LOGI("程序启动");
    
    // 你的程序代码
    
    LOGI("程序结束");
    Logger::Cleanup();
    return 0;
}

// 方式4：手动触发状态转储
void some_function() {
    if (error_condition) {
        CRASH_HANDLER_DUMP("检测到错误状态");
        // 如果启用了日志集成，这也会记录到日志系统
    }
}

// 方式5：动态控制日志集成
int main() {
    CrashHandlerGuard crash_guard("/tmp/crash_logs", false);  // 初始禁用日志
    
    // ... 一些代码 ...
    
    // 运行时启用日志集成
    crash_guard.EnableLogger(true);
    
    // ... 更多代码 ...
    
    return 0;
}
*/

