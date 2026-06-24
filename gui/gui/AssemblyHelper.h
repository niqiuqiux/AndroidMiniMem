#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "DisassemblyHelper.h"

// 汇编结果结构
struct AssemblyResult {
    bool success;                    // 是否成功
    std::vector<uint8_t> bytes;      // 汇编后的机器码字节
    size_t statementCount;           // 成功汇编的语句数
    std::string errorMessage;        // 错误信息

    AssemblyResult() : success(false), statementCount(0) {}
};

// 汇编助手类（Keystone 封装）
class AssemblyHelper {
public:
    AssemblyHelper();
    ~AssemblyHelper();

    // 禁用拷贝
    AssemblyHelper(const AssemblyHelper&) = delete;
    AssemblyHelper& operator=(const AssemblyHelper&) = delete;

    // 初始化汇编引擎（指定架构）
    bool initialize(DisassemblyHelper::Architecture arch);

    // 清理资源
    void cleanup();

    // 汇编指令
    // assembly: 汇编文本，如 "MOV X0, #1" 或多条 "MOV X0, #1; RET"
    // address: 指令的虚拟地址（用于PC相对寻址计算）
    AssemblyResult assemble(const std::string& assembly, uint64_t address = 0);

    // 检查是否已初始化
    bool isInitialized() const { return initialized; }

    // 获取当前架构
    DisassemblyHelper::Architecture getCurrentArchitecture() const { return currentArch; }

    // Keystone 是否可用
    static bool isKeystoneAvailable();

private:
    bool initialized;
    DisassemblyHelper::Architecture currentArch;
    void* ksHandle;  // Keystone 句柄（使用 void* 避免头文件依赖）
};
