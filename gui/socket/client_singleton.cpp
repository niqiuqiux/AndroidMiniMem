#include "client_singleton.h"
#include "client.hpp"
#include "socket_request_manager.h"
#include "../gui/AppContext.h"
#include <iostream>
#include <string>

// ==================== WinSocketClientMgr 实现 ====================

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

bool WinSocketClientMgr::ConnectMultiPort(const std::string &host, uint16_t Port) {
  DisconnectMultiPort();
  std::cout << "[MultiPort] Connecting to server..." << std::endl;
  std::cout << "  Main:  " << host << ":" << Port << std::endl;
  AppContext::Get().clearProcess();

  if (!m_main_client.Connect(host, Port)) {
    std::cerr << "[MultiPort] Failed to connect MAIN port" << std::endl;
    return false;
  }
  if (!m_debug_client.Connect(host, Port)) {
    std::cerr << "[MultiPort] Failed to connect DEBUG port" << std::endl;
    m_main_client.Close();
    return false;
  }
  if (!m_error_client.Connect(host, Port)) {
    std::cerr << "[MultiPort] Failed to connect ERROR port" << std::endl;
    m_main_client.Close();
    m_debug_client.Close();
    return false;
  }

  m_connected.store(true, std::memory_order_release);
  std::cout << "[MultiPort] All ports connected successfully!" << std::endl;
  return true;
}

void WinSocketClientMgr::DisconnectMultiPort() {
  bool wasConnected = m_connected.exchange(false, std::memory_order_acq_rel);
  AppContext::Get().clearProcess();
  m_main_client.Close();
  m_debug_client.Close();
  m_error_client.Close();
  if (wasConnected)
    std::cout << "[MultiPort] All ports disconnected" << std::endl;
}

bool WinSocketClientMgr::IsMultiPortConnected() {
  if (!m_connected.load(std::memory_order_acquire))
    return false;
  return m_main_client.IsConnected() && m_debug_client.IsConnected() &&
         m_error_client.IsConnected();
}

// ==================== 进程管理 ====================

void SetCurrentPid(int pid) {
  AppContext::Get().selectedPid.store(pid, std::memory_order_relaxed);
}

int GetCurrentPid() {
  return AppContext::Get().selectedPid.load(std::memory_order_relaxed);
}

bool OpenProcessHandle(int pid, int &outHandle, PortType type) {
  outHandle = 0;
  auto client = GetSocketMgr().GetClient(type);
  if (!client->IsConnected())
    return false;

  auto portMutex = GetSocketMgr().GetMutex(type);
  return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
      portMutex, [&]() -> bool {
        client->DrainPending();
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
        if (handle == 0) {
          AppContext::Get().processHandle.store(0, std::memory_order_relaxed);
          return false;
        }
        outHandle = handle;
        AppContext::Get().processHandle.store(handle, std::memory_order_relaxed);
        return true;
      });
}

bool EnsureOpenHandle(int &outHandle, PortType type) {
  int handle = AppContext::Get().processHandle.load(std::memory_order_relaxed);
  if (handle) {
    outHandle = handle;
    return true;
  }
  int pid = AppContext::Get().selectedPid.load(std::memory_order_relaxed);
  if (pid == 0)
    return false;
  return OpenProcessHandle(pid, outHandle, type);
}

bool CloseProcessHandle(int handle, PortType type) {
  if (handle == 0)
    return true;

  auto client = GetSocketMgr().GetClient(type);
  if (!client || !client->IsConnected())
    return false;

  auto portMutex = GetSocketMgr().GetMutex(type);
  return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
      portMutex, [&]() -> bool {
        client->DrainPending();
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
