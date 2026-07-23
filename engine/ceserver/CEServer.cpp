
#include <asm-generic/mman-common.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <linux/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <zlib.h>
#include <sys/select.h>
#include <errno.h>
#include <elf.h>
#include <signal.h>
#include <sys/prctl.h>
#include <vector>
#include <atomic>
#include <sys/eventfd.h>
#include "ceserver.h"
#include "api.h"
#include "Logger.hpp"
#include "../common/ScanProgress.hpp"
#include "porthelp.h"

// 由 server.cpp 定义的全局停止控制变量
// 使用 weak 属性提供默认值，避免非 socket 接口链接时报未定义符号
__attribute__((weak)) std::atomic<bool> g_running{true};
__attribute__((weak)) int g_stop_eventfd = -1;



#define PORT 3168
#define MAX_HIT_COUNT 5000000
#define MAX_NETWORK_ALLOC_SIZE (256 * 1024 * 1024)  // 网络输入分配上限 256MB
#define MAX_NETWORK_ARRAY_COUNT (10 * 1024 * 1024)   // 网络输入数组元素上限 10M


char versionstring[] = "MiniMem 1.0.1";
char CheatEngineVersion = 1;  // 协议主版本号（与前端 PROTOCOL_VERSION_MAJOR 对齐）


namespace {

template <typename T>
bool SendVectorPayload(Ioserver *IOserver, const std::vector<T> &items,
                       size_t count, const char *tag) {
  if (!IOserver) {
    return false;
  }
  if (count == 0) {
    return true;
  }
  if (count > items.size()) {
    LOGEF("%s: payload count overflow vector size, count=%zu size=%zu", tag,
          count, items.size());
    return false;
  }
  if (count > (std::numeric_limits<size_t>::max() / sizeof(T))) {
    LOGEF("%s: payload byte size overflow, count=%zu itemSize=%zu", tag, count,
          sizeof(T));
    return false;
  }

  const size_t payloadBytes = count * sizeof(T);
  return IOserver->Send(items.data(), payloadBytes);
}

size_t ClampNonNegativeCount(int count, size_t actualSize, const char *tag) {
  if (count < 0) {
    LOGEF("%s: negative count=%d, clamped to 0", tag, count);
    return 0;
  }

  const size_t safeCount = static_cast<size_t>(count);
  if (safeCount > actualSize) {
    LOGEF("%s: count=%zu exceeds actualSize=%zu, clamped", tag, safeCount,
          actualSize);
    return actualSize;
  }
  if (safeCount != actualSize) {
    LOGDF("%s: count=%zu adjusted to actualSize=%zu", tag, safeCount,
          actualSize);
  }
  return safeCount;
}

int ClampIntCount(size_t count, const char *tag) {
  if (count > static_cast<size_t>(std::numeric_limits<int>::max())) {
    LOGEF("%s: count=%zu exceeds int max, clamped", tag, count);
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(count);
}

}

int DispatchCommand_V2(Ioserver *IOserver, unsigned char command) {
  if (!IOserver) {
    LOGE("DispatchCommand: IOserver is null");
    return -1;
  }
  int r;
  switch (command) {
  case CMD_GETVERSION: {
    LOGD("CMD_GETVERSION");
    CeVersion *v;
    int versionsize = strlen(versionstring);
    v = (CeVersion *)malloc(sizeof(CeVersion) + versionsize);
    v->stringsize = versionsize;
    v->version = CheatEngineVersion;

    memcpy((char *)v + sizeof(CeVersion), versionstring, versionsize);

    IOserver->Send(v, sizeof(CeVersion) + versionsize);
    free(v);

    break;
  }

  case CMD_GETMEMTYPE: {
    LOGD("CMD_GETMEMTYPE");
    // 0:null 1:io 2:syscall 3:kernel 4:syshook
    unsigned char type = CApi::GetRWDriverType();
    IOserver->Send(&type, sizeof(type));
    break;
  }

  case CMD_CLOSECONNECTION: {
    LOGD("CMD_CLOSECONNECTION");
    // //printf("Connection %d closed properly\n", currentsocket);
    // fflush(stdout);
    // close(currentsocket);
    LOGD("IOserver closed");
    LOG_CLEANUP();
    IOserver->Close();

    return 0;
  }

  case CMD_TERMINATESERVER: {
    LOGD("CMD_TERMINATESERVER");
    LOG_CLEANUP();
    IOserver->Close();
    // 通知主循环优雅退出，不再直接 exit(0)
    g_running.store(false);
    if (g_stop_eventfd >= 0) {
      uint64_t val = 1;
      write(g_stop_eventfd, &val, sizeof(val));
    }
    return 0;
    break;
  }

  case CMD_CLOSEHANDLE: {
    LOGD("CMD_CLOSEHANDLE");
    HANDLE h;
    int r;
    if (IOserver->Receive(&h, sizeof(h)) > 0) {
      LOGDF("CMD_CLOSEHANDLE %d\n", h);
      CApi::CloseHandle(h);
      r = 1;
      // sendall(currentsocket, &r, sizeof(r), 0); //stupid naggle
      IOserver->Send(&r, sizeof(r));
    }
    break;
  }

  case CMD_OPENPROCESS: {
    LOGD("CMD_OPENPROCESS");
    int pid = 0;

    r = IOserver->Receive(&pid, sizeof(int));
    if (r > 0) {
      HANDLE processhandle = 0;

      LOGDF("OpenProcess pid %d\n", pid);
      processhandle = CApi::OpenProcess(pid);

      LOGDF("processhandle=%d\n", processhandle);
      IOserver->Send(&processhandle, sizeof(HANDLE));
    }

    break;
  }

  case CMD_READPROCESSMEMORY: {
    LOGD("CMD_READPROCESSMEMORY");
    CeReadProcessMemoryInput c;

    r = IOserver->Receive(&c, sizeof(c));
    if (r > 0) {

      if (c.size == 0 || c.size > MAX_NETWORK_ALLOC_SIZE) {
        LOGEF("CMD_READPROCESSMEMORY: invalid size %u", c.size);
        int realread = 0;
        IOserver->Send(&realread, 4);
        break;
      }

      int realread;

      char *data = (char *)calloc(c.size, 1);

      realread = CApi::ReadProcessMemory(c.handle, (void *)(uintptr_t)c.address,
                                         data, c.size);

      LOGDF("ReadProcessMemory %lx %d  real  %d\n", c.address, c.size,
            realread);
      // IOserver->Send(o, sizeof(CeReadProcessMemoryOutput) + c.size);

      IOserver->Send(&realread, 4);
      IOserver->Send(data, c.size);

      if (data)
        free(data);
    }
    break;
  }

  case CMD_WRITEPROCESSMEMORY: {
    LOGD("CMD_WRITEPROCESSMEMORY");
    CeWriteProcessMemoryInput c;

    r = IOserver->Receive(&c, sizeof(c));
    if (r > 0) {
      if (c.size > MAX_NETWORK_ALLOC_SIZE) {
        LOGEF("CMD_WRITEPROCESSMEMORY: invalid size %u", c.size);
        CeWriteProcessMemoryOutput o;
        o.written = 0;
        IOserver->Send(&o, sizeof(CeWriteProcessMemoryOutput));
        break;
      }
      CeWriteProcessMemoryOutput o;
      unsigned char *buf;

      if (c.size) {
        buf = (unsigned char *)malloc(c.size);

        r = IOserver->Receive(buf, c.size);
        if (r > 0) {
          // printf("received %d bytes for the buffer. Wanted %d\n", r,
          // c.size);
          o.written = CApi::WriteProcessMemory(
              c.handle, (void *)(uintptr_t)c.address, buf, c.size);

          IOserver->Send(&o, sizeof(CeWriteProcessMemoryOutput));
          // printf("wpm: returned %d bytes to caller\n", r);

        } else {
          // printf("wpm recv error while reading the data\n");
          o.written = 0;
          IOserver->Send(&o, sizeof(CeWriteProcessMemoryOutput));
        }
        free(buf);
      } else {
        // printf("wpm with a size of 0 bytes");
        o.written = 0;
        IOserver->Send(&o, sizeof(CeWriteProcessMemoryOutput));
        // printf("wpm: returned %d bytes to caller\n", r);
      }
    }
    break;
  }

  case CMD_GETPROCESSLIST: {
    LOGD("CMD_GETPROCESSLIST");
    CeProcessList *pCeProcessList = new CeProcessList();
    CApi::GetProcessListInfo(pCeProcessList->vProcessList);

    struct Process {
      int pid;
      int size;
      // std::string cmdline;
    };
    const int len = ClampIntCount(pCeProcessList->vProcessList.size(),
                                  "CMD_GETPROCESSLIST");
    LOGDF("start to processlist %d", len);

    if (!IOserver->Send(&len, sizeof(len))) {
      delete pCeProcessList;
      break;
    }
    for (int i = 0; i < len; ++i) {
      const auto &process = pCeProcessList->vProcessList[static_cast<size_t>(i)];
      Process p{};
      p.pid = process.pid;
      p.size = ClampIntCount(process.cmdline.size(), "CMD_GETPROCESSLIST.name");
      if (!IOserver->Send(&p, sizeof(Process))) {
        break;
      }
      if (p.size > 0 &&
          !IOserver->Send(process.cmdline.data(), static_cast<size_t>(p.size))) {
        break;
      }
    }

    delete pCeProcessList;
    break;
  }

  case CMD_GETMODULELIST: {
    LOGD("CMD_GETMODULELIST");
    HANDLE h = 0;
    IOserver->Receive(&h, sizeof(HANDLE));
    LOGDF("CMD_GETMODULELIST handle %d", h);
    CeModuleList *pCeModuleList = new CeModuleList();
    CApi::GetModuleList(h, pCeModuleList->vModuleList);
    const int len = ClampIntCount(pCeModuleList->vModuleList.size(),
                                  "CMD_GETMODULELIST");
    LOGDF("CMD_GETMODULELIST len %d", len);

    if (!IOserver->Send(&len, sizeof(len))) {
      LOGD("Failed to send module count");
      delete pCeModuleList;
      break;
    }

    int sent_count = 0;
    CeModuleListEntry e;
    for (int i = 0; i < len; ++i) {
      const auto &module = pCeModuleList->vModuleList[static_cast<size_t>(i)];

      memset(&e, 0, sizeof(e));
      e.modulebase = module.baseAddress;
      e.modulesize = module.moduleSize;
      e.flag = module.flag;
      e.result = module.type;
      e.modulenamesize = ClampIntCount(module.moduleName.size(),
                                       "CMD_GETMODULELIST.name");

      if (!IOserver->Send(&e, sizeof(CeModuleListEntry))) {
        LOGDF("Failed to send module entry at index %d", sent_count);
        break;
      }

      if (e.modulenamesize > 0 &&
          !IOserver->Send(module.moduleName.data(),
                          static_cast<size_t>(e.modulenamesize))) {
        LOGDF("Failed to send module name at index %d", sent_count);
        break;
      }

      sent_count++;
    }

    LOGDF("CMD_GETMODULELIST completed, sent %d/%d modules", sent_count, len);
    delete pCeModuleList;
    break;
  }

  // 读取大批量内存数据 比如一个模块的全部数据
  // 返回结果是 一个数组 每个元素对应有效的页面
  case CMD_READBRATCHMEMORY: {
    LOGD("CMD_READBRATCHMEMORY");
    HANDLE h = 0;
    IOserver->Receive(&h, sizeof(HANDLE));

    CeReadBratchMemory input;
    std::vector<CeReadBratchMemoryOutput> output;
    IOserver->Receive(&input, sizeof(CeReadBratchMemory));

    int RealLen = CApi::ReadBratchMemory(h, input, output);
    const int safeResult = ClampIntCount(
        ClampNonNegativeCount(RealLen, output.size(), "CMD_READBRATCHMEMORY"),
        "CMD_READBRATCHMEMORY");
    IOserver->Send(&safeResult, sizeof(int));

    for (int i = 0; i < safeResult; ++i) {
      auto &o = output[static_cast<size_t>(i)];
      if (!IOserver->Send(&o.addr, sizeof(uint64_t))) {
        break;
      }
      if (!o.data.empty() && !IOserver->Send(o.data.data(), o.data.size())) {
        break;
      }
    }

    LOGDF("end CMD_READBRATCHMEMORY result %d output %zu", safeResult,
          output.size());

    break;
  }

  case CMD_INITRWDRIVER: {

    LOGD("CMD_INITRWDRIVER");
    int len;
    IOserver->Receive(&len, sizeof(int));
    if (len <= 0 || len > MAX_NETWORK_ALLOC_SIZE) {
      LOGEF("CMD_INITRWDRIVER: invalid len %d", len);
      int ret = 0;
      IOserver->Send(&ret, sizeof(int));
      int out_len = 0;
      IOserver->Send(&out_len, sizeof(int));
      break;
    }
    std::vector<unsigned char> input(len);
    IOserver->Receive(input.data(), len);
    std::string key(input.begin(), input.end());
    std::string out_result;
    int ret = CApi::InitReadWriteDriver(key.c_str(), out_result);
    // 返回是否成功 0 失败 1 成功
    IOserver->Send(&ret, sizeof(int));
    int out_len = out_result.size();
    // 字符串结果
    IOserver->Send(&out_len, sizeof(int));
    LOGDF("out_result %s", out_result.c_str());
    IOserver->Send(out_result.data(), out_len);

    break;
  }

  case CMD_SETKERNELHWBPRECLAIM: {
    LOGD("CMD_SETKERNELHWBPRECLAIM");
    unsigned char enabled = 0;
    if (!IOserver->Receive(&enabled, sizeof(enabled))) {
      LOGEF("CMD_SETKERNELHWBPRECLAIM: receive failed");
      return -1;
    }
    int ret = CApi::SetKernelBreakpointForceReclaim(enabled != 0);
    IOserver->Send(&ret, sizeof(ret));
    break;
  }

  // 内核断点相关
  case CMD_KERNEL_SETBREAKPOINT: {
    LOGD("CMD_KERNEL_SETBREAKPOINT");
    HANDLE h = 0;
    IOserver->Receive(&h, sizeof(HANDLE));
    uint64_t address;
    int bpType;
    int bpSize;
    IOserver->Receive(&address, sizeof(uint64_t));
    IOserver->Receive(&bpType, sizeof(int));
    IOserver->Receive(&bpSize, sizeof(int));
    int r = CApi::SetBreakpoint(h, address, bpType, bpSize);
    IOserver->Send(&r, sizeof(int));
    break;
  }

  case CMD_KERNEL_REMOVEBREAKPOINT: {
    LOGD("CMD_KERNEL_REMOVEBREAKPOINT");
    HANDLE h = 0;
    IOserver->Receive(&h, sizeof(HANDLE));
    uint64_t hwaddr;
    IOserver->Receive(&hwaddr, sizeof(uint64_t));
    int r = CApi::RemoveBreakpoint(h, hwaddr);
    IOserver->Send(&r, sizeof(int));
    break;
  }
  case CMD_KERNEL_SUSPENDBREAKPOINT: {
    LOGD("CMD_KERNEL_SUSPENDBREAKPOINT");
    HANDLE h = 0;
    IOserver->Receive(&h, sizeof(HANDLE));
    uint64_t hwaddr;
    IOserver->Receive(&hwaddr, sizeof(uint64_t));
    int r = CApi::SuspendBreakpoint(h, hwaddr);
    IOserver->Send(&r, sizeof(int));
    break;
  }
  case CMD_KERNEL_RESUMEBREAKPOINT: {
    LOGD("CMD_KERNEL_RESUMEBREAKPOINT");
    HANDLE h = 0;
    IOserver->Receive(&h, sizeof(HANDLE));
    uint64_t hwaddr;
    IOserver->Receive(&hwaddr, sizeof(uint64_t));
    int r = CApi::ResumeBreakpoint(h, hwaddr);
    IOserver->Send(&r, sizeof(int));
    break;
  }
  case CMD_KERNEL_READHWBPINFO: {
    LOGD("CMD_KERNEL_READHWBPINFO");
    HANDLE h = 0;
    IOserver->Receive(&h, sizeof(HANDLE));
    uint64_t hwaddr;
    IOserver->Receive(&hwaddr, sizeof(uint64_t));
    uint64_t nHitTotalCount = 0;
    std::vector<HW_HIT_INFO> vOutput;
    int r = CApi::ReadHwBpInfo(h, hwaddr, nHitTotalCount, vOutput);
    const int safeResult = ClampIntCount(
        ClampNonNegativeCount(r, vOutput.size(), "CMD_KERNEL_READHWBPINFO"),
        "CMD_KERNEL_READHWBPINFO");
    IOserver->Send(&safeResult, sizeof(int));
    IOserver->Send(&nHitTotalCount, sizeof(uint64_t));
    LOGDF("CMD_KERNEL_READHWBPINFO nHitTotalCount %llu vOutput %zu result %d",
          static_cast<unsigned long long>(nHitTotalCount), vOutput.size(),
          safeResult);
    if (safeResult > 0 &&
        !SendVectorPayload(IOserver, vOutput, static_cast<size_t>(safeResult),
                           "CMD_KERNEL_READHWBPINFO")) {
      LOGEF("CMD_KERNEL_READHWBPINFO: failed to send payload");
    }
    LOGDF("CMD_KERNEL_READHWBPINFO end");
    break;
  }

  case CMD_KERNEL_QUERYHWBPTHREADS: {
    LOGD("CMD_KERNEL_QUERYHWBPTHREADS");
    HANDLE h = 0;
    uint32_t capacity = 0;
    if (!IOserver->Receive(&h, sizeof(h)) ||
        !IOserver->Receive(&capacity, sizeof(capacity))) {
      LOGEF("CMD_KERNEL_QUERYHWBPTHREADS: receive failed");
      return -1;
    }
    std::vector<HwbpTaskThreadInfo> threads;
    const bool ok = CApi::QueryHardwareBreakpointThreads(h, capacity, threads);
    const int result = ok ? 1 : 0;
    constexpr size_t kMaxThreads = 65536;
    const uint32_t threadCount = ok && threads.size() <= kMaxThreads
        ? static_cast<uint32_t>(threads.size()) : 0;
    if (!IOserver->Send(&result, sizeof(result)) ||
        !IOserver->Send(&threadCount, sizeof(threadCount))) {
      return -1;
    }
    if (!ok || threadCount == 0) {
      break;
    }
    for (uint32_t i = 0; i < threadCount; ++i) {
      const auto& thread = threads[i];
      HwbpTaskThreadHeader header{};
      header.tid = thread.tid;
      header.queryResult = thread.success ? 1 : 0;
      header.errorCode = thread.errorCode;
      header.count = thread.success
          ? static_cast<uint32_t>(thread.entries.size()) : 0;
      header.totalCount = thread.totalCount;
      header.brpCount = thread.brpCount;
      header.wrpCount = thread.wrpCount;
      header.enabledCount = thread.enabledCount;
      header.activeCount = thread.activeCount;
      header.perfCount = thread.perfCount;
      header.ptraceCount = thread.ptraceCount;
      header.moduleCount = thread.moduleCount;
      if (!IOserver->Send(&header, sizeof(header))) {
        return -1;
      }
      if (header.count > capacity || header.count > 64 ||
          header.count != thread.entries.size()) {
        LOGEF("CMD_KERNEL_QUERYHWBPTHREADS: invalid entry count tid=%d count=%u",
              thread.tid, header.count);
        return -1;
      }
      for (const auto& source : thread.entries) {
        HwbpTaskSlot entry{source.eventId, source.moduleHandle, source.address,
                           source.tid, source.onCpu, source.type, source.length,
                           source.state, source.source, source.flags, 0};
        if (!IOserver->Send(&entry, sizeof(entry))) {
          return -1;
        }
      }
    }
    break;
  }

  case CMD_READBRATCHADDR: {
    LOGD("CMD_READBRATCHADDR");
    HANDLE h = 0;
    IOserver->Receive(&h, sizeof(HANDLE));
    int len;
    IOserver->Receive(&len, sizeof(int));
    if (len <= 0 || len > (int)MAX_NETWORK_ARRAY_COUNT) {
      LOGEF("CMD_READBRATCHADDR: invalid len %d", len);
      int r = 0;
      IOserver->Send(&r, sizeof(int));
      break;
    }
    std::vector<CeReadBratchAddr> input(len);
    IOserver->Receive(input.data(), len * sizeof(CeReadBratchAddr));
    std::vector<CeReadBratchAddrOutput> output;

    int r = CApi::ReadBratchAddr(h, input, output);
    const int safeResult = ClampIntCount(
        ClampNonNegativeCount(r, output.size(), "CMD_READBRATCHADDR"),
        "CMD_READBRATCHADDR");
    IOserver->Send(&safeResult, sizeof(int));
    for (int i = 0; i < safeResult; ++i) {
      auto &o = output[static_cast<size_t>(i)];
      if (!IOserver->Send(&o.addr, sizeof(uint64_t))) {
        break;
      }
      // 每条带有效长度前缀（连续可读字节数），消除批量按址读的多页歧义
      uint32_t vlen = static_cast<uint32_t>(o.data.size());
      if (!IOserver->Send(&vlen, sizeof(vlen))) {
        break;
      }
      if (!o.data.empty() && !IOserver->Send(o.data.data(), o.data.size())) {
        break;
      }
    }

    LOGDF("CMD_READBRATCHADDR result %d output %zu", safeResult,
          output.size());
    break;
  }

  case CMD_SYMBOL_INIT: {
    LOGD("CMD_SYMBOL_INIT V2");
    CeSymbolInitInput input;
    if (IOserver->Receive(&input, sizeof(input)) <= 0) {
      CeSymbolInitOutput output = {-1, 0};
      IOserver->Send(&output, sizeof(output));
      break;
    }
    int count = CApi::SymbolInit(input.hProcess, input.moduleBase);
    CeSymbolInitOutput output;
    output.result = (count >= 0) ? 0 : -1;
    output.totalCount = (count >= 0) ? count : 0;
    IOserver->Send(&output, sizeof(output));
    break;
  }

  case CMD_SYMBOL_GETLIST: {
    LOGD("CMD_SYMBOL_GETLIST V2");
    CeGetSymbolListInput input;
    if (IOserver->Receive(&input, sizeof(input)) <= 0) {
      CeGetSymbolListOutput output = {0, 0};
      IOserver->Send(&output, sizeof(output));
      break;
    }
    auto symbols = CApi::SymbolGetList(input.offset, input.count);
    CeGetSymbolListOutput output;
    output.totalCount = CApi::SymbolGetCount();
    output.actualCount = ClampIntCount(symbols.size(), "CMD_SYMBOL_GETLIST");
    IOserver->Send(&output, sizeof(output));

    CeSymbolEntry entry;
    for (int i = 0; i < output.actualCount; ++i) {
      const auto &[addr, name] = symbols[static_cast<size_t>(i)];
      entry.address = addr;
      entry.nameSize = ClampIntCount(name.size(), "CMD_SYMBOL_GETLIST.name");
      IOserver->Send(&entry, sizeof(entry));
      if (entry.nameSize > 0) {
        IOserver->Send(name.data(), static_cast<size_t>(entry.nameSize));
      }
    }
    break;
  }

  case CMD_SYMBOL_FIND: {
    LOGD("CMD_SYMBOL_FIND V2");
    CeFindSymbolInput input;
    if (IOserver->Receive(&input, sizeof(input)) <= 0) {
      CeFindSymbolOutput output = {-1, 0};
      IOserver->Send(&output, sizeof(output));
      break;
    }
    CeFindSymbolOutput output = {-1, 0};
    if (input.nameSize > 0 && input.nameSize <= 4096) {
      std::vector<char> nameBuf(input.nameSize);
      if (IOserver->Receive(nameBuf.data(), input.nameSize) > 0) {
        std::string symName(nameBuf.data(), input.nameSize);
        uintptr_t addr = CApi::SymbolFind(input.hProcess, input.moduleBase, symName);
        if (addr != 0) {
          output.result = 0;
          output.address = addr;
        }
      }
    }
    IOserver->Send(&output, sizeof(output));
    break;
  }

  case CMD_GETSOBASE: {
    LOGD("CMD_GETSOBASE V2");
    CeGetSoBaseInput input;
    if (IOserver->Receive(&input, sizeof(input)) <= 0) {
      CeGetSoBaseOutput output = {-1, 0};
      IOserver->Send(&output, sizeof(output));
      break;
    }

    CeGetSoBaseOutput output = {-1, 0};
    if (input.nameSize > 0 && input.nameSize <= 4096) {
      std::vector<char> nameBuf(input.nameSize);
      if (IOserver->Receive(nameBuf.data(), input.nameSize) > 0) {
        std::string soName(nameBuf.data(), input.nameSize);
        uint64_t base = CApi::GetSoBase(input.hProcess, soName);
        if (base != 0) {
          output.result = 0;
          output.base = base;
        }
      }
    }
    IOserver->Send(&output, sizeof(output));
    break;
  }

  default: {
    LOGDF("Unknow command:%d", command);
    // printf("Unknow command:%d", command);
    // fflush(stdout);
    break;
  }
  }
  return 0;
}
