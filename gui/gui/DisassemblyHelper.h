#pragma once

#include <string>
#include <vector>
#include <cstdint>

// 反汇编指令结构
struct DisassembledInstruction {
    uint64_t address;           // 指令地址
    std::string hexBytes;       // 指令的十六进制字节
    std::string mnemonic;       // 助记符（如 ldr, str, mov）
    std::string operands;       // 操作数
    std::string fullInstruction; // 完整指令字符串
    uint32_t size;              // 指令大小（字节）
    
    DisassembledInstruction() : address(0), size(0) {}
};

// 反汇编结果
struct DisassemblyResult {
    bool success;                                   // 是否成功
    std::vector<DisassembledInstruction> instructions; // 反汇编的指令列表
    std::string errorMessage;                       // 错误信息
    
    DisassemblyResult() : success(false) {}
};

// 反汇编助手类
class DisassemblyHelper {
public:
    // 架构类型枚举
    enum class Architecture {
        ARM64,      // ARM64/AArch64
        ARM,        // ARM32
        X86,        // x86 32位
        X86_64,     // x86 64位
        MIPS,       // MIPS
        UNKNOWN
    };
    
    // 构造函数
    DisassemblyHelper();
    ~DisassemblyHelper();
    
    // 禁用拷贝
    DisassemblyHelper(const DisassemblyHelper&) = delete;
    DisassemblyHelper& operator=(const DisassemblyHelper&) = delete;
    
    // 初始化反汇编引擎（指定架构）
    bool initialize(Architecture arch);
    
    // 清理资源
    void cleanup();
    
    // 反汇编单条指令
    // address: 指令的虚拟地址
    // code: 指令的字节码
    // codeSize: 字节码大小
    DisassemblyResult disassembleSingle(uint64_t address, const uint8_t* code, size_t codeSize);
    
    // 反汇编多条指令
    // address: 起始地址
    // code: 指令字节码
    // codeSize: 字节码总大小
    // maxInstructions: 最大反汇编指令数量（0 表示不限制）
    DisassemblyResult disassembleMultiple(uint64_t address, const uint8_t* code, 
                                         size_t codeSize, size_t maxInstructions = 10);
    
    // 检查是否已初始化
    bool isInitialized() const { return initialized; }
    
    // 获取当前架构
    Architecture getCurrentArchitecture() const { return currentArch; }
    
    // 获取架构名称
    static std::string getArchitectureName(Architecture arch);
    
    // Capstone 是否可用
    static bool isCapstoneAvailable();
    
private:
    bool initialized;
    Architecture currentArch;
    void* csHandle;  // Capstone 句柄（使用 void* 避免头文件依赖）
    
    // 格式化十六进制字节
    std::string formatHexBytes(const uint8_t* bytes, size_t size);
};
