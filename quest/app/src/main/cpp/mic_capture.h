#pragma once

#include "mic_packetizer.h"
#include <aaudio/AAudio.h>
#include <atomic>
#include <chrono>
#include <mutex>

namespace wnvr {

class MicCapture {
public:
    ~MicCapture();
    bool Start();
    void Stop();
    bool IsRunning() const;
    bool HasFailed() const { return m_failed.load(); }
    std::vector<protocol::MicAudioMessage> Take();

private:
    static aaudio_data_callback_result_t OnData(AAudioStream*, void* context, void* data, int32_t frames);
    static void OnError(AAudioStream*, void* context, aaudio_result_t error);
    void CloseStream();
    // close はコールバックの終了を待つため、パケットのロックと制御のロックを分ける。
    mutable std::mutex m_controlMutex;
    std::mutex m_bufferMutex;
    AAudioStream* m_stream = nullptr;
    std::atomic<bool> m_failed{false};
    MicPacketizer m_packetizer;
    uint32_t m_sampleRate = 0;
    uint64_t m_packets = 0;
    uint64_t m_lastDropped = 0;
    std::chrono::steady_clock::time_point m_nextReport{};
};

}
