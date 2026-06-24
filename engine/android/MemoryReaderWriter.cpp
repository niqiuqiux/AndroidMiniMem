#include "MemoryReaderWriter.h"
#include <bits/ioctl.h>
#include <cstdint>
#include <cstring>

#ifdef __linux__
#include <numeric>
#include <dirent.h>

// Anon FD 常量
static constexpr const char* ANON_FD_TAG = "[nisu_driver]";

// 解析 fd 编号
static int parse_fd_number(const char* text) {
    if (text == nullptr || *text == '\0') {
        return -1;
    }
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < 0 || value > std::numeric_limits<int>::max()) {
        return -1;
    }
    return static_cast<int>(value);
}

// 复制 fd 并设置 CLOEXEC
static int dup_cloexec(int fd) {
#ifdef F_DUPFD_CLOEXEC
    int new_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (new_fd >= 0) {
        return new_fd;
    }
    if (errno != EINVAL) {
        return -1;
    }
#endif
    int dup_fd = ::dup(fd);
    if (dup_fd < 0) {
        return -1;
    }
    (void)::fcntl(dup_fd, F_SETFD, FD_CLOEXEC);
    return dup_fd;
}

// 查找已存在的 anon fd
static int find_anon_fd() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir) {
        return -1;
    }

    int found_fd = -1;
    dirent* ent = nullptr;
    char link_path[128];
    char target[256];

    while ((ent = ::readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        int fd = parse_fd_number(ent->d_name);
        if (fd < 0) {
            continue;
        }

        std::snprintf(link_path, sizeof(link_path), "/proc/self/fd/%s", ent->d_name);
        ssize_t len = ::readlink(link_path, target, sizeof(target) - 1);
        if (len <= 0) {
            continue;
        }
        target[len] = '\0';

        if (std::strstr(target, ANON_FD_TAG) != nullptr) {
            found_fd = dup_cloexec(fd);
            break;
        }
    }

    ::closedir(dir);
    return found_fd;
}

// ======================= 内部实现函数 =======================
int CMemoryReaderWriter::_InternalConnectDriver(const std::string& procNodeAuthKey) {
    if (m_nFd >= 0) { return TRUE; }
    int fd = _rwProcMemDriver_Connect(procNodeAuthKey);
    if (fd < 0) {
        LOGEF("ConnectDriver failed rc=%d %s", fd, strerror(errno));
        return fd;
    }
    m_nFd = fd;
    return 0;
}

BOOL CMemoryReaderWriter::_InternalDisconnectDriver() {
    if (m_nFd >= 0) {
        _rwProcMemDriver_Disconnect(m_nFd);
        m_nFd = -1;
        return TRUE;
    }
    return FALSE;
}

BOOL CMemoryReaderWriter::_InternalIsDriverConnected() {
    return m_nFd >= 0 ? TRUE : FALSE;
}

uint64_t CMemoryReaderWriter::_InternalOpenProcess(uint64_t pid) {
    return _rwProcMemDriver_OpenProcess(m_nFd, pid);
}

BOOL CMemoryReaderWriter::_InternalReadProcessMemory(uint64_t pid, uint64_t lpBaseAddress, void* lpBuffer,
                                                     size_t nSize, size_t* lpNumberOfBytesRead, BOOL bIsForceRead) {
    return _rwProcMemDriver_ReadProcessMemory(m_nFd, pid, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead, bIsForceRead);
}

BOOL CMemoryReaderWriter::_InternalWriteProcessMemory(uint64_t pid, uint64_t lpBaseAddress, void* lpBuffer,
                                                      size_t nSize, size_t* lpNumberOfBytesWritten, BOOL bIsForceWrite) {
    return _rwProcMemDriver_WriteProcessMemory(m_nFd, pid, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesWritten, bIsForceWrite);
}

BOOL CMemoryReaderWriter::_InternalCloseHandle(uint64_t pid) {
    return _rwProcMemDriver_CloseHandle(m_nFd, pid);
}

BOOL CMemoryReaderWriter::_InternalVirtualQueryExFull(uint64_t pid, BOOL showPhy, std::vector<DRIVER_REGION_INFO>& vOutput) {
    return _rwProcMemDriver_VirtualQueryExFull(m_nFd, pid, showPhy, vOutput);
}

BOOL CMemoryReaderWriter::_InternalCheckProcessMemAddrValid(uint64_t pid, uint64_t lpBaseAddress) {
    return _rwProcMemDriver_CheckProcessMemAddrValid(m_nFd, pid, lpBaseAddress);
}

BOOL CMemoryReaderWriter::_InternalGetPidList(std::vector<int>& vOutput) {
    return _rwProcMemDriver_GetPidList(m_nFd, vOutput);
}

BOOL CMemoryReaderWriter::_InternalSetProcessRoot(uint64_t pid) {
    return _rwProcMemDriver_SetProcessRoot(m_nFd, pid);
}

BOOL CMemoryReaderWriter::_InternalGetProcessPhyMemSize(uint64_t pid, uint64_t& outRss) {
    return _rwProcMemDriver_GetProcessPhyMemSize(m_nFd, pid, &outRss);
}

BOOL CMemoryReaderWriter::_InternalGetProcessCmdline(uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize) {
    return _rwProcMemDriver_GetProcessCmdline(m_nFd, pid, lpOutCmdlineBuf, bufSize);
}

BOOL CMemoryReaderWriter::_InternalGetProcessComm(uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize) {
    return _rwProcMemDriver_GetProcessComm(m_nFd, pid, lpOutCmdlineBuf, bufSize);
}

BOOL CMemoryReaderWriter::_InternalReadProcessMemoryBulk(uint64_t pid, uint64_t lpBaseAddress, void* lpBuffer,
                                                         size_t nSize, size_t* lpNumberOfBytesRead, BOOL bIsForceRead) {
    return _rwProcMemDriver_ReadProcessMemoryBulk(m_nFd, pid, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead, bIsForceRead);
}

// 硬件断点相关内部函数
int CMemoryReaderWriter::_InternalGetNumBRPS() {
    return _hwbpProcDriver_GetNumBRPS(m_nFd);
}

int CMemoryReaderWriter::_InternalGetNumWRPS() {
    return _hwbpProcDriver_GetNumWRPS(m_nFd);
}

uint64_t CMemoryReaderWriter::_InternalAddProcessHwBp(uint64_t hProcess, uint64_t lpBaseAddress, 
                                                      unsigned int hwbpLen, unsigned int hwbpType) {
    return _hwbpProcDriver_InstProcessHwBp(m_nFd, hProcess, lpBaseAddress, hwbpLen, hwbpType);
}

BOOL CMemoryReaderWriter::_InternalDelProcessHwBp(uint64_t hHwbp) {
    return _hwbpProcDriver_UninstProcessHwBp(m_nFd, hHwbp);
}

BOOL CMemoryReaderWriter::_InternalSuspendProcessHwBp(uint64_t hHwbp) {
    return _hwbpProcDriver_SuspendProcessHwBp(m_nFd, hHwbp);
}

BOOL CMemoryReaderWriter::_InternalResumeProcessHwBp(uint64_t hHwbp) {
    return _hwbpProcDriver_ResumeProcessHwBp(m_nFd, hHwbp);
}

BOOL CMemoryReaderWriter::_InternalReadHwBpInfo(uint64_t hHwbp, uint64_t& nHitTotalCount, 
                                                std::vector<HW_HIT_ITEM>& vOutput) {
    return _hwbpProcDriver_ReadHwBpInfo(m_nFd, hHwbp, nHitTotalCount, vOutput);
}

// void CMemoryReaderWriter::_InternalSetUseBypassSELinuxMode(BOOL bUseBypassSELinuxMode) {
//     _hwbpProcDriver_SetUseBypassSELinuxMode(bUseBypassSELinuxMode);
// }

int CMemoryReaderWriter::_InternalGetLinkFD() {
    return m_nFd;
}

void CMemoryReaderWriter::_InternalSetLinkFD(int fd) {
    m_nFd = fd;
}

BOOL CMemoryReaderWriter::_InternalGetCardTime(uint64_t& outCardTime) {
    uint64_t cardTime = 0;
    if (ioctl(m_nFd, 0, &cardTime) == 0) {
        outCardTime = cardTime;
        return TRUE;
    }
    return FALSE;
} 

// ======================= 驱动通信函数实现 =======================
ssize_t CMemoryReaderWriter::_rwProcMemDriver_MyIoctl(int fd, char cmd, uint64_t param1, uint64_t param2,
                                                      uint64_t param3, char* buf, uint64_t bufSize) {
    constexpr size_t headerSize = sizeof(IoctlRequest);
    size_t totalSize = headerSize + bufSize;

    static thread_local IoctlBufferPool pool;
    char* pBuf = pool.getBuffer(totalSize);
    if (!pBuf) return -ENOMEM;

    IoctlRequest* req = reinterpret_cast<IoctlRequest*>(pBuf);
    req->cmd = cmd;
    req->param1 = param1;
    req->param2 = param2;
    req->param3 = param3;
    req->buf_size = bufSize;
    if (bufSize > 0) {
        memcpy(pBuf + headerSize, buf, bufSize);
    }

    #ifdef USE_ANON_FD
    // 通过 anon_fd 使用 read() 发送命令
    ssize_t outRead;
    do {
        outRead = ::read(fd, pBuf, totalSize);
    } while (outRead < 0 && errno == EINTR);
    #elif defined(SYSCALL_IO)
    auto outRead = svc_call(fd, pBuf, totalSize);
    #elif defined(PROC_IO)
    auto outRead = ::read(fd, pBuf, totalSize);
    #endif

    if (outRead < 0) {
        TRACE("read error %d %s", outRead, strerror(errno));
        return -errno;
    }

    if (bufSize > 0) {
        memcpy(buf, pBuf + headerSize, bufSize);
    }
    return outRead;
}

int CMemoryReaderWriter::_rwProcMemDriver_Connect(const std::string& procNodeAuthKey) {
    (void)procNodeAuthKey; // 未使用，保留兼容

    #ifdef USE_ANON_FD
    // 1. 先尝试查找已存在的 anon fd
    int existing_fd = find_anon_fd();
    if (existing_fd >= 0) {
        return existing_fd;
    }

    // 2. 通过 reboot syscall 请求内核安装 anon fd
    int out_fd = -1;
    errno = 0;
    (void)::syscall(KERNELMEM_SYS_REBOOT,
                    static_cast<int>(KSU_INSTALL_MAGIC1),
                    static_cast<int>(KSU_INSTALL_MAGIC2),
                    static_cast<unsigned int>(KSU_INSTALL_CMD_UNUSED),
                    &out_fd);

    if (out_fd < 0) {
        return errno ? -errno : -ENODEV;
    }
    return out_fd;
    #else
    return -ENODEV;
    #endif
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_Disconnect(int nFd) {
    #ifdef USE_ANON_FD
    if (nFd < 0) { return FALSE; }
    close(nFd);
    #endif
    return TRUE;
}

std::vector<uint8_t> CMemoryReaderWriter::generate_unique_non_zero_bytes(std::size_t count) {
    if (count == 0 || count > 255) {
        return {};
    }
    std::vector<uint8_t> pool(255);
    std::iota(pool.begin(), pool.end(), 1);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(pool.begin(), pool.end(), gen);
    return std::vector<uint8_t>(pool.begin(), pool.begin() + count);
}

std::string CMemoryReaderWriter::GenerateRandomString(size_t length, bool printable_only) {
    std::string result;
    result.reserve(length);

    std::random_device rd;
    std::mt19937 gen(rd());

    if (printable_only) {
        // 只生成可打印ASCII字符 (32-126)
        std::uniform_int_distribution<int> dis(32, 126);
        for (size_t i = 0; i < length; ++i) {
            result.push_back(static_cast<char>(dis(gen)));
        }
    } else {
        // 生成所有字符 (0-255)
        std::uniform_int_distribution<int> dis(0, 255);
        for (size_t i = 0; i < length; ++i) {
            result.push_back(static_cast<char>(dis(gen)));
        }
    }

    return result;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_InitDeviceInfo(int nFd) {
    init_device_info oDevInfo = {0};
    oDevInfo.pid = getpid();
    oDevInfo.tid = gettid();

    char old_name[MY_TASK_COMM_LEN + 1] = {0};
    if (prctl(PR_GET_NAME, old_name, 0, 0, 0)) {
        return FALSE;
    }
    std::string random_name = GenerateRandomString(MY_TASK_COMM_LEN - 1);
    random_name.push_back('\0');
    if (prctl(PR_SET_NAME, random_name.c_str(), 0, 0, 0)) {
        return FALSE;
    }

    std::string myCmdline = _GetFileContent("/proc/self/cmdline");
    strncpy(oDevInfo.myName, random_name.c_str(), sizeof(oDevInfo.myName) - 1);
    strncpy(oDevInfo.myCmdline, myCmdline.c_str(), sizeof(oDevInfo.myCmdline) - 1);

    ssize_t ret = _rwProcMemDriver_MyIoctl(nFd, CMD_INIT_DEVICE_INFO, 0, 0, 0,
                                          (char*)&oDevInfo, sizeof(oDevInfo));

    if (prctl(PR_SET_NAME, old_name, 0, 0, 0)) {
        return FALSE;
    }
    TRACE("InitDeviceInfo ioctl return=%zd\n", ret);
    return (ret == 0) ? TRUE : FALSE;
}

uint64_t CMemoryReaderWriter::_rwProcMemDriver_OpenProcess(int nFd, uint64_t pid) {
    if (nFd < 0 || pid == 0) { return 0; }
    uint64_t handle = 0;
    ssize_t res = _rwProcMemDriver_MyIoctl(nFd, CMD_OPEN_PROCESS, pid, 0, 0, (char*)&handle, sizeof(handle));
    if (res != 0) {
        TRACE("OpenProcess ioctl():%s\n", strerror(errno));
        return 0;
    }
    return handle;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_ReadProcessMemory(int nFd, uint64_t pid, uint64_t lpBaseAddress,
                                                            void* lpBuffer, size_t nSize, size_t* lpNumberOfBytesRead, BOOL bIsForceRead) {
    if (lpBaseAddress == 0 || nFd < 0 || pid == 0 || nSize == 0) {
        return FALSE;
    }
    ssize_t outOfRead = _rwProcMemDriver_MyIoctl(nFd, CMD_READ_PROCESS_MEMORY,
                                                pid, lpBaseAddress, bIsForceRead ? 1 : 0, (char*)lpBuffer, nSize);
    if (outOfRead <= 0) {
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        return FALSE;
    }
    if (lpNumberOfBytesRead) {
        *lpNumberOfBytesRead = outOfRead;
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_WriteProcessMemory(int nFd, uint64_t pid, uint64_t lpBaseAddress,
                                                             void* lpBuffer, size_t nSize, size_t* lpNumberOfBytesWritten, BOOL bIsForceWrite) {
    if (lpBaseAddress == 0 || nFd < 0 || !pid || nSize == 0) {
        return FALSE;
    }
    ssize_t outOfWrite = _rwProcMemDriver_MyIoctl(nFd, CMD_WRITE_PROCESS_MEMORY,
                                                 pid, lpBaseAddress, bIsForceWrite ? 1 : 0, (char*)lpBuffer, nSize);
    if (outOfWrite <= 0) {
        TRACE("WriteProcessMemory ioctl(): %d\n", outOfWrite);
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
        return FALSE;
    }
    if (lpNumberOfBytesWritten) {
        *lpNumberOfBytesWritten = outOfWrite;
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_CloseHandle(int nFd, uint64_t pid) {
    if (nFd < 0 || !pid) { return FALSE; }
    if (_rwProcMemDriver_MyIoctl(nFd, CMD_CLOSE_PROCESS, pid, 0, 0, NULL, 0) != 0) {
        TRACE("CloseHandle ioctl():%s\n", strerror(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_VirtualQueryExFull(int nFd, uint64_t pid, BOOL showPhy, 
                                                             std::vector<DRIVER_REGION_INFO>& vOutput) {
    if (nFd < 0 || !pid) { return FALSE; }
    ssize_t count = _rwProcMemDriver_MyIoctl(nFd, CMD_GET_PROCESS_MAPS_COUNT, pid, 0, 0, NULL, 0);
    TRACE("VirtualQueryExFull count %zd\n", count);
    if (count <= 0) {
        TRACE("VirtualQueryExFull ioctl():%d\n", count);
        return FALSE;
    }

    static thread_local IoctlBufferPool pool;
    uint64_t big_buf_len = sizeof(map_entry) * (count + 50);
    char* big_buf = pool.getBuffer(big_buf_len);
    if (!big_buf) return FALSE;
    memset(big_buf, 0, big_buf_len);
    ssize_t res = _rwProcMemDriver_MyIoctl(nFd, CMD_GET_PROCESS_MAPS_LIST, pid, 0, 0, big_buf, big_buf_len);
    TRACE("VirtualQueryExFull res %zd\n", res);
    if (res <= 0) {
        TRACE("VirtualQueryExFull ioctl():%s\n", strerror(errno));
        return FALSE;
    }
    auto entries = reinterpret_cast<map_entry*>(big_buf);
    for (ssize_t i = 0; i < res; ++i) {
        const map_entry& e = entries[i];
        DRIVER_REGION_INFO rInfo = {0};
        rInfo.baseaddress = e.start;
        rInfo.size = e.end - e.start;
        bool r = e.flags[0], w = e.flags[1], x = e.flags[2];
        if (x) {
            rInfo.protection = w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        } else {
            if (w) rInfo.protection = PAGE_READWRITE;
            else if (r) rInfo.protection = PAGE_READONLY;
            else rInfo.protection = PAGE_NOACCESS;
        }
        // 解析 type
        rInfo.type = (e.flags[3] ? MEM_MAPPED : MEM_PRIVATE);
        // 复制名字
        strncpy(rInfo.name, e.path, sizeof(rInfo.name) - 1);
        if (showPhy) {
            DRIVER_REGION_INFO cur = rInfo;
            bool in_phy = false;
            for (uint64_t addr = e.start; addr < e.end; addr += getpagesize()) {
                if (_rwProcMemDriver_CheckProcessMemAddrValid(nFd, pid, addr)) {
                    if (!in_phy) {
                        in_phy = true;
                        cur.baseaddress = addr;
                    }
                } else if (in_phy) {
                    in_phy = false;
                    cur.size = addr - cur.baseaddress;
                    vOutput.push_back(cur);
                }
            }
            if (in_phy) {
                cur.size = e.end - cur.baseaddress;
                vOutput.push_back(cur);
            }
        } else {
            vOutput.push_back(rInfo);
        }
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_CheckProcessMemAddrValid(int nFd, uint64_t pid, uint64_t lpBaseAddress) {
    if (nFd < 0 || !pid) { return FALSE; }
    if (_rwProcMemDriver_MyIoctl(nFd, CMD_CHECK_PROCESS_ADDR_PHY, pid, lpBaseAddress, 0, NULL, 0) == 1) {
        return TRUE;
    }
    return FALSE;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_GetPidList(int nFd, std::vector<int>& vOutput) {
    if (nFd < 0) return FALSE;

    // 第一次：只用来取 count1
    ssize_t count1 = _rwProcMemDriver_MyIoctl(nFd, CMD_GET_PID_LIST, 0, 0, 0, NULL, 0);
    if (count1 <= 0) {
        return FALSE;
    }

    // 第二次：根据 count1 分配足够空间
    static thread_local IoctlBufferPool pool;
    uint64_t len = (uint64_t)count1 * sizeof(int);
    char* buf = pool.getBuffer(len);
    if (!buf) {
        return FALSE;
    }
    memset(buf, 0, len);
    ssize_t count2 = _rwProcMemDriver_MyIoctl(nFd, CMD_GET_PID_LIST, 0, 0, 0, buf, len);
    if (count2 != count1) {
        return FALSE;
    }

    // 读取数据前，再做一次边界检查
    for (int i = 0; i < count1; i++) {
        int pid = *(int*)(buf + i * sizeof(int));
        vOutput.push_back(pid);
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_SetProcessRoot(int nFd, uint64_t pid) {
    if (nFd < 0 || !pid) { return FALSE; }
    ssize_t res = _rwProcMemDriver_MyIoctl(nFd, CMD_SET_PROCESS_ROOT, pid, 0, 0, NULL, 0);
    if (res != 0) {
        TRACE("SetProcessRoot ioctl():%s\n", strerror(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_GetProcessPhyMemSize(int nFd, uint64_t pid, uint64_t* outRss) {
    if (nFd < 0 || !pid) { return FALSE; }
    uint64_t out = 0;
    ssize_t res = _rwProcMemDriver_MyIoctl(nFd, CMD_GET_PROCESS_RSS, pid, 0, 0, (char*)&out, sizeof(out));
    if (res != 0) {
        TRACE("GetProcessPhyMemSize ioctl():%s\n", strerror(errno));
        return FALSE;
    }
    *outRss = out;
    return TRUE;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_GetProcessCmdline(int nFd, uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize) {
    if (nFd < 0 || !pid || bufSize <= 0) { return FALSE; }
    char tmp[256]={0};
    ssize_t res = _rwProcMemDriver_MyIoctl(nFd, CMD_GET_PROCESS_CMDLINE, pid, 0, 0, tmp, sizeof(tmp));
    if (res <= 0) {
        TRACE("GetProcessCmdline ioctl():%s\n", strerror(-res));
        return FALSE;
    }

    size_t copyLen = strlen(tmp);
    if (copyLen >= bufSize) copyLen = bufSize - 1;
    memcpy(lpOutCmdlineBuf, tmp, copyLen);
    lpOutCmdlineBuf[copyLen] = '\0';
    return copyLen > 0;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_GetProcessComm(int nFd, uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize) {
    if (nFd < 0 || !pid || bufSize <= 0) { return FALSE; }
    ssize_t res = _rwProcMemDriver_MyIoctl(nFd, CMD_GET_PROCESS_COMM, pid, 0, 0, lpOutCmdlineBuf, bufSize);
    if (res < 0) {
        TRACE("GetProcessComm ioctl():%s\n", strerror(-res));
        return FALSE;
    }
    return (res == 0) ? TRUE : FALSE;
}

BOOL CMemoryReaderWriter::_rwProcMemDriver_ReadProcessMemoryBulk(int nFd, uint64_t pid, uint64_t lpBaseAddress,
                                                                void* lpBuffer, size_t nSize, size_t* lpNumberOfBytesRead, BOOL bIsForceRead) {
    if (lpBaseAddress == 0 || nFd < 0 || pid == 0 || nSize == 0) {
        return FALSE;
    }
    ssize_t outOfRead = _rwProcMemDriver_MyIoctl(nFd, CMD_READ_PROCESS_MEMORY_BULK,
                                                pid, lpBaseAddress, bIsForceRead ? 1 : 0, (char*)lpBuffer, nSize);
    if (outOfRead < 0) {
        TRACE("ReadProcessMemoryBulk ioctl(): %zd\n", outOfRead);
        return FALSE;
    }
    if (lpNumberOfBytesRead) { *lpNumberOfBytesRead = (size_t)outOfRead; }
    return TRUE;
}

// 硬件断点相关函数实现
int CMemoryReaderWriter::_hwbpProcDriver_GetNumBRPS(int nDriverLink) {
    if (nDriverLink < 0) { return 0; }
    ssize_t res = _rwProcMemDriver_MyIoctl(nDriverLink, CMD_HWBP_GET_NUM_BRPS, 0, 0, 0, NULL, 0);
    return res;
}

int CMemoryReaderWriter::_hwbpProcDriver_GetNumWRPS(int nDriverLink) {
    if (nDriverLink < 0) { return 0; }
    ssize_t res = _rwProcMemDriver_MyIoctl(nDriverLink, CMD_HWBP_GET_NUM_WRPS, 0, 0, 0, NULL, 0);
    return res;
}

uint64_t CMemoryReaderWriter::_hwbpProcDriver_InstProcessHwBp(int nDriverLink, uint64_t hProcess, uint64_t lpBaseAddress,
                                                             unsigned int hwbpLen, unsigned int hwbpType) {
    if (nDriverLink < 0 || !hProcess || !lpBaseAddress) { return 0; }
  		uint64_t param3 = 0;
		char *p = (char*)&param3;
		p[0] = hwbpLen;
		p[1] = hwbpType;
		
		uint64_t hHwbp;
		ssize_t res = _rwProcMemDriver_MyIoctl(nDriverLink, CMD_HWBP_INSTALL, hProcess, lpBaseAddress, param3, (char*)&hHwbp, sizeof(hHwbp));
		if (res != 0) {
			//printf("InstProcessHwBp _hwbpProcDriver_MyIoctl():%s\n", strerror(errno));
			return 0;
		}
		return hHwbp;
}

BOOL CMemoryReaderWriter::_hwbpProcDriver_UninstProcessHwBp(int nDriverLink, uint64_t hHwbp) {
    if (nDriverLink < 0 || !hHwbp) { return FALSE; }
    ssize_t res = _rwProcMemDriver_MyIoctl(nDriverLink, CMD_HWBP_UNINSTALL, hHwbp, 0, 0, 0, 0);
    if (res != 0) {
        return FALSE;
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_hwbpProcDriver_SuspendProcessHwBp(int nDriverLink, uint64_t hHwbp) {
    if (nDriverLink < 0 || !hHwbp) { return FALSE; }
    ssize_t res = _rwProcMemDriver_MyIoctl(nDriverLink, CMD_HWBP_SUSPEND, hHwbp, 0, 0, 0, 0);
    if (res != 0) {
        return FALSE;
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_hwbpProcDriver_ResumeProcessHwBp(int nDriverLink, uint64_t hHwbp) {
    if (nDriverLink < 0 || !hHwbp) { return FALSE; }
    ssize_t res = _rwProcMemDriver_MyIoctl(nDriverLink, CMD_HWBP_RESUME, hHwbp, 0, 0, 0, 0);
    if (res != 0) {
        return FALSE;
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_hwbpProcDriver_ReadHwBpInfo(int nDriverLink, uint64_t hHwbp, uint64_t& nHitTotalCount,
                                                       std::vector<HW_HIT_ITEM>& vOutput) {
    if (nDriverLink < 0 || !hHwbp) { return FALSE; }
		#pragma pack(1)
		struct {
			uint64_t nHitTotalCount;
			uint64_t nHitItemArrCount;
		} userData = {0};
		#pragma pack()
		ssize_t res = _rwProcMemDriver_MyIoctl(nDriverLink, CMD_HWBP_GET_HIT_COUNT, hHwbp, 0, 0, (char*)&userData, sizeof(userData));
		//printf("ioctl res %d\n", res);
		if (res != 0) {
			//printf("ioctl():%s\n", strerror(errno));
			return FALSE;
		}
		nHitTotalCount = userData.nHitTotalCount;
    //printf("nHitTotalCount:%lu, nHitItemArrCount:%lu\n", userData.nHitTotalCount, userData.nHitItemArrCount);
    if (userData.nHitItemArrCount > 0) {
      			std::vector<char> big_buf;
			big_buf.resize(sizeof(struct HW_HIT_ITEM) * userData.nHitItemArrCount);
			
        res = _rwProcMemDriver_MyIoctl(nDriverLink, CMD_HWBP_READ_INFO, hHwbp, 0, 0, big_buf.data(), big_buf.size());
        if (res < 0) {
            LOGDF("ReadHwBpInfo ioctl():%s\n", strerror(errno));
            return FALSE;
        }

      		auto* items = reinterpret_cast<const HW_HIT_ITEM*>(big_buf.data());
			size_t count = userData.nHitItemArrCount;
			vOutput.insert(vOutput.end(), items, items + count);
    }
    return TRUE;
}

BOOL CMemoryReaderWriter::_hwbpProcDriver_SetHookPC(int nDriverLink, uint64_t pc) {
    if (nDriverLink < 0) { return FALSE; }
    ssize_t res = _rwProcMemDriver_MyIoctl(nDriverLink, CMD_HWBP_SET_HOOK_PC, pc, 0, 0, 0, 0);
    if (res != 0) {
        return FALSE;
    }
    return TRUE;
}


#endif /*__linux__*/

// 公共接口实现
BOOL CMemoryReaderWriter::ReadHwBpInfo(uint64_t hHwbp, uint64_t& nHitTotalCount, std::vector<HW_HIT_ITEM>& vOutput) {
    return _InternalReadHwBpInfo(hHwbp, nHitTotalCount, vOutput);
}

BOOL CMemoryReaderWriter::SetHookPC(uint64_t pc) {
    return _hwbpProcDriver_SetHookPC(m_nFd, pc);
}

// get_maps 实现
BOOL CMemoryReaderWriter::get_maps(uint64_t pid, std::vector<DRIVER_REGION_INFO>& out) {
    out.clear();
    if (m_nFd < 0 || pid == 0) {
        return FALSE;
    }

    ssize_t count = _rwProcMemDriver_MyIoctl(m_nFd, CMD_GET_PROCESS_MAPS_COUNT, pid, 0, 0, nullptr, 0);
    if (count <= 0) {
        return FALSE;
    }

    static thread_local IoctlBufferPool pool;
    uint64_t buf_len = sizeof(map_entry) * (count + 50);
    char* buf = pool.getBuffer(buf_len);
    if (!buf) return FALSE;
    memset(buf, 0, buf_len);

    ssize_t res = _rwProcMemDriver_MyIoctl(m_nFd, CMD_GET_PROCESS_MAPS_LIST, pid, 0, 0, buf, buf_len);
    if (res <= 0) {
        return FALSE;
    }

    auto* entries = reinterpret_cast<map_entry*>(buf);
    for (ssize_t i = 0; i < res; ++i) {
        const map_entry& e = entries[i];
        DRIVER_REGION_INFO ri = {};
        ri.baseaddress = e.start;
        ri.size = e.end - e.start;

        bool r = e.flags[0] != 0;
        bool w = e.flags[1] != 0;
        bool x = e.flags[2] != 0;

        if (x) {
            ri.protection = w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        } else if (w) {
            ri.protection = PAGE_READWRITE;
        } else if (r) {
            ri.protection = PAGE_READONLY;
        } else {
            ri.protection = PAGE_NOACCESS;
        }

        ri.type = e.flags[3] ? MEM_MAPPED : MEM_PRIVATE;
        strncpy(ri.name, e.path, sizeof(ri.name) - 1);

        out.push_back(ri);
    }
    return TRUE;
}

// 工具函数实现
std::string CMemoryReaderWriter::_GetFileContent(const char* lpszFilePath) {
    std::ifstream inFile(lpszFilePath);
    if (!inFile.is_open()) {
        return {};
    }
    const int size = 4096;
    std::shared_ptr<unsigned char> spBuf(new (std::nothrow) unsigned char[size], std::default_delete<unsigned char[]>());
    if (!spBuf) {
        inFile.close();
        return {};
    }
    inFile.read((char*)spBuf.get(), size);
    auto bytesRead = inFile.gcount();
    inFile.close();
    return std::string((char*)spBuf.get(), bytesRead);
} 