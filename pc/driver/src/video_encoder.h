#pragma once

#include <wnvr/protocol/messages.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace wnvr::driver {

class StatsCollector;

// 遅延の内訳を測るための PC の時刻。0（既定値）は「未測定」。
struct FrameTiming {
    std::chrono::steady_clock::time_point poseReceived{};
    std::chrono::steady_clock::time_point presented{};
    std::chrono::steady_clock::time_point encoded{};
};

// フレームに付ける姿勢と、ドライバがその姿勢を受け取った時刻。
struct FramePoseSample {
    protocol::FramePose pose;
    std::chrono::steady_clock::time_point receivedAt{};
};

struct EncodedFrame {
    std::vector<uint8_t> data;
    uint64_t frameIndex = 0;
    uint64_t sourceFrameId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool keyFrame = false;
    double encodeMs = 0;
    protocol::FramePose pose;
    FrameTiming timing;
};

class EncodedFrameSink {
public:
    virtual ~EncodedFrameSink() = default;
    // エンコーダーのスレッドから呼ばれる。
    virtual void OnEncodedFrame(const EncodedFrame& frame) = 0;
};

class VideoEncoder {
public:
    struct Config {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frameRate = 72;
        uint32_t bitrateMbps = 80;
        StatsCollector* statsCollector = nullptr;
    };

    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Initialize / Shutdown は呼び出し側で直列化し、sink は Shutdown 完了まで維持する。
    bool Initialize(void* d3d11Device, const std::vector<void*>& textures, const Config& config, EncodedFrameSink* sink);
    void Shutdown();
    bool Submit(size_t textureIndex, uint64_t sourceFrameId, const protocol::FramePose& pose, const FrameTiming& timing);
    bool IsBusy(size_t textureIndex) const;
    void RequestKeyFrame();
    void SetBitrate(uint32_t mbps);
    // コピー前や GPU 完了待ちでの破棄も同じ統計に含めるために使う。
    void RecordDroppedFrame();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
