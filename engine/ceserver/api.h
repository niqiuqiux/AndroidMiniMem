#pragma once
#include <cstdint>
#include <stdint.h>
#include <pthread.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <vector>
#include <map>
#include "ceserver.h"
#include "porthelp.h"


#include <sys/user.h>


#include <atomic>
#include <string>
#include <mutex>

#include "ScanProgress.hpp"
#include "NiDriver.h"


#define VQE_PAGEDONLY 1
#define VQE_DIRTYONLY 2
#define VQE_NOSHARED 4




// 调试事件（对外与管道传输使用的通用结构）
struct DebugEvent {
	int event_type;     // 1=bp,2=wp,3=step,4=signal
	int tid;            // 线程ID
	uint64_t address;   // 地址
	int bp_index;       // 命中的断点槽
	int signal;         // 信号
};

typedef struct {
  int tid;
  int isPaused;
  int suspendCount;
  DebugEvent suspendedDevent; //debug event to be injected when resumed
} ThreadData, *PThreadData;


// 事件队列（仅占位，与 ProcessData 对齐）
struct DebugEventElement {
	TAILQ_ENTRY(DebugEventElement) entries;
	DebugEvent event;
};
TAILQ_HEAD(debugEventQueueHead, DebugEventElement);


//进程全部信息
typedef struct {
  int ReferenceCount;
  int pid;
  int is64bit;
  int mapfd; //file descriptor for /proc/pid/maps
  char *path;
  char *maps;
  int mem;
  int memrw; //Readwrite when set

  //注入读写so的
  int hasLoadedExtension; //set to true if the ceserver extension has been loaded in this process
  int neverForceLoadExtension; //set to true if you don't want to force load the module (if it's loaded, use it, but don't use the injection method)
  pthread_mutex_t extensionMutex;
  int extensionFD; //socket to communicate with the target

  int isDebugged; //if this is true no need to attach/detach constantly, BUT make sure the debugger thread does do it's job
  pthread_t debuggerThreadID;

  PThreadData threadlist;
  int threadlistmax;
  int threadlistpos;

  DebugEvent debuggedThreadEvent;

  int debuggerServer; //sockets for communicating with the debugger thread by local threads
  int debuggerClient;

  pthread_mutex_t debugEventQueueMutex; //probably not necessary as all queue operations are all done in the debuggerthread of the process

  struct debugEventQueueHead debugEventQueue;

//注入
  uintptr_t dlopen;
  uintptr_t dlerror;
  uintptr_t dlsym;
  int dlopenalt; //when not 0 this means that there is a 3th param: caller
  uintptr_t dlopencaller;
  uintptr_t mmap;
  uintptr_t libc;

//驱动获取的pidstruct 需要做好释放
  uint64_t u64DriverProcessHandle;
} ProcessData, *PProcessData;



struct ModuleListEntry {

	#ifdef ANDROID_CHEAT_ENGINE
	int type;
	int flag;
	#endif
	uint64_t baseAddress;
	int moduleSize;
	std::string moduleName;


};

struct ProcessListEntry {
	int PID;
	std::string ProcessName;

};


#pragma pack(1)
struct RegionInfo {
	uint64_t baseaddress;
	uint64_t size;
	uint32_t protection;
	uint32_t type;
};
#pragma pack()


struct MyProcessInfo {
	int pid;
	size_t total_rss;
	std::string cmdline;
};

struct CeProcessList {
	std::vector<struct MyProcessInfo> vProcessList;
	decltype(vProcessList)::iterator readIter;
};

struct CeModuleList {
	std::vector<ModuleListEntry> vModuleList;
	decltype(vModuleList)::iterator readIter;
};

struct CeOpenProcess {
	std::atomic<int> ReferenceCount{0};
	int pid;

	//注入
	uintptr_t libdl;
  uintptr_t dlopen;
  uintptr_t dlerror;
  uintptr_t dlsym;
    uintptr_t libc;
  uintptr_t mmap;
  uintptr_t munmap;


  //debug - 使用 map 优化查找性能，查找复杂度 O(log n)
 std::map<uint64_t, std::vector<uint64_t>> mHwBpList;
 std::mutex mHwBpMutex;

 std::map<uint64_t, uint32_t> mUxnBpList;
 std::mutex mUxnBpMutex;

 //构造函数
 CeOpenProcess() {
	mHwBpList.clear();
	mUxnBpList.clear();
	libdl = 0;
	dlopen = 0;
	dlerror = 0;
	dlsym = 0;
	libc = 0;
	mmap = 0;
	munmap = 0;
	pid = 0;
 }

};


	
struct ReadBratchMemoryOutput{
		int result;
		int readsize;
		std::vector<unsigned char> data;
};

#pragma pack(1)
struct _user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t orig_x0;
    uint64_t syscallno;
};

struct HW_HIT_INFO {

    uint64_t hit_addr;
    uint64_t hit_time;
    struct _user_pt_regs regs_info;
};
#pragma pack()

struct HwbpTaskEntryInfo {
	uint64_t eventId = 0;
	uint64_t moduleHandle = 0;
	uint64_t address = 0;
	int32_t tid = 0;
	int32_t onCpu = 0;
	uint32_t type = 0;
	uint32_t length = 0;
	uint32_t state = 0;
	uint32_t source = 0;
	uint32_t flags = 0;
};

struct HwbpTaskThreadInfo {
	int32_t tid = 0;
	bool success = false;
	int32_t errorCode = 0;
	uint32_t count = 0;
	uint32_t totalCount = 0;
	uint32_t brpCount = 0;
	uint32_t wrpCount = 0;
	uint32_t enabledCount = 0;
	uint32_t activeCount = 0;
	uint32_t perfCount = 0;
	uint32_t ptraceCount = 0;
	uint32_t moduleCount = 0;
	std::vector<HwbpTaskEntryInfo> entries;
};

class CApi {
public:
	static BOOL InitReadWriteDriver(const char* procNodeAuthKey,std::string& out_result);

	static unsigned char GetRWDriverType();
	static BOOL SetKernelBreakpointForceReclaim(BOOL enabled);
	static HANDLE CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID);
	static BOOL Process32First(HANDLE hSnapshot, ProcessListEntry & processentry);
	static BOOL Process32Next(HANDLE hSnapshot, ProcessListEntry &processentry);
	static BOOL Module32First(HANDLE hSnapshot, ModuleListEntry & moduleentry);
	static BOOL Module32Next(HANDLE hSnapshot, ModuleListEntry & moduleentry);
	static HANDLE OpenProcess(DWORD pid);
	static void CloseHandle(HANDLE h);
	static int VirtualQueryExFull(HANDLE hProcess, uint32_t flags, std::vector<RegionInfo> & vRinfo);
	static int VirtualQueryEx(HANDLE hProcess, uint64_t lpAddress, RegionInfo & rinfo, std::string & memName);
	static int ReadProcessMemory(HANDLE hProcess, void *lpAddress, void *buffer, int size);
	static int ReadBratchMemory(HANDLE hProcess, CeReadBratchMemory & input,std::vector<CeReadBratchMemoryOutput> & output);
	static int ReadBratchAddr(HANDLE hProcess,std::vector<CeReadBratchAddr> &input,std::vector<CeReadBratchAddrOutput> &output);
	static int WriteProcessMemory(HANDLE hProcess, void *lpAddress, void *buffer, int size);

	static void GetProcessListInfo(std::vector<MyProcessInfo>& vOutput);
	static void GetModuleList(HANDLE hProcess,std::vector<ModuleListEntry>& vOutput);
	static uint64_t GetSoBase(HANDLE hProcess, const std::string& soName);

	// 断点与调试控制（内核硬件断点）
	static int SetBreakpoint(HANDLE hProcess,  uint64_t address, int bpType, int bpSize);
	static int RemoveBreakpoint(HANDLE hProcess,uint64_t hwaddr);
	static int SuspendBreakpoint(HANDLE hProcess,uint64_t hwaddr);
	static int ResumeBreakpoint(HANDLE hProcess,uint64_t hwaddr);
	static int ReadHwBpInfo(HANDLE hProcess,uint64_t hwaddr,uint64_t& nHitTotalCount, std::vector<HW_HIT_INFO>& vOutput);
	static bool QueryHardwareBreakpointThreads(
		HANDLE hProcess, uint32_t capacity,
		std::vector<HwbpTaskThreadInfo>& vOutput);
	static bool InstallUxnBreakpoint(HANDLE hProcess, uint64_t address,
		uint32_t flags, ni_uxn_install& output, int& errorCode);
	static bool RemoveUxnBreakpoint(HANDLE hProcess, uint64_t address,
		int& errorCode);
	static bool WaitUxnBreakpoint(ni_uxn_wait& request, int& errorCode);
	static bool ResumeUxnBreakpoint(const ni_uxn_resume& request,
		int& errorCode);
	static bool GetUxnBreakpointStatus(uint32_t slot, ni_uxn_status& status,
		int& errorCode);
	static bool ClearUxnBreakpoints(int& errorCode);


// ELF 符号解析
static int SymbolInit(HANDLE hProcess, uint64_t moduleBase);
static int SymbolGetCount();
static std::vector<std::pair<uintptr_t, std::string>> SymbolGetList(int offset, int count);
static uintptr_t SymbolFind(HANDLE hProcess, uint64_t moduleBase, const std::string& name);

protected:
};
