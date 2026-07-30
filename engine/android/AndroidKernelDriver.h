#ifndef ANDROID_KERNEL_DRIVER_H_
#define ANDROID_KERNEL_DRIVER_H_

#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

#include "AndroidDriverTypes.h"
#include "NiDriver.h"

struct HardwareBreakpointInstallResult {
    uint64_t handle = 0;
    int interfaceResult = -1;
    int errorCode = 0;
    uint32_t reclaimedSlots = 0;
    uint32_t blockedSlots = 0;
};

struct HardwareBreakpointTaskQueryResult {
    int tid = 0;
    bool success = false;
    int errorCode = 0;
    ni_hwbp_task_query summary{};
    std::vector<ni_hwbp_task_entry> entries;
};

struct UxnOperationResult {
    bool success = false;
    int errorCode = 0;
};

class AndroidKernelDriver {
public:
    AndroidKernelDriver() = default;
    ~AndroidKernelDriver() { Disconnect(); }

    AndroidKernelDriver(const AndroidKernelDriver&) = delete;
    AndroidKernelDriver& operator=(const AndroidKernelDriver&) = delete;

    int Connect();
    bool Disconnect();
    bool IsConnected() const;
    bool GetExpireTime(uint64_t& outExpireTime);

    uint64_t OpenProcess(int pid);
    bool CloseProcess(uint64_t processHandle);

    ssize_t ReadMemory(uint64_t processHandle, uint64_t address, void* buffer, size_t size);
    ssize_t WriteMemory(uint64_t processHandle, uint64_t address, const void* buffer, size_t size);

    bool QueryMaps(uint64_t processHandle, bool showPhysical, std::vector<DRIVER_REGION_INFO>& out);
    bool IsAddressValid(uint64_t processHandle, uint64_t address);

    bool GetPidList(std::vector<int>& out);
    bool GetProcessCmdline(int pid, char* out, size_t outSize);
    bool GetProcessComm(int pid, char* out, size_t outSize);
    uint64_t GetSoBaseAddress(int pid, const std::string& soName);

    HardwareBreakpointInstallResult AddHardwareBreakpoint(
        int tid, uint64_t address, unsigned int len, unsigned int type,
        bool forceReclaim = false);
    bool QueryHardwareBreakpointTask(int tid, uint32_t capacity,
                                     HardwareBreakpointTaskQueryResult& out);
    bool RemoveHardwareBreakpoint(uint64_t handle);
    bool DisableHardwareBreakpoint(uint64_t handle);
    bool EnableHardwareBreakpoint(uint64_t handle);
    bool ReadHardwareBreakpointInfo(uint64_t handle,
                                    uint64_t& totalHitCount,
                                    std::vector<HW_HIT_ITEM>& out);

    UxnOperationResult InstallUxnBreakpoint(ni_uxn_install& request);
    UxnOperationResult RemoveUxnBreakpoint(uint32_t pid, uint64_t address);
    UxnOperationResult WaitUxnBreakpoint(ni_uxn_wait& request);
    UxnOperationResult ResumeUxnBreakpoint(const ni_uxn_resume& request);
    UxnOperationResult GetUxnBreakpointStatus(uint32_t slot,
                                              ni_uxn_status& status);
    UxnOperationResult ClearUxnBreakpoints();

private:
    NiDriver* driverLocked();
    const NiDriver* driverLocked() const;
    static DRIVER_REGION_INFO ConvertMapEntry(const ni_map_entry& entry);
    static HW_HIT_ITEM ConvertHwbpEvent(const ni_hwbp_event& event);

    mutable std::mutex mutex_;
    std::unique_ptr<NiDriver> ni_;

    std::mutex hwbpMutex_;
    std::unordered_map<uint64_t, uint64_t> hwbpHitTotals_;
};

AndroidKernelDriver& KernelDriver();

#endif /* ANDROID_KERNEL_DRIVER_H_ */
