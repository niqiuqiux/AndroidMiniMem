#include "CrashHandler.hpp"
#include "SocketManager.hpp"
#include "../../ceserver/ceserver.h"
#include "../../common/Logger.hpp"
#include <android/looper.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <csignal>
#include <string>
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <memory>

// ==================== 全局状态 ====================
std::atomic<bool> g_running{true};
int g_stop_eventfd = -1;

static void handle_signal(int) {
	g_running = false;
	if (g_stop_eventfd >= 0) {
		uint64_t val = 1;
		write(g_stop_eventfd, &val, sizeof(val));
	}
	LOGD("handle_signal");
}

static constexpr int MAX_CONNECTIONS = 32;

// ==================== 连接信息 ====================
struct ConnectionInfo {
	uint32_t id;
	SocketManager* socket;
	std::thread worker_thread;
	std::atomic<bool> stopped{false};

	ConnectionInfo() : id(0), socket(nullptr) {}
	~ConnectionInfo() {
		stopped.store(true);
		delete socket;
		socket = nullptr;
	}
	ConnectionInfo(const ConnectionInfo&) = delete;
	ConnectionInfo& operator=(const ConnectionInfo&) = delete;
};

// ==================== 连接清理队列 ====================
static std::mutex g_cleanup_mutex;
static std::queue<uint32_t> g_cleanup_queue;

// ==================== 连接管理器 ====================
class ConnectionManager {
private:
	mutable std::mutex mutex_;
	std::unordered_map<uint32_t, std::unique_ptr<ConnectionInfo>> connections_;
	uint32_t next_id_ = 1;

	static void WorkerThread(ConnectionInfo* info) {
		LOGDF("[Conn#%u] Worker started", info->id);
		while (!info->stopped.load() && info->socket && info->socket->IsValid()) {
			unsigned char command = 0;
			if (!info->socket->Receive(&command, sizeof(command))) {
				LOGDF("[Conn#%u] Socket closed", info->id);
				break;
			}
			int r = DispatchCommand_V2(info->socket, command);
			if (r == -1) {
				LOGDF("[Conn#%u] Command %d failed", info->id, command);
			}
		}
		LOGDF("[Conn#%u] Worker stopped", info->id);
		{
			std::lock_guard<std::mutex> lock(g_cleanup_mutex);
			g_cleanup_queue.push(info->id);
		}
	}

public:
	// 注册新连接，返回ID，失败返回0
	uint32_t Register(SocketManager* socket) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (connections_.size() >= MAX_CONNECTIONS) return 0;

		uint32_t id = next_id_++;
		auto info = std::make_unique<ConnectionInfo>();
		info->id = id;
		info->socket = socket;
		ConnectionInfo* raw = info.get();
		info->worker_thread = std::thread(WorkerThread, raw);

		connections_[id] = std::move(info);
		LOGDF("Registered conn #%u, total=%zu", id, connections_.size());
		return id;
	}

	// 移除连接（从主循环调用）
	void Remove(uint32_t id) {
		std::unique_ptr<ConnectionInfo> conn;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = connections_.find(id);
			if (it == connections_.end()) return;
			conn = std::move(it->second);
			connections_.erase(it);
			LOGDF("Removing conn #%u, remaining=%zu", id, connections_.size());
		}
		conn->stopped.store(true);
		if (conn->socket) conn->socket->Close();
		if (conn->worker_thread.joinable()) conn->worker_thread.join();
	}

	void ShutdownAll() {
		std::vector<uint32_t> ids;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (auto& [id, _] : connections_) ids.push_back(id);
		}
		for (uint32_t id : ids) Remove(id);
	}

	int GetConnectionCount() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return static_cast<int>(connections_.size());
	}
};

static ConnectionManager g_connMgr;

// ==================== ALooper 回调 ====================
int stop_callback(int fd, int events, void* data) {
	LOGD("Stop event received");
	g_running = false;
	uint64_t val;
	read(fd, &val, sizeof(val));
	return 1;
}

int accept_callback(int fd, int events, void* data) {
	SocketManager* listener = static_cast<SocketManager*>(data);
	if (!(events & ALOOPER_EVENT_INPUT)) return 1;

	auto* conn_socket = new SocketManager();
	if (!listener->AcceptTo(conn_socket, 0)) {
		delete conn_socket;
		return 1;
	}

	LOGDF("New connection from %s", conn_socket->GetClientInfo().c_str());

	uint32_t id = g_connMgr.Register(conn_socket);
	if (id == 0) {
		LOGD("Connection rejected (limit reached)");
		delete conn_socket;
		return 1;
	}

	LOGDF("Connection #%u accepted from %s", id, conn_socket->GetClientInfo().c_str());
	return 1;
}

// ==================== 主函数 ====================
int main(int argc, char** argv) {
	Logger::Initialize(LogLevel::DEBUG, LogMode::CONSOLE);
	CrashHandlerGuard crash_guard("./crash_logs", true);

	std::string host = "0.0.0.0";
	uint16_t port = 52736;

	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if ((a == "-p" || a == "--port") && i + 1 < argc) {
			port = static_cast<uint16_t>(std::stoi(argv[++i]));
		} else if ((a == "-h" || a == "--host") && i + 1 < argc) {
			host = argv[++i];
		}
	}

	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);

	LOGDF("=================================================");
	LOGDF("Multi-Connection Socket Server");
	LOGDF("Listening on: %s:%d (max %d connections)", host.c_str(), port, MAX_CONNECTIONS);
	LOGDF("=================================================");

	g_stop_eventfd = eventfd(0, EFD_NONBLOCK);
	if (g_stop_eventfd < 0) {
		LOGD("Failed to create eventfd");
		return 1;
	}

	ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
	if (!looper) {
		LOGD("Failed to create ALooper");
		close(g_stop_eventfd);
		return 1;
	}

	ALooper_addFd(looper, g_stop_eventfd, ALOOPER_POLL_CALLBACK,
				  ALOOPER_EVENT_INPUT, stop_callback, nullptr);

	SocketManager listener;
	if (!listener.BindAndListen(host, port, false, SOMAXCONN)) {
		LOGDF("BindAndListen failed on %s:%d", host.c_str(), port);
		close(g_stop_eventfd);
		return 1;
	}
	LOGDF("Listening on %s:%d", host.c_str(), port);

	int listen_fd = listener.GetListenFd();
	ALooper_addFd(looper, listen_fd, ALOOPER_POLL_CALLBACK,
				  ALOOPER_EVENT_INPUT, accept_callback, &listener);

	LOGDF("Server ready, entering event loop...");
	while (g_running) {
		int result = ALooper_pollOnce(100, nullptr, nullptr, nullptr);
		if (result == ALOOPER_POLL_ERROR) {
			LOGD("ALooper poll error");
			break;
		}

		// 排空清理队列：回收已断开的连接
		{
			std::lock_guard<std::mutex> lock(g_cleanup_mutex);
			while (!g_cleanup_queue.empty()) {
				uint32_t id = g_cleanup_queue.front();
				g_cleanup_queue.pop();
				g_connMgr.Remove(id);
			}
		}
	}

	LOGDF("Shutting down...");
	ALooper_removeFd(looper, listen_fd);
	ALooper_removeFd(looper, g_stop_eventfd);

	g_connMgr.ShutdownAll();
	listener.Close();

	close(g_stop_eventfd);
	g_stop_eventfd = -1;

	LOGDF("=================================================");
	LOGDF("Server stopped successfully");
	LOGDF("=================================================");
	return 0;
}
