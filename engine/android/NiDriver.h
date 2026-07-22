#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>

/* ══════════════════════════════════════════════════════════════════
 *  Kernel protocol structures (must match kernel binary layout)
 * ══════════════════════════════════════════════════════════════════ */

struct ni_request {
	uint8_t  cmd;
	uint8_t  flags;
	uint16_t reserved;
	uint32_t reserved2;
	uint64_t param1;
	uint64_t param2;
	uint64_t param3;
	uint64_t buf_size;
} __attribute__((packed));

constexpr uint32_t NI_CTL_MAGIC = 0x4e494d54u;
constexpr uint16_t NI_CTL_VERSION = 1;

struct ni_protocol_info {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
} __attribute__((packed));

static_assert(sizeof(ni_request) == 40, "ni_request ABI size mismatch");
static_assert(offsetof(ni_request, param1) == 8, "ni_request ABI alignment mismatch");

/* ── Commands ───────────────────────────────────────────────────── */

enum NiMemCmd : uint8_t {
	NI_CMD_INIT_DEVICE_INFO = 1,
	NI_CMD_OPEN_PROCESS,
	NI_CMD_READ_PROCESS_MEMORY,
	NI_CMD_WRITE_PROCESS_MEMORY,
	NI_CMD_CLOSE_PROCESS,
	NI_CMD_GET_PROCESS_MAPS_COUNT,
	NI_CMD_GET_PROCESS_MAPS_LIST,
	NI_CMD_CHECK_PROCESS_ADDR_PHY,
	NI_CMD_GET_PID_LIST,
	NI_CMD_SET_PROCESS_ROOT,
	NI_CMD_GET_PROCESS_RSS,
	NI_CMD_GET_PROCESS_CMDLINE,
	NI_CMD_HIDE_USER_PROCESS,
	NI_CMD_ALLOC_REMOTE_ANON,
	NI_CMD_VMMMAP,
	NI_CMD_READ_PROCESS_MEMORY_BULK,
	NI_CMD_GET_PROCESS_COMM,
	NI_CMD_GET_SO_BASE_ADDRESS,
};

enum NiHwbpCmd : uint8_t {
	NI_CMD_HWBP_GET_CAPS = 100,
	NI_CMD_HWBP_INSTALL,
	NI_CMD_HWBP_UNINSTALL,
	NI_CMD_HWBP_DISABLE,
	NI_CMD_HWBP_ENABLE,
	NI_CMD_HWBP_PAUSE,
	NI_CMD_HWBP_CONTINUE,
	NI_CMD_HWBP_SINGLE_STEP,
	NI_CMD_HWBP_STEP_OVER,
	NI_CMD_HWBP_GET_STATUS,
	NI_CMD_HWBP_READ_EVENTS,
	NI_CMD_HWBP_GET_REGS,
	NI_CMD_HWBP_SET_REGS,
	NI_CMD_HWBP_QUERY_TASK,
	NI_CMD_HWBP_CLEANUP_TASK,
};

/* ── Map entry (matches kernel, packed) ─────────────────────────── */

constexpr int NI_PATH_MAX = 1024;

struct ni_map_entry {
	unsigned long start;
	unsigned long end;
	unsigned char flags[4];
	char          path[NI_PATH_MAX];
} __attribute__((packed));

/* ── HWBP constants ─────────────────────────────────────────────── */

constexpr int NI_HWBP_MAX_EVENTS_PER_READ = 256;
constexpr uint32_t NI_HWBP_MAX_QUERY_ENTRIES = 64;

enum NiHwbpEventType : uint32_t {
	NI_HWBP_EVENT_HIT = 1,
	NI_HWBP_EVENT_STEP,
	NI_HWBP_EVENT_STEP_OVER,
	NI_HWBP_EVENT_RESUME,
	NI_HWBP_EVENT_PAUSE,
	NI_HWBP_EVENT_DISABLED,
	NI_HWBP_EVENT_ERROR,
};

enum NiHwbpRunState : uint32_t {
	NI_HWBP_STATE_RUNNING = 0,
	NI_HWBP_STATE_PAUSED,
	NI_HWBP_STATE_STEPPING,
	NI_HWBP_STATE_STEP_OVER,
	NI_HWBP_STATE_DISABLED,
	NI_HWBP_STATE_DEAD,
};

enum NiHwbpInstallFlags : uint32_t {
	NI_HWBP_F_PAUSE_ON_HIT  = 1u << 0,
	NI_HWBP_F_AUTO_REARM    = 1u << 1,
	NI_HWBP_F_FORCE_RECLAIM = 1u << 2,
};

enum NiHwbpSource : uint32_t {
	NI_HWBP_SOURCE_PERF = 0,
	NI_HWBP_SOURCE_PTRACE,
	NI_HWBP_SOURCE_MODULE,
};

enum NiHwbpPerfState : uint32_t {
	NI_HWBP_PERF_STATE_UNKNOWN = 0,
	NI_HWBP_PERF_STATE_DEAD,
	NI_HWBP_PERF_STATE_EXIT,
	NI_HWBP_PERF_STATE_ERROR,
	NI_HWBP_PERF_STATE_OFF,
	NI_HWBP_PERF_STATE_INACTIVE,
	NI_HWBP_PERF_STATE_ACTIVE,
};

enum NiHwbpQueryEntryFlags : uint32_t {
	NI_HWBP_ENTRY_F_ENABLED = 1u << 0,
	NI_HWBP_ENTRY_F_ACTIVE = 1u << 1,
	NI_HWBP_ENTRY_F_PINNED = 1u << 2,
	NI_HWBP_ENTRY_F_INHERITED = 1u << 3,
	NI_HWBP_ENTRY_F_SIGTRAP = 1u << 4,
};

enum NiHwbpCleanupFlags : uint32_t {
	NI_HWBP_CLEANUP_F_THREAD = 1u << 0,
};

constexpr uint32_t NI_HW_BREAKPOINT_X   = 4;
constexpr uint32_t NI_HW_BREAKPOINT_R   = 1;
constexpr uint32_t NI_HW_BREAKPOINT_W   = 2;
constexpr uint32_t NI_HW_BREAKPOINT_RW  = 3;

constexpr uint32_t NI_HW_BREAKPOINT_LEN_1 = 1;
constexpr uint32_t NI_HW_BREAKPOINT_LEN_2 = 2;
constexpr uint32_t NI_HW_BREAKPOINT_LEN_3 = 3;
constexpr uint32_t NI_HW_BREAKPOINT_LEN_4 = 4;
constexpr uint32_t NI_HW_BREAKPOINT_LEN_8 = 8;

/* ── HWBP structures ────────────────────────────────────────────── */

struct ni_hwbp_user_regs {
	uint64_t    regs[31];
	uint64_t    sp;
	uint64_t    pc;
	uint64_t    pstate;
	uint64_t    orig_x0;
	uint64_t    syscallno;
	__uint128_t fp_regs[32];
	uint32_t    fpsr;
	uint32_t    fpcr;
};

struct ni_hwbp_install {
	int32_t  tid;
	uint32_t flags;
	uint64_t addr;
	uint32_t len;
	uint32_t type;
	uint64_t handle;
	uint32_t reclaimed_slots;
	uint32_t reserved;
};

struct ni_hwbp_handle_arg {
	uint64_t handle;
};

struct ni_hwbp_event {
	uint64_t handle;
	uint32_t type;
	int32_t  tid;
	uint64_t hit_index;
	uint64_t timestamp_ns;
	uint64_t addr;
	uint64_t bp_addr;
	uint32_t bp_type;
	uint32_t bp_len;
	uint32_t state;
	uint32_t reserved;
	ni_hwbp_user_regs regs;
};

struct ni_hwbp_read_events {
	uint64_t handle;
	uint64_t events;
	uint32_t capacity;
	uint32_t count;
};

struct ni_hwbp_status {
	uint64_t handle;
	int32_t  tid;
	uint32_t state;
	uint64_t bp_addr;
	uint32_t bp_type;
	uint32_t bp_len;
	uint32_t flags;
	uint32_t pending_events;
	uint64_t total_events;
	uint64_t dropped_events;
	uint64_t last_pc;
	uint64_t last_sp;
	uint32_t step_available;
	uint32_t reclaimed_slots;
};

struct ni_hwbp_regs_io {
	uint64_t handle;
	ni_hwbp_user_regs regs;
};

struct ni_hwbp_caps {
	uint32_t brp_slots;
	uint32_t wrp_slots;
	uint32_t step_available;
	uint32_t ring_size;
};

struct ni_hwbp_task_entry {
	uint64_t event_id;
	uint64_t module_handle;
	uint64_t addr;
	int32_t  tid;
	int32_t  oncpu;
	uint32_t type;
	uint32_t len;
	uint32_t state;
	uint32_t source;
	uint32_t flags;
	uint32_t reserved;
};

struct ni_hwbp_task_query {
	int32_t  tid;
	uint32_t flags;
	uint64_t entries;
	uint32_t capacity;
	uint32_t count;
	uint32_t total_count;
	uint32_t brp_count;
	uint32_t wrp_count;
	uint32_t enabled_count;
	uint32_t active_count;
	uint32_t perf_count;
	uint32_t ptrace_count;
	uint32_t module_count;
	uint32_t reserved;
};

struct ni_hwbp_task_cleanup {
	int32_t  pid;
	uint32_t flags;
	uint32_t cleaned_count;
	uint32_t reserved;
};

static_assert(sizeof(ni_hwbp_task_entry) == 56,
	      "ni_hwbp_task_entry ABI size mismatch");
static_assert(sizeof(ni_hwbp_task_query) == 64,
	      "ni_hwbp_task_query ABI size mismatch");
static_assert(sizeof(ni_hwbp_task_cleanup) == 16,
	      "ni_hwbp_task_cleanup ABI size mismatch");

struct ni_hwbp_task_snapshot {
	ni_hwbp_task_query summary;
	std::vector<ni_hwbp_task_entry> entries;
};

/* ══════════════════════════════════════════════════════════════════
 *  NiDriver — RAII wrapper
 * ══════════════════════════════════════════════════════════════════ */

class NiDriver {
public:
	NiDriver();
	~NiDriver();

	NiDriver(const NiDriver &) = delete;
	NiDriver &operator=(const NiDriver &) = delete;
	NiDriver(NiDriver &&o) noexcept;
	NiDriver &operator=(NiDriver &&o) noexcept;

	bool valid() const { return fd_ >= 0; }
	int  fd()    const { return fd_; }

	/* ── Module ─────────────────────────────────────────────── */

	int64_t get_expire_time();
	bool    is_module_hidden();
	bool    hide_module();

	/* ── Process — basic ────────────────────────────────────── */

	int     open_process(int pid);
	void    close_process(int pid);
	ssize_t read_memory(int pid, uint64_t addr, void *buf, size_t size);
	ssize_t write_memory(int pid, uint64_t addr, const void *data, size_t size);
	ssize_t read_memory_bulk(int pid, uint64_t addr, void *buf, size_t size);

	template <typename T>
	bool read_val(int pid, uint64_t addr, T &out) {
		return read_memory(pid, addr, &out, sizeof(T)) == static_cast<ssize_t>(sizeof(T));
	}

	template <typename T>
	bool write_val(int pid, uint64_t addr, const T &val) {
		return write_memory(pid, addr, &val, sizeof(T)) >= 0;
	}

	/* ── Maps ───────────────────────────────────────────────── */

	int  get_maps_count(int pid);
	int  get_maps_list(int pid, ni_map_entry *out, size_t buf_size);
	std::vector<ni_map_entry> get_all_maps(int pid);
	uint64_t get_module_base(int pid, const std::string &name);

	/* ── Process info ───────────────────────────────────────── */

	bool     check_phy_addr(int pid, uint64_t addr);
	std::vector<int32_t> get_pid_list();
	int      set_process_root(int pid);
	uint64_t get_process_rss(int pid);
	std::string get_process_cmdline(int pid);
	std::string get_process_comm(int pid);
	uint64_t get_so_base_address(int pid, const std::string &so_name);

	/* ── Process helpers ────────────────────────────────────── */

	int find_pid_by_name(const std::string &name);

	/* ── HWBP — basic ───────────────────────────────────────── */

	std::optional<ni_hwbp_caps> hwbp_get_caps();
	int  hwbp_install(ni_hwbp_install &req);
	int  hwbp_uninstall(uint64_t handle);
	int  hwbp_enable(uint64_t handle);
	int  hwbp_disable(uint64_t handle);
	int  hwbp_pause(uint64_t handle);
	int  hwbp_continue(uint64_t handle);
	int  hwbp_single_step(uint64_t handle);
	int  hwbp_step_over(uint64_t handle);
	std::optional<ni_hwbp_status> hwbp_get_status(uint64_t handle);
	std::vector<ni_hwbp_event> hwbp_read_events(uint64_t handle,
						     uint32_t capacity = NI_HWBP_MAX_EVENTS_PER_READ);
	int  hwbp_get_regs(ni_hwbp_regs_io &regs_io);
	int  hwbp_set_regs(const ni_hwbp_regs_io &regs_io);
	std::optional<ni_hwbp_task_snapshot> hwbp_query_task(
		int tid, uint32_t capacity = NI_HWBP_MAX_QUERY_ENTRIES);
	int  hwbp_cleanup_task(int pid, uint32_t flags = 0,
			       uint32_t *cleaned_count = nullptr);

	/* ── HWBP — helpers ─────────────────────────────────────── */

	std::optional<uint64_t> hwbp_install_exec(int tid, uint64_t addr,
						  uint32_t flags = NI_HWBP_F_PAUSE_ON_HIT);
	std::optional<uint64_t> hwbp_install_watch(int tid, uint64_t addr,
						   uint32_t type, uint32_t len,
						   uint32_t flags = NI_HWBP_F_PAUSE_ON_HIT);
	std::vector<ni_hwbp_event> hwbp_poll_events(uint64_t handle,
						    uint32_t capacity,
						    int timeout_ms,
						    int interval_us = 1000);

private:
	int fd_;

	ssize_t cmd(uint8_t cmd, uint64_t p1, uint64_t p2, uint64_t p3,
		    void *buf, uint64_t buf_size);
	int handle_cmd(uint8_t cmd, uint64_t handle);
};
