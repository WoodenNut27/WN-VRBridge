#pragma once

#include "virtual_display.h"

#include <openvr_driver.h>
#include <wnvr/protocol/messages.h>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>

namespace wnvr::driver {

class HmdDevice : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent {
public:
    static constexpr char kSerialNumber[] = "WNVR-HMD-0001";
    static constexpr char kModelName[] = "WN-VRBridge HMD";

    explicit HmdDevice(StatsCollector* statsCollector = nullptr);
    HmdDevice(const HmdDevice&) = delete;
    HmdDevice& operator=(const HmdDevice&) = delete;

    vr::EVRInitError Activate(uint32_t objectId) override;
    void Deactivate() override;
    void EnterStandby() override;
    void* GetComponent(const char* name) override;
    void DebugRequest(const char* request, char* responseBuffer, uint32_t bufferSize) override;
    vr::DriverPose_t GetPose() override;
    void RunFrame();

    // ネットワークのスレッドから呼ばれる。
    void UpdateHeadPose(const protocol::DevicePose& head, uint64_t sampleTimeUs);
    FramePoseSample GetFramePose();
    void SetFrameSink(EncodedFrameSink* sink);
    void RequestKeyFrame();
    void SetBitrate(uint32_t mbps);
    void SetFoveation(const protocol::Foveation& foveation);
    void OnDisplayFeedback(const protocol::DisplayFeedbackMessage& feedback);
    void SetClientConnected(bool connected);
    void ApplyClientDisplay(const protocol::HelloMessage& hello);
    const protocol::Fov& RenderLeftFov() const { return m_leftEyeFov; }
    const protocol::Fov& RenderRightFov() const { return m_rightEyeFov; }
    float RenderIpdMeters() const { return m_ipdMeters; }

    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) override;
    bool IsDisplayOnDesktop() override;
    bool IsDisplayRealDisplay() override;
    void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) override;
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width, uint32_t* height) override;
    void GetProjectionRaw(vr::EVREye eye, float* left, float* right, float* top, float* bottom) override;
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eye, float u, float v) override;
    bool ComputeInverseDistortion(vr::HmdVector2_t* result, vr::EVREye eye, uint32_t channel, float u, float v) override;

private:
    vr::DriverPose_t GetPoseLocked() const;
    void PublishPoseLocked();
    void PublishDisplayLocked();

    // 受け取った姿勢の履歴。フレームを描くのに SteamVR が使った姿勢は、フレームを受け取った時点の
    // 最新の姿勢より古いので、受け取った時刻から m_framePoseDelay だけ前の姿勢をフレームに付ける。
    struct PoseHistoryEntry {
        std::chrono::steady_clock::time_point receivedAt;
        protocol::FramePose pose;
    };
    std::deque<PoseHistoryEntry> m_poseHistory;
    std::chrono::microseconds m_framePoseDelay{0};

    std::mutex m_poseMutex;
    uint64_t m_sampleTimeUs = 0;
    protocol::Pose m_headPose{{0.0f, 1.6f, 0.0f}, {}, {}, {}};
    bool m_clientConnected = false;
    bool m_poseValid = false;
    vr::TrackedDeviceIndex_t m_objectId = vr::k_unTrackedDeviceIndexInvalid;
    protocol::Fov m_leftEyeFov;
    protocol::Fov m_rightEyeFov;
    float m_ipdMeters = 0.063f;
    uint32_t m_renderWidth;
    uint32_t m_renderHeight;
    float m_displayFrequency;
    std::unique_ptr<VirtualDisplay> m_virtualDisplay;
};

}
