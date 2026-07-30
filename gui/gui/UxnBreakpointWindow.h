#pragma once

#include "Window.h"
#include "../mem/MemResult.h"
#include "../mem/MemTypes.h"
#include "../utils/ScopedThread.h"

#include <atomic>
#include <mutex>
#include <optional>

namespace Mem { class IMemService; }

class UxnBreakpointWindow : public Window {
public:
    explicit UxnBreakpointWindow(Mem::IMemService& service);
    ~UxnBreakpointWindow() override;

    void draw() override;
    void onDraw() override;
    unsigned int getWindowFlags() const override;

private:
    struct WaitOutcome {
        Mem::OperationContext context;
        Mem::Result<Mem::UxnEvent> result;
    };

    void startWait();
    void stopWait();
    void pollWait();
    void drainClosedWait();
    void resumeEvent(bool writeX0);
    void drawEvent();
    void drawStatus();
    bool parseAddress(uint64_t& address) const;

    Mem::IMemService& service_;
    ScopedThread waitThread_;
    std::mutex waitMutex_;
    std::optional<WaitOutcome> waitOutcome_;
    std::optional<Mem::UxnEvent> event_;
    std::optional<Mem::OperationContext> eventContext_;
    std::optional<Mem::UxnStatus> status_;
    bool monitoring_ = false;
    bool writeX0_ = false;
    uint64_t editedX0_ = 0;
    uint64_t installedAddress_ = 0;
    uint64_t lastSequence_ = 0;
    int slot_ = 0;
    int timeoutMs_ = 1000;
    char addressText_[32] = "0x0";
};
