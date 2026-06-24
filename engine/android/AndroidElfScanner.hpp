#pragma once
#include "../common/IElfScanner.hpp"
#include <cxxabi.h>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>


/*
关于符号表的问题
1.
是发送全部信息
还是根据地址查询对应的符号区域

2.
是解析文件还是解析内存
因为壳的缘故 有些文件或内存可能无法正常解析
*/
class AndroidElfScanner : public IElfScanner {
private:
    IMemoryOp* memOp_;
    uintptr_t elfBase_;
    
    Elf64_Ehdr ehdr_;// ELF头
    std::vector<Elf64_Phdr> phdrs_;// 程序头表
    std::vector<Elf64_Dyn> dynamics_;// 动态表
    
    uintptr_t loadBias_;
    size_t loadSize_;
    
    uintptr_t stringTable_;
    uintptr_t symbolTable_;
    size_t stringTableSize_;
    size_t symbolEntrySize_;
    
    ProcessMap baseSegment_;
    std::vector<ProcessMap> segments_;
    std::vector<std::pair<uintptr_t, std::string>> symbols_;
    bool symbolsInitialized_;

    std::string GetElfPath() const {
        if (!baseSegment_.pathname.empty() && baseSegment_.pathname[0] == '/') {
            return baseSegment_.pathname;
        }
        for (const auto& segment : segments_) {
            if (!segment.pathname.empty() && segment.pathname[0] == '/') {
                return segment.pathname;
            }
        }
        return "";
    }

    static bool ShouldSkipSymbolName(const char* name) {
        return !name || *name == '\0' || name[0] == '$';
    }

    static std::string GetReadableSymbolName(const char* rawName) {
        if (ShouldSkipSymbolName(rawName)) {
            return "";
        }

        int status = 0;
        char* demangled = abi::__cxa_demangle(rawName, nullptr, nullptr, &status);
        if (status == 0 && demangled) {
            std::string result(demangled);
            std::free(demangled);
            return result;
        }

        if (demangled) {
            std::free(demangled);
        }
        return std::string(rawName);
    }

    bool ReadFull(uintptr_t addr, void* buf, size_t len) const {
        return memOp_->Read(addr, buf, len) == len;
    }

    void Reset() {
        elfBase_ = 0;
        loadBias_ = 0;
        loadSize_ = 0;
        stringTable_ = 0;
        symbolTable_ = 0;
        stringTableSize_ = 0;
        symbolEntrySize_ = 0;
        symbolsInitialized_ = false;
        phdrs_.clear();
        dynamics_.clear();
        symbols_.clear();
        segments_.clear();
        baseSegment_ = ProcessMap();
    }

    bool ParseElfHeader() {
        // 读取ELF头
        if (!ReadFull(elfBase_, &ehdr_, sizeof(Elf64_Ehdr))) {
            return false;
        }

        // 验证ELF魔数
        if (memcmp(ehdr_.e_ident, ELFMAG, SELFMAG) != 0) {
            return false;
        }

        return true;
    }

    bool ParseProgramHeaders() {
        phdrs_.resize(ehdr_.e_phnum);

        // 读取程序头表
        if (!ReadFull(elfBase_ + ehdr_.e_phoff, phdrs_.data(),
                     ehdr_.e_phnum * sizeof(Elf64_Phdr))) {
            return false;
        }

        // 计算加载信息
        bool loadBiasSet = false;
        uintptr_t minVaddr = UINTPTR_MAX;
        uintptr_t maxEnd = 0;
        for (const auto& phdr : phdrs_) {
            if (phdr.p_type == PT_LOAD) {
                if (phdr.p_vaddr < minVaddr) {
                    minVaddr = phdr.p_vaddr;
                }
                uintptr_t segEnd = phdr.p_vaddr + phdr.p_memsz;
                if (segEnd > maxEnd) {
                    maxEnd = segEnd;
                }
                loadBiasSet = true;
            }
        }

        if (loadBiasSet) {
            loadBias_ = elfBase_ - minVaddr;
            loadSize_ = static_cast<size_t>(maxEnd - minVaddr);
        }

        return loadBiasSet && loadSize_;
    }

    bool ParseDynamics() {
        // 查找动态段
        const Elf64_Phdr* dynamic_phdr = nullptr;
        for (const auto& phdr : phdrs_) {
            if (phdr.p_type == PT_DYNAMIC) {
                dynamic_phdr = &phdr;
                break;
            }
        }
        
        if (!dynamic_phdr) return false;

        // 读取动态表
        size_t dyn_count = dynamic_phdr->p_memsz / sizeof(Elf64_Dyn);
        dynamics_.resize(dyn_count);
        
        if (!ReadFull(loadBias_ + dynamic_phdr->p_vaddr,
                     dynamics_.data(), dynamic_phdr->p_memsz)) {
            return false;
        }

        // 解析动态表信息
        for (const auto& dyn : dynamics_) {
            switch (dyn.d_tag) {
                case DT_SYMTAB:
                    symbolTable_ = loadBias_ + dyn.d_un.d_ptr;
                    break;
                case DT_STRTAB:
                    stringTable_ = loadBias_ + dyn.d_un.d_ptr;
                    break;
                case DT_STRSZ:
                    stringTableSize_ = dyn.d_un.d_val;
                    break;
                case DT_SYMENT:
                    symbolEntrySize_ = dyn.d_un.d_val;
                    break;
            }
        }

        return stringTable_ && symbolTable_ && stringTableSize_ && symbolEntrySize_;
    }

public:
    AndroidElfScanner() : memOp_(nullptr), elfBase_(0), loadBias_(0), loadSize_(0),
                         stringTable_(0), symbolTable_(0), stringTableSize_(0),
                         symbolEntrySize_(0), symbolsInitialized_(false) {}

    bool Initialize(IMemoryOp* memOp, uintptr_t elfBase) override {
        Reset();
        memOp_ = memOp;
        elfBase_ = elfBase;

        if (!memOp_ || !elfBase_) return false;

        bool ok = ParseElfHeader() && ParseProgramHeaders() && ParseDynamics();
        if (ok) {
            InitializeSegments();
        }
        return ok;
    }

    bool IsValid() const override {
        return memOp_ && elfBase_ && loadBias_ && loadSize_;
    }

    uintptr_t GetBase() const override {
        return elfBase_;
    }

    uintptr_t GetEnd() const override {
        return elfBase_ + loadSize_;
    }

    uintptr_t GetLoadBias() const override {
        return loadBias_;
    }

    size_t GetLoadSize() const override {
        return loadSize_;
    }

    const std::vector<Elf64_Phdr>& GetProgramHeaders() const override {
        return phdrs_;
    }

    const std::vector<Elf64_Dyn>& GetDynamics() const override {
        return dynamics_;
    }

    std::vector<std::pair<uintptr_t, std::string>> GetSymbols() override {
        if (symbolsInitialized_) {
            return symbols_;
        }

        symbols_.clear();
        if (!IsValid() || !symbolTable_ || !stringTable_) {
            return symbols_;
        }

        // 计算符号表大小
        size_t symCount = 0;

        // 优先使用 DT_HASH
        for (const auto& dyn : dynamics_) {
            if (dyn.d_tag == DT_HASH) {
                // DT_HASH: 第二个 uint32 是 nchain = 符号数量
                uint32_t hashHeader[2];
                if (ReadFull(loadBias_ + dyn.d_un.d_ptr, hashHeader, sizeof(hashHeader))) {
                    symCount = hashHeader[1]; // nchain
                }
                break;
            }
        }

        // 尝试 DT_GNU_HASH 推算符号数量
        if (symCount == 0) {
            for (const auto& dyn : dynamics_) {
                if (dyn.d_tag == DT_GNU_HASH) {
                    symCount = GetGnuHashSymCount(loadBias_ + dyn.d_un.d_ptr);
                    break;
                }
            }
        }

        // fallback: 用地址差估算
        if (symCount == 0 && stringTable_ > symbolTable_ && symbolEntrySize_ > 0) {
            symCount = (stringTable_ - symbolTable_) / symbolEntrySize_;
        }

        size_t symtab_size = symCount * symbolEntrySize_;
        if (symtab_size == 0) {
            return symbols_;
        }

        // 读取符号表和字符串表
        std::vector<char> symtab_buffer(symtab_size);
        std::vector<char> strtab_buffer(stringTableSize_ + 1); // +1 保证末尾 \0

        if (!ReadFull(symbolTable_, symtab_buffer.data(), symtab_size) ||
            !ReadFull(stringTable_, strtab_buffer.data(), stringTableSize_)) {
            return symbols_;
        }
        strtab_buffer[stringTableSize_] = '\0'; // 确保字符串表以 \0 结尾

        // 解析符号
        for (size_t offset = 0; offset < symtab_size; offset += symbolEntrySize_) {
            auto* sym = reinterpret_cast<Elf64_Sym*>(symtab_buffer.data() + offset);

            if (sym->st_name >= stringTableSize_ || sym->st_value == 0) {
                continue;
            }

            const char* name = strtab_buffer.data() + sym->st_name;
            std::string readableName = GetReadableSymbolName(name);
            if (readableName.empty()) {
                continue;
            }
            uintptr_t addr = loadBias_ + sym->st_value;

            symbols_.emplace_back(addr, readableName);
        }

        if (symbols_.empty()) {
            LoadSymbolsFromFile();
        }

        symbolsInitialized_ = true;
        return symbols_;
    }

    uintptr_t FindSymbol(const std::string& name) override {
        for (const auto& sym : GetSymbols()) {
            if (sym.second == name) {
                return sym.first;
            }
        }
        return 0;
    }

    ProcessMap GetBaseSegment() const override {
        if (!IsValid()) {
            return ProcessMap();
        }
        return baseSegment_;
    }

    std::vector<ProcessMap> GetSegments() const override {
        if (!IsValid()) {
            return std::vector<ProcessMap>();
        }
        return segments_;
    }

private:
    // 从 DT_GNU_HASH 推算符号数量
    // GNU hash 结构: [nbuckets, symoffset, bloom_size, bloom_shift, blooms..., buckets..., chains...]
    size_t GetGnuHashSymCount(uintptr_t gnuHashAddr) {
        uint32_t header[4]; // nbuckets, symoffset, bloom_size, bloom_shift
        if (!ReadFull(gnuHashAddr, header, sizeof(header))) {
            return 0;
        }

        uint32_t nbuckets = header[0];
        uint32_t symoffset = header[1];
        uint32_t bloom_size = header[2];

        if (nbuckets == 0) return 0;

        // 跳过 bloom filter，读取 buckets
        uintptr_t bucketsAddr = gnuHashAddr + sizeof(header) + bloom_size * sizeof(uint64_t);
        std::vector<uint32_t> buckets(nbuckets);
        if (!ReadFull(bucketsAddr, buckets.data(), nbuckets * sizeof(uint32_t))) {
            return 0;
        }

        // 找到最大的 bucket 值（最大符号索引起点）
        uint32_t maxBucket = 0;
        for (uint32_t b : buckets) {
            if (b > maxBucket) maxBucket = b;
        }

        if (maxBucket == 0) return symoffset; // 所有 bucket 为空

        // 从 chain 数组遍历到末尾（最低位为1表示链结束）
        uintptr_t chainsAddr = bucketsAddr + nbuckets * sizeof(uint32_t);
        uint32_t idx = maxBucket;
        while (true) {
            uint32_t chainVal;
            if (!ReadFull(chainsAddr + (idx - symoffset) * sizeof(uint32_t),
                         &chainVal, sizeof(chainVal))) {
                return 0;
            }
            if (chainVal & 1) break; // 链结束
            idx++;
        }

        return idx + 1; // 符号总数 = 最后一个索引 + 1
    }

    // 初始化段信息
    void InitializeSegments() {
        if (!memOp_) return;

        auto maps = memOp_->GetProcessMaps();
        uintptr_t elfEnd = elfBase_ + loadSize_;
        for (const auto& map : maps) {
            if (map.startAddress <= elfBase_ && map.endAddress > elfBase_) {
                baseSegment_ = map;
                segments_.push_back(map);
            } else if (map.startAddress > elfBase_ && map.startAddress < elfEnd) {
                segments_.push_back(map);
            }
        }
    }

    bool LoadSymbolsFromFile() {
        const std::string elfPath = GetElfPath();
        if (elfPath.empty()) {
            return false;
        }

        FILE* fp = std::fopen(elfPath.c_str(), "rb");
        if (!fp) {
            return false;
        }

        auto closeFile = [&fp]() {
            if (fp) {
                std::fclose(fp);
                fp = nullptr;
            }
        };

        Elf64_Ehdr fileEhdr{};
        if (std::fread(&fileEhdr, 1, sizeof(fileEhdr), fp) != sizeof(fileEhdr) ||
            std::memcmp(fileEhdr.e_ident, ELFMAG, SELFMAG) != 0 ||
            fileEhdr.e_shoff == 0 || fileEhdr.e_shnum == 0) {
            closeFile();
            return false;
        }

        if (std::fseek(fp, static_cast<long>(fileEhdr.e_shoff), SEEK_SET) != 0) {
            closeFile();
            return false;
        }

        std::vector<Elf64_Shdr> shdrs(fileEhdr.e_shnum);
        if (std::fread(shdrs.data(), sizeof(Elf64_Shdr), shdrs.size(), fp) != shdrs.size()) {
            closeFile();
            return false;
        }

        const Elf64_Shdr* symtab = nullptr;
        const Elf64_Shdr* strtab = nullptr;
        for (const auto& shdr : shdrs) {
            if (shdr.sh_type == SHT_SYMTAB && shdr.sh_link < shdrs.size()) {
                symtab = &shdr;
                strtab = &shdrs[shdr.sh_link];
                break;
            }
        }

        if (!symtab || !strtab || symtab->sh_entsize == 0 || strtab->sh_size == 0) {
            closeFile();
            return false;
        }

        std::vector<char> strtabBuffer(strtab->sh_size + 1, '\0');
        if (std::fseek(fp, static_cast<long>(strtab->sh_offset), SEEK_SET) != 0 ||
            std::fread(strtabBuffer.data(), 1, strtab->sh_size, fp) != strtab->sh_size) {
            closeFile();
            return false;
        }

        const size_t symCount = symtab->sh_size / symtab->sh_entsize;
        if (symCount == 0) {
            closeFile();
            return false;
        }

        if (std::fseek(fp, static_cast<long>(symtab->sh_offset), SEEK_SET) != 0) {
            closeFile();
            return false;
        }

        std::unordered_set<std::string> seen;
        seen.reserve(symbols_.size() + symCount);
        for (const auto& [addr, name] : symbols_) {
            seen.insert(std::to_string(addr) + '\n' + name);
        }

        for (size_t i = 0; i < symCount; ++i) {
            Elf64_Sym sym{};
            if (std::fread(&sym, 1, sizeof(sym), fp) != sizeof(sym)) {
                break;
            }

            if (sym.st_name >= strtab->sh_size || sym.st_value == 0) {
                continue;
            }

            const unsigned char symType = ELF64_ST_TYPE(sym.st_info);
            if (symType == STT_FILE || symType == STT_SECTION) {
                continue;
            }

            const char* name = strtabBuffer.data() + sym.st_name;
            const std::string readableName = GetReadableSymbolName(name);
            if (readableName.empty()) {
                continue;
            }

            const uintptr_t addr = loadBias_ + sym.st_value;
            const std::string key = std::to_string(addr) + '\n' + readableName;
            if (seen.insert(key).second) {
                symbols_.emplace_back(addr, readableName);
            }
        }

        closeFile();
        return !symbols_.empty();
    }
};
