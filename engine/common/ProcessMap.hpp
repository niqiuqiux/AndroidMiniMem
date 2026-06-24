#pragma once
#include <cstdint>
#include <string>
#include <sys/types.h>

enum MemoryType {
    All = -1,
    Anonymous = 1 << 5,//32
    C_Alloc = 1 << 2, //4
    C_Heap = 1 << 0, //1
    C_Data = 1 << 3, //8
    C_Bss = 1 << 4, //16
    Java_Heap = 1 << 1, //2
    Java = 1 << 16, //65536
    Stack = 1 << 6, //64
    Video = 1 << 20, //1048576
    Code_App = 1 << 14, //16384
    Code_System = 1 << 15, //32768
    Ashmem = 1 << 19, //524288
    Bad = 1 << 17, //131072
    Other = 1 << 21 //2097152
};


inline bool contains(const std::string &str, const std::string &substr) {
  return str.find(substr) != std::string::npos;
}

static inline int DetermineMemoryType(const std::string &name,
                                      const std::string &perms) {
  if ((name.empty() || name==" ")
   //&& (perms[0]=='r' /* || perms[1]=='w'*/) 
  ) {
    return MemoryType::Anonymous;
  }

  if (contains(name, "dalvik-allocation") || contains(name, "dalvik-main") ||
      contains(name, "dalvik-large") || contains(name, "dalvik-free")) {
    return MemoryType::Java_Heap;
  }

  if (contains(name, "dalvik-CompilerMetadata") ||
      contains(name, "dalvik-indirect") || contains(name, "dalvik-mark") ||
      contains(name, "dalvik-LinearAlloc") || contains(name, "dalvik-rosalloc") ||
      contains(name, "dalvik-card") ||
      (contains(name, "dalvik-") )) {
    return MemoryType::Java;
  }

  if (name.find("[anon:.bss") != std::string::npos) {
    return MemoryType::C_Bss;
  }
  if ((contains(name, "/data/app/") || contains(name, "/data/data")) 
  && perms.find("xp") != std::string::npos) {
    return MemoryType::Code_App;
  }

  if (contains(name, "/data/app/")) {
    return MemoryType::C_Data;
  }

  if (contains(name, "[anon:libc_malloc") || contains(name, "[anon:scudo:")) {
    return MemoryType::C_Alloc;
  }

  if (contains(name, "/dev/ashmem/")) {
    return MemoryType::Ashmem;
  }

  if (contains(name, "/system/fonts")) {
    return MemoryType::Bad;
  }

  if (name.starts_with("/system/framework/")
    || name.starts_with("/vendor/lib")
    || name.starts_with("/system/lib")
    || name.starts_with("/apex/com.android.")
  ) {
    return MemoryType::Code_System;
  }

  if (name == "[heap]") {
    return MemoryType::C_Heap;
  }

  if (contains(name, "[stack")) {
    return MemoryType::Stack;
  }

  if (contains(name, "/dev/kgsl-3d0")) {
    return MemoryType::Video;
  }



  return MemoryType::Other;
}

struct ProcessMap {
    pid_t pid;
    std::string allinfo;
    uintptr_t startAddress;
    uintptr_t endAddress;
    uintptr_t offset;
    size_t length;
    size_t inode;
    std::string perms;
    int protection;
    int type;
    std::string dev;
    std::string pathname;
    
    bool readable;
    bool writable; 
    bool executable;
    bool is_private;
    bool is_shared;
    int flag;

    ProcessMap() : pid(0), startAddress(0), endAddress(0), offset(0),
                  length(0), inode(0), protection(0), type(0),
                  readable(false), writable(false), executable(false),
                  is_private(false), is_shared(false) {}

    bool IsValid() const {
        return pid && startAddress && endAddress && length;
    }
    
    bool Contains(uintptr_t address) const {
        return address >= startAddress && address < endAddress;
    }

    bool IsUnknown() const {
        return pathname.empty();
    }
    std::string getType() const {
        switch (type) {
            case MemoryType::Anonymous:
                return "Anonymous";
            case MemoryType::C_Heap:
                return "C_Heap";
            case MemoryType::C_Alloc:
                return "C_Alloc";
            case MemoryType::C_Data:
                return "C_Data";
            case MemoryType::C_Bss:
                return "C_Bss";
            case MemoryType::Java_Heap:
                return "Java_Heap";
            case MemoryType::Java:
                return "Java";
            case MemoryType::Stack:
                return "Stack";
            case MemoryType::Video:
                return "Video";
            case MemoryType::Code_App:
                return "Code_App";
            case MemoryType::Code_System:
                return "Code_System";
            case MemoryType::Ashmem:
                return "Ashmem";
            case MemoryType::Bad:
                return "Bad";
            case MemoryType::Other:
                return "Other";
            default:
                return "Unknown";

        }
    }
}; 