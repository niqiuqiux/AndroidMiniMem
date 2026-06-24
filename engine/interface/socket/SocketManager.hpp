#pragma once
#include <string>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include "../../common/Logger.hpp"
#include "../../ceserver/ioserver.hpp"

class SocketManager : public Ioserver {
private:
    int listenFd_;
    int connFd_;
    bool isServer_;
    std::string clientIp_;
    uint16_t clientPort_;
    int recvTimeout_ms_ = 0; // 0 = 无超时

public:
    SocketManager() : listenFd_(-1), connFd_(-1), isServer_(false), clientPort_(0) {}

    ~SocketManager() {
        Close();
    }

    bool BindAndListen(const std::string &host, uint16_t port, bool nonblock = false, int backlog = 1) {
        isServer_ = true;
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ == -1) {
            LOGDF("socket() failed: %s", strerror(errno));
            return false;
        }

        int yes = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (host.empty() || host == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
                LOGDF("inet_pton failed for host %s", host.c_str());
                ::close(listenFd_);
                listenFd_ = -1;
                return false;
            }
        }

        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
            LOGDF("bind() failed: %s", strerror(errno));
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        if (nonblock) {
            int flags = fcntl(listenFd_, F_GETFL, 0);
            fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);
        }

        if (::listen(listenFd_, backlog) == -1) {
            LOGDF("listen() failed: %s", strerror(errno));
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        LOGD("Socket server bind+listen OK");
        return true;
    }

    bool Accept(int timeout_ms = -1) {
        if (listenFd_ == -1) return false;

        if (timeout_ms >= 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listenFd_, &rfds);
            timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
            int r = ::select(listenFd_ + 1, &rfds, nullptr, nullptr, &tv);
            if (r <= 0) {
                if (r == 0) LOGDF("Accept timeout after %d ms", timeout_ms);
                else LOGDF("select() before accept failed: %s", strerror(errno));
                return false;
            }
        }

        sockaddr_in cli{}; socklen_t cl = sizeof(cli);
        int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&cli), &cl);
        if (fd == -1) {
            LOGDF("accept() failed: %s", strerror(errno));
            return false;
        }
        connFd_ = fd;
        
        // 保存客户端信息
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        clientIp_ = ip;
        clientPort_ = ntohs(cli.sin_port);
        
        LOGDF("Client connected from %s:%d", clientIp_.c_str(), clientPort_);
        return true;
    }

    // Accept并将连接分配给另一个SocketManager对象
    bool AcceptTo(SocketManager* target, int timeout_ms = -1) {
        if (listenFd_ == -1 || !target) return false;

        if (timeout_ms >= 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listenFd_, &rfds);
            timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
            int r = ::select(listenFd_ + 1, &rfds, nullptr, nullptr, &tv);
            if (r <= 0) {
                if (r == 0) LOGDF("AcceptTo timeout after %d ms", timeout_ms);
                else LOGDF("select() before accept failed: %s", strerror(errno));
                return false;
            }
        }

        sockaddr_in cli{}; socklen_t cl = sizeof(cli);
        int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&cli), &cl);
        if (fd == -1) {
            LOGDF("accept() failed: %s", strerror(errno));
            return false;
        }
        
        // 保存客户端信息到目标对象
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        target->clientIp_ = ip;
        target->clientPort_ = ntohs(cli.sin_port);
        
        // 将连接fd分配给目标对象
        target->connFd_ = fd;
        target->isServer_ = false;
        LOGDF("Client connected from %s:%d and assigned to target", 
              target->clientIp_.c_str(), target->clientPort_);
        return true;
    }

    void CloseListener() {
        if (listenFd_ != -1) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
    }

    bool Connect(const std::string &host, uint16_t port) {
        isServer_ = false;
        connFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (connFd_ == -1) {
            LOGDF("socket() failed: %s", strerror(errno));
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            LOGDF("inet_pton failed for host %s", host.c_str());
            ::close(connFd_);
            connFd_ = -1;
            return false;
        }
        if (::connect(connFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
            LOGDF("connect() failed: %s", strerror(errno));
            ::close(connFd_);
            connFd_ = -1;
            return false;
        }
        LOGD("Socket client connected");
        return true;
    }

    int GetReadFd() override {
        return connFd_;
    }

    int GetWriteFd() override {
        return connFd_;
    }

    bool Send(const void *data, size_t size) override {
        if (connFd_ == -1) return false;
        const char *p = static_cast<const char*>(data);
        size_t total = 0;
        while (total < size) {
            ssize_t n = ::send(connFd_, p + total, size - total, 0);
            if (n == -1) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // small wait
                    timeval tv{0, 1000};
                    select(0, nullptr, nullptr, nullptr, &tv);
                    continue;
                }
                LOGDF("send() failed: %s", strerror(errno));
                return false;
            }
            if (n == 0) return false;
            total += static_cast<size_t>(n);
        }
        return true;
    }

    bool Receive(void *buffer, size_t size) override {
        if (connFd_ == -1) return false;
        char *p = static_cast<char*>(buffer);
        size_t total = 0;
        while (total < size) {
            // 超时检查
            if (recvTimeout_ms_ > 0) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(connFd_, &rfds);
                timeval tv;
                tv.tv_sec = recvTimeout_ms_ / 1000;
                tv.tv_usec = (recvTimeout_ms_ % 1000) * 1000;
                int sel = ::select(connFd_ + 1, &rfds, nullptr, nullptr, &tv);
                if (sel == 0) {
                    LOGD("recv() timeout");
                    return false;
                }
                if (sel < 0) {
                    if (errno == EINTR) continue;
                    LOGDF("select() failed: %s", strerror(errno));
                    return false;
                }
            }
            ssize_t n = ::recv(connFd_, p + total, size - total, 0);
            if (n == -1) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // small wait
                    timeval tv{0, 1000};
                    select(0, nullptr, nullptr, nullptr, &tv);
                    continue;
                }
                LOGDF("recv() failed: %s", strerror(errno));
                return false;
            }
            if (n == 0) {
                LOGDF("peer closed");
                return false;
            }
            total += static_cast<size_t>(n);
        }
        return true;
    }

    void SetReceiveTimeout(int timeout_ms) override {
        recvTimeout_ms_ = timeout_ms;
    }

    void Close() override {
        if (connFd_ != -1) {
            ::close(connFd_);
            connFd_ = -1;
        }
        if (listenFd_ != -1) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
    }

    bool IsValid() const override {
        return connFd_ != -1;
    }
    
    // 获取客户端信息
    std::string GetClientIp() const { return clientIp_; }
    uint16_t GetClientPort() const { return clientPort_; }
    std::string GetClientInfo() const {
        if (clientIp_.empty()) return "unknown";
        return clientIp_ + ":" + std::to_string(clientPort_);
    }

    // 获取监听socket的fd（用于epoll/ALooper注册）
    int GetListenFd() const { return listenFd_; }
};