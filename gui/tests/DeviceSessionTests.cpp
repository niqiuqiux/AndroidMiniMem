#include "../socket/DeviceSession.h"

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    auto& session = DeviceSession::GetInstance();
    check(!session.AcquireRequest(),
          "disconnected session rejects requests");

    {
        auto lifecycle = session.AcquireLifecycle();
        session.BeginConnect();
        session.FinishConnect(true);
    }
    const uint64_t firstGeneration = session.GetGeneration();
    check(session.IsConnected() && firstGeneration > 0,
          "connect publishes a generation");

    {
        auto outer = session.AcquireRequest();
        auto nested = session.AcquireRequest();
        check(outer && nested &&
                  outer.generation() == nested.generation(),
              "nested commands share the active request generation");
        session.MarkPoisoned();
        check(session.IsPoisoned(), "I/O failure poisons the session");
        check(!outer.isCurrent() && !nested.isCurrent(),
              "poisoning invalidates all leases immediately");
    }

    check(session.GetGeneration() > firstGeneration,
          "poisoning advances the generation");
    {
        auto lifecycle = session.AcquireLifecycle();
        session.BeginConnect();
        session.FinishConnect(true);
    }
    check(session.IsConnected(), "explicit reconnect restores availability");
    {
        auto request = session.AcquireRequest();
        check(request && request.isCurrent(),
              "reconnected generation accepts fresh requests");
    }
    {
        auto lifecycle = session.AcquireLifecycle();
        session.Disconnect();
    }
    check(!session.IsConnected() && !session.AcquireRequest(),
          "disconnect rejects later requests");

    if (failures != 0) {
        return 1;
    }
    std::cout << "DeviceSession tests passed\n";
    return 0;
}
