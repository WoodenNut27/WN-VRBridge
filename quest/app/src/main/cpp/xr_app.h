#pragma once

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_TIMESPEC
#include <time.h>
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "egl_context.h"
#include "mic_capture.h"
#include "network_client.h"
#include "video_decoder.h"
#include "video_renderer.h"
#include "status_overlay.h"
#include "restart_notice.h"
#include "xr_input.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct android_app;

namespace wnvr::quest {

class XrApp {
public:
    XrApp() = default;
    ~XrApp() { Shutdown(); }
    XrApp(const XrApp&) = delete;
    XrApp& operator=(const XrApp&) = delete;

    bool Initialize(android_app* app);
    void PollEvents(bool& exitRequested);
    bool IsSessionRunning() const { return m_sessionRunning; }
    void RenderFrame();
    void Shutdown();
    void SetResumed(bool resumed);

private:
    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        std::vector<GLuint> framebuffers;
    };

    // 姿勢の予測に使う時間の上限の既定値（ms）。これより長い遅延は、予測しても当たらない。
    static constexpr int kDefaultMaxPredictionMs = 100;

    bool CheckXr(XrResult result, const char* call);
    bool CreateSpaces();
    static std::ifstream OpenConfigFile(android_app* app, const char* name);
    void LoadPredictionConfig(android_app* app);
    void LoadDecoderConfig(android_app* app);
    void OpenNetwork(android_app* app);
    protocol::HelloMessage MakeHello() const;
    protocol::DevicePose LocatePose(XrSpace space, XrTime time);
    protocol::PoseMessage LocatePoses(XrTime headTime, XrTime handTime);
    protocol::InputMessage SyncActions();
    bool DetectTriggerPull(const protocol::InputMessage& input);
    void ReportDisplayFeedback(bool newImage, size_t queueDepth, XrDuration displayPeriod);
    void SendPose(const protocol::PoseMessage& pose);
    void SendPoses();
    bool CreateSwapchains();
    bool LogRefreshRates();
    void RequestRefreshRate();
    bool RenderEye(size_t eyeIndex, XrCompositionLayerProjectionView& projectionView,
        const protocol::DevicePose& head, const std::optional<NetworkClient::RenderDisplay>& renderDisplay);
    void ReleaseVideoImages();
    void LogHeadPose();
    void MeasureLatency(XrTime predictedDisplayTime, XrDuration predictedDisplayPeriod);
    void ReportClientStats();
    bool MicPermission(bool request);
    void UpdateMic();
    void SendMicAudio();
    MicCapture m_micCapture;
    protocol::QuestMicState m_micState = protocol::QuestMicState::NotRequested;
    android_app* m_app = nullptr;
    bool m_resumed = false;
    bool m_micPermissionRequested = false;
    bool m_micPermissionGranted = false;
    std::chrono::steady_clock::time_point m_nextMicPermissionCheck{};
    std::chrono::steady_clock::time_point m_nextMicRetry{};

    std::optional<EyeSize> m_savedEyeSize;
    std::string m_eyeSizePath;
    EyeSize m_recommendedEyeSize;
    EyeSize m_maximumEyeSize;
    RestartNotice m_restartNotice;
    EglContext m_egl;
    NetworkClient m_network;
    VideoDecoder m_decoder;
    VideoDecoder::Config m_decoderConfig;
    VideoRenderer m_renderer;
    StatusOverlay m_statusOverlay;
    XrInput m_input;
    std::optional<DecodedImage> m_currentImage;
    std::optional<DecodedImage> m_previousImage;
    bool m_haveLocatedViews = false;
    bool m_currentImageDrawn = false;
    bool m_videoBound = false;
    uint64_t m_videoFrameCount = 0;
    uint64_t m_staleVideoCount = 0;
    uint64_t m_feedbackFrames = 0;
    uint64_t m_feedbackQueueDepth = 0;
    uint64_t m_feedbackRepeated = 0;
    uint64_t m_feedbackDropped = 0;
    std::chrono::steady_clock::time_point m_feedbackTime{};
    uint64_t m_queueStatsFrames = 0;
    uint64_t m_queueStatsDepth = 0;
    uint64_t m_queueStatsRepeated = 0;
    uint64_t m_queueStatsDropped = 0;
    std::chrono::steady_clock::time_point m_lastVideoImageTime{};
    PFN_xrConvertTimeToTimespecTimeKHR m_convertTime = nullptr;
    PFN_xrConvertTimespecTimeToTimeKHR m_convertPoseTime = nullptr;
    std::atomic<bool> m_poseStopping{true};
    std::thread m_poseThread;
    // アクション同期・姿勢取得・セッション開始終了を直列化する。
    // ハンドルはスレッド開始前に作り、join 後まで変更・破棄しない。
    std::mutex m_trackingMutex;
    struct PredictionSample {
        uint64_t sampleTimeUs = 0;
        int64_t predictionUs = 0;
    };
    std::deque<PredictionSample> m_predictionHistory;
    int m_maxPredictionMs = kDefaultMaxPredictionMs;
    std::optional<int> m_fixedPredictionMs;
    int m_controllerMinusMs = 0;
    std::atomic<int64_t> m_predictionUs{0};
    std::optional<double> m_averageLatencyMs;
    uint64_t m_predictionFrames = 0;
    double m_totalPredictionMs = 0;
    double m_totalPredictionErrorMs = 0;
    double m_minPredictionErrorMs = 0;
    double m_maxPredictionErrorMs = 0;
    std::array<bool, 2> m_handActive{};
    protocol::PoseMessage m_latestSentPose;
    uint64_t m_sentPoseCount = 0;
    bool m_loggedTimeFallback = false;
    struct ClientStats {
        uint64_t displayedFrames = 0;
        uint64_t repeatedFrames = 0;
        uint64_t latencyFrames = 0;
        double latencyTotalMs = 0;
        double latencyMaxMs = 0;
        uint64_t displayWaitTotalUs = 0;
        uint64_t displayWaitFrames = 0;
        uint64_t imageQueueTotalUs = 0;
        uint64_t imageQueueFrames = 0;
        uint64_t drawToDisplayTotalUs = 0;
        uint64_t drawToDisplayFrames = 0;
        uint64_t displayPeriodTotalNs = 0;
        uint64_t displayPeriodFrames = 0;
    };
    ClientStats m_clientStats;
    // 1 秒ごとの送信で消える合計と件数を残し、5 秒のログもフレーム数で平均する。
    std::array<double, 8> m_intervalTotalUs{};
    std::array<uint64_t, 8> m_intervalFrames{};
    uint64_t m_statsGeneration = 0;
    std::chrono::steady_clock::time_point m_clientStatsTime{};
    uint64_t m_latencyFrames = 0;
    double m_totalLatencyMs = 0;
    double m_minLatencyMs = 0;
    double m_maxLatencyMs = 0;
    std::string m_deviceId;
    uint32_t m_sessionId = 0;
    bool m_startRequested = false;
    std::array<bool, 2> m_triggerPressed{};
    protocol::HelloMessage m_hello;
    PFN_xrGetDisplayRefreshRateFB m_getRefreshRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB m_requestRefreshRate = nullptr;
    bool m_supports72Hz = false;
    JavaVM* m_vm = nullptr;
    bool m_attachedThread = false;
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_baseSpace = XR_NULL_HANDLE;
    XrSpace m_viewSpace = XR_NULL_HANDLE;
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
    std::atomic<bool> m_sessionRunning{false};
    std::atomic<bool> m_exitRequested{false};
    std::vector<EyeSwapchain> m_eyes;
    std::vector<XrView> m_views;
    uint64_t m_frameCount = 0;
    std::chrono::steady_clock::time_point m_lastPoseLog{};
};

}
