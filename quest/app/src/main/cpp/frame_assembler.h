#pragma once

#include <wnvr/protocol/messages.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace wnvr::quest {

struct AssembledFrame {
    uint32_t frameIndex = 0;
    bool keyFrame = false;
    uint16_t videoWidth = 0;
    uint16_t videoHeight = 0;
    uint64_t connectionGeneration = 0;
    protocol::FramePose pose;
    std::chrono::steady_clock::time_point firstFragmentTime{};
    std::chrono::steady_clock::time_point completedTime{};
    std::vector<uint8_t> data;
};

class FrameAssembler {
public:
    struct ClientStats {
        uint64_t receivedFrames = 0;
        uint64_t lostFrames = 0;
        uint64_t receivedFragments = 0;
        uint64_t receivedVideoBytes = 0;
        uint64_t firstFragmentTotalUs = 0;
        uint64_t firstFragmentFrames = 0;
        uint64_t assembleTotalUs = 0;
        uint64_t assembleFrames = 0;
    };
    ClientStats TakeClientStats();
    void AddFragment(const protocol::VideoFragmentMessage& fragment, int64_t kernelTimestampNs);
    double TakeKernelAssembleAverageMs();
    std::optional<AssembledFrame> PopFrame();
    void Reset();

private:
    struct PendingFrame {
        AssembledFrame frame;
        uint16_t fragmentCount = 0;
        std::vector<bool> received;
        size_t receivedCount = 0;
        int64_t firstKernelTimestampNs = 0;
        int64_t lastKernelTimestampNs = 0;
    };

    void LogStatsLocked();

    ClientStats m_clientStats;
    uint64_t m_kernelAssembleTotalNs = 0;
    uint64_t m_kernelAssembleFrames = 0;
    std::mutex m_mutex;
    std::deque<PendingFrame> m_pending;
    std::deque<AssembledFrame> m_completed;
    std::optional<uint32_t> m_lastCompleted;
    std::chrono::steady_clock::time_point m_statsTime = std::chrono::steady_clock::now();
    uint64_t m_assembledFrames = 0;
    uint64_t m_lostFrames = 0;
    uint64_t m_fragments = 0;
    uint64_t m_bytes = 0;
    uint64_t m_keyFrames = 0;
};

}
