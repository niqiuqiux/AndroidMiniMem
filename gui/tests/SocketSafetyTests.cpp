#include "../socket/SocketCommand.h"
#include "../socket/client.hpp"
#include "../socket/socket_io_timeout.h"
#include "../socket/socket_platform.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void testTimeoutScopes() {
    check(!SocketIoTimeout::HasThreadTimeout(),
          "thread timeout should be absent by default");
    {
        SocketIoTimeout::ScopedTimeout timeout(1);
        check(SocketIoTimeout::HasThreadTimeout() &&
                  SocketIoTimeout::GetThreadTimeoutMs() <= 1000,
              "second-based timeout should publish its budget");
        const auto outerDeadline = SocketIoTimeout::GetThreadDeadline();
        {
            SocketIoTimeout::ScopedTimeout nested(10);
            check(SocketIoTimeout::GetThreadDeadline() == outerDeadline,
                  "nested timeout must not extend the outer deadline");
            {
                SocketIoTimeout::ScopedTimeout unlimited(0);
                check(SocketIoTimeout::GetThreadDeadline() == outerDeadline,
                      "nested unlimited scope must retain the outer deadline");
            }
        }
        check(SocketIoTimeout::GetThreadDeadline() == outerDeadline,
              "nested timeout should restore the outer deadline");

        const auto shorterDeadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(100);
        {
            SocketIoTimeout::ScopedTimeout nested(shorterDeadline);
            check(SocketIoTimeout::GetThreadDeadline() == shorterDeadline,
                  "nested timeout should accept an earlier deadline");
        }
        check(SocketIoTimeout::GetThreadDeadline() == outerDeadline,
              "earlier nested deadline should restore the outer deadline");
    }
    check(!SocketIoTimeout::HasThreadTimeout(),
          "timeout scope should restore the default state");

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    {
        SocketIoTimeout::ScopedTimeout timeout(deadline);
        check(SocketIoTimeout::HasThreadTimeout() &&
                  SocketIoTimeout::GetRemainingTimeoutMs() <= 2000,
              "deadline-based timeout should expose remaining time");
    }
}

void testSocketPoisoning() {
    check(SocketPlatform::Startup(), "socket platform should initialize");
    SocketPlatform::ScopedSocket listener(
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    check(listener.valid(), "loopback listener should be created");
    if (!listener.valid()) {
        SocketPlatform::Cleanup();
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    const bool bound =
        ::bind(listener.get(), reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != SOCKET_ERROR;
    check(bound, "loopback listener should bind");
    const bool listening = bound &&
        ::listen(listener.get(), 2) != SOCKET_ERROR;
    check(listening, "loopback listener should listen");

    SocketPlatform::SocketOptionLength addressLength = sizeof(address);
    const bool hasPort = listening &&
        ::getsockname(listener.get(),
                      reinterpret_cast<sockaddr*>(&address),
                      &addressLength) != SOCKET_ERROR;
    check(hasPort, "loopback listener should expose its port");
    if (!hasPort) {
        listener.reset();
        SocketPlatform::Cleanup();
        return;
    }
    const uint16_t port = ntohs(address.sin_port);

    std::thread server([socket = listener.get()] {
        SOCKET first = ::accept(socket, nullptr, nullptr);
        if (first != INVALID_SOCKET) {
            const char byte = 'x';
            (void)::send(first, &byte, 1, 0);
            SocketPlatform::Close(first);
        }
        SOCKET second = ::accept(socket, nullptr, nullptr);
        if (second != INVALID_SOCKET) {
            char byte = 0;
            (void)::recv(second, &byte, 1, 0);
            SocketPlatform::Close(second);
        }
    });

    {
        std::atomic<int> poisonCount{0};
        WindowsSocketClient client;
        client.SetPoisonCallback([&] { ++poisonCount; });
        check(client.Connect("127.0.0.1", port),
              "client should connect for truncated response test");
        char response[2]{};
        check(!client.Receive(response, sizeof(response)) &&
                  poisonCount.load() == 1 && !client.IsConnected(),
              "truncated I/O should poison and close the connection");
    }

    {
        std::atomic<int> poisonCount{0};
        WindowsSocketClient client;
        client.SetPoisonCallback([&] { ++poisonCount; });
        check(client.Connect("127.0.0.1", port),
              "client should connect for malformed response test");
        check(!SocketCommand::rejectMalformedResponse(&client) &&
                  poisonCount.load() == 1 && !client.IsConnected(),
              "malformed response should poison and close the connection");
    }

    server.join();
    listener.reset();
    SocketPlatform::Cleanup();
}

} // namespace

int main() {
    testTimeoutScopes();
    testSocketPoisoning();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "Socket safety tests passed\n";
    return 0;
}
