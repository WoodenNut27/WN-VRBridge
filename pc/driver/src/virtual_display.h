#pragma once

#include "video_encoder.h"

#include <openvr_driver.h>
#include <wnvr/protocol/messages.h>

#include <cstdint>
#include <memory>
#include <functional>

namespace wnvr::driver {

class EncodedFrameSink;
class StatsCollector;

class VirtualDisplay : public vr::IVRVirtualDisplay {
public:
    struct Config {
        int32_t adapterIndex = -1;
        float displayFrequency = 72.0f;
        bool dumpFrame = false;
        uint32_t bitrateMbps = 80;
        uint32_t dumpVideoSeconds = 0;
        std::function<FramePoseSample()> getFramePose;
        StatsCollector* statsCollector = nullptr;
    };

    VirtualDisplay();
    ~VirtualDisplay();
    VirtualDisplay(const VirtualDisplay&) = delete;
    VirtualDisplay& operator=(const VirtualDisplay&) = delete;

    bool Initialize(const Config& config);
    uint64_t AdapterLuid() const;
    void SetFrameSink(EncodedFrameSink* sink);
    void RequestKeyFrame();
    void SetBitrate(uint32_t mbps);
    void SetFoveation(const protocol::Foveation& foveation);
    void OnDisplayFeedback(const protocol::DisplayFeedbackMessage& feedback);
    void ResetDisplayFeedback();

    void Present(const vr::PresentInfo_t* presentInfo, uint32_t presentInfoSize) override;
    void WaitForPresent() override;
    bool GetTimeSinceLastVsync(float* secondsSinceLastVsync, uint64_t* frameCounter) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
