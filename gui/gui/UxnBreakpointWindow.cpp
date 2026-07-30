#include "UxnBreakpointWindow.h"

#include "Gui.h"
#include "../imgui/imgui.h"
#include "../mem/IMemService.h"

#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace {

const char* stateName(Mem::UxnState state) {
    switch (state) {
    case Mem::UxnState::Empty: return "EMPTY";
    case Mem::UxnState::Armed: return "ARMED";
    case Mem::UxnState::Paused: return "PAUSED";
    case Mem::UxnState::Stepping: return "STEPPING";
    }
    return "UNKNOWN";
}

std::string hexValue(uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0')
           << std::setw(16) << value;
    return output.str();
}

} // namespace

UxnBreakpointWindow::UxnBreakpointWindow(Mem::IMemService& service)
    : service_(service) {
    name = "UXN 异常断点";
}

UxnBreakpointWindow::~UxnBreakpointWindow() {
    waitThread_.requestStop();
    waitThread_.stop();
    drainClosedWait();
    resumeEvent(false);
}

unsigned int UxnBreakpointWindow::getWindowFlags() const {
    return ImGuiWindowFlags_NoDocking;
}

void UxnBreakpointWindow::draw() {
    Window::draw();
    if (!pOpen) {
        stopWait();
        drainClosedWait();
        resumeEvent(false);
    }
}

bool UxnBreakpointWindow::parseAddress(uint64_t& address) const {
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed =
        std::strtoull(addressText_, &end, 0);
    if (end == addressText_ || *end != '\0' || errno == ERANGE) {
        return false;
    }
    address = static_cast<uint64_t>(parsed);
    return address != 0 && (address & 3u) == 0;
}

void UxnBreakpointWindow::startWait() {
    if (waitThread_.running() || event_) {
        return;
    }
    waitThread_.stop();
    const Mem::OperationContext context = service_.captureContext(true);
    Mem::UxnWaitRequest request;
    request.slot = static_cast<uint32_t>(slot_);
    request.timeoutMs = static_cast<uint32_t>(timeoutMs_);
    request.lastSequence = lastSequence_;
    waitThread_.launch(
        [this, context, request](const std::atomic<bool>& cancel) {
            auto result = service_.waitUxnBreakpoint(context, request);
            if (cancel.load(std::memory_order_acquire) && result.ok()) {
                Mem::UxnResumeRequest resume;
                resume.slot = result.value().slot;
                auto resumed = service_.resumeUxnBreakpoint(context, resume);
                if (!resumed.ok()) {
                    Gui::log("UXN 自动恢复失败: %s",
                             resumed.error().message.c_str());
                }
                return;
            }
            std::lock_guard<std::mutex> lock(waitMutex_);
            waitOutcome_.emplace(
                WaitOutcome{context, std::move(result)});
        });
}

void UxnBreakpointWindow::stopWait() {
    monitoring_ = false;
    waitThread_.requestStop();
}

void UxnBreakpointWindow::pollWait() {
    std::optional<WaitOutcome> outcome;
    {
        std::lock_guard<std::mutex> lock(waitMutex_);
        outcome.swap(waitOutcome_);
    }
    if (!outcome) {
        if (monitoring_ && !waitThread_.running() && !event_) {
            startWait();
        }
        return;
    }

    waitThread_.stop();
    if (outcome->result.ok()) {
        event_ = outcome->result.value();
        eventContext_ = outcome->context;
        slot_ = static_cast<int>(event_->slot);
        lastSequence_ = event_->sequence;
        editedX0_ = event_->registers.general[0];
        monitoring_ = false;
        Gui::log("UXN 命中 slot=%u tid=%u seq=%llu",
                 event_->slot, event_->tid,
                 static_cast<unsigned long long>(event_->sequence));
        return;
    }

    const auto& error = outcome->result.error();
    if (error.code != Mem::ErrorCode::Timeout) {
        monitoring_ = false;
        Gui::log("UXN 等待失败: %s", error.message.c_str());
    }
    if (monitoring_) {
        startWait();
    }
}

void UxnBreakpointWindow::drainClosedWait() {
    if (waitThread_.running()) {
        return;
    }
    waitThread_.stop();
    std::optional<WaitOutcome> outcome;
    {
        std::lock_guard<std::mutex> lock(waitMutex_);
        outcome.swap(waitOutcome_);
    }
    if (!outcome || !outcome->result.ok()) {
        return;
    }
    Mem::UxnResumeRequest request;
    request.slot = outcome->result.value().slot;
    auto resumed = service_.resumeUxnBreakpoint(outcome->context, request);
    if (!resumed.ok()) {
        Gui::log("UXN 窗口关闭后的自动恢复失败: %s",
                 resumed.error().message.c_str());
    }
}

void UxnBreakpointWindow::resumeEvent(bool writeX0) {
    if (!event_ || !eventContext_) {
        return;
    }
    Mem::UxnResumeRequest request;
    request.slot = event_->slot;
    request.writeRegisters = writeX0;
    request.registers = event_->registers;
    if (writeX0) {
        request.registers.general[0] = editedX0_;
    }
    auto result = service_.resumeUxnBreakpoint(*eventContext_, request);
    if (!result.ok()) {
        Gui::log("UXN 恢复失败: %s", result.error().message.c_str());
        return;
    }
    Gui::log("UXN slot=%u 已恢复%s", event_->slot,
             writeX0 ? "并写回 X0" : "");
    event_.reset();
    eventContext_.reset();
}

void UxnBreakpointWindow::drawStatus() {
    if (!status_) {
        return;
    }
    const auto& status = *status_;
    ImGui::SeparatorText("槽位状态");
    ImGui::Text("slot=%u  used=%s  state=%s", status.slot,
                status.used ? "true" : "false", stateName(status.state));
    ImGui::Text("PID=%u  TID=%u  errno=%d", status.pid, status.tid,
                status.lastError);
    ImGui::Text("地址=%s  页=%s", hexValue(status.address).c_str(),
                hexValue(status.page).c_str());
    ImGui::Text("hits=%llu  false=%llu  step=%llu  resumes=%llu  seq=%llu",
                static_cast<unsigned long long>(status.hits),
                static_cast<unsigned long long>(status.falseHits),
                static_cast<unsigned long long>(status.stepHits),
                static_cast<unsigned long long>(status.resumes),
                static_cast<unsigned long long>(status.sequence));
}

void UxnBreakpointWindow::drawEvent() {
    if (!event_) {
        return;
    }
    const auto& event = *event_;
    ImGui::SeparatorText("暂停事件");
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                       "目标线程已暂停，请恢复、移除或清理断点");
    ImGui::Text("slot=%u  PID=%u  TID=%u  seq=%llu  hits=%llu",
                event.slot, event.pid, event.tid,
                static_cast<unsigned long long>(event.sequence),
                static_cast<unsigned long long>(event.hits));
    ImGui::Text("PC=%s  FAR=%s  ESR=%s",
                hexValue(event.registers.programCounter).c_str(),
                hexValue(event.faultAddress).c_str(),
                hexValue(event.esr).c_str());

    ImGui::Checkbox("恢复时写回 X0", &writeX0_);
    if (writeX0_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputScalar("##uxn_x0", ImGuiDataType_U64, &editedX0_,
                           nullptr, nullptr, "0x%016llX",
                           ImGuiInputTextFlags_CharsHexadecimal);
    }
    if (ImGui::Button("恢复线程")) {
        resumeEvent(writeX0_);
    }

    if (ImGui::CollapsingHeader("通用寄存器",
                                ImGuiTreeNodeFlags_DefaultOpen) &&
        ImGui::BeginTable("uxn_regs", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        for (size_t i = 0; i < event.registers.general.size(); ++i) {
            ImGui::TableNextColumn();
            ImGui::Text("X%zu  %s", i,
                        hexValue(event.registers.general[i]).c_str());
        }
        ImGui::TableNextColumn();
        ImGui::Text("SP  %s",
                    hexValue(event.registers.stackPointer).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("PC  %s",
                    hexValue(event.registers.programCounter).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("PSTATE  %s",
                    hexValue(event.registers.pstate).c_str());
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("FPSIMD 寄存器")) {
        ImGui::Text("快照有效: %s  FPSR=%s  FPCR=%s",
                    event.fpsimd.valid ? "是" : "否",
                    hexValue(event.fpsimd.fpsr).c_str(),
                    hexValue(event.fpsimd.fpcr).c_str());
        if (event.fpsimd.valid &&
            ImGui::BeginTable("uxn_fp_regs", 2,
                              ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Borders)) {
            for (size_t i = 0; i < event.fpsimd.vector.size(); ++i) {
                const auto& value = event.fpsimd.vector[i];
                ImGui::TableNextColumn();
                ImGui::Text("V%zu  0x%016llx%016llx", i,
                            static_cast<unsigned long long>(value.high),
                            static_cast<unsigned long long>(value.low));
            }
            ImGui::EndTable();
        }
    }
}

void UxnBreakpointWindow::onDraw() {
    pollWait();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputText("执行地址", addressText_, sizeof(addressText_));
    ImGui::SameLine();
    if (ImGui::Button("安装 UXN")) {
        uint64_t address = 0;
        if (!parseAddress(address)) {
            Gui::log("UXN 地址必须为非零且 4 字节对齐");
        } else {
            auto result = service_.installUxnBreakpoint(
                service_.captureContext(true),
                Mem::UxnInstallRequest{address});
            if (!result.ok()) {
                Gui::log("UXN 安装失败: %s", result.error().message.c_str());
            } else {
                installedAddress_ = result.value().address;
                slot_ = static_cast<int>(result.value().slot);
                lastSequence_ = 0;
                status_.reset();
                Gui::log("UXN 已安装 slot=%u address=%s",
                         result.value().slot,
                         hexValue(result.value().address).c_str());
            }
        }
    }

    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt("槽位", &slot_);
    slot_ = slot_ < 0 ? 0 : (slot_ > 15 ? 15 : slot_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("等待毫秒", &timeoutMs_);
    timeoutMs_ = timeoutMs_ < 1 ? 1 :
        (timeoutMs_ > 60000 ? 60000 : timeoutMs_);

    if (!monitoring_ && !event_) {
        if (ImGui::Button("开始异步监听")) {
            monitoring_ = true;
            startWait();
        }
    } else if (monitoring_) {
        if (ImGui::Button("停止监听")) {
            stopWait();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("正在等待命中...");
    }

    ImGui::SameLine();
    if (ImGui::Button("查询状态")) {
        auto result = service_.queryUxnBreakpointStatus(
            service_.captureContext(true),
            Mem::UxnStatusRequest{static_cast<uint32_t>(slot_)});
        if (!result.ok()) {
            Gui::log("UXN 状态查询失败: %s",
                     result.error().message.c_str());
        } else {
            status_ = result.value();
        }
    }

    if (ImGui::Button("移除当前地址")) {
        uint64_t address = installedAddress_;
        if (address == 0 && !parseAddress(address)) {
            Gui::log("没有可移除的 UXN 地址");
        } else {
            auto result = service_.removeUxnBreakpoint(
                service_.captureContext(true), Mem::UxnRemoveRequest{address});
            if (!result.ok()) {
                Gui::log("UXN 移除失败: %s", result.error().message.c_str());
            } else {
                stopWait();
                event_.reset();
                eventContext_.reset();
                installedAddress_ = 0;
                status_.reset();
                Gui::log("UXN 地址 %s 已移除", hexValue(address).c_str());
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("全局清理 UXN")) {
        auto result = service_.clearUxnBreakpoints(
            service_.captureContext(false));
        if (!result.ok()) {
            Gui::log("UXN 清理失败: %s", result.error().message.c_str());
        } else {
            stopWait();
            event_.reset();
            eventContext_.reset();
            installedAddress_ = 0;
            lastSequence_ = 0;
            status_.reset();
            Gui::log("UXN 断点已全部清理");
        }
    }

    drawStatus();
    drawEvent();
}
