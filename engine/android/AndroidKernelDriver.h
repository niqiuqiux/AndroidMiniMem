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

    uint64_t AddHardwareBreakpoint(int tid, uint64_t address, unsigned int len, unsigned int type);
    bool RemoveHardwareBreakpoint(uint64_t handle);
    bool DisableHardwareBreakpoint(uint64_t handle);
    bool EnableHardwareBreakpoint(uint64_t handle);
    bool ReadHardwareBreakpointInfo(uint64_t handle,
                                    uint64_t& totalHitCount,
                                    std::vector<HW_HIT_ITEM>& out);

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
