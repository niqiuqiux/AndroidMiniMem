#include "AndroidKernelDriver.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <unistd.h>

#include "Logger.hpp"

namespace {

void CopyStringToBuffer(const std::string& value, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    std::memset(out, 0, outSize);
    const size_t copyLen = std::min(outSize - 1, value.size());
    if (copyLen > 0) {
        std::memcpy(out, value.data(), copyLen);
    }
}

bool IsValidPidHandle(uint64_t handle) {
    return handle > 0 && handle <= static_cast<uint64_t>(std::numeric_limits<int>::max());
}

size_t PageSizeOrDefault() {
    long pageSize = ::sysconf(_SC_PAGESIZE);
    return pageSize > 0 ? static_cast<size_t>(pageSize) : 4096;
}

UxnOperationResult UxnResultFromCall(int result) {
    UxnOperationResult operation;
    operation.success = result == 0;
    operation.errorCode = operation.success ? 0 : (errno != 0 ? errno : EIO);
    return operation;
}

}  // namespace

AndroidKernelDriver& KernelDriver() {
    static auto* driver = new AndroidKernelDriver();
    return *driver;
}

int AndroidKernelDriver::Connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ni_ && ni_->valid()) {
        return 0;
    }

    errno = 0;
    auto next = std::make_unique<NiDriver>();
    if (!next->valid()) {
        int savedErrno = errno != 0 ? errno : ENODEV;
        LOGEF("NiDriver connect failed: %s", std::strerror(savedErrno));
        ni_.reset();
        return -savedErrno;
    }

    LOGDF("NiDriver connected fd=%d", next->fd());
    ni_ = std::move(next);
    return 0;
}

bool AndroidKernelDriver::Disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool hadDriver = ni_ && ni_->valid();
    ni_.reset();
    {
        std::lock_guard<std::mutex> hlock(hwbpMutex_);
        hwbpHitTotals_.clear();
    }
    return hadDriver;
}

bool AndroidKernelDriver::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ni_ && ni_->valid();
}

bool AndroidKernelDriver::GetExpireTime(uint64_t& outExpireTime) {
    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return false;
    }

    int64_t expireTime = d->get_expire_time();
    if (expireTime < 0) {
        return false;
    }

    outExpireTime = static_cast<uint64_t>(expireTime);
    return true;
}

uint64_t AndroidKernelDriver::OpenProcess(int pid) {
    if (pid <= 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return 0;
    }

    int handle = d->open_process(pid);
    return handle > 0 ? static_cast<uint64_t>(handle) : 0;
}

bool AndroidKernelDriver::CloseProcess(uint64_t processHandle) {
    if (!IsValidPidHandle(processHandle)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return false;
    }

    d->close_process(static_cast<int>(processHandle));
    return true;
}

ssize_t AndroidKernelDriver::ReadMemory(uint64_t processHandle,
                                        uint64_t address,
                                        void* buffer,
                                        size_t size) {
    if (!IsValidPidHandle(processHandle) || address == 0 || buffer == nullptr || size == 0) {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return -ENODEV;
    }

    return d->read_memory(static_cast<int>(processHandle), address, buffer, size);
}

ssize_t AndroidKernelDriver::WriteMemory(uint64_t processHandle,
                                         uint64_t address,
                                         const void* buffer,
                                         size_t size) {
    if (!IsValidPidHandle(processHandle) || address == 0 || buffer == nullptr || size == 0) {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return -ENODEV;
    }

    return d->write_memory(static_cast<int>(processHandle), address, buffer, size);
}

bool AndroidKernelDriver::QueryMaps(uint64_t processHandle,
                                    bool showPhysical,
                                    std::vector<DRIVER_REGION_INFO>& out) {
    out.clear();
    if (!IsValidPidHandle(processHandle)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return false;
    }

    auto maps = d->get_all_maps(static_cast<int>(processHandle));
    if (maps.empty()) {
        return false;
    }

    const size_t pageSize = PageSizeOrDefault();
    for (const auto& entry : maps) {
        DRIVER_REGION_INFO region = ConvertMapEntry(entry);
        if (!showPhysical) {
            out.push_back(region);
            continue;
        }

        bool inPhysicalRange = false;
        DRIVER_REGION_INFO current = region;
        for (uint64_t addr = entry.start; addr < entry.end; addr += pageSize) {
            bool valid = d->check_phy_addr(static_cast<int>(processHandle), addr);
            if (valid && !inPhysicalRange) {
                inPhysicalRange = true;
                current = region;
                current.baseaddress = addr;
            } else if (!valid && inPhysicalRange) {
                inPhysicalRange = false;
                current.size = addr - current.baseaddress;
                out.push_back(current);
            }

            if (std::numeric_limits<uint64_t>::max() - addr < pageSize) {
                break;
            }
        }

        if (inPhysicalRange) {
            current.size = entry.end - current.baseaddress;
            out.push_back(current);
        }
    }

    return true;
}

bool AndroidKernelDriver::IsAddressValid(uint64_t processHandle, uint64_t address) {
    if (!IsValidPidHandle(processHandle) || address == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    return d && d->check_phy_addr(static_cast<int>(processHandle), address);
}

bool AndroidKernelDriver::GetPidList(std::vector<int>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return false;
    }

    auto pids = d->get_pid_list();
    out.reserve(pids.size());
    for (int32_t pid : pids) {
        out.push_back(static_cast<int>(pid));
    }
    return true;
}

bool AndroidKernelDriver::GetProcessCmdline(int pid, char* out, size_t outSize) {
    if (pid <= 0 || !out || outSize == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return false;
    }

    std::string value = d->get_process_cmdline(pid);
    CopyStringToBuffer(value, out, outSize);
    return !value.empty();
}

bool AndroidKernelDriver::GetProcessComm(int pid, char* out, size_t outSize) {
    if (pid <= 0 || !out || outSize == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return false;
    }

    std::string value = d->get_process_comm(pid);
    CopyStringToBuffer(value, out, outSize);
    return !value.empty();
}

uint64_t AndroidKernelDriver::GetSoBaseAddress(int pid, const std::string& soName) {
    if (pid <= 0 || soName.empty()) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return 0;
    }

    return d->get_so_base_address(pid, soName);
}

HardwareBreakpointInstallResult AndroidKernelDriver::AddHardwareBreakpoint(
    int tid, uint64_t address, unsigned int len, unsigned int type,
    bool forceReclaim) {
    HardwareBreakpointInstallResult result;
    if (tid <= 0 || address == 0) {
        result.errorCode = EINVAL;
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        result.errorCode = ENODEV;
        return result;
    }

    ni_hwbp_install req{};
    req.tid = tid;
    req.addr = address;
    req.len = type == NI_HW_BREAKPOINT_X ? NI_HW_BREAKPOINT_LEN_4 : len;
    req.type = type;
    req.flags = NI_HWBP_F_AUTO_REARM;
    if (forceReclaim) {
        req.flags |= NI_HWBP_F_FORCE_RECLAIM;
    }

    errno = 0;
    result.interfaceResult = d->hwbp_install(req);
    result.reclaimedSlots = req.reclaimed_slots;
    result.blockedSlots = req.blocked_slots;
    if (result.interfaceResult != 0) {
        result.errorCode = errno != 0 ? errno : EIO;
        return result;
    }
    if (req.handle == 0) {
        result.interfaceResult = -1;
        result.errorCode = EPROTO;
        return result;
    }

    {
        std::lock_guard<std::mutex> hlock(hwbpMutex_);
        hwbpHitTotals_[req.handle] = 0;
    }
    result.handle = req.handle;
    return result;
}

bool AndroidKernelDriver::QueryHardwareBreakpointTask(
    int tid, uint32_t capacity, HardwareBreakpointTaskQueryResult& out) {
    out = HardwareBreakpointTaskQueryResult{};
    out.tid = tid;
    out.summary.tid = tid;
    if (tid <= 0 || capacity == 0 || capacity > NI_HWBP_MAX_QUERY_ENTRIES) {
        out.errorCode = EINVAL;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        out.errorCode = ENODEV;
        return false;
    }

    errno = 0;
    const auto snapshot = d->hwbp_query_task(tid, capacity);
    if (!snapshot) {
        out.errorCode = errno != 0 ? errno : EIO;
        return false;
    }

    out.success = true;
    out.errorCode = 0;
    out.summary = snapshot->summary;
    out.entries = snapshot->entries;
    out.summary.entries = 0;
    if (out.summary.tid != tid ||
        out.summary.count != out.entries.size() ||
        out.summary.count > capacity) {
        out.success = false;
        out.errorCode = EPROTO;
        out.entries.clear();
        out.summary.count = 0;
        return false;
    }
    return true;
}

bool AndroidKernelDriver::RemoveHardwareBreakpoint(uint64_t handle) {
    if (handle == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    if (!d) {
        return false;
    }

    const bool ok = d->hwbp_uninstall(handle) == 0;
    {
        std::lock_guard<std::mutex> hlock(hwbpMutex_);
        hwbpHitTotals_.erase(handle);
    }
    return ok;
}

bool AndroidKernelDriver::DisableHardwareBreakpoint(uint64_t handle) {
    if (handle == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    return d && d->hwbp_disable(handle) == 0;
}

bool AndroidKernelDriver::EnableHardwareBreakpoint(uint64_t handle) {
    if (handle == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* d = driverLocked();
    return d && d->hwbp_enable(handle) == 0;
}

bool AndroidKernelDriver::ReadHardwareBreakpointInfo(uint64_t handle,
                                                    uint64_t& totalHitCount,
                                                    std::vector<HW_HIT_ITEM>& out) {
    totalHitCount = 0;
    if (handle == 0) {
        return false;
    }

    uint64_t newHits = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        NiDriver* d = driverLocked();
        if (!d) {
            return false;
        }

        constexpr uint32_t capacity = NI_HWBP_MAX_EVENTS_PER_READ;
        for (int drain = 0; drain < 16; ++drain) {
            auto events = d->hwbp_read_events(handle, capacity);
            if (events.empty()) {
                break;
            }

            for (const auto& event : events) {
                if (event.type != NI_HWBP_EVENT_HIT) {
                    continue;
                }
                out.push_back(ConvertHwbpEvent(event));
                ++newHits;
            }

            if (events.size() < capacity) {
                break;
            }
        }
    }

    std::lock_guard<std::mutex> hlock(hwbpMutex_);
    uint64_t& total = hwbpHitTotals_[handle];
    total += newHits;
    totalHitCount = total;
    return true;
}

UxnOperationResult AndroidKernelDriver::InstallUxnBreakpoint(
    ni_uxn_install& request) {
    if (request.pid == 0 || request.addr == 0 || (request.addr & 0x3) != 0 ||
        request.flags != 0) {
        return {false, EINVAL};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* driver = driverLocked();
    if (!driver) {
        return {false, ENODEV};
    }

    errno = 0;
    UxnOperationResult result = UxnResultFromCall(driver->uxn_install(request));
    if (result.success && request.slot >= NI_UXN_MAX_SLOTS) {
        return {false, EPROTO};
    }
    return result;
}

UxnOperationResult AndroidKernelDriver::RemoveUxnBreakpoint(
    uint32_t pid, uint64_t address) {
    if (pid == 0 || address == 0 || (address & 0x3) != 0) {
        return {false, EINVAL};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* driver = driverLocked();
    if (!driver) {
        return {false, ENODEV};
    }

    errno = 0;
    return UxnResultFromCall(driver->uxn_remove(pid, address));
}

UxnOperationResult AndroidKernelDriver::WaitUxnBreakpoint(
    ni_uxn_wait& request) {
    if (request.slot != NI_UXN_WAIT_ANY_SLOT &&
        request.slot >= NI_UXN_MAX_SLOTS) {
        return {false, EINVAL};
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!driverLocked()) {
            return {false, ENODEV};
        }
    }

    errno = 0;
    NiDriver waitDriver;
    if (!waitDriver.valid()) {
        return {false, errno != 0 ? errno : ENODEV};
    }
    errno = 0;
    return UxnResultFromCall(waitDriver.uxn_wait(request));
}

UxnOperationResult AndroidKernelDriver::ResumeUxnBreakpoint(
    const ni_uxn_resume& request) {
    if (request.slot >= NI_UXN_MAX_SLOTS ||
        (request.flags & ~NI_UXN_RESUME_F_SET_REGS) != 0) {
        return {false, EINVAL};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* driver = driverLocked();
    if (!driver) {
        return {false, ENODEV};
    }

    errno = 0;
    return UxnResultFromCall(driver->uxn_resume(request));
}

UxnOperationResult AndroidKernelDriver::GetUxnBreakpointStatus(
    uint32_t slot, ni_uxn_status& status) {
    status = {};
    status.slot = slot;
    if (slot >= NI_UXN_MAX_SLOTS) {
        return {false, EINVAL};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* driver = driverLocked();
    if (!driver) {
        return {false, ENODEV};
    }

    errno = 0;
    auto queried = driver->uxn_get_status(slot);
    if (!queried) {
        return {false, errno != 0 ? errno : EIO};
    }
    if (queried->slot != slot || queried->state > NI_UXN_STATE_STEPPING) {
        return {false, EPROTO};
    }
    status = *queried;
    return {true, 0};
}

UxnOperationResult AndroidKernelDriver::ClearUxnBreakpoints() {
    std::lock_guard<std::mutex> lock(mutex_);
    NiDriver* driver = driverLocked();
    if (!driver) {
        return {false, ENODEV};
    }

    errno = 0;
    return UxnResultFromCall(driver->uxn_clear());
}

NiDriver* AndroidKernelDriver::driverLocked() {
    return (ni_ && ni_->valid()) ? ni_.get() : nullptr;
}

const NiDriver* AndroidKernelDriver::driverLocked() const {
    return (ni_ && ni_->valid()) ? ni_.get() : nullptr;
}

DRIVER_REGION_INFO AndroidKernelDriver::ConvertMapEntry(const ni_map_entry& entry) {
    DRIVER_REGION_INFO region{};
    region.baseaddress = static_cast<uint64_t>(entry.start);
    region.size = entry.end > entry.start ? static_cast<uint64_t>(entry.end - entry.start) : 0;

    const bool readable = entry.flags[0] != 0;
    const bool writable = entry.flags[1] != 0;
    const bool executable = entry.flags[2] != 0;
    if (executable) {
        region.protection = writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    } else if (writable) {
        region.protection = PAGE_READWRITE;
    } else if (readable) {
        region.protection = PAGE_READONLY;
    } else {
        region.protection = PAGE_NOACCESS;
    }

    region.type = entry.flags[3] ? MEM_MAPPED : MEM_PRIVATE;
    std::strncpy(region.name, entry.path, sizeof(region.name) - 1);
    return region;
}

HW_HIT_ITEM AndroidKernelDriver::ConvertHwbpEvent(const ni_hwbp_event& event) {
    HW_HIT_ITEM item{};
    item.task_id = static_cast<uint64_t>(event.tid);
    item.hit_addr = event.addr;
    item.hit_time = event.timestamp_ns;
    std::memcpy(item.regs_info.regs, event.regs.regs, sizeof(item.regs_info.regs));
    item.regs_info.sp = event.regs.sp;
    item.regs_info.pc = event.regs.pc;
    item.regs_info.pstate = event.regs.pstate;
    item.regs_info.orig_x0 = event.regs.orig_x0;
    item.regs_info.syscallno = event.regs.syscallno;
    return item;
}
