#include "xr_app.h"

#include "config_file.h"
#include "device_identity.h"
#include "log.h"
#include "xr_math.h"

#include <android_native_app_glue.h>

#include <algorithm>
#include <arpa/inet.h>
#include <charconv>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <random>
#include <string_view>
#include <utility>

namespace wnvr::quest {
namespace {

// 全フィールドをゼロ初期化し、拡張構造体の next も確実に空にする。
template<typename T>
T XrInfo(XrStructureType type) {
    T value{};
    value.type = type;
    return value;
}

// 整数へ変換する前に上限を適用し、停止後の大きな期間やカウントも折り返さない。
template<typename T, typename U>
T Saturate(U value) {
    return static_cast<T>(std::clamp(static_cast<double>(value), 0.0,
        static_cast<double>(std::numeric_limits<T>::max())));
}

constexpr XrViewConfigurationType kViewConfiguration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

// トリガーを「引いた」「離した」とみなす深さ（0〜1）。あいだを空けて、境目での細かい反復を防ぐ。
constexpr float kTriggerPullLevel = 0.8f;
constexpr float kTriggerReleaseLevel = 0.2f;
constexpr auto kDisplayFeedbackInterval = std::chrono::milliseconds(500);

std::pair<std::string, std::string> StatusText(const NetworkClient::Status& status) {
    using State = NetworkClient::ConnectionState;
    const auto& name = status.hostName;
    switch (status.state) {
    case State::Searching:
        return {"PC を探しています", "同じネットワークの PC で\nWN-VRBridge のホストを起動してください"};
    case State::Connecting:
        return {"PC に接続しています", name.empty() ? status.hostAddress : name + "（" + status.hostAddress + "）"};
    case State::PendingApproval:
        return {"PC で接続を承認してください", name + " に確認の画面が出ています"};
    case State::Starting:
        return {"PC で SteamVR を起動しています", name + "\nしばらくお待ちください"};
    case State::Stopped:
        return {"SteamVR が起動していません", "トリガーを引くと、PC で SteamVR を\n起動します"};
    case State::Rejected:
        return {"PC に接続を拒否されました", name + " の設定画面で承認を確かめてください"};
    case State::Busy:
        return {"PC はほかの機器とつながっています", name};
    case State::VersionMismatch:
        return {"PC とバージョンが合いません", "PC: " + std::to_string(status.hostProtocolVersion)
            + " / Quest: " + std::to_string(protocol::kProtocolVersion) + "\n両方を同じバージョンにしてください"};
    case State::Connected:
        return {"接続しました", "映像を待っています"};
    }
    return {};
}

}

bool XrApp::CheckXr(XrResult result, const char* call) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char text[XR_MAX_RESULT_STRING_SIZE]{};
    if (m_instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(m_instance, result, text))) {
        std::snprintf(text, sizeof(text), "XrResult(%d)", static_cast<int>(result));
    }
    WNVR_LOGE("%s failed: %s (%d)", call, text, static_cast<int>(result));
    m_exitRequested = true;
    return false;
}

bool XrApp::Initialize(android_app* app) {
    Shutdown();
    m_startRequested = false;
    if (m_deviceId.empty()) {
        m_deviceId = LoadDeviceId(app->activity->internalDataPath);
    }
    m_savedEyeSize.reset();
    m_restartNotice.Reset();
    m_eyeSizePath.clear();
    if (app->activity->internalDataPath) {
        m_eyeSizePath = std::string(app->activity->internalDataPath) + "/eye_size.txt";
        std::ifstream file(m_eyeSizePath);
        if (file) {
            std::vector<std::string> warnings;
            m_savedEyeSize = ReadEyeSize(file, &warnings);
            for (const auto& warning : warnings) {
                WNVR_LOGW("%s", warning.c_str());
            }
        }
    }
    WNVR_LOGI("端末 ID: %.8s", m_deviceId.c_str());
    m_sessionId = MakeSessionId(m_sessionId);
    m_triggerPressed = {};
    WNVR_LOGI("セッション ID: %08" PRIx32, m_sessionId);
    m_app = app;
    m_vm = app->activity->vm;
    JNIEnv* environment = nullptr;
    // native_app_glue のスレッドは JVM に未接続なので、ローダーの JNI 利用前に接続する。
    const jint envResult = m_vm->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6);
    if (envResult == JNI_EDETACHED) {
        if (m_vm->AttachCurrentThread(&environment, nullptr) != JNI_OK) {
            WNVR_LOGE("AttachCurrentThread failed");
            return false;
        }
        m_attachedThread = true;
    } else if (envResult != JNI_OK) {
        WNVR_LOGE("GetEnv failed: %d", envResult);
        return false;
    }

    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (!CheckXr(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader)), "xrGetInstanceProcAddr(xrInitializeLoaderKHR)")) {
        return false;
    }
    auto loaderInfo = XrInfo<XrLoaderInitInfoAndroidKHR>(XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR);
    loaderInfo.applicationVM = m_vm;
    loaderInfo.applicationContext = app->activity->clazz;
    if (!CheckXr(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)), "xrInitializeLoaderKHR")) {
        return false;
    }

    uint32_t count = 0;
    if (!CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }
    std::vector<XrExtensionProperties> extensions(count, XrInfo<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES));
    if (!CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()), "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }
    const auto hasExtension = [&extensions](const char* name) {
        return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
    };
    std::vector<const char*> enabledExtensions;
    for (const char* name : {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME}) {
        if (!hasExtension(name)) {
            WNVR_LOGE("Required OpenXR extension unavailable: %s", name);
            return false;
        }
        enabledExtensions.push_back(name);
    }
    const bool hasRefreshRate = hasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (hasRefreshRate) {
        enabledExtensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    const bool hasConvertTime = hasExtension(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
    if (hasConvertTime) {
        enabledExtensions.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
    }
    auto androidInfo = XrInfo<XrInstanceCreateInfoAndroidKHR>(XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR);
    androidInfo.applicationVM = m_vm;
    androidInfo.applicationActivity = app->activity->clazz;
    auto instanceInfo = XrInfo<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
    instanceInfo.next = &androidInfo;
    std::snprintf(instanceInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "WN-VRBridge");
    instanceInfo.applicationInfo.applicationVersion = 1;
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    instanceInfo.enabledExtensionNames = enabledExtensions.data();
    if (!CheckXr(xrCreateInstance(&instanceInfo, &m_instance), "xrCreateInstance")) {
        return false;
    }
    if (hasConvertTime) {
        if (XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrConvertTimeToTimespecTimeKHR",
                reinterpret_cast<PFN_xrVoidFunction*>(&m_convertTime)))) {
            m_convertTime = nullptr;
        }
        if (XR_FAILED(xrGetInstanceProcAddr(m_instance, "xrConvertTimespecTimeToTimeKHR",
                reinterpret_cast<PFN_xrVoidFunction*>(&m_convertPoseTime)))) {
            m_convertPoseTime = nullptr;
        }
    }
    if (!m_convertTime) {
        WNVR_LOGW("Display latency uses draw-time steady_clock: XR_KHR_convert_timespec_time unavailable");
        m_loggedTimeFallback = true;
    }
    auto systemInfo = XrInfo<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!CheckXr(xrGetSystem(m_instance, &systemInfo, &m_systemId), "xrGetSystem")) {
        return false;
    }
    auto properties = XrInfo<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    if (!CheckXr(xrGetSystemProperties(m_instance, m_systemId, &properties), "xrGetSystemProperties")) {
        return false;
    }
    WNVR_LOGI("OpenXR system: %s", properties.systemName);
    m_hello.deviceName = std::string(properties.systemName).substr(0, protocol::kMaxDeviceNameLength);
    m_hello.ipdMeters = 0.063f;
    m_hello.refreshRate = 72.0f;

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    if (!CheckXr(xrGetInstanceProcAddr(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)), "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)")) {
        return false;
    }
    auto requirements = XrInfo<XrGraphicsRequirementsOpenGLESKHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR);
    if (!CheckXr(getRequirements(m_instance, m_systemId, &requirements), "xrGetOpenGLESGraphicsRequirementsKHR")
        || !m_egl.Create()) {
        return false;
    }
    if (!m_renderer.Initialize(m_egl.Display())) {
        WNVR_LOGW("Video renderer initialization failed; continuing with solid colors");
    }
    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const XrVersion version = XR_MAKE_VERSION(major, minor, 0);
    if (version < requirements.minApiVersionSupported || version > requirements.maxApiVersionSupported) {
        WNVR_LOGE("OpenGL ES %d.%d is outside OpenXR graphics requirements", major, minor);
        return false;
    }
    auto binding = XrInfo<XrGraphicsBindingOpenGLESAndroidKHR>(XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR);
    binding.display = m_egl.Display();
    binding.config = m_egl.Config();
    binding.context = m_egl.Context();
    auto sessionInfo = XrInfo<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    sessionInfo.next = &binding;
    sessionInfo.systemId = m_systemId;
    if (!CheckXr(xrCreateSession(m_instance, &sessionInfo, &m_session), "xrCreateSession")
        || !CreateSpaces()
        || !m_input.Create(m_instance, m_session, [this](XrResult result, const char* call) { return CheckXr(result, call); })
        || !CreateSwapchains()) {
        return false;
    }
    if (hasRefreshRate) {
        if (!LogRefreshRates()) {
            return false;
        }
    } else {
        WNVR_LOGW("XR_FB_display_refresh_rate unavailable");
    }
    if (!m_statusOverlay.Initialize(m_vm, m_session)) {
        WNVR_LOGW("Status overlay initialization failed; continuing without status text");
    }
    m_lastPoseLog = std::chrono::steady_clock::now();
    m_lastVideoImageTime = {};
    LoadPredictionConfig(app);
    LoadDecoderConfig(app);
    OpenNetwork(app);
    if (!m_decoder.Start([this] { return m_network.PopFrame(); },
        [this] { return m_network.ConnectionGeneration(); },
        [this](uint32_t frameIndex, protocol::KeyFrameReason reason) {
            m_network.SendKeyFrameRequest(frameIndex, reason);
        }, m_decoderConfig)) {
        return false;
    }
    if (m_convertPoseTime) {
        m_poseStopping.store(false);
        try {
            m_poseThread = std::thread([this] { SendPoses(); });
        } catch (const std::exception& error) {
            m_poseStopping.store(true);
            WNVR_LOGW("Pose thread failed: %s; sending poses from render loop", error.what());
        }
    } else {
        WNVR_LOGW("xrConvertTimespecTimeToTimeKHR unavailable; sending poses from render loop");
    }
    return true;
}

// アプリのフォルダ（adb で置ける場所）にある調整用のファイルを開く。無ければ開いていないストリームを返す。
std::ifstream XrApp::OpenConfigFile(android_app* app, const char* name) {
    const char* directory = app->activity->externalDataPath;
    if (directory == nullptr || directory[0] == '\0') {
        return {};
    }
    return std::ifstream(std::string(directory) + "/" + name);
}

// prediction.txt: 姿勢の予測の見比べ用（max_ms / fixed_ms / controller_minus_ms）。
void XrApp::LoadPredictionConfig(android_app* app) {
    m_maxPredictionMs = kDefaultMaxPredictionMs;
    m_fixedPredictionMs.reset();
    m_controllerMinusMs = 0;
    auto file = OpenConfigFile(app, "prediction.txt");
    if (!file.is_open()) {
        WNVR_LOGI("prediction.txt を読めないため、予測の既定値を使います");
    } else {
        const IntSetting settings[]{
            {"max_ms", 0, 200, [this](int value) { m_maxPredictionMs = value; }},
            {"fixed_ms", 0, 200, [this](int value) { m_fixedPredictionMs = value; }},
            {"controller_minus_ms", 0, 100, [this](int value) { m_controllerMinusMs = value; }},
        };
        for (const auto& warning : ReadIntSettings(file, "prediction.txt", settings)) {
            WNVR_LOGW("%s", warning.c_str());
        }
    }
    const std::string fixed = m_fixedPredictionMs ? std::to_string(*m_fixedPredictionMs) + " ms" : "なし";
    WNVR_LOGI("姿勢の予測: max=%d ms, fixed=%s, controllerMinus=%d ms",
        m_maxPredictionMs, fixed.c_str(), m_controllerMinusMs);
    // 固定値は実測を待たずに使い、未計測の自動予測だけを 0 にする。
    m_predictionUs.store(static_cast<int64_t>(std::min(m_fixedPredictionMs.value_or(0), m_maxPredictionMs)) * 1000);
}

// decoder.txt: デコーダの設定の見比べ用（qti_low_latency / operating_rate）。
void XrApp::LoadDecoderConfig(android_app* app) {
    m_decoderConfig = {};
    auto file = OpenConfigFile(app, "decoder.txt");
    if (!file.is_open()) {
        WNVR_LOGI("decoder.txt を読めないため、デコーダの既定値を使います");
    } else {
        const IntSetting settings[]{
            {"qti_low_latency", 0, 1, [this](int value) { m_decoderConfig.qtiLowLatency = value != 0; }},
            {"operating_rate", 0, 1000, [this](int value) { m_decoderConfig.operatingRate = value; }},
        };
        for (const auto& warning : ReadIntSettings(file, "decoder.txt", settings)) {
            WNVR_LOGW("%s", warning.c_str());
        }
    }
    WNVR_LOGI("デコーダの設定: qtiLowLatency=%d, operatingRate=%d",
        m_decoderConfig.qtiLowLatency, m_decoderConfig.operatingRate);
}

// host.txt があればそのアドレスにつなぎ、無ければ同じネットワークの PC を自動で探す。
void XrApp::OpenNetwork(android_app* app) {
    const char* directory = app->activity->externalDataPath;
    if (directory == nullptr || directory[0] == '\0') {
        WNVR_LOGI("externalDataPath unavailable; 自動発見を使います");
        m_network.OpenDiscovery();
        return;
    }
    const std::string path = std::string(directory) + "/host.txt";
    errno = 0;
    std::ifstream file(path);
    const int openError = errno;
    if (!file.is_open()) {
        if (openError == ENOENT) {
            WNVR_LOGI("host.txt が無いため、自動発見を使います");
            m_network.OpenDiscovery();
        } else {
            WNVR_LOGW("host.txt unreadable: %s", path.c_str());
        }
        return;
    }
    std::string line;
    std::getline(file, line);
    const auto address = file.bad() ? std::nullopt : ParseHostLine(line, protocol::kDefaultPort);
    in_addr parsed{};
    if (!address || inet_pton(AF_INET, address->host.c_str(), &parsed) != 1) {
        WNVR_LOGW("Invalid host.txt (expected IPv4[:port]): %s", path.c_str());
        return;
    }
    m_network.Open(address->host, address->port);
}

protocol::HelloMessage XrApp::MakeHello() const {
    auto hello = m_hello;
    hello.deviceId = m_deviceId;
    hello.sessionId = m_sessionId;
    hello.startRequested = m_startRequested ? 1 : 0;
    if (m_getRefreshRate != nullptr) {
        float rate = 0;
        if (XR_SUCCEEDED(m_getRefreshRate(m_session, &rate))) {
            hello.refreshRate = rate;
        }
    }
    return hello;
}

protocol::DevicePose XrApp::LocatePose(XrSpace space, XrTime time) {
    protocol::DevicePose result;
    auto velocity = XrInfo<XrSpaceVelocity>(XR_TYPE_SPACE_VELOCITY);
    auto location = XrInfo<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    location.next = &velocity;
    if (!CheckXr(xrLocateSpace(space, m_baseSpace, time, &location), "xrLocateSpace")) {
        return result;
    }
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    result.valid = (location.locationFlags & kValid) == kValid;
    const auto& p = location.pose.position;
    const auto& q = location.pose.orientation;
    result.pose.position = {p.x, p.y, p.z};
    result.pose.orientation = {q.x, q.y, q.z, q.w};
    if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) {
        const auto& v = velocity.linearVelocity;
        result.pose.linearVelocity = {v.x, v.y, v.z};
    }
    if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0) {
        const auto& v = velocity.angularVelocity;
        result.pose.angularVelocity = {v.x, v.y, v.z};
    }
    return result;
}

protocol::PoseMessage XrApp::LocatePoses(XrTime headTime, XrTime handTime) {
    protocol::PoseMessage pose;
    pose.head = LocatePose(m_viewSpace, headTime);
    std::array<protocol::DevicePose*, 2> hands{&pose.leftHand, &pose.rightHand};
    for (size_t index = 0; index < hands.size(); ++index) {
        if (m_handActive[index]) {
            *hands[index] = LocatePose(m_input.HandSpace(index), handTime);
        }
    }
    return pose;
}

protocol::InputMessage XrApp::SyncActions() {
    // フォーカスが無いあいだ（Quest のメニューを開いているときなど）は、入力も手の姿勢も送らない。
    std::lock_guard lock(m_trackingMutex);
    return m_input.Sync(m_sessionState == XR_SESSION_STATE_FOCUSED, m_handActive);
}

void XrApp::SendPose(const protocol::PoseMessage& pose) {
    // 呼び出し側が m_trackingMutex を保持し、ログ用の姿勢と件数も同時に更新する。
    if (m_network.SendPose(pose)) {
        m_latestSentPose = pose;
        ++m_sentPoseCount;
    }
}

void XrApp::SendPoses() {
    constexpr auto kPoseInterval = std::chrono::milliseconds(2);
    auto nextSend = std::chrono::steady_clock::now();
    while (!m_poseStopping.load()) {
        bool active = false;
        {
            std::lock_guard lock(m_trackingMutex);
            active = IsSessionRunning() && !m_exitRequested.load() && m_network.IsConnected();
            if (active) {
                timespec monotonic{};
                XrTime time = 0;
                if (clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0) {
                    WNVR_LOGE("Pose clock_gettime failed: %s", std::strerror(errno));
                    m_exitRequested.store(true);
                } else if (CheckXr(m_convertPoseTime(m_instance, &monotonic, &time),
                        "xrConvertTimespecTimeToTimeKHR")) {
                    const int64_t predictionUs = m_predictionUs.load();
                    const int64_t handPredictionUs = std::max(int64_t{0},
                        predictionUs - static_cast<int64_t>(m_controllerMinusMs) * 1000);
                    auto pose = LocatePoses(time + predictionUs * 1000, time + handPredictionUs * 1000);
                    // 遅延は採取から表示までを測るため、未来の照会時刻を記録しない。
                    pose.sampleTimeUs = static_cast<uint64_t>(monotonic.tv_sec) * 1000000
                        + static_cast<uint64_t>(monotonic.tv_nsec) / 1000;
                    if (!m_exitRequested.load()) {
                        SendPose(pose);
                        m_predictionHistory.push_back({pose.sampleTimeUs, predictionUs});
                        constexpr size_t kMaxPredictionHistory = 1024;
                        if (m_predictionHistory.size() > kMaxPredictionHistory) {
                            m_predictionHistory.pop_front();
                        }
                    }
                }
            }
        }
        SendMicAudio();
        const auto now = std::chrono::steady_clock::now();
        if (!active) {
            nextSend = now + std::chrono::milliseconds(10);
        } else {
            nextSend += kPoseInterval;
            // 遅れた分を連続送信すると古い姿勢が詰まるので、次の周期から再開する。
            if (nextSend <= now) {
                nextSend = now + kPoseInterval;
            }
        }
        std::this_thread::sleep_until(nextSend);
    }
}

bool XrApp::CreateSpaces() {
    uint32_t count = 0;
    if (!CheckXr(xrEnumerateReferenceSpaces(m_session, 0, &count, nullptr), "xrEnumerateReferenceSpaces")) {
        return false;
    }
    std::vector<XrReferenceSpaceType> types(count);
    if (!CheckXr(xrEnumerateReferenceSpaces(m_session, count, &count, types.data()), "xrEnumerateReferenceSpaces")) {
        return false;
    }
    const bool hasStage = std::find(types.begin(), types.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != types.end();
    auto info = XrInfo<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    info.referenceSpaceType = hasStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace.orientation.w = 1.0f;
    if (!CheckXr(xrCreateReferenceSpace(m_session, &info, &m_baseSpace), "xrCreateReferenceSpace(base)")) {
        return false;
    }
    WNVR_LOGI("Reference space: %s", hasStage ? "STAGE" : "LOCAL");
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    return CheckXr(xrCreateReferenceSpace(m_session, &info, &m_viewSpace), "xrCreateReferenceSpace(VIEW)");
}

bool XrApp::CreateSwapchains() {
    uint32_t count = 0;
    if (!CheckXr(xrEnumerateViewConfigurationViews(m_instance, m_systemId, kViewConfiguration, 0, &count, nullptr), "xrEnumerateViewConfigurationViews")) {
        return false;
    }
    if (count != 2) {
        WNVR_LOGE("PRIMARY_STEREO must provide two views, got %u", count);
        return false;
    }
    std::vector<XrViewConfigurationView> configs(count, XrInfo<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW));
    if (!CheckXr(xrEnumerateViewConfigurationViews(m_instance, m_systemId, kViewConfiguration, count, &count, configs.data()), "xrEnumerateViewConfigurationViews")) {
        return false;
    }
    if (!CheckXr(xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr), "xrEnumerateSwapchainFormats")) {
        return false;
    }
    if (count == 0) {
        WNVR_LOGE("No swapchain formats available");
        return false;
    }
    std::vector<int64_t> formats(count);
    if (!CheckXr(xrEnumerateSwapchainFormats(m_session, count, &count, formats.data()), "xrEnumerateSwapchainFormats")) {
        return false;
    }
    const int64_t format = std::find(formats.begin(), formats.end(), GL_SRGB8_ALPHA8) != formats.end()
        ? GL_SRGB8_ALPHA8 : formats.front();
    m_eyes.resize(configs.size());
    m_views.assign(configs.size(), XrInfo<XrView>(XR_TYPE_VIEW));

    for (size_t eyeIndex = 0; eyeIndex < m_eyes.size(); ++eyeIndex) {
        auto& eye = m_eyes[eyeIndex];
        const auto& config = configs[eyeIndex];
        const auto size = ChooseEyeSize({config.recommendedImageRectWidth, config.recommendedImageRectHeight},
            {config.maxImageRectWidth, config.maxImageRectHeight}, m_savedEyeSize);
        eye.width = static_cast<int32_t>(size.width);
        eye.height = static_cast<int32_t>(size.height);
        if (eyeIndex == 0) {
            m_recommendedEyeSize = {config.recommendedImageRectWidth, config.recommendedImageRectHeight};
            m_maximumEyeSize = {config.maxImageRectWidth, config.maxImageRectHeight};
            m_hello.eyeWidth = size.width;
            m_hello.eyeHeight = size.height;
        }
        auto info = XrInfo<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = format;
        // 動画の全画面描画に MSAA は不要。画像を GL_TEXTURE_2D として FBO に接続する。
        info.sampleCount = 1;
        info.width = size.width;
        info.height = size.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!CheckXr(xrCreateSwapchain(m_session, &info, &eye.handle), "xrCreateSwapchain")
            || !CheckXr(xrEnumerateSwapchainImages(eye.handle, 0, &count, nullptr), "xrEnumerateSwapchainImages")) {
            return false;
        }
        if (count == 0) {
            WNVR_LOGE("Swapchain has no images");
            return false;
        }
        eye.images.assign(count, XrInfo<XrSwapchainImageOpenGLESKHR>(XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR));
        if (!CheckXr(xrEnumerateSwapchainImages(eye.handle, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())), "xrEnumerateSwapchainImages")) {
            return false;
        }
        eye.framebuffers.resize(count);
        glGenFramebuffers(static_cast<GLsizei>(count), eye.framebuffers.data());
        for (uint32_t index = 0; index < count; ++index) {
            glBindFramebuffer(GL_FRAMEBUFFER, eye.framebuffers[index]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye.images[index].image, 0);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            const GLenum error = glGetError();
            if (status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR) {
                WNVR_LOGE("Framebuffer creation failed: status=0x%x GL error=0x%x", status, error);
                return false;
            }
        }
        WNVR_LOGI("Eye %zu: recommended=%ux%u max=%ux%u using=%ux%u format=0x%" PRIx64 " images=%u",
            eyeIndex, config.recommendedImageRectWidth, config.recommendedImageRectHeight,
            config.maxImageRectWidth, config.maxImageRectHeight, size.width, size.height,
            static_cast<uint64_t>(format), count);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool XrApp::LogRefreshRates() {
    PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
    if (!CheckXr(xrGetInstanceProcAddr(m_instance, "xrEnumerateDisplayRefreshRatesFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&enumerateRates)), "xrGetInstanceProcAddr(xrEnumerateDisplayRefreshRatesFB)")
        || !CheckXr(xrGetInstanceProcAddr(m_instance, "xrGetDisplayRefreshRateFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&getRate)), "xrGetInstanceProcAddr(xrGetDisplayRefreshRateFB)")
        || !CheckXr(xrGetInstanceProcAddr(m_instance, "xrRequestDisplayRefreshRateFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&m_requestRefreshRate)), "xrGetInstanceProcAddr(xrRequestDisplayRefreshRateFB)")) {
        return false;
    }
    uint32_t count = 0;
    if (!CheckXr(enumerateRates(m_session, 0, &count, nullptr), "xrEnumerateDisplayRefreshRatesFB")) {
        return false;
    }
    std::vector<float> rates(count);
    if (!CheckXr(enumerateRates(m_session, count, &count, rates.data()), "xrEnumerateDisplayRefreshRatesFB")) {
        return false;
    }
    for (float rate : rates) {
        WNVR_LOGI("Available refresh rate: %.2f Hz", rate);
    }
    m_supports72Hz = std::find(rates.begin(), rates.end(), 72.0f) != rates.end();
    float current = 0.0f;
    if (!CheckXr(getRate(m_session, &current), "xrGetDisplayRefreshRateFB")) {
        return false;
    }
    WNVR_LOGI("Current refresh rate: %.2f Hz", current);
    m_getRefreshRate = getRate;
    m_hello.refreshRate = current;
    return true;
}

void XrApp::RequestRefreshRate() {
    if (!m_supports72Hz || !m_requestRefreshRate || !m_getRefreshRate) {
        return;
    }
    // セッション開始前の要求は受け付けられないため、xrBeginSession 後に行う。
    const XrResult requested = m_requestRefreshRate(m_session, 72.0f);
    WNVR_LOGI("xrRequestDisplayRefreshRateFB(72 Hz): result=%d", static_cast<int>(requested));
    float current = 0.0f;
    const XrResult queried = m_getRefreshRate(m_session, &current);
    WNVR_LOGI("Refresh rate after request: result=%d, %.2f Hz", static_cast<int>(queried), current);
    if (XR_SUCCEEDED(queried)) {
        m_hello.refreshRate = current;
    }
}

void XrApp::PollEvents(bool& exitRequested) {
    while (!m_exitRequested) {
        auto event = XrInfo<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
        const XrResult result = xrPollEvent(m_instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) {
            break;
        }
        if (!CheckXr(result, "xrPollEvent")) {
            break;
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& change = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            WNVR_LOGI("OpenXR session state: %d", static_cast<int>(change.state));
            if (change.session != m_session) {
                continue;
            }
            std::lock_guard lock(m_trackingMutex);
            m_sessionState = change.state;
            if (change.state != XR_SESSION_STATE_FOCUSED) {
                m_handActive.fill(false);
            }
            if (change.state == XR_SESSION_STATE_READY && !m_sessionRunning) {
                auto info = XrInfo<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
                info.primaryViewConfigurationType = kViewConfiguration;
                m_sessionRunning = CheckXr(xrBeginSession(m_session, &info), "xrBeginSession");
                if (m_sessionRunning) {
                    RequestRefreshRate();
                }
            } else if (change.state == XR_SESSION_STATE_STOPPING && m_sessionRunning) {
                CheckXr(xrEndSession(m_session), "xrEndSession");
                m_sessionRunning = false;
                m_micCapture.Stop();
            } else if (change.state == XR_SESSION_STATE_EXITING || change.state == XR_SESSION_STATE_LOSS_PENDING) {
                m_exitRequested = true;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            m_exitRequested = true;
        }
    }
    exitRequested = exitRequested || m_exitRequested;
}

bool XrApp::RenderEye(size_t eyeIndex, XrCompositionLayerProjectionView& projectionView,
    const protocol::DevicePose& head, const std::optional<NetworkClient::RenderDisplay>& renderDisplay) {
    auto& eye = m_eyes[eyeIndex];
    uint32_t imageIndex = 0;
    auto acquireInfo = XrInfo<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    if (!CheckXr(xrAcquireSwapchainImage(eye.handle, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage")) {
        return false;
    }
    auto waitInfo = XrInfo<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    waitInfo.timeout = XR_INFINITE_DURATION;
    const XrResult waitResult = xrWaitSwapchainImage(eye.handle, &waitInfo);
    if (!CheckXr(waitResult, "xrWaitSwapchainImage") || waitResult == XR_TIMEOUT_EXPIRED) {
        // 待機が成立していない画像は release できないため、セッションごと破棄する。
        m_exitRequested = true;
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, eye.framebuffers[imageIndex]);
    glViewport(0, 0, eye.width, eye.height);
    glDisable(GL_SCISSOR_TEST);
    if (m_videoBound) {
        m_renderer.SetFoveation(m_network.GetFoveation());
        m_renderer.DrawEye(static_cast<int>(eyeIndex));
    } else {
        glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // ランタイムが共有画像を読む前に、このコンテキストのコマンドを送る。
    glFlush();
    const GLenum error = glGetError();
    auto releaseInfo = XrInfo<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    const bool released = CheckXr(xrReleaseSwapchainImage(eye.handle, &releaseInfo), "xrReleaseSwapchainImage");
    if (error != GL_NO_ERROR) {
        WNVR_LOGE("Eye %zu rendering failed: GL error 0x%x", eyeIndex, error);
        m_exitRequested = true;
        return false;
    }
    projectionView.pose = m_views[eyeIndex].pose;
    if (m_videoBound && m_currentImage->hasPose && m_currentImage->pose.sampleTimeUs != 0
        && (renderDisplay || head.valid)) {
        const auto& framePose = m_currentImage->pose;
        XrPosef headToEye{{0, 0, 0, 1}, {0, 0, 0}};
        if (renderDisplay) {
            // PC の描画時と同じ、頭の座標での左右の目の位置を再現する。
            headToEye.position.x = (eyeIndex == 0 ? -0.5f : 0.5f) * renderDisplay->ipdMeters;
        } else {
            // VIEW と目は同じ予測表示時刻・同じ基準空間なので、頭から見た目だけを取り出せる。
            headToEye = Compose(Inverse(ToXrPose(head.pose.position, head.pose.orientation)),
                m_views[eyeIndex].pose);
        }
        projectionView.pose = Compose(ToXrPose(framePose.position, framePose.orientation), headToEye);
    }
    projectionView.fov = m_views[eyeIndex].fov;
    if (m_videoBound && renderDisplay) {
        const auto& fov = eyeIndex == 0 ? renderDisplay->leftFov : renderDisplay->rightFov;
        projectionView.fov = {std::atan(fov.left), std::atan(fov.right),
            std::atan(-fov.top), std::atan(-fov.bottom)};
    }
    projectionView.subImage.swapchain = eye.handle;
    projectionView.subImage.imageRect.extent = {eye.width, eye.height};
    return released;
}

void XrApp::MeasureLatency(XrTime predictedDisplayTime, XrDuration predictedDisplayPeriod) {
    if (!m_currentImage || !m_currentImage->hasPose || m_currentImage->pose.sampleTimeUs == 0) {
        return;
    }
    int64_t displayUs = 0;
    timespec monotonic{};
    if (m_convertTime && XR_SUCCEEDED(m_convertTime(m_instance, predictedDisplayTime, &monotonic))) {
        // Android の steady_clock は CLOCK_MONOTONIC と同じ時計なので、そのまま姿勢の採取時刻と比較できる。
        displayUs = static_cast<int64_t>(monotonic.tv_sec) * 1000000 + monotonic.tv_nsec / 1000;
    } else {
        if (!m_loggedTimeFallback) {
            WNVR_LOGW("Display time conversion failed; latency uses draw-time steady_clock");
            m_loggedTimeFallback = true;
        }
        displayUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    if (displayUs < 0 || static_cast<uint64_t>(displayUs) < m_currentImage->pose.sampleTimeUs) {
        return;
    }
    const auto decodedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        m_currentImage->decodedTime.time_since_epoch()).count();
    if (m_currentImage->decodedTime != std::chrono::steady_clock::time_point{} && displayUs >= decodedUs) {
        m_clientStats.displayWaitTotalUs += static_cast<uint64_t>(displayUs - decodedUs);
        ++m_clientStats.displayWaitFrames;
        if (m_currentImage->takenTime != std::chrono::steady_clock::time_point{}) {
            const auto takenUs = std::chrono::duration_cast<std::chrono::microseconds>(
                m_currentImage->takenTime.time_since_epoch()).count();
            if (takenUs >= decodedUs) {
                m_clientStats.imageQueueTotalUs += static_cast<uint64_t>(takenUs - decodedUs);
                ++m_clientStats.imageQueueFrames;
            }
            if (displayUs >= takenUs) {
                m_clientStats.drawToDisplayTotalUs += static_cast<uint64_t>(displayUs - takenUs);
                ++m_clientStats.drawToDisplayFrames;
            }
        }
        if (predictedDisplayPeriod > 0) {
            m_clientStats.displayPeriodTotalNs += static_cast<uint64_t>(predictedDisplayPeriod);
            ++m_clientStats.displayPeriodFrames;
        }
    }
    const double ms = static_cast<double>(static_cast<uint64_t>(displayUs) - m_currentImage->pose.sampleTimeUs) / 1000.0;
    m_totalLatencyMs += ms;
    ++m_clientStats.latencyFrames;
    m_clientStats.latencyTotalMs += ms;
    m_clientStats.latencyMaxMs = std::max(m_clientStats.latencyMaxMs, ms);
    m_minLatencyMs = m_latencyFrames == 0 ? ms : std::min(m_minLatencyMs, ms);
    m_maxLatencyMs = std::max(m_maxLatencyMs, ms);
    ++m_latencyFrames;
    const double boundedMs = std::clamp(ms, 0.0, 200.0);
    if (m_averageLatencyMs) {
        *m_averageLatencyMs += 0.05 * (boundedMs - *m_averageLatencyMs);
    } else {
        m_averageLatencyMs = boundedMs;
    }
    const double predictionMs = m_fixedPredictionMs ? static_cast<double>(*m_fixedPredictionMs) : *m_averageLatencyMs;
    m_predictionUs.store(static_cast<int64_t>(std::min(predictionMs, static_cast<double>(m_maxPredictionMs)) * 1000.0));
    std::lock_guard lock(m_trackingMutex);
    const auto sample = std::find_if(m_predictionHistory.begin(), m_predictionHistory.end(),
        [this](const PredictionSample& entry) {
            return entry.sampleTimeUs == m_currentImage->pose.sampleTimeUs;
        });
    if (sample != m_predictionHistory.end()) {
        const double usedPredictionMs = static_cast<double>(sample->predictionUs) / 1000.0;
        const double errorMs = ms - usedPredictionMs;
        m_totalPredictionMs += usedPredictionMs;
        m_totalPredictionErrorMs += errorMs;
        m_minPredictionErrorMs = m_predictionFrames == 0 ? errorMs : std::min(m_minPredictionErrorMs, errorMs);
        m_maxPredictionErrorMs = m_predictionFrames == 0 ? errorMs : std::max(m_maxPredictionErrorMs, errorMs);
        ++m_predictionFrames;
    }
}

bool XrApp::MicPermission(bool request) {
    // JNI は JVM に接続済みの描画スレッドからだけ呼び、ローカル参照をフレームごとに返す。
    JNIEnv* environment = nullptr;
    if (m_vm == nullptr || m_app == nullptr
        || m_vm->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) != JNI_OK) {
        return false;
    }
    const auto failed = [environment] {
        if (environment->ExceptionCheck()) {
            environment->ExceptionClear();
            return true;
        }
        return false;
    };
    constexpr jint kLocalReferenceCapacity = 8;
    const jint pushed = environment->PushLocalFrame(kLocalReferenceCapacity);
    if (failed() || pushed != JNI_OK) {
        return false;
    }
    struct LocalScope {
        JNIEnv* environment;
        ~LocalScope() {
            environment->PopLocalFrame(nullptr);
            if (environment->ExceptionCheck()) {
                environment->ExceptionClear();
            }
        }
    } scope{environment};
    const auto activity = m_app->activity->clazz;
    const auto type = environment->GetObjectClass(activity);
    if (failed() || type == nullptr) { return false; }
    const auto permission = environment->NewStringUTF("android.permission.RECORD_AUDIO");
    if (failed() || permission == nullptr) { return false; }
    const auto check = environment->GetMethodID(type, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (failed() || check == nullptr) { return false; }
    const jint granted = environment->CallIntMethod(activity, check, permission);
    if (failed()) { return false; }
    constexpr jint kPermissionGranted = 0;
    if (granted == kPermissionGranted) {
        return true;
    }
    if (!request) { return false; }
    m_micPermissionRequested = true;
    const auto requestMethod = environment->GetMethodID(type, "requestPermissions", "([Ljava/lang/String;I)V");
    if (failed() || requestMethod == nullptr) { return false; }
    const auto stringType = environment->FindClass("java/lang/String");
    if (failed() || stringType == nullptr) { return false; }
    const auto permissions = environment->NewObjectArray(1, stringType, permission);
    if (failed() || permissions == nullptr) { return false; }
    constexpr jint kMicPermissionRequest = 1;
    environment->CallVoidMethod(activity, requestMethod, permissions, kMicPermissionRequest);
    failed();
    return false;
}

void XrApp::SetResumed(bool resumed) {
    m_resumed = resumed;
    if (!resumed) {
        m_micCapture.Stop();
        m_micState = protocol::QuestMicState::NotRequested;
    }
}

void XrApp::UpdateMic() {
    constexpr auto kPermissionCheckInterval = std::chrono::seconds(1);
    constexpr auto kRetryInterval = std::chrono::seconds(5);
    const auto now = std::chrono::steady_clock::now();
    if (!m_resumed || !m_network.MicRequested()) {
        m_micCapture.Stop();
        m_micState = protocol::QuestMicState::NotRequested;
        m_nextMicRetry = {};
        return;
    }
    if (now >= m_nextMicPermissionCheck) {
        const bool request = !m_micPermissionRequested;
        m_micPermissionGranted = MicPermission(request);
        m_nextMicPermissionCheck = now + kPermissionCheckInterval;
    }
    if (!m_micPermissionGranted) {
        m_micCapture.Stop();
        m_micState = protocol::QuestMicState::PermissionNeeded;
        return;
    }
    if (m_micCapture.HasFailed() && !m_micCapture.IsRunning()
        && m_micState == protocol::QuestMicState::Capturing) {
        m_micCapture.Stop();
        m_nextMicRetry = now + kRetryInterval;
    }
    if (!m_micCapture.IsRunning() && now >= m_nextMicRetry) {
        if (!m_micCapture.Start()) {
            m_nextMicRetry = now + kRetryInterval;
        }
    }
    if (m_micCapture.HasFailed()) {
        m_micCapture.Stop();
        if (m_nextMicRetry <= now) {
            m_nextMicRetry = now + kRetryInterval;
        }
        m_micState = protocol::QuestMicState::Failed;
    } else {
        m_micState = protocol::QuestMicState::Capturing;
    }
}

void XrApp::SendMicAudio() {
    for (const auto& mic : m_micCapture.Take()) {
        m_network.SendMicAudio(mic);
    }
}

void XrApp::ReportClientStats() {
    const auto now = std::chrono::steady_clock::now();
    const auto generation = m_network.ConnectionGeneration();
    const bool connected = m_network.IsConnected();
    bool eyeSizePending = false;
    if (connected) {
        const auto requested = m_network.RequestedEyeSize();
        if (requested && !m_eyes.empty()) {
            const auto size = ChooseEyeSize(m_recommendedEyeSize, m_maximumEyeSize, requested);
            eyeSizePending = size != EyeSize{static_cast<uint32_t>(m_eyes[0].width),
                static_cast<uint32_t>(m_eyes[0].height)};
        }
        if (requested && requested != m_savedEyeSize) {
            std::ofstream file(m_eyeSizePath, std::ios::binary | std::ios::trunc);
            file << FormatEyeSize(*requested);
            file.close();
            if (file) {
                m_savedEyeSize = requested;
                WNVR_LOGI("描画先の大きさ %ux%u を保存しました。次にアプリを起動したときから使います",
                    requested->width, requested->height);
            } else {
                WNVR_LOGW("eye_size.txt を保存できませんでした");
            }
        }
    }
    const bool foveationPending = connected && m_network.FoveationPending();
    m_restartNotice.Update(eyeSizePending, foveationPending, now);
    if (m_restartNotice.JustStarted()) {
        WNVR_LOGI("設定の変更を知らせます: 描画の解像度=%d foveation=%d",
            eyeSizePending ? 1 : 0, foveationPending ? 1 : 0);
    }
    if (!connected || generation != m_statsGeneration
        || m_clientStatsTime == std::chrono::steady_clock::time_point{}) {
        m_network.TakeClientStats();
        m_decoder.TakeClientStats();
        m_clientStats = {};
        m_intervalTotalUs = {};
        m_intervalFrames = {};
        m_statsGeneration = generation;
        m_clientStatsTime = now;
        return;
    }
    const auto periodUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_clientStatsTime).count();
    if (periodUs < 1000000) {
        return;
    }
    const auto network = m_network.TakeClientStats();
    const auto decode = m_decoder.TakeClientStats();
    protocol::ClientStatsMessage stats;
    stats.periodUs = Saturate<uint32_t>(periodUs);
    stats.micState = static_cast<uint8_t>(m_micState);
    stats.receivedFrames = Saturate<uint16_t>(network.video.receivedFrames);
    stats.lostFrames = Saturate<uint16_t>(network.video.lostFrames);
    stats.receivedFragments = Saturate<uint32_t>(network.video.receivedFragments);
    stats.receivedVideoBytes = Saturate<uint32_t>(network.video.receivedVideoBytes);
    stats.decodedFrames = Saturate<uint16_t>(decode.decodedFrames);
    stats.decodeAverageUs = Saturate<uint32_t>(decode.timedFrames
        ? decode.totalMs * 1000.0 / static_cast<double>(decode.timedFrames) : 0.0);
    stats.decodeMaxUs = Saturate<uint32_t>(decode.maxMs * 1000.0);
    stats.displayedFrames = Saturate<uint16_t>(m_clientStats.displayedFrames);
    stats.repeatedFrames = Saturate<uint16_t>(m_clientStats.repeatedFrames);
    stats.latencyAverageUs = Saturate<uint32_t>(m_clientStats.latencyFrames
        ? m_clientStats.latencyTotalMs * 1000.0 / static_cast<double>(m_clientStats.latencyFrames) : 0.0);
    stats.latencyMaxUs = Saturate<uint32_t>(m_clientStats.latencyMaxMs * 1000.0);
    stats.roundTripUs = Saturate<uint32_t>(network.roundTripMs * 1000.0);
    stats.audioUnderruns = Saturate<uint16_t>(network.audio.underruns);
    stats.audioMissingFrames = Saturate<uint32_t>(network.audio.missingFrames);
    stats.audioBufferAverageUs = Saturate<uint32_t>(network.audio.callbacks
        ? network.audio.bufferTotalMs * 1000.0 / static_cast<double>(network.audio.callbacks) : 0.0);
    if (network.generation == m_statsGeneration) {
        stats.firstFragmentAverageUs = Saturate<uint32_t>(network.video.firstFragmentFrames
            ? network.video.firstFragmentTotalUs / network.video.firstFragmentFrames : 0);
        stats.assembleAverageUs = Saturate<uint32_t>(network.video.assembleFrames
            ? network.video.assembleTotalUs / network.video.assembleFrames : 0);
        stats.decoderWaitAverageUs = Saturate<uint32_t>(decode.decoderWaitFrames
            ? decode.decoderWaitTotalUs / decode.decoderWaitFrames : 0);
        stats.displayWaitAverageUs = Saturate<uint32_t>(m_clientStats.displayWaitFrames
            ? m_clientStats.displayWaitTotalUs / m_clientStats.displayWaitFrames : 0);
        const std::array<double, 8> totals{static_cast<double>(network.video.firstFragmentTotalUs),
            static_cast<double>(network.video.assembleTotalUs), static_cast<double>(decode.decoderWaitTotalUs),
            decode.totalMs * 1000.0, static_cast<double>(m_clientStats.displayWaitTotalUs),
            static_cast<double>(m_clientStats.imageQueueTotalUs),
            static_cast<double>(m_clientStats.drawToDisplayTotalUs),
            static_cast<double>(m_clientStats.displayPeriodTotalNs) / 1000.0};
        const std::array<uint64_t, 8> counts{network.video.firstFragmentFrames, network.video.assembleFrames,
            decode.decoderWaitFrames, decode.timedFrames, m_clientStats.displayWaitFrames,
            m_clientStats.imageQueueFrames, m_clientStats.drawToDisplayFrames, m_clientStats.displayPeriodFrames};
        for (size_t i = 0; i < totals.size(); ++i) {
            m_intervalTotalUs[i] += totals[i];
            m_intervalFrames[i] += counts[i];
        }
        m_network.SendClientStats(stats, m_statsGeneration);
    }
    // UDP で送れない期間も捨て、次回の期間とカウンターの対応を保つ。
    m_clientStats = {};
    m_clientStatsTime = now;
}

void XrApp::LogHeadPose() {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastPoseLog < std::chrono::seconds(5)) {
        return;
    }
    protocol::PoseMessage pose;
    uint64_t sentPoses = 0;
    {
        std::lock_guard lock(m_trackingMutex);
        pose = m_latestSentPose;
        sentPoses = m_sentPoseCount;
        m_sentPoseCount = 0;
    }
    WNVR_LOGI("姿勢から表示: 平均=%.2f / 最小=%.2f / 最大=%.2f ms, frames=%" PRIu64
        ", videoMissing1s=%d, lastImageAge=%.2f s",
        m_latencyFrames ? m_totalLatencyMs / static_cast<double>(m_latencyFrames) : 0.0,
        m_minLatencyMs, m_maxLatencyMs, m_latencyFrames,
        now - m_lastVideoImageTime >= std::chrono::seconds(1),
        std::chrono::duration<double>(now - m_lastVideoImageTime).count());
    WNVR_LOGI("姿勢の予測: 平均=%.2f ms, 誤差: 平均=%.2f / 最小=%.2f / 最大=%.2f ms, frames=%" PRIu64,
        m_predictionFrames ? m_totalPredictionMs / static_cast<double>(m_predictionFrames) : 0.0,
        m_predictionFrames ? m_totalPredictionErrorMs / static_cast<double>(m_predictionFrames) : 0.0,
        m_minPredictionErrorMs, m_maxPredictionErrorMs, m_predictionFrames);
    std::array<double, 8> intervals{};
    for (size_t i = 0; i < intervals.size(); ++i) {
        intervals[i] = m_intervalFrames[i]
            ? m_intervalTotalUs[i] / static_cast<double>(m_intervalFrames[i]) / 1000.0 : 0;
    }
    WNVR_LOGI("遅延の内訳: 姿勢→最初の断片=%.1f, 組み立て=%.1f, デコード待ち=%.1f, デコード=%.1f, 表示待ち=%.1f ms",
        intervals[0], intervals[1], intervals[2], intervals[3], intervals[4]);
    WNVR_LOGI("表示待ちの内訳: 列=%.1f, 描画→表示=%.1f ms（表示の周期=%.2f ms）",
        intervals[5], intervals[6], intervals[7]);
    m_intervalTotalUs = {};
    m_intervalFrames = {};
    m_predictionFrames = 0;
    m_totalPredictionMs = 0;
    m_totalPredictionErrorMs = 0;
    m_minPredictionErrorMs = 0;
    m_maxPredictionErrorMs = 0;
    m_latencyFrames = 0;
    m_totalLatencyMs = m_minLatencyMs = m_maxLatencyMs = 0;
    m_lastPoseLog = now;
    WNVR_LOGI("表示キュー: average=%.3f, repeated=%" PRIu64 ", dropped=%" PRIu64,
        m_queueStatsFrames ? static_cast<double>(m_queueStatsDepth) / static_cast<double>(m_queueStatsFrames) : 0.0,
        m_queueStatsRepeated, m_queueStatsDropped);
    m_queueStatsFrames = m_queueStatsDepth = m_queueStatsRepeated = m_queueStatsDropped = 0;
    const auto& p = pose.head.pose.position;
    const auto& q = pose.head.pose.orientation;
    WNVR_LOGI("Head: position=(%.3f, %.3f, %.3f) orientation=(%.3f, %.3f, %.3f, %.3f) valid=%d left=%d right=%d frames=%" PRIu64
        " videoFrames=%" PRIu64 " staleVideo=%" PRIu64 " sentPoses5s=%" PRIu64,
        p.x, p.y, p.z, q.x, q.y, q.z, q.w, pose.head.valid, pose.leftHand.valid, pose.rightHand.valid, m_frameCount,
        m_videoFrameCount, m_staleVideoCount, sentPoses);
}

// トリガーが「離した状態から深く引いた状態」に変わった瞬間だけ true を返す。
// 引いたままで何度も反応しないよう、いったん浅い位置まで戻るのを待つ。
bool XrApp::DetectTriggerPull(const protocol::InputMessage& input) {
    const std::array<float, 2> triggers{input.left.trigger, input.right.trigger};
    bool pulled = false;
    for (size_t index = 0; index < triggers.size(); ++index) {
        if (triggers[index] < kTriggerReleaseLevel) {
            m_triggerPressed[index] = false;
        } else if (triggers[index] >= kTriggerPullLevel && !m_triggerPressed[index]) {
            m_triggerPressed[index] = true;
            pulled = true;
        }
    }
    return pulled;
}

// 映像を 1 コマ描くたびに呼ぶ。たまっていた画像の数などを集め、0.5 秒ごとに PC へ知らせる。
// PC はこの値で仮想の垂直同期の速さを合わせ、Quest に画像がたまりすぎも不足もしないようにする。
void XrApp::ReportDisplayFeedback(bool newImage, size_t queueDepth, XrDuration displayPeriod) {
    const auto now = std::chrono::steady_clock::now();
    if (m_feedbackFrames == 0) {
        m_feedbackTime = now;
    }
    const uint64_t repeated = newImage ? 0u : 1u;
    const auto dropped = m_decoder.TakeDroppedFrames();
    ++m_feedbackFrames;
    m_feedbackQueueDepth += queueDepth;
    m_feedbackRepeated += repeated;
    m_feedbackDropped += dropped;
    // 5 秒ごとのログ用にも、同じ値を別に数える。
    ++m_queueStatsFrames;
    m_queueStatsDepth += queueDepth;
    m_queueStatsRepeated += repeated;
    m_queueStatsDropped += dropped;
    if (now - m_feedbackTime < kDisplayFeedbackInterval) {
        return;
    }
    protocol::DisplayFeedbackMessage feedback;
    feedback.averageQueueDepth = static_cast<float>(m_feedbackQueueDepth)
        / static_cast<float>(m_feedbackFrames);
    // 長い停止があっても、通信上のカウンターを折り返さない。
    feedback.repeatedFrames = static_cast<uint16_t>(std::min<uint64_t>(m_feedbackRepeated, 65535));
    feedback.droppedFrames = static_cast<uint16_t>(std::min<uint64_t>(m_feedbackDropped, 65535));
    feedback.displayPeriodUs = static_cast<uint32_t>(std::clamp<int64_t>(displayPeriod / 1000, 0, UINT32_MAX));
    m_network.SendDisplayFeedback(feedback);
    m_feedbackFrames = m_feedbackQueueDepth = m_feedbackRepeated = m_feedbackDropped = 0;
}

void XrApp::RenderFrame() {
    auto waitInfo = XrInfo<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
    auto frameState = XrInfo<XrFrameState>(XR_TYPE_FRAME_STATE);
    if (!CheckXr(xrWaitFrame(m_session, &waitInfo, &frameState), "xrWaitFrame")) {
        return;
    }
    auto beginInfo = XrInfo<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
    if (!CheckXr(xrBeginFrame(m_session, &beginInfo), "xrBeginFrame")) {
        return;
    }
    UpdateMic();
    ReportClientStats();
    ++m_frameCount;
    const auto input = SyncActions();
    // 「SteamVR が起動していません」の表示中にトリガーを引いたら、新しいセッションとしてつなぎ直す。
    if (DetectTriggerPull(input) && m_network.GetStatus().state == NetworkClient::ConnectionState::Stopped) {
        m_sessionId = MakeSessionId(m_sessionId);
        m_startRequested = true;
        WNVR_LOGI("トリガーでセッション ID を更新しました: %08" PRIx32, m_sessionId);
    }
    m_network.SendInput(input);
    // 描画用の予測姿勢は送信せず、頭から目への変換だけに使う。
    protocol::DevicePose head;
    {
        std::lock_guard lock(m_trackingMutex);
        head = LocatePose(m_viewSpace, frameState.predictedDisplayTime);
    }
    std::vector<XrCompositionLayerProjectionView> projectionViews(m_eyes.size(),
        XrInfo<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW));
    bool rendered = false;
    m_videoBound = false;
    if (frameState.shouldRender) {
        // reader 再生成後も acquire 済みの AHardwareBuffer を保持し、回復画像が来るまで描き続ける。
        size_t queueDepthBefore = 0;
        auto image = m_decoder.TakeNextImage(&queueDepthBefore);
        if (image) {
            image->takenTime = std::chrono::steady_clock::now();
            if (m_previousImage) {
                // さらに前の画像だけを返し、直前の描画で GPU が読む画像は保持する。
                glFinish();
                m_decoder.ReleaseImage(*m_previousImage);
            }
            m_previousImage = m_currentImage;
            m_currentImage = image;
            m_currentImageDrawn = false;
            m_lastVideoImageTime = std::chrono::steady_clock::now();
        }
        auto locateInfo = XrInfo<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
        locateInfo.viewConfigurationType = kViewConfiguration;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = m_baseSpace;
        auto viewState = XrInfo<XrViewState>(XR_TYPE_VIEW_STATE);
        uint32_t count = 0;
        const bool located = CheckXr(xrLocateViews(m_session, &locateInfo, &viewState,
            static_cast<uint32_t>(m_views.size()), &count, m_views.data()), "xrLocateViews");
        constexpr XrViewStateFlags kValidPose = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        if (located && count == m_eyes.size() && (viewState.viewStateFlags & kValidPose) == kValidPose) {
            const auto& left = m_views[0].pose.position;
            const auto& right = m_views[1].pose.position;
            const float dx = right.x - left.x;
            const float dy = right.y - left.y;
            const float dz = right.z - left.z;
            m_hello.ipdMeters = std::sqrt(dx * dx + dy * dy + dz * dz);
            const auto convertFov = [](const XrFovf& fov) -> protocol::Fov {
                return {std::tan(fov.angleLeft), std::tan(fov.angleRight),
                    -std::tan(fov.angleUp), -std::tan(fov.angleDown)};
            };
            m_hello.leftEyeFov = convertFov(m_views[0].fov);
            m_hello.rightEyeFov = convertFov(m_views[1].fov);
            m_haveLocatedViews = true;
            m_videoBound = m_currentImage && m_renderer.BindImage(m_currentImage->buffer, m_currentImage->generation);
            rendered = true;
            // 両目で同じ接続の描画条件を使うため、フレームごとに一度だけ取得する。
            const auto renderDisplay = m_network.GetRenderDisplay();
            for (size_t eyeIndex = 0; eyeIndex < m_eyes.size(); ++eyeIndex) {
                if (!RenderEye(eyeIndex, projectionViews[eyeIndex], head, renderDisplay)) {
                    rendered = false;
                    break;
                }
            }
            if (rendered && m_videoBound) {
                ReportDisplayFeedback(image.has_value(), queueDepthBefore, frameState.predictedDisplayPeriod);
                ++m_videoFrameCount;
                ++m_clientStats.displayedFrames;
                if (m_currentImageDrawn) {
                    ++m_staleVideoCount;
                    ++m_clientStats.repeatedFrames;
                }
                if (!m_currentImageDrawn) {
                    MeasureLatency(frameState.predictedDisplayTime, frameState.predictedDisplayPeriod);
                }
                m_currentImageDrawn = true;
            }
        }
    }
    if (m_haveLocatedViews) {
        m_network.Update(MakeHello());
    }
    if (!m_poseThread.joinable()) {
        SendMicAudio();
        std::lock_guard lock(m_trackingMutex);
        auto pose = LocatePoses(frameState.predictedDisplayTime, frameState.predictedDisplayTime);
        pose.sampleTimeUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        SendPose(pose);
    }
    LogHeadPose();
    auto layer = XrInfo<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    layer.space = m_baseSpace;
    layer.viewCount = static_cast<uint32_t>(projectionViews.size());
    layer.views = projectionViews.data();
    const XrCompositionLayerBaseHeader* layers[2]{};
    uint32_t layerCount = 0;
    if (!m_exitRequested && frameState.shouldRender) {
        if (rendered) {
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        }
        const auto status = m_network.GetStatus();
        const bool showingVideo = status.state == NetworkClient::ConnectionState::Connected
            && m_lastVideoImageTime != std::chrono::steady_clock::time_point{}
            && std::chrono::steady_clock::now() - m_lastVideoImageTime <= std::chrono::seconds(1);
        const bool showingNotice = m_restartNotice.Visible(std::chrono::steady_clock::now());
        if (showingNotice || !showingVideo) {
            const auto [title, detail] = showingNotice ? m_restartNotice.Text() : StatusText(status);
            m_statusOverlay.SetText(title, detail);
            if (const auto* overlay = m_statusOverlay.Layer(m_viewSpace)) {
                layers[layerCount++] = overlay;
            }
        }
    }
    auto endInfo = XrInfo<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = endInfo.layerCount != 0 ? layers : nullptr;
    CheckXr(xrEndFrame(m_session, &endInfo), "xrEndFrame");
}

void XrApp::ReleaseVideoImages() {
    if (m_currentImage || m_previousImage) {
        // 再生成・終了時は次フレームを待てないため、読み取りの完了を保証して返す。
        glFinish();
    }
    if (m_previousImage) {
        m_decoder.ReleaseImage(*m_previousImage);
        m_previousImage.reset();
    }
    if (m_currentImage) {
        m_decoder.ReleaseImage(*m_currentImage);
        m_currentImage.reset();
    }
    m_currentImageDrawn = false;
    m_videoBound = false;
}

void XrApp::Shutdown() {
    m_poseStopping.store(true);
    if (m_poseThread.joinable()) {
        m_poseThread.join();
    }
    ReleaseVideoImages();
    m_decoder.Stop();
    m_renderer.Shutdown();
    m_micCapture.Stop();
    m_network.Close();
    m_statusOverlay.Shutdown();
    m_input.DestroySpaces();
    for (auto& eye : m_eyes) {
        if (!eye.framebuffers.empty()) {
            glDeleteFramebuffers(static_cast<GLsizei>(eye.framebuffers.size()), eye.framebuffers.data());
        }
    }
    for (auto& eye : m_eyes) {
        if (eye.handle != XR_NULL_HANDLE) {
            CheckXr(xrDestroySwapchain(eye.handle), "xrDestroySwapchain");
        }
    }
    m_eyes.clear();
    m_views.clear();
    if (m_viewSpace != XR_NULL_HANDLE) {
        CheckXr(xrDestroySpace(m_viewSpace), "xrDestroySpace(VIEW)");
        m_viewSpace = XR_NULL_HANDLE;
    }
    if (m_baseSpace != XR_NULL_HANDLE) {
        CheckXr(xrDestroySpace(m_baseSpace), "xrDestroySpace(base)");
        m_baseSpace = XR_NULL_HANDLE;
    }
    if (m_session != XR_NULL_HANDLE) {
        CheckXr(xrDestroySession(m_session), "xrDestroySession");
        m_session = XR_NULL_HANDLE;
    }
    if (m_instance != XR_NULL_HANDLE) {
        m_input.DestroyActions();
        CheckXr(xrDestroyInstance(m_instance), "xrDestroyInstance");
        m_instance = XR_NULL_HANDLE;
    }
    m_egl.Destroy();
    if (m_attachedThread) {
        if (m_vm->DetachCurrentThread() != JNI_OK) {
            WNVR_LOGW("DetachCurrentThread failed");
        }
        m_attachedThread = false;
    }
    m_vm = nullptr;
    m_app = nullptr;
    m_systemId = XR_NULL_SYSTEM_ID;
    m_sessionRunning = false;
    m_exitRequested = false;
    m_frameCount = 0;
    m_videoFrameCount = 0;
    m_staleVideoCount = 0;
    m_feedbackFrames = m_feedbackQueueDepth = m_feedbackRepeated = m_feedbackDropped = 0;
    m_queueStatsFrames = m_queueStatsDepth = m_queueStatsRepeated = m_queueStatsDropped = 0;
    m_haveLocatedViews = false;
    m_sessionState = XR_SESSION_STATE_UNKNOWN;
    m_getRefreshRate = nullptr;
    m_requestRefreshRate = nullptr;
    m_supports72Hz = false;
    m_convertTime = nullptr;
    m_convertPoseTime = nullptr;
    m_handActive.fill(false);
    m_latestSentPose = {};
    m_sentPoseCount = 0;
    m_loggedTimeFallback = false;
    m_averageLatencyMs.reset();
    m_predictionUs.store(0);
    {
        std::lock_guard lock(m_trackingMutex);
        m_predictionHistory.clear();
    }
    m_predictionFrames = 0;
    m_totalPredictionMs = 0;
    m_totalPredictionErrorMs = 0;
    m_minPredictionErrorMs = 0;
    m_maxPredictionErrorMs = 0;
    m_latencyFrames = 0;
    m_totalLatencyMs = m_minLatencyMs = m_maxLatencyMs = 0;
    m_clientStats = {};
    m_statsGeneration = 0;
    m_clientStatsTime = {};
    m_hello = {};
}

}
