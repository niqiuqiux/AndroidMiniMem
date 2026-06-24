#pragma once
#include <sys/types.h>
#include <cstdarg>

class ITracer {
public:
    virtual ~ITracer() = default;

    // 初始化
    virtual bool Initialize(pid_t pid) = 0;

    // 调试跟踪接口
    virtual bool Attach() = 0;
    virtual bool Detach() = 0;
    virtual bool GetRegs(void* regs) = 0;
    virtual bool SetRegs(void* regs) = 0;
    
    // 进程控制
    virtual bool Continue() = 0;
    virtual bool Step() = 0;
    virtual bool Wait(int* status) = 0;
    
    // 内存操作
    virtual bool ReadMemory(uintptr_t address, void* buffer, size_t size) = 0;
    virtual bool WriteMemory(uintptr_t address, const void* buffer, size_t size) = 0;
    
    // 远程调用
    virtual uintptr_t CallFunction(uintptr_t address, int nargs, ...) = 0;
    virtual uintptr_t CallFunctionV(uintptr_t address, int nargs, va_list args) = 0;
}; 