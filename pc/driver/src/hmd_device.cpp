#include "hmd_device.h"

#include "driver_log.h"
#include "driver_settings.h"

#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace wnvr::driver {
namespace {

constexpr const char* kLeftFovKeys[]{"leftFovLeft", "leftFovRight", "leftFovTop", "leftFovBottom"};
constexpr const char* kRightFovKeys[]{"rightFovLeft", "rightFovRight", "rightFovTop", "rightFovBottom"};
constexpr float kDefaultIpdMeters = 0.063f;
// 視野角と IPD は、この差より大きく変わったときだけ保存し直す。
constexpr float kDisplayTolerance = 1e-4f;
// 500Hz でも約 0.5 秒分を残し、遅延を指定したフレームの姿勢を選べるようにする。
constexpr size_t kMaxPoseHistory = 256;
// Direct3D 11 のテクスチャの一辺の上限。
constexpr int32_t kMaxRenderSize = 16384;

using protocol::IsValidFov;
using protocol::IsValidIpd;

// 前回の接続で保存した Quest の視野角を読む。4 つそろっていなければ既定値（対称 90°）を使う。
protocol::Fov ReadFovSetting(const char* const (&keys)[4], const char* eye) {
    std::array<std::optional<float>, 4> values;
    bool valid = true;
    for (size_t index = 0; index < values.size(); ++index) {
        values[index] = settings::TryReadFloat(keys[index]);
        valid = valid && values[index].has_value();
    }
    protocol::Fov fov;
    if (valid) {
        fov = {*values[0], *values[1], *values[2], *values[3]};
        valid = IsValidFov(fov);
    }
    if (!valid) {
        fov = {};
    }
    Log("%s 描画視野角 (%s): left=%.5f right=%.5f top=%.5f bottom=%.5f",
        eye, valid ? "保存値" : "既定値", fov.left, fov.right, fov.top, fov.bottom);
    return fov;
}

float ReadIpdSetting() {
    const auto value = settings::TryReadFloat("ipdMeters");
    const bool valid = value && IsValidIpd(*value);
    const float ipd = valid ? *value : kDefaultIpdMeters;
    Log("描画 IPD (%s): %.5f m", valid ? "保存値" : "既定値", ipd);
    return ipd;
}

}

HmdDevice::HmdDevice(StatsCollector* statsCollector)
    : m_leftEyeFov(ReadFovSetting(kLeftFovKeys, "左目")),
      m_rightEyeFov(ReadFovSetting(kRightFovKeys, "右目")),
      m_ipdMeters(ReadIpdSetting()),
      m_renderWidth(static_cast<uint32_t>(settings::ReadInt("renderWidth", 1, kMaxRenderSize, 2064))),
      m_renderHeight(static_cast<uint32_t>(settings::ReadInt("renderHeight", 1, kMaxRenderSize, 2208))),
      m_displayFrequency(settings::ReadFloat("displayFrequency", 1.0f, 1000.0f, 72.0f)) {
    m_framePoseDelay = std::chrono::milliseconds(settings::ReadInt("framePoseDelayMs", 0, 100, 2));
    Log("フレームに付ける姿勢: 受け取った時刻の %lld ms 前",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(m_framePoseDelay).count()));
    VirtualDisplay::Config config;
    // -1 は「いちばん VRAM の多い GPU を自動で選ぶ」。
    config.adapterIndex = settings::ReadInt("adapterIndex", -1, 63, -1);
    config.displayFrequency = m_displayFrequency;
    config.dumpFrame = settings::ReadBool("dumpFrame", false);
    config.bitrateMbps = static_cast<uint32_t>(settings::ReadInt("bitrateMbps", 1, 1000, 80));
    config.dumpVideoSeconds = static_cast<uint32_t>(settings::ReadInt("dumpVideoSeconds", 0, 60, 0));
    config.getFramePose = [this] { return GetFramePose(); };
    config.statsCollector = statsCollector;
    m_virtualDisplay = std::make_unique<VirtualDisplay>();
    if (!m_virtualDisplay->Initialize(config)) {
        Log("仮想ディスプレイを初期化できませんでした。HMD のみで動作を続けます。");
        m_virtualDisplay.reset();
    }
}

vr::EVRInitError HmdDevice::Activate(uint32_t objectId) {
    {
        std::lock_guard lock(m_poseMutex);
        m_objectId = objectId;
    }
    auto* properties = vr::VRProperties();
    const auto container = properties->TrackedDeviceToPropertyContainer(objectId);
    properties->SetStringProperty(container, vr::Prop_ModelNumber_String, kModelName);
    properties->SetStringProperty(container, vr::Prop_ManufacturerName_String, "WoodenNut27");
    properties->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "wnvrbridge");
    properties->SetStringProperty(container, vr::Prop_SerialNumber_String, kSerialNumber);
    properties->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, m_displayFrequency);
    properties->SetFloatProperty(container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
    properties->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
    properties->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);
    properties->SetBoolProperty(container, vr::Prop_HasVirtualDisplayComponent_Bool, true);
    properties->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, false);
    if (m_virtualDisplay) {
        properties->SetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64, m_virtualDisplay->AdapterLuid());
    }
    {
        std::lock_guard lock(m_poseMutex);
        PublishDisplayLocked();
    }
    return vr::VRInitError_None;
}

void HmdDevice::Deactivate() {
    std::lock_guard lock(m_poseMutex);
    m_objectId = vr::k_unTrackedDeviceIndexInvalid;
}

void HmdDevice::EnterStandby() {
}

void* HmdDevice::GetComponent(const char* name) {
    if (name != nullptr && std::strcmp(name, vr::IVRDisplayComponent_Version) == 0) {
        return static_cast<vr::IVRDisplayComponent*>(this);
    }
    if (name != nullptr && std::strcmp(name, vr::IVRVirtualDisplay_Version) == 0 && m_virtualDisplay) {
        return static_cast<vr::IVRVirtualDisplay*>(m_virtualDisplay.get());
    }
    return nullptr;
}

void HmdDevice::DebugRequest(const char*, char* responseBuffer, uint32_t bufferSize) {
    if (bufferSize > 0 && responseBuffer != nullptr) {
        responseBuffer[0] = '\0';
    }
}

vr::DriverPose_t HmdDevice::GetPose() {
    std::lock_guard lock(m_poseMutex);
    return GetPoseLocked();
}

vr::DriverPose_t HmdDevice::GetPoseLocked() const {
    vr::DriverPose_t pose{};
    pose.vecPosition[0] = m_headPose.position.x;
    pose.vecPosition[1] = m_headPose.position.y;
    pose.vecPosition[2] = m_headPose.position.z;
    pose.qRotation = {m_headPose.orientation.w, m_headPose.orientation.x,
        m_headPose.orientation.y, m_headPose.orientation.z};
    // SteamVR に速度を渡すと、SteamVR は表示時刻まで予測した姿勢で描く。
    // Quest はフレームに付けた予測前の姿勢で描かれた前提で再投影するので、
    // 動いている間だけずれてガクつく。予測は Quest の OS の再投影に任せる。
    for (size_t index = 0; index < 3; ++index) {
        pose.vecVelocity[index] = 0.0;
        pose.vecAngularVelocity[index] = 0.0;
    }
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;
    pose.poseIsValid = m_clientConnected && m_poseValid;
    pose.deviceIsConnected = true;
    pose.result = pose.poseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    pose.poseTimeOffset = 0.0;
    return pose;
}

void HmdDevice::RunFrame() {
    std::lock_guard lock(m_poseMutex);
    PublishPoseLocked();
}

void HmdDevice::PublishPoseLocked() {
    // 送信までロックを維持し、RunFrame の古い姿勢が切断通知を追い越すのを防ぐ。
    if (m_objectId != vr::k_unTrackedDeviceIndexInvalid) {
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_objectId, GetPoseLocked(), sizeof(vr::DriverPose_t));
    }
}

FramePoseSample HmdDevice::GetFramePose() {
    std::lock_guard lock(m_poseMutex);
    if (m_poseHistory.empty()) {
        return {{m_sampleTimeUs, m_headPose.position, m_headPose.orientation}, {}};
    }
    // 受け取った時刻が「今 − 遅らせる時間」以前の、最も新しい姿勢を使う。無ければ最も古い姿勢。
    const auto target = std::chrono::steady_clock::now() - m_framePoseDelay;
    const PoseHistoryEntry* chosen = &m_poseHistory.front();
    for (const auto& entry : m_poseHistory) {
        if (entry.receivedAt > target) {
            break;
        }
        chosen = &entry;
    }
    return {chosen->pose, chosen->receivedAt};
}

void HmdDevice::SetFrameSink(EncodedFrameSink* sink) {
    if (m_virtualDisplay) {
        m_virtualDisplay->SetFrameSink(sink);
    }
}

void HmdDevice::SetFoveation(const protocol::Foveation& foveation) {
    if (m_virtualDisplay) {
        m_virtualDisplay->SetFoveation(foveation);
    }
}

void HmdDevice::SetBitrate(uint32_t mbps) {
    if (m_virtualDisplay) {
        m_virtualDisplay->SetBitrate(mbps);
    }
}

void HmdDevice::RequestKeyFrame() {
    if (m_virtualDisplay) {
        m_virtualDisplay->RequestKeyFrame();
    }
}

void HmdDevice::OnDisplayFeedback(const protocol::DisplayFeedbackMessage& feedback) {
    if (m_virtualDisplay) {
        m_virtualDisplay->OnDisplayFeedback(feedback);
    }
}

void HmdDevice::UpdateHeadPose(const protocol::DevicePose& head, uint64_t sampleTimeUs) {
    const auto isFinite = [](const protocol::Vec3& vector) {
        return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
    };
    const auto& orientation = head.pose.orientation;
    if (!head.valid || !isFinite(head.pose.position) || !isFinite(head.pose.linearVelocity)
        || !isFinite(head.pose.angularVelocity) || !std::isfinite(orientation.x)
        || !std::isfinite(orientation.y) || !std::isfinite(orientation.z) || !std::isfinite(orientation.w)) {
        return;
    }
    // float の最大値に近い成分でも二乗がオーバーフローしないよう double で計算する。
    const double x = orientation.x;
    const double y = orientation.y;
    const double z = orientation.z;
    const double w = orientation.w;
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (length < 1e-6) {
        return;
    }
    std::lock_guard lock(m_poseMutex);
    if (!m_clientConnected) {
        return;
    }
    m_headPose = head.pose;
    m_sampleTimeUs = sampleTimeUs;
    m_headPose.orientation = {static_cast<float>(x / length), static_cast<float>(y / length),
        static_cast<float>(z / length), static_cast<float>(w / length)};
    m_poseValid = true;
    m_poseHistory.push_back({std::chrono::steady_clock::now(),
        {sampleTimeUs, m_headPose.position, m_headPose.orientation}});
    if (m_poseHistory.size() > kMaxPoseHistory) {
        m_poseHistory.pop_front();
    }
    PublishPoseLocked();
}

void HmdDevice::SetClientConnected(bool connected) {
    if (!connected && m_virtualDisplay) {
        m_virtualDisplay->ResetDisplayFeedback();
    }
    std::lock_guard lock(m_poseMutex);
    m_clientConnected = connected;
    // 再接続時も、新しい相手から最初の有効な姿勢を受け取るまで無効にする。
    m_poseValid = false;
    m_poseHistory.clear();
    if (!connected) {
        PublishPoseLocked();
    }
}

void HmdDevice::GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) {
    *x = 0;
    *y = 0;
    *width = m_renderWidth * 2;
    *height = m_renderHeight;
}

bool HmdDevice::IsDisplayOnDesktop() {
    return false;
}

bool HmdDevice::IsDisplayRealDisplay() {
    return false;
}

void HmdDevice::GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) {
    *width = m_renderWidth;
    *height = m_renderHeight;
}

void HmdDevice::GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width, uint32_t* height) {
    *x = eye == vr::Eye_Left ? 0 : m_renderWidth;
    *y = 0;
    *width = m_renderWidth;
    *height = m_renderHeight;
}

void HmdDevice::ApplyClientDisplay(const protocol::HelloMessage& hello) {
    // SetDisplayProjectionRaw / SetDisplayEyeToHead は接続後には反映されないため呼ばない。
    // 描画中の値は固定し、Quest の値は次の SteamVR 起動用に保存する。
    if (!vr::VRSettings()) {
        Log("表示設定を保存できません: VRSettings がありません。");
        return;
    }
    bool changed = false;
    bool saved = true;
    const auto saveFloat = [&](const char* key, float value) {
        saved = settings::WriteFloat(key, value) && saved;
    };
    const auto differs = [](float a, float b) { return std::abs(a - b) > kDisplayTolerance; };
    const auto saveFov = [&](const protocol::Fov& source, const protocol::Fov& render,
                             const char* const (&keys)[4]) {
        if (!IsValidFov(source)) {
            return;
        }
        if (!differs(source.left, render.left) && !differs(source.right, render.right)
            && !differs(source.top, render.top) && !differs(source.bottom, render.bottom)) {
            return;
        }
        changed = true;
        saveFloat(keys[0], source.left);
        saveFloat(keys[1], source.right);
        saveFloat(keys[2], source.top);
        saveFloat(keys[3], source.bottom);
    };
    saveFov(hello.leftEyeFov, m_leftEyeFov, kLeftFovKeys);
    saveFov(hello.rightEyeFov, m_rightEyeFov, kRightFovKeys);
    if (IsValidIpd(hello.ipdMeters) && differs(hello.ipdMeters, m_ipdMeters)) {
        changed = true;
        saveFloat("ipdMeters", hello.ipdMeters);
    }
    if (changed && saved) {
        Log("Quest の視野角・IPD を保存しました。次に SteamVR を起動したときから反映されます。");
    }
}

void HmdDevice::PublishDisplayLocked() {
    if (m_objectId == vr::k_unTrackedDeviceIndexInvalid) {
        return;
    }
    auto* properties = vr::VRProperties();
    properties->SetFloatProperty(properties->TrackedDeviceToPropertyContainer(m_objectId),
        vr::Prop_UserIpdMeters_Float, m_ipdMeters);
}

void HmdDevice::GetProjectionRaw(vr::EVREye eye, float* left, float* right, float* top, float* bottom) {
    std::lock_guard lock(m_poseMutex);
    const auto& fov = eye == vr::Eye_Left ? m_leftEyeFov : m_rightEyeFov;
    *left = fov.left;
    *right = fov.right;
    // protocol::Fov は top = −tan(上の角度)、bottom = −tan(下の角度)（OpenXR から作った値）。
    // SteamVR は GetProjectionRaw の bottom を上側の範囲として描くことを実機で確かめた
    // （上 44°・下 55° の視野角で、真正面の地平線が画像の上から 59.7% に写った）。
    // そのため上下を入れ替えて符号を反転し、上側に −fov.top（上の tan）が来るようにする。
    *top = -fov.bottom;
    *bottom = -fov.top;
}

vr::DistortionCoordinates_t HmdDevice::ComputeDistortion(vr::EVREye, float u, float v) {
    return {{u, v}, {u, v}, {u, v}};
}

bool HmdDevice::ComputeInverseDistortion(vr::HmdVector2_t*, vr::EVREye, uint32_t, float, float) {
    return false;
}

}
