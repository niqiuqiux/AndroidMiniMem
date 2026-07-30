#include "NiDriver.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

static constexpr uint32_t KSU_MAGIC1 = 0xDEADBEEF;
static constexpr uint32_t KSU_MAGIC2 = 0x114514;
static constexpr size_t   STACK_THRESHOLD = 4096;

#define NI_IOCTL_GET_EXPIRE_TIME   _IOR('N', 0x01, int64_t)
#define NI_IOCTL_IS_MODULE_HIDDEN  _IO('N', 0x02)
#define NI_IOCTL_HIDE_MODULE       _IO('N', 0x03)
#define NI_IOCTL_GET_PROTOCOL_INFO _IOR('N', 0x04, ni_protocol_info)

static bool ni_validate_protocol(int fd)
{
	ni_protocol_info info{};

	if (fd < 0)
		return false;
	if (::ioctl(fd, NI_IOCTL_GET_PROTOCOL_INFO, &info) < 0)
		return false;
	return info.magic == NI_CTL_MAGIC &&
	       info.version == NI_CTL_VERSION &&
	       info.header_size == sizeof(ni_request);
}

/* ══════════════════════════════════════════════════════════════════
 *  Low-level transport
 * ══════════════════════════════════════════════════════════════════ */

ssize_t NiDriver::cmd(uint8_t c, uint64_t p1, uint64_t p2, uint64_t p3,
		      void *buf, uint64_t buf_size)
{
	constexpr size_t hdr_sz = sizeof(ni_request);
	const size_t total = hdr_sz + buf_size;

	char stack[STACK_THRESHOLD];
	char *pkt = (total <= STACK_THRESHOLD)
		    ? stack
		    : static_cast<char *>(std::malloc(total));
	if (!pkt)
		return -1;

	auto *hdr = reinterpret_cast<ni_request *>(pkt);
	std::memset(hdr, 0, hdr_sz);
	hdr->cmd         = c;
	hdr->param1      = p1;
	hdr->param2      = p2;
	hdr->param3      = p3;
	hdr->buf_size    = buf_size;

	if (buf && buf_size > 0)
		std::memcpy(pkt + hdr_sz, buf, buf_size);

	ssize_t ret = ::read(fd_, pkt, total);

	if (buf && buf_size > 0)
		std::memcpy(buf, pkt + hdr_sz, buf_size);

	if (pkt != stack)
		std::free(pkt);
	return ret;
}

int NiDriver::handle_cmd(uint8_t c, uint64_t handle)
{
	ni_hwbp_handle_arg arg{};
	arg.handle = handle;
	return static_cast<int>(cmd(c, 0, 0, 0, &arg, sizeof(arg)));
}

/* ══════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ══════════════════════════════════════════════════════════════════ */

NiDriver::NiDriver()
	: fd_(-1)
{
	syscall(__NR_reboot, KSU_MAGIC1, KSU_MAGIC2, 0, &fd_);
	if (fd_ >= 0 && !ni_validate_protocol(fd_)) {
		::close(fd_);
		fd_ = -1;
		errno = EPROTO;
	}
}

NiDriver::~NiDriver()
{
	if (fd_ >= 0)
		::close(fd_);
}

NiDriver::NiDriver(NiDriver &&o) noexcept
	: fd_(o.fd_)
{
	o.fd_ = -1;
}

NiDriver &NiDriver::operator=(NiDriver &&o) noexcept
{
	if (this != &o) {
		if (fd_ >= 0)
			::close(fd_);
		fd_ = o.fd_;
		o.fd_ = -1;
	}
	return *this;
}

/* ══════════════════════════════════════════════════════════════════
 *  Module
 * ══════════════════════════════════════════════════════════════════ */

int64_t NiDriver::get_expire_time()
{
	int64_t t = 0;
	if (::ioctl(fd_, NI_IOCTL_GET_EXPIRE_TIME, &t) < 0)
		return -1;
	return t;
}

bool NiDriver::is_module_hidden()
{
	return ::ioctl(fd_, NI_IOCTL_IS_MODULE_HIDDEN, 0) == 1;
}

bool NiDriver::hide_module()
{
	return ::ioctl(fd_, NI_IOCTL_HIDE_MODULE, 0) == 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  Process — basic
 * ══════════════════════════════════════════════════════════════════ */

int NiDriver::open_process(int pid)
{
	uint64_t handle = 0;
	ssize_t ret = cmd(NI_CMD_OPEN_PROCESS, pid, 0, 0,
			  &handle, sizeof(handle));
	return ret < 0 ? -1 : static_cast<int>(handle);
}

void NiDriver::close_process(int pid)
{
	uint64_t dummy = 0;
	cmd(NI_CMD_CLOSE_PROCESS, pid, 0, 0, &dummy, sizeof(dummy));
}

ssize_t NiDriver::read_memory(int pid, uint64_t addr, void *buf, size_t size)
{
	return cmd(NI_CMD_READ_PROCESS_MEMORY, pid, addr, 0, buf, size);
}

ssize_t NiDriver::write_memory(int pid, uint64_t addr,
			       const void *data, size_t size)
{
	return cmd(NI_CMD_WRITE_PROCESS_MEMORY, pid, addr, 0,
		   const_cast<void *>(data), size);
}

ssize_t NiDriver::read_memory_bulk(int pid, uint64_t addr,
				   void *buf, size_t size)
{
	return cmd(NI_CMD_READ_PROCESS_MEMORY_BULK, pid, addr, 0, buf, size);
}

/* ══════════════════════════════════════════════════════════════════
 *  Maps
 * ══════════════════════════════════════════════════════════════════ */

int NiDriver::get_maps_count(int pid)
{
	return static_cast<int>(cmd(NI_CMD_GET_PROCESS_MAPS_COUNT,
				    pid, 0, 0, nullptr, 0));
}

int NiDriver::get_maps_list(int pid, ni_map_entry *out, size_t buf_size)
{
	return static_cast<int>(cmd(NI_CMD_GET_PROCESS_MAPS_LIST,
				    pid, 0, 0, out, buf_size));
}

std::vector<ni_map_entry> NiDriver::get_all_maps(int pid)
{
	int count = get_maps_count(pid);
	if (count <= 0)
		return {};

	std::vector<ni_map_entry> maps(count);
	int got = get_maps_list(pid, maps.data(),
				count * sizeof(ni_map_entry));
	if (got <= 0)
		return {};

	maps.resize(got);
	return maps;
}

uint64_t NiDriver::get_module_base(int pid, const std::string &name)
{
	auto maps = get_all_maps(pid);
	for (const auto &m : maps) {
		if (m.path[0] && std::strstr(m.path, name.c_str()))
			return m.start;
	}
	return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  Process info
 * ══════════════════════════════════════════════════════════════════ */

bool NiDriver::check_phy_addr(int pid, uint64_t addr)
{
	return cmd(NI_CMD_CHECK_PROCESS_ADDR_PHY, pid, addr, 0,
		   nullptr, 0) == 1;
}

std::vector<int32_t> NiDriver::get_pid_list()
{
	ssize_t total = cmd(NI_CMD_GET_PID_LIST, 0, 0, 0, nullptr, 0);
	if (total <= 0)
		return {};

	std::vector<int32_t> pids(total);
	ssize_t got = cmd(NI_CMD_GET_PID_LIST, 0, 0, 0,
			  pids.data(), total * sizeof(int32_t));
	if (got <= 0)
		return {};

	pids.resize(got);
	return pids;
}

int NiDriver::set_process_root(int pid)
{
	return static_cast<int>(cmd(NI_CMD_SET_PROCESS_ROOT,
				    pid, 0, 0, nullptr, 0));
}

uint64_t NiDriver::get_process_rss(int pid)
{
	uint64_t rss = 0;
	if (cmd(NI_CMD_GET_PROCESS_RSS, pid, 0, 0, &rss, sizeof(rss)) < 0)
		return 0;
	return rss;
}

std::string NiDriver::get_process_cmdline(int pid)
{
	char buf[256]{};
	ssize_t ret = cmd(NI_CMD_GET_PROCESS_CMDLINE, pid, 0, 0,
			  buf, sizeof(buf));
	if (ret <= 0)
		return {};
	return std::string(buf, strnlen(buf, sizeof(buf)));
}

std::string NiDriver::get_process_comm(int pid)
{
	char buf[17]{};
	if (cmd(NI_CMD_GET_PROCESS_COMM, pid, 0, 0, buf, sizeof(buf)) < 0)
		return {};
	return std::string(buf, strnlen(buf, sizeof(buf)));
}

uint64_t NiDriver::get_so_base_address(int pid, const std::string &so_name)
{
	uint64_t base = 0;

	if (so_name.empty())
		return 0;

	std::vector<char> payload(sizeof(base) + so_name.size() + 1);
	std::memcpy(payload.data(), &base, sizeof(base));
	std::memcpy(payload.data() + sizeof(base), so_name.c_str(), so_name.size() + 1);

	ssize_t ret = cmd(NI_CMD_GET_SO_BASE_ADDRESS, pid, 0, 0,
			  payload.data(), payload.size());
	if (ret < 0)
		return 0;

	std::memcpy(&base, payload.data(), sizeof(base));
	return base;
}

/* ══════════════════════════════════════════════════════════════════
 *  Process helpers
 * ══════════════════════════════════════════════════════════════════ */

int NiDriver::find_pid_by_name(const std::string &name)
{
	auto pids = get_pid_list();
	if (pids.empty())
		return -1;

	for (int32_t pid : pids) {
		auto comm = get_process_comm(pid);
		if (!comm.empty() && comm.find(name) != std::string::npos)
			return pid;
	}

	for (int32_t pid : pids) {
		auto cmdline = get_process_cmdline(pid);
		if (!cmdline.empty() && cmdline.find(name) != std::string::npos)
			return pid;
	}

	return -1;
}

/* ══════════════════════════════════════════════════════════════════
 *  HWBP — basic
 * ══════════════════════════════════════════════════════════════════ */

std::optional<ni_hwbp_caps> NiDriver::hwbp_get_caps()
{
	ni_hwbp_caps caps{};
	if (cmd(NI_CMD_HWBP_GET_CAPS, 0, 0, 0, &caps, sizeof(caps)) < 0)
		return std::nullopt;
	return caps;
}

int NiDriver::hwbp_install(ni_hwbp_install &req)
{
	return static_cast<int>(cmd(NI_CMD_HWBP_INSTALL,
				    0, 0, 0, &req, sizeof(req)));
}

int NiDriver::hwbp_uninstall(uint64_t h)  { return handle_cmd(NI_CMD_HWBP_UNINSTALL, h); }
int NiDriver::hwbp_enable(uint64_t h)     { return handle_cmd(NI_CMD_HWBP_ENABLE, h); }
int NiDriver::hwbp_disable(uint64_t h)    { return handle_cmd(NI_CMD_HWBP_DISABLE, h); }
int NiDriver::hwbp_pause(uint64_t h)      { return handle_cmd(NI_CMD_HWBP_PAUSE, h); }
int NiDriver::hwbp_continue(uint64_t h)   { return handle_cmd(NI_CMD_HWBP_CONTINUE, h); }
int NiDriver::hwbp_single_step(uint64_t h){ return handle_cmd(NI_CMD_HWBP_SINGLE_STEP, h); }
int NiDriver::hwbp_step_over(uint64_t h)  { return handle_cmd(NI_CMD_HWBP_STEP_OVER, h); }

std::optional<ni_hwbp_status> NiDriver::hwbp_get_status(uint64_t handle)
{
	ni_hwbp_status st{};
	st.handle = handle;
	if (cmd(NI_CMD_HWBP_GET_STATUS, 0, 0, 0, &st, sizeof(st)) < 0)
		return std::nullopt;
	return st;
}

std::vector<ni_hwbp_event> NiDriver::hwbp_read_events(uint64_t handle,
						       uint32_t capacity)
{
	std::vector<ni_hwbp_event> events(capacity);

	ni_hwbp_read_events req{};
	req.handle   = handle;
	req.events   = reinterpret_cast<uint64_t>(events.data());
	req.capacity = capacity;

	if (cmd(NI_CMD_HWBP_READ_EVENTS, 0, 0, 0, &req, sizeof(req)) < 0)
		return {};

	events.resize(req.count);
	return events;
}

int NiDriver::hwbp_get_regs(ni_hwbp_regs_io &regs_io)
{
	return static_cast<int>(cmd(NI_CMD_HWBP_GET_REGS,
				    0, 0, 0, &regs_io, sizeof(regs_io)));
}

int NiDriver::hwbp_set_regs(const ni_hwbp_regs_io &regs_io)
{
	auto copy = regs_io;
	return static_cast<int>(cmd(NI_CMD_HWBP_SET_REGS,
				    0, 0, 0, &copy, sizeof(copy)));
}

std::optional<ni_hwbp_task_snapshot>
NiDriver::hwbp_query_task(int tid, uint32_t capacity)
{
	if (tid <= 0 || capacity > NI_HWBP_MAX_QUERY_ENTRIES) {
		errno = EINVAL;
		return std::nullopt;
	}

	ni_hwbp_task_snapshot snapshot{};
	snapshot.entries.resize(capacity);
	snapshot.summary.tid = tid;
	snapshot.summary.entries = capacity
		? reinterpret_cast<uint64_t>(snapshot.entries.data()) : 0;
	snapshot.summary.capacity = capacity;

	if (cmd(NI_CMD_HWBP_QUERY_TASK, 0, 0, 0, &snapshot.summary,
		sizeof(snapshot.summary)) < 0)
		return std::nullopt;

	if (snapshot.summary.count > capacity ||
		snapshot.summary.total_count < snapshot.summary.count) {
		errno = EPROTO;
		return std::nullopt;
	}
	snapshot.entries.resize(snapshot.summary.count);
	snapshot.summary.entries = 0;
	return snapshot;
}

int NiDriver::hwbp_cleanup_task(int pid, uint32_t flags,
				uint32_t *cleaned_count)
{
	ni_hwbp_task_cleanup cleanup{};

	if (pid <= 0 || flags & ~NI_HWBP_CLEANUP_F_THREAD) {
		errno = EINVAL;
		return -1;
	}

	cleanup.pid = pid;
	cleanup.flags = flags;
	if (cmd(NI_CMD_HWBP_CLEANUP_TASK, 0, 0, 0,
		&cleanup, sizeof(cleanup)) < 0)
		return -1;

	if (cleaned_count)
		*cleaned_count = cleanup.cleaned_count;
	return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  HWBP — helpers
 * ══════════════════════════════════════════════════════════════════ */

std::optional<uint64_t> NiDriver::hwbp_install_exec(int tid, uint64_t addr,
						    uint32_t flags)
{
	ni_hwbp_install req{};
	req.tid   = tid;
	req.addr  = addr;
	req.len   = NI_HW_BREAKPOINT_LEN_4;
	req.type  = NI_HW_BREAKPOINT_X;
	req.flags = flags;

	if (hwbp_install(req) != 0)
		return std::nullopt;
	return req.handle;
}

std::optional<uint64_t> NiDriver::hwbp_install_watch(int tid, uint64_t addr,
						     uint32_t type, uint32_t len,
						     uint32_t flags)
{
	ni_hwbp_install req{};
	req.tid   = tid;
	req.addr  = addr;
	req.len   = len;
	req.type  = type;
	req.flags = flags;

	if (hwbp_install(req) != 0)
		return std::nullopt;
	return req.handle;
}

std::vector<ni_hwbp_event> NiDriver::hwbp_poll_events(uint64_t handle,
						      uint32_t capacity,
						      int timeout_ms,
						      int interval_us)
{
	if (interval_us <= 0)
		interval_us = 1000;

	struct timespec start{};
	clock_gettime(CLOCK_MONOTONIC, &start);

	auto ms_now = [](const struct timespec &ts) -> uint64_t {
		return static_cast<uint64_t>(ts.tv_sec) * 1000 +
		       static_cast<uint64_t>(ts.tv_nsec) / 1000000;
	};

	for (;;) {
		auto ev = hwbp_read_events(handle, capacity);
		if (!ev.empty())
			return ev;

		if (timeout_ms > 0) {
			struct timespec now{};
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (ms_now(now) - ms_now(start) >=
			    static_cast<uint64_t>(timeout_ms)) {
				errno = ETIMEDOUT;
				return {};
			}
		}

		::usleep(interval_us);
	}
}

/* ══════════════════════════════════════════════════════════════════
 *  UXN exception breakpoints
 * ══════════════════════════════════════════════════════════════════ */

int NiDriver::uxn_install(ni_uxn_install &req)
{
	return static_cast<int>(cmd(NI_CMD_UXN_INSTALL, 0, 0, 0,
				    &req, sizeof(req)));
}

int NiDriver::uxn_remove(uint32_t pid, uint64_t addr)
{
	ni_uxn_remove req{};
	req.pid = pid;
	req.addr = addr;
	return static_cast<int>(cmd(NI_CMD_UXN_REMOVE, 0, 0, 0,
				    &req, sizeof(req)));
}

int NiDriver::uxn_wait(ni_uxn_wait &req)
{
	return static_cast<int>(cmd(NI_CMD_UXN_WAIT, 0, 0, 0,
				    &req, sizeof(req)));
}

int NiDriver::uxn_resume(const ni_uxn_resume &req)
{
	auto copy = req;
	return static_cast<int>(cmd(NI_CMD_UXN_RESUME, 0, 0, 0,
				    &copy, sizeof(copy)));
}

std::optional<ni_uxn_status> NiDriver::uxn_get_status(uint32_t slot)
{
	ni_uxn_status status{};
	status.slot = slot;
	if (cmd(NI_CMD_UXN_STATUS, 0, 0, 0, &status, sizeof(status)) < 0)
		return std::nullopt;
	return status;
}

int NiDriver::uxn_clear()
{
	return static_cast<int>(cmd(NI_CMD_UXN_CLEAR, 0, 0, 0, nullptr, 0));
}
