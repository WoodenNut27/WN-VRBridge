#pragma once

#include <wnvr/protocol/messages.h>
#include <aaudio/AAudio.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

namespace wnvr {

class AudioPlayer {
public:
    struct ClientStats {
        uint64_t underruns = 0;
        uint64_t missingFrames = 0;
        uint64_t callbacks = 0;
        double bufferTotalMs = 0;
    };
    ClientStats TakeClientStats();
    AudioPlayer();
    ~AudioPlayer();
    void Push(const protocol::AudioMessage& audio);
    void Stop();
    // 無音で受信が途切れた期間も、描画側の定期処理から統計を出せるようにする。
    void ReportStats();

private:
    static aaudio_data_callback_result_t OnData(AAudioStream* stream, void* context, void* data, int32_t frames);
    static void OnError(AAudioStream* stream, void* context, aaudio_result_t error);
    bool Open(uint32_t sampleRate);
    void CloseStream();
    void LogStats();

    // 制御用とバッファ用を分け、close がコールバックの終了を待ってもデッドロックしない。
    ClientStats m_clientStats;
    std::mutex m_controlMutex;
    std::mutex m_bufferMutex;
    AAudioStream* m_stream = nullptr;
    std::atomic<aaudio_result_t> m_error{AAUDIO_OK};
    std::vector<int16_t> m_buffer;
    size_t m_readOffset = 0;
    size_t m_size = 0;
    uint32_t m_sampleRate = 0;
    bool m_preparing = true;
    std::optional<uint64_t> m_nextFrame;
    uint64_t m_packets = 0;
    uint32_t m_receivedSampleRate = 0;
    uint64_t m_missingFrames = 0;
    uint64_t m_underruns = 0;
    uint64_t m_droppedFrames = 0;
    double m_bufferTotalMs = 0;
    uint64_t m_callbacks = 0;
    std::chrono::steady_clock::time_point m_nextReport{};
};

}
