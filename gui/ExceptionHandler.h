#pragma once

#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <signal.h>
#include <exception>
#include <string>
#include <time.h>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "DbgHelp.lib")
#pragma comment(lib, "Psapi.lib")

namespace ExceptionHandler
{
    // 异常类型枚举
    enum class ExceptionType
    {
        SEH_EXCEPTION,              // Windows SEH异常
        CPP_TERMINATE,              // C++ terminate
        CPP_UNEXPECTED,             // C++ unexpected
        PURE_CALL,                  // 纯虚函数调用
        INVALID_PARAMETER,          // 无效参数
        SIGNAL_ABORT,               // SIGABRT信号
        SIGNAL_FPE,                 // SIGFPE信号（浮点异常）
        SIGNAL_ILLEGAL,             // SIGILL信号（非法指令）
        SIGNAL_INT,                 // SIGINT信号（中断）
        SIGNAL_SEGV,                // SIGSEGV信号（段错误）
       // SIGNAL_TERM                 // SIGTERM信号（终止）
    };

    // 异常信息结构
    struct ExceptionInfo
    {
        ExceptionType type;
        DWORD exceptionCode;
        PVOID exceptionAddress;
        std::string description;
        std::string dumpFilePath;
        EXCEPTION_POINTERS* pExceptionPointers;
    };

    // 异常回调函数类型
    typedef void (*ExceptionCallback)(const ExceptionInfo& info);

    // 内部变量
    static ExceptionCallback g_exceptionCallback = nullptr;
    static std::string g_dumpDirectory = ".\\";
    static std::string g_applicationName = "Application";

    // 生成时间戳字符串
    inline std::string GetTimestamp()
    {
        time_t now = time(nullptr);
        tm timeInfo;
        localtime_s(&timeInfo, &now);
        
        std::ostringstream oss;
        oss << std::setfill('0')
            << std::setw(4) << (timeInfo.tm_year + 1900)
            << std::setw(2) << (timeInfo.tm_mon + 1)
            << std::setw(2) << timeInfo.tm_mday << "_"
            << std::setw(2) << timeInfo.tm_hour
            << std::setw(2) << timeInfo.tm_min
            << std::setw(2) << timeInfo.tm_sec;
        
        return oss.str();
    }

    // 获取调用堆栈信息
    inline std::string GetStackTrace(CONTEXT* context)
    {
        std::ostringstream oss;
        HANDLE process = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();
        
        // 初始化符号处理
        SymInitialize(process, nullptr, TRUE);
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        
        STACKFRAME64 stackFrame = {};
        DWORD machineType;
        
#ifdef _M_IX86
        machineType = IMAGE_FILE_MACHINE_I386;
        stackFrame.AddrPC.Offset = context->Eip;
        stackFrame.AddrPC.Mode = AddrModeFlat;
        stackFrame.AddrFrame.Offset = context->Ebp;
        stackFrame.AddrFrame.Mode = AddrModeFlat;
        stackFrame.AddrStack.Offset = context->Esp;
        stackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
        machineType = IMAGE_FILE_MACHINE_AMD64;
        stackFrame.AddrPC.Offset = context->Rip;
        stackFrame.AddrPC.Mode = AddrModeFlat;
        stackFrame.AddrFrame.Offset = context->Rsp;
        stackFrame.AddrFrame.Mode = AddrModeFlat;
        stackFrame.AddrStack.Offset = context->Rsp;
        stackFrame.AddrStack.Mode = AddrModeFlat;
#endif
        
        oss << "\n调用堆栈 (Call Stack):\n";
        oss << "----------------------------------------\n";
        
        int frameCount = 0;
        while (frameCount < 64)
        {
            if (!StackWalk64(
                machineType,
                process,
                thread,
                &stackFrame,
                context,
                nullptr,
                SymFunctionTableAccess64,
                SymGetModuleBase64,
                nullptr))
            {
                break;
            }
            
            if (stackFrame.AddrPC.Offset == 0)
                break;
                
            // 获取符号信息
            char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
            PSYMBOL_INFO symbol = (PSYMBOL_INFO)symbolBuffer;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            
            DWORD64 displacement = 0;
            oss << "  [" << frameCount << "] 0x" << std::hex << std::setw(16) << std::setfill('0') 
                << stackFrame.AddrPC.Offset << " ";
            
            if (SymFromAddr(process, stackFrame.AddrPC.Offset, &displacement, symbol))
            {
                oss << symbol->Name << " + 0x" << std::hex << displacement;
                
                // 尝试获取行号信息
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(process, stackFrame.AddrPC.Offset, &lineDisplacement, &line))
                {
                    oss << " (" << line.FileName << ":" << std::dec << line.LineNumber << ")";
                }
            }
            else
            {
                // 获取模块名
                IMAGEHLP_MODULE64 moduleInfo = {};
                moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
                if (SymGetModuleInfo64(process, stackFrame.AddrPC.Offset, &moduleInfo))
                {
                    oss << moduleInfo.ModuleName << " + 0x" << std::hex 
                        << (stackFrame.AddrPC.Offset - moduleInfo.BaseOfImage);
                }
                else
                {
                    oss << "<unknown>";
                }
            }
            
            oss << "\n";
            frameCount++;
        }
        
        SymCleanup(process);
        return oss.str();
    }
    
    // 获取寄存器信息
    inline std::string GetRegisterInfo(CONTEXT* context)
    {
        std::ostringstream oss;
        oss << "\n寄存器状态 (Registers):\n";
        oss << "----------------------------------------\n";
        oss << std::hex << std::setfill('0');
        
#ifdef _M_X64
        oss << "RAX: 0x" << std::setw(16) << context->Rax << "  ";
        oss << "RBX: 0x" << std::setw(16) << context->Rbx << "\n";
        oss << "RCX: 0x" << std::setw(16) << context->Rcx << "  ";
        oss << "RDX: 0x" << std::setw(16) << context->Rdx << "\n";
        oss << "RSI: 0x" << std::setw(16) << context->Rsi << "  ";
        oss << "RDI: 0x" << std::setw(16) << context->Rdi << "\n";
        oss << "RBP: 0x" << std::setw(16) << context->Rbp << "  ";
        oss << "RSP: 0x" << std::setw(16) << context->Rsp << "\n";
        oss << "RIP: 0x" << std::setw(16) << context->Rip << "\n";
        oss << "R8:  0x" << std::setw(16) << context->R8 << "  ";
        oss << "R9:  0x" << std::setw(16) << context->R9 << "\n";
        oss << "R10: 0x" << std::setw(16) << context->R10 << "  ";
        oss << "R11: 0x" << std::setw(16) << context->R11 << "\n";
        oss << "R12: 0x" << std::setw(16) << context->R12 << "  ";
        oss << "R13: 0x" << std::setw(16) << context->R13 << "\n";
        oss << "R14: 0x" << std::setw(16) << context->R14 << "  ";
        oss << "R15: 0x" << std::setw(16) << context->R15 << "\n";
        oss << "EFlags: 0x" << std::setw(8) << context->EFlags << "\n";
#elif _M_IX86
        oss << "EAX: 0x" << std::setw(8) << context->Eax << "  ";
        oss << "EBX: 0x" << std::setw(8) << context->Ebx << "\n";
        oss << "ECX: 0x" << std::setw(8) << context->Ecx << "  ";
        oss << "EDX: 0x" << std::setw(8) << context->Edx << "\n";
        oss << "ESI: 0x" << std::setw(8) << context->Esi << "  ";
        oss << "EDI: 0x" << std::setw(8) << context->Edi << "\n";
        oss << "EBP: 0x" << std::setw(8) << context->Ebp << "  ";
        oss << "ESP: 0x" << std::setw(8) << context->Esp << "\n";
        oss << "EIP: 0x" << std::setw(8) << context->Eip << "\n";
        oss << "EFlags: 0x" << std::setw(8) << context->EFlags << "\n";
#endif
        
        return oss.str();
    }
    
    // 获取加载模块信息
    inline std::string GetModulesInfo()
    {
        std::ostringstream oss;
        oss << "\n加载的模块 (Loaded Modules):\n";
        oss << "----------------------------------------\n";
        
        HANDLE hProcess = GetCurrentProcess();
        HMODULE hModules[1024];
        DWORD cbNeeded;
        
        if (EnumProcessModules(hProcess, hModules, sizeof(hModules), &cbNeeded))
        {
            int moduleCount = cbNeeded / sizeof(HMODULE);
            for (int i = 0; i < moduleCount; i++)
            {
                char moduleName[MAX_PATH];
                if (GetModuleFileNameA(hModules[i], moduleName, sizeof(moduleName)))
                {
                    MODULEINFO modInfo;
                    if (GetModuleInformation(hProcess, hModules[i], &modInfo, sizeof(modInfo)))
                    {
                        oss << "  [" << std::dec << i << "] 0x" << std::hex << std::setw(16) 
                            << std::setfill('0') << (DWORD64)modInfo.lpBaseOfDll
                            << " - 0x" << std::hex << std::setw(16) 
                            << ((DWORD64)modInfo.lpBaseOfDll + modInfo.SizeOfImage)
                            << " " << moduleName << "\n";
                    }
                }
            }
        }
        
        return oss.str();
    }
    
    // 获取系统信息
    inline std::string GetSystemInfo()
    {
        std::ostringstream oss;
        oss << "\n系统信息 (System Information):\n";
        oss << "----------------------------------------\n";
        
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        
        oss << "处理器架构: ";
        switch (sysInfo.wProcessorArchitecture)
        {
        case PROCESSOR_ARCHITECTURE_AMD64: oss << "x64 (AMD64)\n"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: oss << "x86\n"; break;
        case PROCESSOR_ARCHITECTURE_ARM: oss << "ARM\n"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: oss << "ARM64\n"; break;
        default: oss << "Unknown\n"; break;
        }
        
        oss << "处理器数量: " << std::dec << sysInfo.dwNumberOfProcessors << "\n";
        oss << "页面大小: " << sysInfo.dwPageSize << " bytes\n";
        
        MEMORYSTATUSEX memStatus = {};
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus))
        {
            oss << "物理内存总量: " << (memStatus.ullTotalPhys / 1024 / 1024) << " MB\n";
            oss << "物理内存可用: " << (memStatus.ullAvailPhys / 1024 / 1024) << " MB\n";
            oss << "虚拟内存总量: " << (memStatus.ullTotalVirtual / 1024 / 1024) << " MB\n";
            oss << "内存使用率: " << memStatus.dwMemoryLoad << "%\n";
        }
        
        return oss.str();
    }

    // 创建文本格式的异常报告
    inline bool CreateExceptionReport(EXCEPTION_POINTERS* pExceptionPointers, const std::string& filePath, const ExceptionInfo& info)
    {
        HANDLE hFile = CreateFileA(
            filePath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        std::ostringstream report;
        
        // 头部信息
        report << "========================================\n";
        report << "异常报告 (Exception Report)\n";
        report << "========================================\n";
        report << "应用程序: " << g_applicationName << "\n";
        report << "时间戳: " << GetTimestamp() << "\n";
        report << "进程ID: " << std::dec << GetCurrentProcessId() << "\n";
        report << "线程ID: " << GetCurrentThreadId() << "\n";
        report << "\n";
        
        // 异常信息
        report << "异常信息 (Exception Information):\n";
        report << "----------------------------------------\n";
        report << "异常类型: " << info.description << "\n";
        report << "异常代码: 0x" << std::hex << std::setw(8) << std::setfill('0') 
               << info.exceptionCode << "\n";
        report << "异常地址: 0x" << std::hex << std::setw(16) << std::setfill('0') 
               << (DWORD64)info.exceptionAddress << "\n";
        
        if (pExceptionPointers && pExceptionPointers->ExceptionRecord)
        {
            EXCEPTION_RECORD* pRecord = pExceptionPointers->ExceptionRecord;
            report << "异常标志: 0x" << std::hex << pRecord->ExceptionFlags << "\n";
            
            if (pRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2)
            {
                report << "访问类型: " << (pRecord->ExceptionInformation[0] == 0 ? "读取" : 
                         pRecord->ExceptionInformation[0] == 1 ? "写入" : "执行") << "\n";
                report << "访问地址: 0x" << std::hex << std::setw(16) << std::setfill('0') 
                       << pRecord->ExceptionInformation[1] << "\n";
            }
        }
        
        // 寄存器信息
        if (pExceptionPointers && pExceptionPointers->ContextRecord)
        {
            report << GetRegisterInfo(pExceptionPointers->ContextRecord);
        }
        
        // 调用堆栈
        if (pExceptionPointers && pExceptionPointers->ContextRecord)
        {
            report << GetStackTrace(pExceptionPointers->ContextRecord);
        }
        
        // 系统信息
        report << GetSystemInfo();
        
        // 模块信息
        report << GetModulesInfo();
        
        report << "\n========================================\n";
        report << "报告结束\n";
        report << "========================================\n";
        
        // 写入文件
        std::string reportStr = report.str();
        DWORD written;
        BOOL success = WriteFile(hFile, reportStr.c_str(), (DWORD)reportStr.length(), &written, nullptr);
        
        CloseHandle(hFile);
        return success != FALSE;
    }

    // 获取异常代码描述
    inline std::string GetExceptionCodeDescription(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:         return "Access Violation (访问违例)";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "Array Bounds Exceeded (数组越界)";
        case EXCEPTION_BREAKPOINT:               return "Breakpoint (断点)";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "Datatype Misalignment (数据类型不对齐)";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "Float Denormal Operand (浮点非正规操作数)";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "Float Divide by Zero (浮点除零)";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "Float Inexact Result (浮点不精确结果)";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "Float Invalid Operation (浮点无效操作)";
        case EXCEPTION_FLT_OVERFLOW:             return "Float Overflow (浮点溢出)";
        case EXCEPTION_FLT_STACK_CHECK:          return "Float Stack Check (浮点堆栈检查)";
        case EXCEPTION_FLT_UNDERFLOW:            return "Float Underflow (浮点下溢)";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "Illegal Instruction (非法指令)";
        case EXCEPTION_IN_PAGE_ERROR:            return "In Page Error (页面错误)";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "Integer Divide by Zero (整数除零)";
        case EXCEPTION_INT_OVERFLOW:             return "Integer Overflow (整数溢出)";
        case EXCEPTION_INVALID_DISPOSITION:      return "Invalid Disposition (无效配置)";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "Noncontinuable Exception (不可继续的异常)";
        case EXCEPTION_PRIV_INSTRUCTION:         return "Privileged Instruction (特权指令)";
        case EXCEPTION_SINGLE_STEP:              return "Single Step (单步执行)";
        case EXCEPTION_STACK_OVERFLOW:           return "Stack Overflow (堆栈溢出)";
        default:
            char buffer[64];
            sprintf_s(buffer, "Unknown Exception (0x%08X)", code);
            return buffer;
        }
    }

    // SEH异常过滤器
    inline LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* pExceptionPointers)
    {
        ExceptionInfo info = {};
        info.type = ExceptionType::SEH_EXCEPTION;
        info.pExceptionPointers = pExceptionPointers;
        
        if (pExceptionPointers && pExceptionPointers->ExceptionRecord)
        {
            info.exceptionCode = pExceptionPointers->ExceptionRecord->ExceptionCode;
            info.exceptionAddress = pExceptionPointers->ExceptionRecord->ExceptionAddress;
            info.description = GetExceptionCodeDescription(info.exceptionCode);
        }
        else
        {
            info.exceptionCode = 0;
            info.exceptionAddress = nullptr;
            info.description = "Unknown SEH Exception";
        }

        // 生成文本格式异常报告
        std::string reportFileName = g_applicationName + "_crash_" + GetTimestamp() + ".txt";
        info.dumpFilePath = g_dumpDirectory + reportFileName;
        
        if (pExceptionPointers)
        {
            CreateExceptionReport(pExceptionPointers, info.dumpFilePath, info);
        }

        // 调用用户回调
        if (g_exceptionCallback)
        {
            g_exceptionCallback(info);
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }

    // C++ terminate处理器
    inline void TerminateHandler()
    {
        ExceptionInfo info = {};
        info.type = ExceptionType::CPP_TERMINATE;
        info.exceptionCode = 0;
        info.exceptionAddress = nullptr;
        info.description = "C++ Terminate Called (调用了terminate)";
        info.pExceptionPointers = nullptr;

        // 生成文本格式异常报告
        std::string reportFileName = g_applicationName + "_terminate_" + GetTimestamp() + ".txt";
        info.dumpFilePath = g_dumpDirectory + reportFileName;
        CreateExceptionReport(nullptr, info.dumpFilePath, info);

        if (g_exceptionCallback)
        {
            g_exceptionCallback(info);
        }

        // 终止程序
        ExitProcess(1);
    }

    // 纯虚函数调用处理器
    inline void PureCallHandler()
    {
        ExceptionInfo info = {};
        info.type = ExceptionType::PURE_CALL;
        info.exceptionCode = 0;
        info.exceptionAddress = nullptr;
        info.description = "Pure Virtual Function Called (调用了纯虚函数)";
        info.pExceptionPointers = nullptr;

        std::string reportFileName = g_applicationName + "_purecall_" + GetTimestamp() + ".txt";
        info.dumpFilePath = g_dumpDirectory + reportFileName;
        CreateExceptionReport(nullptr, info.dumpFilePath, info);

        if (g_exceptionCallback)
        {
            g_exceptionCallback(info);
        }

        ExitProcess(1);
    }

    // 无效参数处理器
    inline void InvalidParameterHandler(
        const wchar_t* expression,
        const wchar_t* function,
        const wchar_t* file,
        unsigned int line,
        uintptr_t pReserved)
    {
        ExceptionInfo info = {};
        info.type = ExceptionType::INVALID_PARAMETER;
        info.exceptionCode = 0;
        info.exceptionAddress = nullptr;
        info.description = "Invalid Parameter (无效参数)";
        info.pExceptionPointers = nullptr;

        std::string reportFileName = g_applicationName + "_invalidparam_" + GetTimestamp() + ".txt";
        info.dumpFilePath = g_dumpDirectory + reportFileName;
        CreateExceptionReport(nullptr, info.dumpFilePath, info);

        if (g_exceptionCallback)
        {
            g_exceptionCallback(info);
        }

        ExitProcess(1);
    }

    // 信号处理器
    inline void SignalHandler(int signal)
    {
        ExceptionInfo info = {};
        info.exceptionCode = signal;
        info.exceptionAddress = nullptr;
        info.pExceptionPointers = nullptr;

        switch (signal)
        {
        case SIGABRT:
            info.type = ExceptionType::SIGNAL_ABORT;
            info.description = "Signal SIGABRT (程序异常终止)";
            break;
        case SIGFPE:
            info.type = ExceptionType::SIGNAL_FPE;
            info.description = "Signal SIGFPE (浮点异常)";
            break;
        case SIGILL:
            info.type = ExceptionType::SIGNAL_ILLEGAL;
            info.description = "Signal SIGILL (非法指令)";
            break;
        case SIGINT:
            info.type = ExceptionType::SIGNAL_INT;
            info.description = "Signal SIGINT (中断)";
            break;
        case SIGSEGV:
            info.type = ExceptionType::SIGNAL_SEGV;
            info.description = "Signal SIGSEGV (段错误)";
            break;
        // case SIGTERM:
        //     info.type = ExceptionType::SIGNAL_TERM;
        //     info.description = "Signal SIGTERM (终止请求)";
        //     break;
        default:
            info.type = ExceptionType::SIGNAL_ABORT;
            info.description = "Unknown Signal (未知信号)";
            break;
        }

        std::string reportFileName = g_applicationName + "_signal_" + GetTimestamp() + ".txt";
        info.dumpFilePath = g_dumpDirectory + reportFileName;
        CreateExceptionReport(nullptr, info.dumpFilePath, info);

        if (g_exceptionCallback)
        {
            g_exceptionCallback(info);
        }

        ExitProcess(1);
    }

    // 初始化异常处理器
    inline bool Initialize(
        const char* applicationName = "Application",
        const char* dumpDirectory = ".\\",
        ExceptionCallback callback = nullptr)
    {
        g_applicationName = applicationName;
        g_dumpDirectory = dumpDirectory;
        g_exceptionCallback = callback;

        // 确保dump目录以反斜杠结尾
        if (!g_dumpDirectory.empty() && g_dumpDirectory.back() != '\\')
        {
            g_dumpDirectory += "\\";
        }

        // 创建dump目录（如果不存在）
        CreateDirectoryA(g_dumpDirectory.c_str(), nullptr);

        // 设置SEH异常过滤器
        SetUnhandledExceptionFilter(UnhandledExceptionFilter);

        // 设置C++异常处理器
        std::set_terminate(TerminateHandler);

        // 设置纯虚函数调用处理器
        _set_purecall_handler(PureCallHandler);

        // 设置无效参数处理器
        _set_invalid_parameter_handler(InvalidParameterHandler);

        // 设置信号处理器
        signal(SIGABRT, SignalHandler);
        signal(SIGFPE, SignalHandler);
        signal(SIGILL, SignalHandler);
        signal(SIGINT, SignalHandler);
        signal(SIGSEGV, SignalHandler);
        // signal(SIGTERM, SignalHandler);

        return true;
    }

    // 手动触发异常报告（用于测试）
    inline void TriggerExceptionReport(const char* reason = "Manual Trigger")
    {
        ExceptionInfo info = {};
        info.type = ExceptionType::CPP_TERMINATE;
        info.exceptionCode = 0;
        info.exceptionAddress = nullptr;
        info.description = reason;
        info.pExceptionPointers = nullptr;

        std::string reportFileName = g_applicationName + "_manual_" + GetTimestamp() + ".txt";
        info.dumpFilePath = g_dumpDirectory + reportFileName;
        CreateExceptionReport(nullptr, info.dumpFilePath, info);

        if (g_exceptionCallback)
        {
            g_exceptionCallback(info);
        }
    }
}

