#include "client_singleton.h"
#include "client.hpp"
#include "socket_request_manager.h"
#include "../gui/AppContext.h"
#include <iostream>
#include <string>

// ==================== WinSocketClientMgr 实现 ====================

WinSocketClientMgr::WinSocketClientMgr() {
  auto poison = [] { DeviceSession::GetInstance().MarkPoisoned(); };
  m_main_client.SetPoisonCallback(poison);
  m_debug_client.SetPoisonCallback(poison);
  m_error_client.SetPoisonCallback(std::move(poison));
}

WinSocketClientMgr::~WinSocketClientMgr() {
  auto &session = DeviceSession::GetInstance();
  auto lifecycle = session.AcquireLifecycle();
  session.Disconnect();
  CloseClients();
}

void WinSocketClientMgr::CloseClients() {
  m_main_client.Close();
  m_debug_client.Close();
  m_error_client.Close();
}

WindowsSocketClient *WinSocketClientMgr::GetClient(PortType type) {
  switch (type) {
  case PORT_MAIN:  return &m_main_client;
  case PORT_DEBUG: return &m_debug_client;
  case PORT_ERROR: return &m_error_client;
  default:         return nullptr;
  }
}

std::mutex *WinSocketClientMgr::GetMutex(PortType type) {
  switch (type) {
  case PORT_MAIN:  return &m_main_mutex;
  case PORT_DEBUG: return &m_debug_mutex;
  case PORT_ERROR: return &m_error_mutex;
  default:         return nullptr;
  }
}

std::recursive_timed_mutex *WinSocketClientMgr::GetTransactionMutex(PortType type) {
  switch (type) {
  case PORT_MAIN:  return &m_main_transaction_mutex;
  case PORT_DEBUG: return &m_debug_transaction_mutex;
  case PORT_ERROR: return &m_error_transaction_mutex;
  default:         return nullptr;
  }
}

bool WinSocketClientMgr::ConnectMultiPort(const std::string &host, uint16_t Port) {
  auto &session = DeviceSession::GetInstance();
  auto lifecycle = session.AcquireLifecycle();
  ResetTrackedKernelBreakpoints();
  session.BeginConnect();
  CloseClients();
  std::cout << "[MultiPort] Connecting to server..." << std::endl;
  std::cout << "  Main:  " << host << ":" << Port << std::endl;
  AppContext::Get().clearProcessForDisconnect();

  if (!m_main_client.Connect(host, Port)) {
    std::cerr << "[MultiPort] Failed to connect MAIN port" << std::endl;
    session.FinishConnect(false);
    return false;
  }
  if (!m_debug_client.Connect(host, Port)) {
    std::cerr << "[MultiPort] Failed to connect DEBUG port" << std::endl;
    CloseClients();
    session.FinishConnect(false);
    return false;
  }
  if (!m_error_client.Connect(host, Port)) {
    std::cerr << "[MultiPort] Failed to connect ERROR port" << std::endl;
    CloseClients();
    session.FinishConnect(false);
    return false;
  }

  session.FinishConnect(true);
  std::cout << "[MultiPort] All ports connected successfully!" << std::endl;
  return true;
}

void WinSocketClientMgr::DisconnectMultiPort() {
  auto &session = DeviceSession::GetInstance();
  auto lifecycle = session.AcquireLifecycle();
  ResetTrackedKernelBreakpoints();
  const bool hadSession =
      session.GetState() != DeviceSession::State::Disconnected;
  session.Disconnect();
  AppContext::Get().clearProcessForDisconnect();
  CloseClients();
  if (hadSession)
    std::cout << "[MultiPort] All ports disconnected" << std::endl;
}

bool WinSocketClientMgr::IsMultiPortConnected() const {
  return DeviceSession::GetInstance().IsConnected();
}

// ==================== 进程管理 ====================

bool OpenProcessHandle(int pid, int &outHandle, PortType type) {
  outHandle = 0;
  auto &socketMgr = GetSocketMgr();
  auto lease = socketMgr.AcquireRequestLease();
  if (!lease)
    return false;

  auto client = socketMgr.GetClient(type);
  if (!client)
    return false;

  auto portMutex = socketMgr.GetMutex(type);
  return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
      portMutex, [&]() -> bool {
        if (!lease.isCurrent() || !client->IsConnected())
          return false;
#pragma pack(1)
        struct { unsigned char command; int pid; } op;
#pragma pack()
        op.command = CMD_OPENPROCESS;
        op.pid = pid;
        if (!client->Send(&op, sizeof(op)))
          return false;
        int handle = 0;
        if (!client->Receive(&handle, sizeof(handle)))
          return false;
        if (handle == 0)
          return false;
        outHandle = handle;
        return true;
      });
}

bool EnsureOpenHandle(int &outHandle, PortType type) {
  (void)type;
  int handle = AppContext::Get().processHandle.load(std::memory_order_relaxed);
  if (handle) {
    outHandle = handle;
    return true;
  }
  return false;
}

bool CloseProcessHandle(int handle, PortType type) {
  if (handle == 0)
    return true;

  auto &socketMgr = GetSocketMgr();
  auto lease = socketMgr.AcquireRequestLease();
  if (!lease)
    return false;

  auto client = socketMgr.GetClient(type);
  if (!client)
    return false;

  auto portMutex = socketMgr.GetMutex(type);
  return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
      portMutex, [&]() -> bool {
        if (!lease.isCurrent() || !client->IsConnected())
          return false;
        unsigned char command = CMD_CLOSEHANDLE;
        if (!client->Send(&command, sizeof(command)))
          return false;
        if (!client->Send(&handle, sizeof(handle)))
          return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
          return false;
        return result != 0;
      });
}
