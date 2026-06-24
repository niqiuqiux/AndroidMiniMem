#pragma once
#include <string>
#include <vector>
#include <elf.h>
#include "IMemoryOp.hpp"
//二进制文件解析
class IElfScanner {
public:
    virtual ~IElfScanner() = default;

    // 初始化
    virtual bool Initialize(IMemoryOp* memOp, uintptr_t elfBase) = 0;

    // 基本信息
    virtual bool IsValid() const = 0;
    virtual uintptr_t GetBase() const = 0;
    virtual uintptr_t GetEnd() const = 0;
    
    // ELF 头部信息
    virtual uintptr_t GetLoadBias() const = 0;
    virtual size_t GetLoadSize() const = 0;
    virtual const std::vector<Elf64_Phdr>& GetProgramHeaders() const = 0;
    virtual const std::vector<Elf64_Dyn>& GetDynamics() const = 0;

    // 符号查找
    virtual uintptr_t FindSymbol(const std::string& name) = 0;
    virtual std::vector<std::pair<uintptr_t, std::string>> GetSymbols() = 0;

    // 段信息
    virtual ProcessMap GetBaseSegment() const = 0;
    virtual std::vector<ProcessMap> GetSegments() const = 0;
}; 