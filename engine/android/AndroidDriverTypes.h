#ifndef ANDROID_DRIVER_TYPES_H_
#define ANDROID_DRIVER_TYPES_H_

#include <cstdint>

#ifdef __linux__
using BOOL = int;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define PAGE_NOACCESS 1
#define PAGE_READONLY 2
#define PAGE_READWRITE 4
#define PAGE_WRITECOPY 8
#define PAGE_EXECUTE 16
#define PAGE_EXECUTE_READ 32
#define PAGE_EXECUTE_READWRITE 64

#define MEM_MAPPED 262144
#define MEM_PRIVATE 131072
#endif

#pragma pack(push, 1)
struct DRIVER_REGION_INFO {
    uint64_t baseaddress;
    uint64_t size;
    uint32_t protection;
    uint32_t type;
    char name[4096];
};

struct my_user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t orig_x0;
    uint64_t syscallno;
};

struct HW_HIT_ITEM {
    uint64_t task_id;
    uint64_t hit_addr;
    uint64_t hit_time;
    my_user_pt_regs regs_info;
};
#pragma pack(pop)

#endif /* ANDROID_DRIVER_TYPES_H_ */
