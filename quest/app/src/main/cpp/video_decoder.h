#pragma once

#include "frame_assembler.h"

#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace wnvr::quest {

struct DecodedImage {
    // 取得後は描画側が所有し、直接 delete せず ReleaseImage で返す。
    AImage* image = nullptr;
    AHardwareBuffer* buffer = nullptr;
    uint32_t frameIndex = 0;
    protocol::FramePose pose;
    bool hasPose = false;
    std::chrono::steady_clock::time_point decodedTime{};
    std::chrono::steady_clock::time_point takenTime{};
    // 取得と世代の読み取りの間に再生成されても、古い画像を識別できるようにする。
    uint64_t generation = 0;
};

class VideoDecoder {
public:
    struct Config {
        bool qtiLowLatency = true;
        int operatingRate = 72;
    };

    struct ClientStats {
        uint64_t decodedFrames = 0;
        uint64_t timedFrames = 0;
        double totalMs = 0;
        double maxMs = 0;
        uint64_t decoderWaitTotalUs = 0;
        uint64_t decoderWaitFrames = 0;
    };
    ClientStats TakeClientStats();
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Start / Stop は描画スレッドで直列化し、供給元は Stop 完了まで維持する。
    bool Start(std::function<std::optional<AssembledFrame>()> frameSource,
        std::function<uint64_t()> generationSource = {},
        std::function<void(uint32_t, protocol::KeyFrameReason)> keyFrameRequest = {},
        Config config = {true, 72});
    void Stop();
    // Take / Release は描画スレッドで直列化し、Stop の前に保持画像をすべて返す。
    // 深さは遅延抑制の破棄より前に計測し、PC に混雑を知らせる。
    std::optional<DecodedImage> TakeNextImage(size_t* queueDepthBefore);
    uint64_t TakeDroppedFrames();
    void ReleaseImage(DecodedImage& image);
    uint64_t DecoderGeneration() const;

private:
    using Clock = std::chrono::steady_clock;

    void Run();
    bool CreateDecoder(uint16_t width, uint16_t height);
    void DestroyDecoder();
    bool Submit(const AssembledFrame& frame);
    bool DrainOutput();
    void WaitForKeyFrame(protocol::KeyFrameReason reason, bool resetOutput = false);
    void RequestKeyFrame();
    void RecoverDecoder();
    bool TryCreateDecoder(uint16_t width, uint16_t height);
    void LogStats();
    static void OnImageAvailable(void* context, AImageReader* reader);

    std::function<std::optional<AssembledFrame>()> m_frameSource;
    Config m_config;
    std::function<uint64_t()> m_generationSource;
    std::function<void(uint32_t, protocol::KeyFrameReason)> m_keyFrameRequest;
    std::atomic<bool> m_stopping{true};
    std::thread m_thread;
    AMediaCodec* m_codec = nullptr;
    AImageReader* m_reader = nullptr;
    bool m_started = false;
    bool m_waitingForKeyFrame = true;
    bool m_failed = false;
    std::optional<uint32_t> m_lastGoodFrame;
    protocol::KeyFrameReason m_waitReason = protocol::KeyFrameReason::DecoderStart;
    std::optional<Clock::time_point> m_waitStarted;
    Clock::time_point m_nextKeyRequest{};
    Clock::time_point m_nextCreate{};
    std::deque<Clock::time_point> m_createFailures;
    uint16_t m_targetWidth = 0;
    uint16_t m_targetHeight = 0;
    double m_keyWaitMs = 0;

    // コールバック中の画像取得・破棄と、reader の無効化を直列化する。
    std::mutex m_mutex;
    ClientStats m_clientStats;
    std::deque<DecodedImage> m_imageQueue;
    uint64_t m_droppedFrames = 0;
    // 画像コールバックにも境界を公開し、入力再開と表示再開を分ける。
    bool m_filterOutput = true;
    std::optional<uint32_t> m_outputBoundary;
    std::optional<uint32_t> m_recoveryFrame;
    std::map<AImage*, uint64_t> m_renderImages;
    std::atomic<uint64_t> m_decoderGeneration{0};
    AImageReader* m_callbackReader = nullptr;
    struct InputFrame {
        Clock::time_point time;
        protocol::FramePose pose;
    };
    std::map<uint32_t, InputFrame> m_inputTimes;
    Clock::time_point m_statsTime{};
    uint64_t m_inputs = 0;
    uint64_t m_decoded = 0;
    uint64_t m_timedFrames = 0;
    uint64_t m_inputDrops = 0;
    uint64_t m_keyFrameDrops = 0;
    double m_totalMs = 0;
    double m_maxMs = 0;
};

}
