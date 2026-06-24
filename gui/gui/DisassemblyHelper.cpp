#include "DisassemblyHelper.h"
#include <sstream>
#include <iomanip>

#ifdef HAVE_CAPSTONE
#include <capstone/capstone.h>
#endif

DisassemblyHelper::DisassemblyHelper() 
    : initialized(false), currentArch(Architecture::UNKNOWN), csHandle(nullptr) {
}

DisassemblyHelper::~DisassemblyHelper() {
    cleanup();
}

bool DisassemblyHelper::isCapstoneAvailable() {
#ifdef HAVE_CAPSTONE
    return true;
#else
    return false;
#endif
}

bool DisassemblyHelper::initialize(Architecture arch) {
#ifdef HAVE_CAPSTONE
    // 如果已经初始化，先清理
    if (initialized) {
        cleanup();
    }
    
    csh handle;
    cs_err err;
    
    // 根据架构类型初始化 Capstone
    switch (arch) {
        case Architecture::ARM64:
            // Capstone 对 ARM64/AArch64 的支持：
            // - CS_ARCH_ARM64: 如果定义了 CAPSTONE_AARCH64_COMPAT_HEADER
            // - CS_ARCH_AARCH64: 默认名称
            #ifdef CAPSTONE_AARCH64_COMPAT_HEADER
                err = cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle);
            #else
                err = cs_open(CS_ARCH_AARCH64, CS_MODE_ARM, &handle);
            #endif
            break;
        case Architecture::ARM:
            err = cs_open(CS_ARCH_ARM, CS_MODE_ARM, &handle);
            break;
        case Architecture::X86:
            err = cs_open(CS_ARCH_X86, CS_MODE_32, &handle);
            break;
        case Architecture::X86_64:
            err = cs_open(CS_ARCH_X86, CS_MODE_64, &handle);
            break;
        case Architecture::MIPS:
            err = cs_open(CS_ARCH_MIPS, CS_MODE_MIPS64, &handle);
            break;
        default:
            return false;
    }
    
    if (err != CS_ERR_OK) {
        return false;
    }
    
    // 设置反汇编细节级别
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    
    csHandle = reinterpret_cast<void*>(handle);
    currentArch = arch;
    initialized = true;
    
    return true;
#else
    return false;
#endif
}

void DisassemblyHelper::cleanup() {
#ifdef HAVE_CAPSTONE
    if (initialized && csHandle) {
        csh handle = reinterpret_cast<csh>(csHandle);
        cs_close(&handle);
        csHandle = nullptr;
    }
#endif
    initialized = false;
    currentArch = Architecture::UNKNOWN;
}

std::string DisassemblyHelper::formatHexBytes(const uint8_t* bytes, size_t size) {
    std::stringstream ss;
    for (size_t i = 0; i < size; i++) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)bytes[i];
        if (i < size - 1) {
            ss << " ";
        }
    }
    return ss.str();
}

DisassemblyResult DisassemblyHelper::disassembleSingle(uint64_t address, const uint8_t* code, size_t codeSize) {
#ifdef HAVE_CAPSTONE
    DisassemblyResult result;
    
    if (!initialized || !csHandle) {
        result.success = false;
        result.errorMessage = "Disassembly engine not initialized";
        return result;
    }
    
    if (!code || codeSize == 0) {
        result.success = false;
        result.errorMessage = "Invalid code buffer";
        return result;
    }
    
    csh handle = reinterpret_cast<csh>(csHandle);
    cs_insn* insn;
    
    // 反汇编一条指令
    size_t count = cs_disasm(handle, code, codeSize, address, 1, &insn);
    
    if (count > 0) {
        DisassembledInstruction di;
        di.address = insn[0].address;
        di.mnemonic = insn[0].mnemonic;
        di.operands = insn[0].op_str;
        di.fullInstruction = std::string(insn[0].mnemonic) + " " + std::string(insn[0].op_str);
        di.size = insn[0].size;
        di.hexBytes = formatHexBytes(insn[0].bytes, insn[0].size);
        
        result.instructions.push_back(di);
        result.success = true;
        
        cs_free(insn, count);
    } else {
        result.success = false;
        result.errorMessage = "Failed to disassemble instruction";
    }
    
    return result;
#else
    DisassemblyResult result;
    result.success = false;
    result.errorMessage = "Capstone not available (compile with HAVE_CAPSTONE)";
    return result;
#endif
}

DisassemblyResult DisassemblyHelper::disassembleMultiple(uint64_t address, const uint8_t* code, 
                                                        size_t codeSize, size_t maxInstructions) {
#ifdef HAVE_CAPSTONE
    DisassemblyResult result;
    
    if (!initialized || !csHandle) {
        result.success = false;
        result.errorMessage = "Disassembly engine not initialized";
        return result;
    }
    
    if (!code || codeSize == 0) {
        result.success = false;
        result.errorMessage = "Invalid code buffer";
        return result;
    }
    
    csh handle = reinterpret_cast<csh>(csHandle);
    cs_insn* insn;
    
    // 反汇编多条指令（0 表示尽可能多）
    size_t count = cs_disasm(handle, code, codeSize, address, maxInstructions, &insn);
    
    if (count > 0) {
        for (size_t i = 0; i < count; i++) {
            DisassembledInstruction di;
            di.address = insn[i].address;
            di.mnemonic = insn[i].mnemonic;
            di.operands = insn[i].op_str;
            di.fullInstruction = std::string(insn[i].mnemonic) + " " + std::string(insn[i].op_str);
            di.size = insn[i].size;
            di.hexBytes = formatHexBytes(insn[i].bytes, insn[i].size);
            
            result.instructions.push_back(di);
        }
        result.success = true;
        
        cs_free(insn, count);
    } else {
        // 检查 Capstone 错误代码
        cs_err err = cs_errno(handle);
        result.success = false;
        
        // 根据错误代码提供更详细的错误信息
        if (err == CS_ERR_OK) {
            // 没有错误，但也没有反汇编出指令（可能是数据不是有效的指令）
            result.errorMessage = "无法反汇编：数据不是有效的指令（可能是数据段而非代码段）";
        } else {
            // 有错误，根据错误代码提供详细信息
            switch (err) {
                case CS_ERR_MEM:
                    result.errorMessage = "Capstone 内存分配失败";
                    break;
                case CS_ERR_ARCH:
                    result.errorMessage = "不支持的架构";
                    break;
                case CS_ERR_HANDLE:
                case CS_ERR_CSH:
                    result.errorMessage = "无效的 Capstone 句柄";
                    break;
                case CS_ERR_MODE:
                    result.errorMessage = "不支持的架构模式";
                    break;
                case CS_ERR_DETAIL:
                    result.errorMessage = "详细信息不可用";
                    break;
                case CS_ERR_MEMSETUP:
                    result.errorMessage = "内存设置失败";
                    break;
                case CS_ERR_VERSION:
                    result.errorMessage = "不支持的版本";
                    break;
                default:
                    result.errorMessage = "反汇编失败（错误代码: " + std::to_string(err) + "）";
                    break;
            }
        }
    }
    
    return result;
#else
    DisassemblyResult result;
    result.success = false;
    result.errorMessage = "Capstone not available (compile with HAVE_CAPSTONE)";
    return result;
#endif
}

std::string DisassemblyHelper::getArchitectureName(Architecture arch) {
    switch (arch) {
        case Architecture::ARM64:   return "ARM64/AArch64";
        case Architecture::ARM:     return "ARM32";
        case Architecture::X86:     return "x86 (32-bit)";
        case Architecture::X86_64:  return "x86-64 (64-bit)";
        case Architecture::MIPS:    return "MIPS";
        default:                    return "Unknown";
    }
}
