#include "api.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <malloc.h>
#include <regex>
#include <sstream>
#include <memory>
#include <random>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <cinttypes>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <algorithm>
#include <atomic>
#include <sys/stat.h>
#include <errno.h>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include "LinuxProc.hpp"
#include "Logger.hpp"
#include "ceserver.h"
#include "porthelp.h"
#include <sys/syscall.h>
#include <sys/klog.h>

//#include "AndroidMemoryIO.hpp"//pread mem
#include "AndroidMemorySys.hpp"//sys_process_vm_readv
#include "AndroidMemKernel.hpp"//ko
#include "MemoryReaderWriter.h"//硬件断点驱动（必需，driver_变量定义在此）

#include "AndroidTracer.hpp"
#include "AndroidElfScanner.hpp"

#include "klog.hpp"
#include "utils.h"

//todo:support more process
static std::atomic<int> g_pid{0};
std::unique_ptr<IMemoryOp> g_memIO = std::make_unique<AndroidMemorySys>();
//std::unique_ptr<IMemoryOp> g_memIO = std::make_unique<AndroidMemKernel>();
std::unique_ptr<AndroidTracer> g_tracer = std::make_unique<AndroidTracer>();
std::unique_ptr<AndroidElfScanner> g_sym = std::make_unique<AndroidElfScanner>();

// 全局互斥锁：保护 g_memIO 等全局单例的替换和并发访问
static std::shared_mutex g_globalMutex;


//static SingleCodeClient client;


static std::vector<uint8_t> read_or_create_binary(const std::string& path, std::function<std::vector<uint8_t>()> generator) {
    // 尝试以二进制模式读取文件
    std::ifstream ifs(path, std::ios::binary);
    if (ifs.good()) {
        // 读取整个文件内容
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)), 
                                 std::istreambuf_iterator<char>());
        return data;
    }
    
    // 如果文件不存在或读取失败，生成新数据并直接覆盖写入
    std::vector<uint8_t> data = generator();
    
    // 以二进制模式直接覆盖写入（trunc模式会清空文件）
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    
    return data;
}




static std::string XorString(const std::string &str, const std::string &key) {
	std::string result;
	result.reserve(str.size());
	for (size_t i = 0; i < str.size(); ++i) {
		result.push_back(str[i] ^ key[i % key.size()]);
	}
	return result;
}


static bool ExtractEndTimestampFromKernelLog(std::string &out_end_ts, std::string &out_info) {
	// 获取内核日志缓冲区大小
	int buf_size = klogctl(10 /*SYSLOG_ACTION_SIZE_BUFFER*/, nullptr, 0);
	if (buf_size <= 0) {
		// 尝试获取未读大小
		buf_size = klogctl(9 /*SYSLOG_ACTION_SIZE_UNREAD*/, nullptr, 0);
		if (buf_size <= 0) {
			out_info = "无法获取内核日志大小";
			return false;
		}
	}

	std::string buffer;
	buffer.resize(static_cast<size_t>(buf_size) + 1);
	int read_len = klogctl(3 /*SYSLOG_ACTION_READ_ALL*/, buffer.data(), static_cast<int>(buffer.size() - 1));
	if (read_len < 0) {
		out_info = std::string("读取内核日志失败: ") + strerror(errno);
		return false;
	}
	buffer[static_cast<size_t>(read_len)] = '\0';

	// 查找形如 "endTimestamp: <digits>" 的最后一次出现
	std::regex re("endTimestamp:\\s*([0-9]+)");
	std::sregex_iterator it(buffer.begin(), buffer.end(), re);
	std::sregex_iterator end;
	std::string last_match;
	for (; it != end; ++it) {
		if ((*it).size() >= 2) {
			last_match = (*it)[1].str();
		}
	}

	if (last_match.empty()) {
		out_info = "未在内核日志中找到 endTimestamp";
		return false;
	}

	// 提取成功后清空内核日志
	if (klogctl(5 /*SYSLOG_ACTION_CLEAR*/, nullptr, 0) < 0) {
		out_info = std::string("提取成功，但清空内核日志失败: ") + strerror(errno);
		// 仍然返回成功，因为时间戳已获取
	}

	out_end_ts = last_match;
	return true;
}

BOOL CApi::InitReadWriteDriver(const char *procNodeAuthKey,
                               std::string &out_result) {
  out_result = "加载模块失败";

  // 先尝试通过 anon_fd 连接驱动
  std::unique_ptr<AndroidMemKernel> memKernel = std::make_unique<AndroidMemKernel>();
  if (memKernel->connect()) {
    // 连接成功
    {
      std::unique_lock<std::shared_mutex> wlock(g_globalMutex);
      g_memIO = std::move(memKernel);
    }

    uint64_t cardTime = 0;
    {
      std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
      g_memIO->GetCardTime(cardTime);
    }
    out_result = std::to_string(cardTime);

    LOGDF("InitReadWriteDriver: connected to existing driver via anon_fd");
    return 2;
  }

  LOGDF("InitReadWriteDriver: driver not loaded, trying to load module...");

  // 连接失败，继续执行现有的模块加载流程
  // 分割字符串
  std::string procNodeAuthKeyStr = std::string(procNodeAuthKey);
  ssize_t pos = procNodeAuthKeyStr.find_last_of("-");
  if (pos == -1) {
    out_result = "procNodeAuthKey格式错误";
    return FALSE;
  }
  std::string card = procNodeAuthKeyStr.substr(0, pos);
  std::string kernelType = procNodeAuthKeyStr.substr(pos + 1);

  //先解析域名获取ip 列表
  std::vector<std::string> ips;
  if (!resolve("yz.blyfw.cn", ips)||ips.empty()) {
    out_result = "域名解析失败，无法进行登录";
    return FALSE;
  }

  std::string MacPath = "/data/adb/.aceMacc";
    // mac
	std::string mac = readfs(MacPath, []() {
		return GenerateRandomDigitString(16); });
			//先清空文件
	file_clear(MacPath);
	//写入mac到文件
	writefs_append(MacPath, mac);
	writefs_append(MacPath, "\n");
	//写入ip列表到文件
	for (const auto& ip : ips) {
		writefs_append(MacPath, ip+"\n");
	}



  // 目标模块路径尝试顺序：5系先cfi，再mem 6系直接mem
  std::string modulePath = GetLocalPath();
  if (kernelType == "5") {
    std::string cfiPath = modulePath + "/CFI.ko";
    if (access(cfiPath.c_str(), F_OK) != 0) {
      // cfi不存在
      out_result = "CFI.ko 不存在";
      LOGEF("InitReadWriteDriver: CFI module not found: %s", cfiPath.c_str());
      return FALSE;
    }
    // 加载cfi
    int fd = open(cfiPath.c_str(), O_RDONLY);
    if (fd < 0) {
      out_result = std::string("打开模块失败: ") + strerror(errno);
      LOGEF("InitReadWriteDriver: CFI open failed: %s", strerror(errno));
      return FALSE;
    }

    int ret = -1;

    ret = syscall(SYS_finit_module, fd, "", 0);

    int saved_errno = errno;
    close(fd);
    if (ret < 0 && saved_errno != EEXIST) {
      out_result = std::string("加载模块失败: ") + strerror(saved_errno);
      LOGEF("InitReadWriteDriver: CFI load failed: %s", strerror(saved_errno));
      return FALSE;
    }
  }

  std::string memPath = modulePath + "/Mem.ko";

  if (memPath.empty() || access(memPath.c_str(), F_OK) != 0) {
    memPath = "/data/local/tmp/Mem.ko";
  }

  if (access(memPath.c_str(), F_OK) != 0) {
    out_result = "Mem.ko 不存在: " + memPath;
    LOGEF("InitReadWriteDriver: Mem module not found: %s", memPath.c_str());
    return FALSE;
  }


  std::string params = "card=" + card;

  // 优先使用 finit_module(syscall)
  int fd = open(memPath.c_str(), O_RDONLY);
  if (fd < 0) {
    out_result = std::string("打开模块失败: ") + strerror(errno);
    LOGEF("InitReadWriteDriver: Mem open failed: %s", strerror(errno));
    return FALSE;
  }

  int ret = -1;

  ret = syscall(SYS_finit_module, fd, params.c_str(), 0);

  int saved_errno = errno;
  close(fd);

  if (ret != 0) {

    KernelLogMatches logs;
    std::string info;
    ExtractMemAndNetVerifyLogsFromKernelLog(logs, info);
    LOGE("=== Mem 日志 ===");
    for (auto inf : logs.mem_logs) {
      LOGEF("%s", inf.c_str());
    }

    LOGE("=== NET_VERIFY 日志 ===");
    for (auto inf : logs.net_verify_logs) {
      LOGEF("%s", inf.c_str());
    }

    std::string err_msg = "加载模块失败：";
    if (ret == -6) {
      err_msg += "域名解析失败";
    } else if (ret == -7) {
      err_msg += "版本检测失败";
    } else if (ret == -8) {
      err_msg += "登录失败";
    } else {
      err_msg += strerror(saved_errno);
    }
    err_msg += " code " + std::to_string(saved_errno);
    out_result = err_msg;
    LOGEF("InitReadWriteDriver: Mem load failed(%d): %s", saved_errno,
          err_msg.c_str());
    return FALSE;
  }

  out_result = "加载模块成功";
  LOGDF("InitReadWriteDriver: Mem module loaded, card %s\n", card.c_str());

  // 模块加载成功后，再尝试连接
  memKernel = std::make_unique<AndroidMemKernel>();
  if (!memKernel->connect()) {
    out_result = "驱动连接失败";
    LOGEF("InitReadWriteDriver: driver connect failed after load");
    return FALSE;
  }

  {
    std::unique_lock<std::shared_mutex> wlock(g_globalMutex);
    g_memIO = std::move(memKernel);
  }

  uint64_t cardTime = 0;
  {
    std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
    g_memIO->GetCardTime(cardTime);
  }
  out_result = std::to_string(cardTime);
  ClearKernelLog();

  return 1;
}

unsigned char CApi::GetRWDriverType() {
	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
	return g_memIO->type;
}



//获取进程列表信息
BOOL _GetProcessListInfo(IMemoryOp* pDriver, BOOL bGetPhyMemorySize, std::vector<MyProcessInfo>& vOutput) {
	//驱动_获取进程PID列表
	std::vector<std::pair<int, std::string>> vPID;
	BOOL bOutListCompleted;
	vPID = pDriver->GetProcessPidList();
	for (const auto& [pid, name] : vPID) {
		MyProcessInfo pInfo = { 0 };
		pInfo.pid = pid;
		if (bGetPhyMemorySize) {
			//todo
			//pInfo.total_rss =pDriver->GetProcessPhyMemSize(pid);
		}
		pInfo.cmdline = name;
		vOutput.push_back(pInfo);
	}
	
	return TRUE;
}


void CApi::GetProcessListInfo(std::vector<MyProcessInfo>& vOutput) {
	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
	_GetProcessListInfo(g_memIO.get(), FALSE, vOutput);
}


HANDLE CApi::CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID) {

	if (dwFlags & TH32CS_SNAPPROCESS) {
		////printf("TH32CS_SNAPPROCESS\n");
		//获取进程列表
		CeProcessList * pCeProcessList = new CeProcessList();

		//获取进程列表
		{
			std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
			_GetProcessListInfo(g_memIO.get(), FALSE, pCeProcessList->vProcessList);
		}


		pCeProcessList->readIter = pCeProcessList->vProcessList.begin();
		return CPortHelper::CreateHandleFromPointer((uint64_t)pCeProcessList, htTHSProcess);
	} else if (dwFlags & TH32CS_SNAPMODULE) {
		//获取模块列表
		////printf("TH32CS_SNAPMODULE\n");
		HANDLE hm = CPortHelper::FindHandleByPID(th32ProcessID);
		if (!hm) {
			//如果没有打开此进程，就不允许获取此进程的模块列表
			return 0;
		}

		//CeOpenProcess *pCeOpenProcess = (CeOpenProcess*)CPortHelper::GetPointerFromHandle(hm);

		//取出驱动进程句柄
		//uint64_t u64DriverProcessHandle = pCeOpenProcess->u64DriverProcessHandle;

		//驱动_获取进程内存块列表
		std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
		std::vector<ProcessMap> vMaps = g_memIO->GetProcessMaps();
		rlock.unlock();
		LOGDF("Call GetProcessMaps return:%zu\n", vMaps.size());
		if (!vMaps.size()) {
			LOGD("GetProcessMaps failed\n");
			return 0;
		}
		CeModuleList * pCeModuleList = new CeModuleList();
		for (const auto& map : vMaps) {

#ifdef CHEAT_ENGINE
			int isExist = 0;
			for (auto iter = pCeModuleList->vModuleList.begin(); iter != pCeModuleList->vModuleList.end(); iter++) {
				if (iter->moduleName == std::string(map.pathname)) {
					isExist = 1;
					ModuleListEntry newReplace = *iter;
					newReplace.moduleSize += map.length;
					iter = pCeModuleList->vModuleList.insert(iter, newReplace);
					iter++;
					if (iter != pCeModuleList->vModuleList.end()) {
						pCeModuleList->vModuleList.erase(iter);

						break;
					}
				}
			}
			if (isExist) {
				continue;
			}

			uint32_t magic = 0;
			{
				std::shared_lock<std::shared_mutex> rlock2(g_globalMutex);
				BOOL b = g_memIO->Read( map.startAddress, &magic, 4);
			}
			if (b == FALSE) {
				////printf("%s is unreadable(%llx)\n", modulepath, start);
				continue; //unreadable
			}
			if (magic != 0x464c457f) //  7f 45 4c 46
			{
				////printf("%s is not an ELF(%llx).  tempbuf=%s\n", modulepath, start, tempbuf);
				continue; //not an ELF
			}
#endif
			ModuleListEntry newModInfo;
			newModInfo.baseAddress = map.startAddress;
			newModInfo.moduleSize = map.length;
			newModInfo.moduleName = map.pathname;
			////printf("%s\n", newModInfo.moduleName.c_str());
			#ifdef ANDROID_CHEAT_ENGINE
			newModInfo.type = map.type;
			#endif

			pCeModuleList->vModuleList.push_back(newModInfo);

			////printf("+++Start:%llx,Size:%lld,Protection:%d,Type:%d,Name:%s\n", map.start, map.end - map.start, map.protection, map.type, map.name.c_str());
		}


		pCeModuleList->readIter = pCeModuleList->vModuleList.begin();
		return CPortHelper::CreateHandleFromPointer((uint64_t)pCeModuleList, htTHSModule);
	}


	return 0;
}


BOOL CApi::Process32First(HANDLE hSnapshot, ProcessListEntry & processentry) {
	//Get a processentry from the processlist snapshot. fill the given processentry with the data.

	if (CPortHelper::GetHandleType(hSnapshot) == htTHSProcess) {
		CeProcessList *pCeProcessList = (CeProcessList*)CPortHelper::GetPointerFromHandle(hSnapshot);
		pCeProcessList->readIter = pCeProcessList->vProcessList.begin();
		if (pCeProcessList->readIter != pCeProcessList->vProcessList.end()) {
			processentry.PID = pCeProcessList->readIter->pid;
			processentry.ProcessName = pCeProcessList->readIter->cmdline;

			return TRUE;
		}
	}
	return FALSE;
}


BOOL CApi::Process32Next(HANDLE hSnapshot, ProcessListEntry &processentry) {
	//get the current iterator of the list and increase it. If the max has been reached, return false
   // //printf("Process32Next\n");

	if (CPortHelper::GetHandleType(hSnapshot) == htTHSProcess) {
		CeProcessList *pCeProcessList = (CeProcessList*)CPortHelper::GetPointerFromHandle(hSnapshot);
		pCeProcessList->readIter++;
		if (pCeProcessList->readIter != pCeProcessList->vProcessList.end()) {
			processentry.PID = pCeProcessList->readIter->pid;
			processentry.ProcessName = pCeProcessList->readIter->cmdline;
			return TRUE;
		}
	}
	return FALSE;
}


BOOL CApi::Module32First(HANDLE hSnapshot, ModuleListEntry & moduleentry) {
	if (CPortHelper::GetHandleType(hSnapshot) == htTHSModule) {
		CeModuleList *pCeModuleList = (CeModuleList*)CPortHelper::GetPointerFromHandle(hSnapshot);

		pCeModuleList->readIter = pCeModuleList->vModuleList.begin();
		if (pCeModuleList->readIter != pCeModuleList->vModuleList.end()) {
			moduleentry.baseAddress = pCeModuleList->readIter->baseAddress;
			moduleentry.moduleSize = pCeModuleList->readIter->moduleSize;
			moduleentry.moduleName = pCeModuleList->readIter->moduleName;
			return TRUE;
		}
	}
	return FALSE;
}

BOOL CApi::Module32Next(HANDLE hSnapshot, ModuleListEntry & moduleentry) {
	//get the current iterator of the list and increase it. If the max has been reached, return false
	//printf("Module32First/Next(%d)\n", hSnapshot);
	if (CPortHelper::GetHandleType(hSnapshot) == htTHSModule) {
		CeModuleList *pCeModuleList = (CeModuleList*)CPortHelper::GetPointerFromHandle(hSnapshot);
		pCeModuleList->readIter++;
		if (pCeModuleList->readIter != pCeModuleList->vModuleList.end()) {
			moduleentry.baseAddress = pCeModuleList->readIter->baseAddress;
			moduleentry.moduleSize = pCeModuleList->readIter->moduleSize;
			moduleentry.moduleName = pCeModuleList->readIter->moduleName;
			return TRUE;
		}
	}
	return FALSE;
}



HANDLE CApi::OpenProcess(DWORD pid) {
	//check if this process has already been opened
	HANDLE hm = CPortHelper::FindHandleByPID(pid);
	if (hm) {
		CeOpenProcess* pd =(CeOpenProcess*)CPortHelper::GetPointerFromHandle(hm);
		pd->ReferenceCount++;
		std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
		BOOL b = g_memIO->OpenProcess(pid);
		rlock.unlock();
		// if (!b) {
		// 	return 0;
		// }
		g_pid = pid;
		g_tracer->Initialize(pid);
		return hm;
	}
	//still here, so not opened yet

	//驱动_打开进程
	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
	uint64_t u64DriverProcessHandle =
	g_memIO->OpenProcess(pid);
	rlock.unlock();
	if (u64DriverProcessHandle == 0) {
		return 0;
	}
	g_tracer->Initialize(pid);
	g_pid = pid;

	// ❌ 不能使用 memset！CeOpenProcess 包含 std::map 等 C++ 对象
	CeOpenProcess *pCeOpenProcess = new CeOpenProcess();
	pCeOpenProcess->pid = pid;

	return CPortHelper::CreateHandleFromPointer((uint64_t)pCeOpenProcess, htProcesHandle);
}


void CApi::CloseHandle(HANDLE h) {

	int i;
	handleType ht = CPortHelper::GetHandleType(h);
	uint64_t pl = CPortHelper::GetPointerFromHandle(h);

	//printf("CloseHandle %d %" PRIu64 "\n", h, pl);

	if (ht == htTHSModule) {
		auto pCeModuleList = (CeModuleList*)pl;
		delete pCeModuleList;
	} else if (ht == htTHSProcess) {
		auto pProcessList = (CeProcessList*)pl;
		delete pProcessList;
	} else if (ht == htProcesHandle) {
		auto pOpenProcess = (CeOpenProcess*)pl;

		{
			std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
			g_memIO->CloseHandle();
		}
		// 不自动停止调试，交由调用方控制 StopDebug
		delete pOpenProcess;
	}
	// else
	// {
	// 	if (ht == htNativeThreadHandle)
	// 	{
	// 		uint64_t *th = (uint64_t*)CPortHelper::GetPointerFromHandle(h);
	// 		//printf("Closing thread handle\n");

	// 		free(th);
	// 		CPortHelper::RemoveHandle(h);
	// 	}
	// 	else
	// 	{
	// 		CPortHelper::RemoveHandle(h); //no idea what it is...
	// 	}

	// }
	CPortHelper::RemoveHandle(h);
}



int CApi::VirtualQueryExFull(HANDLE hProcess, uint32_t flags, std::vector<RegionInfo> & vRinfo)
/*
 * creates a full list of the maps file (less seeking)
 */
{
	//printf("VirtualQueryExFull: %d \n", hProcess);

	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		//printf("VirtualQueryExFull handle Error: %d \n", hProcess);
		return 0;
	}
	//CeOpenProcess *pCeOpenProcess = (CeOpenProcess*)CPortHelper::GetPointerFromHandle(hProcess);

	//取出驱动进程句柄
	//uint64_t u64DriverProcessHandle = pCeOpenProcess->u64DriverProcessHandle;

	//int pagedonly = flags & VQE_PAGEDONLY;
	//int dirtyonly = flags & VQE_DIRTYONLY;
	//int noshared = flags & VQE_NOSHARED;

	vRinfo.clear();

	//驱动_获取进程内存块列表
	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
	std::vector<ProcessMap> vMaps = g_memIO->GetProcessMaps();
	rlock.unlock();
	LOGDF("Call GetProcessMaps return:%zu\n", vMaps.size());
	if (!vMaps.size()) {
		LOGD("GetProcessMaps failed\n");
		return 0;
	}
	for (auto & map : vMaps) {

		//按flag过滤
	// 	if (rinfo.protection == PAGE_NOACCESS) {
	// 		//此地址不可访问
	// 		continue;
	// 	} else if (rinfo.type == MEM_MAPPED) //some checks to see if it passed
	// 	{
	// 		if (noshared) {
	// 			continue;
	// 		}
	// 	}

		RegionInfo newInfo = { 0 };
		newInfo.baseaddress = map.startAddress;
		newInfo.size = map.length;
		newInfo.protection = map.protection;
		newInfo.type = map.type;
		vRinfo.push_back(newInfo);
	}

	return 1;

}

int CApi::VirtualQueryEx(HANDLE hProcess, uint64_t lpAddress, RegionInfo & rinfo, std::string & memName) {
	/*
	 * Alternate method: read pagemaps and look up the pfn in /proc/kpageflags (needs to 2 files open and random seeks through both files, so not sure if slow or painfully slow...)
	 */

	 //VirtualQueryEx stub port. Not a real port, and returns true if successful and false on error
	int found = 0;

	////printf("VirtualQueryEx %d (%p)\n", hProcess, lpAddress);


	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	//CeOpenProcess *pCeOpenProcess = (CeOpenProcess*)CPortHelper::GetPointerFromHandle(hProcess);
	//以lpaddress为终止地址过滤

	//取出驱动进程句柄
	//uint64_t u64DriverProcessHandle = pCeOpenProcess->u64DriverProcessHandle;
	// std::vector<DRIVER_REGION_INFO> vMaps;
	// BOOL b = g_driver.VirtualQueryExFull(u64DriverProcessHandle, FALSE, vMaps);
	// //printf("Call VirtualQueryExFull(FALSE) return:%d, size:%zu\n", b, vMaps.size());
	// fflush(stdout);
	// if (!vMaps.size()) {
	// 	//printf("VirtualQueryExFull failed\n");
	// 	fflush(stdout);
	// 	return 0;
	// }
	// rinfo.protection = 0;
	// rinfo.baseaddress = (uint64_t)lpAddress & ~0xfff;
	// lpAddress = (uint64_t)(rinfo.baseaddress);

	// //显示进程内存块地址列表
	// for (const DRIVER_REGION_INFO & r : vMaps) {
	// 	uint64_t stop = r.baseaddress + r.size;
	// 	if (stop > lpAddress) //we passed it
	// 	{
	// 		found = 1;

	// 		if (lpAddress >= r.baseaddress) {
	// 			//it's inside the region, so useable

	// 			rinfo.protection = r.protection;
	// 			rinfo.type = r.type;
	// 			rinfo.size = stop - rinfo.baseaddress;
	// 		} else {
	// 			rinfo.size = r.baseaddress - rinfo.baseaddress;
	// 			rinfo.protection = PAGE_NOACCESS;
	// 			rinfo.type = 0;
	// 		}
	// 		memName = r.name;

	// 		////printf("+++Start:%llx,Size:%lld %lld,Protection:%d,Type:%d\n", rinfo.baseaddress, rinfo.size, r.size, rinfo.protection, rinfo.type);
	// 		break;
	// 	}
	// }

	return found;
}




int CApi::ReadProcessMemory(HANDLE hProcess, void *lpAddress, void *buffer, int size) {
	//idea in case this is too slow. always read a full page and keep the last 16 accessed pages.
	//only on cache miss, or if the cache is older than 1000 milliseconds fetch the page.
	//keep in mind that this routine can get called by multiple threads at the same time

	size_t bread = 0;

	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	//驱动_读取进程内存
	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
	bread = g_memIO->Read( (uint64_t)lpAddress, buffer, size);

	return (int)bread;
}

int CApi::ReadBratchMemory(HANDLE hProcess, CeReadBratchMemory&input,
	std::vector<CeReadBratchMemoryOutput> & output){
	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);

	//input 是 一个起始地址 和 大小
	//output 是 一个数组 每个元素对应有效的页面
	ssize_t Size = input.size;
	uint64_t addr = input.addr;
	uint64_t end = addr + Size;
	//todo 一次读多个页
	for(;addr < end; addr += PAGE_SIZE){
		CeReadBratchMemoryOutput o;
		o.addr = addr;
		o.data.resize(PAGE_SIZE);
		int bread = g_memIO->Read(addr, o.data.data(), PAGE_SIZE);
		if(bread > 0){
			output.push_back(o);
		}
	}

	return output.size();
}


int CApi::ReadBratchAddr(HANDLE hProcess,std::vector<CeReadBratchAddr> &input,std::vector<CeReadBratchAddrOutput> &output){
	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	output.clear();
	if (input.empty()) {
		return 0;
	}

	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
	output.reserve(input.size());

	for(auto &i : input){
		CeReadBratchAddrOutput o;
		o.addr = i.addr;
		if (i.size > 0) {
			o.data.resize(i.size);
			// 连续前缀语义：data 截到实际连续可读长度，调用方据 data.size() 判断有效字节
			size_t bread = g_memIO->Read(i.addr, o.data.data(), i.size);
			o.data.resize(bread);
		}
		output.push_back(std::move(o));
	}

	return static_cast<int>(output.size());
}


int CApi::WriteProcessMemory(HANDLE hProcess, void *lpAddress, void *buffer, int size) {
	size_t written = 0;
	////printf("WriteProcessMemory(%d, %p, %p, %d\n", hProcess, lpAddress, buffer, size);


	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
	written = g_memIO->Write( (uint64_t)lpAddress, buffer, size);

	return (int)written;
}


void CApi::GetModuleList(HANDLE hProcess,std::vector<ModuleListEntry>& vOutput){


	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return;
	}

	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
	std::vector<ProcessMap> vMaps = g_memIO->GetProcessMaps();
	rlock.unlock();
	LOGDF("mod count %d", vMaps.size());
	for (const auto& map : vMaps) {
		ModuleListEntry e;
		e.moduleName = map.pathname;
		//e.moduleName = map.allinfo;
		//LOGDF("GetModuleList moduleName %s", e.moduleName.c_str());
		e.moduleSize = map.length;
		e.baseAddress = map.startAddress;
		e.type = map.type;
		e.flag = map.flag;
		vOutput.push_back(e);
	}

 }


//================断点相关================


BOOL GetProcessTask(int pid, std::vector<int> & vOutput) {
	DIR *dir = NULL;
	struct dirent *ptr = NULL;
	char szTaskPath[256] = { 0 };
	sprintf(szTaskPath, "/proc/%d/task", pid);

	dir = opendir(szTaskPath);
	if (NULL != dir) {
		while ((ptr = readdir(dir)) != NULL) {
			if ((strcmp(ptr->d_name, ".") == 0) || (strcmp(ptr->d_name, "..") == 0)) {
				continue;
			} else if (ptr->d_type != DT_DIR) {
				continue;
			} else if (strspn(ptr->d_name, "1234567890") != strlen(ptr->d_name)) {
				continue;
			}

			int task = atoi(ptr->d_name);
			vOutput.push_back(task);
		}
		closedir(dir);
		return TRUE;
	}
	return FALSE;
}


//static std::vector<uint64_t> vHwBpHandle;

int CApi::SetBreakpoint(HANDLE hProcess, uint64_t address, int bpType, int bpSize){
	{
		std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
		if(!driver_->IsDriverConnected()||g_memIO->type!=MemType_Kernel) {
			LOGD("SetBreakpoint: driver not connected or not kernel mode\n");
			return 0;
		}
	}

	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		LOGD("SetBreakpoint: invalid handle type\n");
		return 0;
	}
	
	uint64_t pl = CPortHelper::GetPointerFromHandle(hProcess);
	if (pl == 0) {
		LOGD("SetBreakpoint: null pointer from handle\n");
		return 0;
	}
	
	CeOpenProcess *processdata = (CeOpenProcess *)pl;
	if (!processdata || !processdata->pid) {
		LOGD("SetBreakpoint: invalid process data or pid\n");
		return 0;
	}


	//获取当前进程所有的task
	std::vector<int> vTask;
	GetProcessTask(processdata->pid, vTask);
	if (vTask.size() == 0) {
		LOGD("GetProcessTask failed\n");
		return 0;
	}

	//设置进程硬件断点
	std::vector<uint64_t> HwBpHandle;
	for (int tid : vTask) {
		uint64_t hwBpHandle = _SetBreakpoint(hProcess, tid, address, bpType, bpSize);
		if (hwBpHandle != 0) {
			HwBpHandle.push_back(hwBpHandle);
		}
	}
	
	//添加或更新断点记录（使用 map 结构，自动去重和快速查找）
	if (HwBpHandle.empty()) {
		return 0;
	}

	std::vector<uint64_t> oldHandles;
	{
		std::lock_guard<std::mutex> lock(processdata->mHwBpMutex);
		auto old = processdata->mHwBpList.find(address);
		if (old != processdata->mHwBpList.end()) {
			oldHandles = std::move(old->second);
		}
		processdata->mHwBpList[address] = HwBpHandle;
	}
	for (auto hwhandle : oldHandles) {
		driver_->DelProcessHwBp(hwhandle);
	}
	return HwBpHandle.size();

}


uint64_t CApi::_SetBreakpoint(HANDLE hProcess, int tid, uint64_t address, int bpType, int bpSize){
	if(!driver_->IsDriverConnected()) {
		return 0;
	}

	uint64_t hwBpHandle = driver_->AddProcessHwBp(tid, address, bpSize, bpType);
	if (hwBpHandle == 0) {
		return 0;
	}

	return hwBpHandle;

}

int CApi::RemoveBreakpoint(HANDLE hProcess,uint64_t hwaddr){
	{
		std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
		if(!driver_->IsDriverConnected()||g_memIO->type!=MemType_Kernel) {
			return 0;
		}
	}

	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	uint64_t pl = CPortHelper::GetPointerFromHandle(hProcess);
	if (pl == 0) {
		return 0;
	}
	
	CeOpenProcess *processdata = (CeOpenProcess *)pl;
	if (!processdata || !processdata->pid) {
		return 0;
	}

	std::vector<uint64_t> handles;
	{
		std::lock_guard<std::mutex> lock(processdata->mHwBpMutex);
		auto it = processdata->mHwBpList.find(hwaddr);
		if (it != processdata->mHwBpList.end()) {
			handles = std::move(it->second);
			processdata->mHwBpList.erase(it);
		}
	}
	if (!handles.empty()) {
		for (auto hwhandle : handles) {
			driver_->DelProcessHwBp(hwhandle);
		}
		return 1;
	}

	return 0;
}

int CApi::SuspendBreakpoint(HANDLE hProcess,uint64_t hwaddr){
	{
		std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
		if(!driver_->IsDriverConnected()||g_memIO->type!=MemType_Kernel) {
			return 0;
		}
	}

	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	uint64_t pl = CPortHelper::GetPointerFromHandle(hProcess);
	if (pl == 0) {
		return 0;
	}
	
	CeOpenProcess *processdata = (CeOpenProcess *)pl;
	if (!processdata || !processdata->pid) {
		return 0;
	}

	std::vector<uint64_t> handles;
	{
		std::lock_guard<std::mutex> lock(processdata->mHwBpMutex);
		auto it = processdata->mHwBpList.find(hwaddr);
		if (it != processdata->mHwBpList.end()) {
			handles = it->second;
		}
	}
	if (!handles.empty()) {
		for (auto hwhandle : handles) {
			driver_->SuspendProcessHwBp(hwhandle);
		}
		return 1;
	}
	
	return 0; // 未找到对应的断点地址

}

int CApi::ResumeBreakpoint(HANDLE hProcess,uint64_t hwaddr){
	{
		std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
		if(!driver_->IsDriverConnected()||g_memIO->type!=MemType_Kernel) {
			return 0;
		}
	}

	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	uint64_t pl = CPortHelper::GetPointerFromHandle(hProcess);
	if (pl == 0) {
		return 0;
	}
	
	CeOpenProcess *processdata = (CeOpenProcess *)pl;
	if (!processdata || !processdata->pid) {
		return 0;
	}
	
	std::vector<uint64_t> handles;
	{
		std::lock_guard<std::mutex> lock(processdata->mHwBpMutex);
		auto it = processdata->mHwBpList.find(hwaddr);
		if (it != processdata->mHwBpList.end()) {
			handles = it->second;
		}
	}
	if (!handles.empty()) {
		for (auto hwhandle : handles) {
			driver_->ResumeProcessHwBp(hwhandle);
		}
		return 1;
	}
	
	return 0; // 未找到对应的断点地址

}

int CApi::ReadHwBpInfo(HANDLE hProcess,uint64_t hwaddr,uint64_t& nHitTotalCount, std::vector<HW_HIT_INFO>& vOutput){
	{
		std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
		if(!driver_->IsDriverConnected()||g_memIO->type!=MemType_Kernel) {
			return 0;
		}
	}

	nHitTotalCount = 0;
	vOutput.clear();
	std::vector<HW_HIT_ITEM> vHwBpInfo;

	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	uint64_t pl = CPortHelper::GetPointerFromHandle(hProcess);
	if (pl == 0) {
		return 0;
	}
	
	CeOpenProcess *processdata = (CeOpenProcess *)pl;
	if (!processdata || !processdata->pid) {
		return 0;
	}
	
	LOGDF("ReadHwBpInfo hwaddr %lx", hwaddr);
	
	std::vector<uint64_t> handles;
	{
		std::lock_guard<std::mutex> lock(processdata->mHwBpMutex);
		auto it = processdata->mHwBpList.find(hwaddr);
		if (it == processdata->mHwBpList.end()) {
			return 0;
		}
		handles = it->second;
	}

	for (auto hwhandle : handles) {
		LOGDF("ReadHwBpInfo hwhandle %lx", hwhandle);
		uint64_t hitCount = 0;
		std::vector<HW_HIT_ITEM> vHandleHwBpInfo;
		if (!driver_->ReadHwBpInfo(hwhandle, hitCount, vHandleHwBpInfo)) {
			continue;
		}
		nHitTotalCount += hitCount;
		vHwBpInfo.insert(vHwBpInfo.end(), vHandleHwBpInfo.begin(), vHandleHwBpInfo.end());
	}
	
	for (auto& item : vHwBpInfo) {
		HW_HIT_INFO hwBpInfo = {};
		hwBpInfo.hit_addr = item.hit_addr;
		hwBpInfo.hit_time = item.hit_time;
		memcpy(&hwBpInfo.regs_info, &item.regs_info, sizeof(HW_HIT_INFO::regs_info));
		memcpy(&hwBpInfo.fpsimd_info, &item.fpsimd_info, sizeof(HW_HIT_INFO::fpsimd_info));
		LOGDF("addr %lx time %lx pc %lx", hwBpInfo.hit_addr, hwBpInfo.hit_time, hwBpInfo.regs_info.pc);
		vOutput.push_back(hwBpInfo);
	}

	return static_cast<int>(vOutput.size());
}


// ================== ELF 符号解析 ==================

// 缓存的符号列表（初始化后保留，避免重复解析）
static std::vector<std::pair<uintptr_t, std::string>> g_cachedSymbols;
static uint64_t g_cachedModuleBase = 0;

int CApi::SymbolInit(HANDLE hProcess, uint64_t moduleBase) {
	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		LOGD("SymbolInit: invalid handle type");
		return -1;
	}

	// 如果已经缓存了同一模块的符号，直接返回
	if (moduleBase == g_cachedModuleBase && !g_cachedSymbols.empty()) {
		LOGDF("SymbolInit: cache hit, base=0x%lx count=%zu", moduleBase, g_cachedSymbols.size());
		return static_cast<int>(g_cachedSymbols.size());
	}

	std::shared_lock<std::shared_mutex> rlock(g_globalMutex);
	g_sym = std::make_unique<AndroidElfScanner>();
	if (!g_sym->Initialize(g_memIO.get(), moduleBase)) {
		LOGDF("SymbolInit: failed to initialize ELF at base=0x%lx", moduleBase);
		g_cachedSymbols.clear();
		g_cachedModuleBase = 0;
		return -1;
	}

	g_cachedSymbols = g_sym->GetSymbols();
	g_cachedModuleBase = moduleBase;
	LOGDF("SymbolInit: base=0x%lx symbols=%zu", moduleBase, g_cachedSymbols.size());
	return static_cast<int>(g_cachedSymbols.size());
}

int CApi::SymbolGetCount() {
	return static_cast<int>(g_cachedSymbols.size());
}

std::vector<std::pair<uintptr_t, std::string>> CApi::SymbolGetList(int offset, int count) {
	std::vector<std::pair<uintptr_t, std::string>> result;
	int total = static_cast<int>(g_cachedSymbols.size());

	if (offset < 0 || offset >= total || count <= 0) {
		return result;
	}

	int end = std::min(offset + count, total);
	result.assign(g_cachedSymbols.begin() + offset, g_cachedSymbols.begin() + end);
	return result;
}

uintptr_t CApi::SymbolFind(HANDLE hProcess, uint64_t moduleBase, const std::string& name) {
	if (CPortHelper::GetHandleType(hProcess) != htProcesHandle) {
		return 0;
	}

	// 如果缓存的不是目标模块，先初始化
	if (moduleBase != g_cachedModuleBase || g_cachedSymbols.empty()) {
		if (SymbolInit(hProcess, moduleBase) < 0) {
			return 0;
		}
	}

	for (const auto& [addr, symName] : g_cachedSymbols) {
		if (symName == name) {
			return addr;
		}
	}
	return 0;
}
