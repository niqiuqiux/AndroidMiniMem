#ifndef MEM_READER_WRITER_PROXY_H_
#define MEM_READER_WRITER_PROXY_H_
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <mutex>
#include <thread>
#include <sstream>
#include <stdint.h>

#ifdef __linux__
#include <asm/unistd.h>
#include <unistd.h>
#include <sys/sysinfo.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define PAGE_NOACCESS 1
#define PAGE_READONLY 2
#define PAGE_READWRITE 4
#define PAGE_WRITECOPY 8
#define PAGE_EXECUTE 16
#define PAGE_EXECUTE_READ 32
#define PAGE_EXECUTE_READWRITE 64

#define MEM_MAPPED 262144
#define MEM_PRIVATE 131072
#else
#include <windows.h>
#endif

#pragma pack(1)
typedef struct {
	uint64_t baseaddress;
	uint64_t size;
	uint32_t protection;
	uint32_t type;
	char name[4096];
} DRIVER_REGION_INFO, *PDRIVER_REGION_INFO;
#pragma pack()


struct IMemReaderWriterProxy {
	virtual BOOL ReadProcessMemory(
		uint64_t hProcess,
		uint64_t lpBaseAddress,
		void *lpBuffer,
		size_t nSize,
		size_t * lpNumberOfBytesRead = NULL,
		BOOL bIsForceRead = FALSE) = 0;
	virtual BOOL WriteProcessMemory(
		uint64_t hProcess,
		uint64_t lpBaseAddress,
		void * lpBuffer,
		size_t nSize,
		size_t * lpNumberOfBytesWritten = NULL,
		BOOL bIsForceWrite = FALSE) = 0;
	virtual BOOL VirtualQueryExFull(
		uint64_t hProcess,
		BOOL showPhy,
		std::vector<DRIVER_REGION_INFO> & vOutput) = 0;

	virtual BOOL CheckProcessMemAddrValid(
		uint64_t hProcess,
		uint64_t lpBaseAddress) = 0;
};
#if defined(__linux__) && defined(__aarch64__)
static inline ssize_t _svc_call(long syscall_num,
                            long x0, long x1, long x2,
                            long x3, long x4, long x5) {
    register long r8  __asm__("x8") = syscall_num;
    register long r0  __asm__("x0") = x0;
    register long r1  __asm__("x1") = x1;
    register long r2  __asm__("x2") = x2;
    register long r3  __asm__("x3") = x3;
    register long r4  __asm__("x4") = x4;
    register long r5  __asm__("x5") = x5;

    __asm__ volatile (
        "svc #0"
        : "+r"(r0)
        : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r8)
        : "memory");

    return r0;
}
#endif

#endif /* MEM_READER_WRITER_PROXY_H_ */

