#pragma once
#include <atomic>
#include <functional>
#include <iostream>

struct ScanProgress {
    int msgType{0}; // 1=进度, 2=结果, 3=错误
    float percent{0.0f};
    std::atomic<size_t> totalBytes{0};
    std::atomic<size_t> scannedBytes{0};
    std::atomic<size_t> matchCount{0};
    
    // float GetProgress() const {
    //     return totalBytes > 0 ? 
    //         static_cast<float>(scannedBytes) / totalBytes : 0.0f;
    // }

    float GetProgress() {
        if(percent == 1.0f) return percent;
        percent = totalBytes > 0 ? 
            static_cast<float>(scannedBytes) / totalBytes : 0.0f;
        return percent;
    }
};

// 线传输安全DTO，避免直接发送含有atomic的对象
struct ScanProgressMsg {
    int msgType;
    float percent;
    uint64_t totalBytes;
    uint64_t scannedBytes;
    uint64_t matchCount;
};

inline ScanProgressMsg MakeScanProgressMsg(const ScanProgress& p){
    ScanProgressMsg m;
    m.msgType = p.msgType;
    m.percent = p.percent;
    m.totalBytes = p.totalBytes.load();
    m.scannedBytes = p.scannedBytes.load();
    m.matchCount = p.matchCount.load();
    return m;
}

using ProgressCallback = std::function<void( ScanProgress&)>; 