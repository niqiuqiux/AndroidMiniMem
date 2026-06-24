#include "AssemblyHelper.h"

#ifdef HAVE_KEYSTONE
#include <keystone/keystone.h>
#endif

AssemblyHelper::AssemblyHelper()
    : initialized(false), currentArch(DisassemblyHelper::Architecture::UNKNOWN), ksHandle(nullptr) {
}

AssemblyHelper::~AssemblyHelper() {
    cleanup();
}

bool AssemblyHelper::isKeystoneAvailable() {
#ifdef HAVE_KEYSTONE
    return true;
#else
    return false;
#endif
}

bool AssemblyHelper::initialize(DisassemblyHelper::Architecture arch) {
#ifdef HAVE_KEYSTONE
    if (initialized) {
        cleanup();
    }

    ks_engine* ks = nullptr;
    ks_err err;

    switch (arch) {
        case DisassemblyHelper::Architecture::ARM64:
            err = ks_open(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN, &ks);
            break;
        case DisassemblyHelper::Architecture::ARM:
            err = ks_open(KS_ARCH_ARM, KS_MODE_ARM + KS_MODE_LITTLE_ENDIAN, &ks);
            break;
        case DisassemblyHelper::Architecture::X86:
            err = ks_open(KS_ARCH_X86, KS_MODE_32, &ks);
            break;
        case DisassemblyHelper::Architecture::X86_64:
            err = ks_open(KS_ARCH_X86, KS_MODE_64, &ks);
            break;
        case DisassemblyHelper::Architecture::MIPS:
            err = ks_open(KS_ARCH_MIPS, KS_MODE_MIPS64 + KS_MODE_LITTLE_ENDIAN, &ks);
            break;
        default:
            return false;
    }

    if (err != KS_ERR_OK) {
        return false;
    }

    ksHandle = reinterpret_cast<void*>(ks);
    currentArch = arch;
    initialized = true;
    return true;
#else
    return false;
#endif
}

void AssemblyHelper::cleanup() {
#ifdef HAVE_KEYSTONE
    if (initialized && ksHandle) {
        ks_engine* ks = reinterpret_cast<ks_engine*>(ksHandle);
        ks_close(ks);
        ksHandle = nullptr;
    }
#endif
    initialized = false;
    currentArch = DisassemblyHelper::Architecture::UNKNOWN;
}

AssemblyResult AssemblyHelper::assemble(const std::string& assembly, uint64_t address) {
    AssemblyResult result;

#ifdef HAVE_KEYSTONE
    if (!initialized || !ksHandle) {
        result.errorMessage = "汇编引擎未初始化";
        return result;
    }

    ks_engine* ks = reinterpret_cast<ks_engine*>(ksHandle);
    unsigned char* encode = nullptr;
    size_t size = 0;
    size_t count = 0;

    if (ks_asm(ks, assembly.c_str(), address, &encode, &size, &count) != KS_ERR_OK) {
        result.errorMessage = ks_strerror(ks_errno(ks));
        return result;
    }

    result.bytes.assign(encode, encode + size);
    result.statementCount = count;
    result.success = true;

    ks_free(encode);
#else
    result.errorMessage = "Keystone 库不可用 (未编译 HAVE_KEYSTONE)";
#endif

    return result;
}
