#include "controller_device.h"

#include "driver_log.h"
#include "driver_settings.h"

#include <cmath>
#include <numbers>

namespace wnvr::driver {
namespace {

// 補正の上限。これを超える値は書きまちがいとみなして使わない。
constexpr float kMaxAngleOffsetDeg = 180.0f;
constexpr float kMaxPositionOffsetMeters = 0.2f;
// グリップには接触センサーがないので、握り量がこの値を超えたら触れているとみなす。
constexpr float kGripTouchThreshold = 0.1f;

// Quest の grip の姿勢と SteamVR の 3D モデルの原点のずれを埋める補正を読む。
float ReadAngleOffset(const char* key) {
    return settings::ReadFloat(key, -kMaxAngleOffsetDeg, kMaxAngleOffsetDeg, 0.0f);
}

float ReadPositionOffset(const char* key) {
    return settings::ReadFloat(key, -kMaxPositionOffsetMeters, kMaxPositionOffsetMeters, 0.0f);
}

}

ControllerDevice::ControllerDevice(Hand hand) : m_hand(hand) {
    // 設定は右手の値で書き、左手は左右を反転して使う。
    const double pitch = ReadAngleOffset("controllerPitchOffsetDeg");
    const double mirror = hand == Hand::Left ? -1.0 : 1.0;
    const double yaw = mirror * ReadAngleOffset("controllerYawOffsetDeg");
    const double roll = mirror * ReadAngleOffset("controllerRollOffsetDeg");
    m_translationOffset[0] = mirror * ReadPositionOffset("controllerOffsetX");
    m_translationOffset[1] = ReadPositionOffset("controllerOffsetY");
    m_translationOffset[2] = ReadPositionOffset("controllerOffsetZ");

    constexpr double kHalfRadiansPerDegree = std::numbers::pi / 360.0;
    const double sx = std::sin(pitch * kHalfRadiansPerDegree);
    const double cx = std::cos(pitch * kHalfRadiansPerDegree);
    const double sy = std::sin(yaw * kHalfRadiansPerDegree);
    const double cy = std::cos(yaw * kHalfRadiansPerDegree);
    const double sz = std::sin(roll * kHalfRadiansPerDegree);
    const double cz = std::cos(roll * kHalfRadiansPerDegree);
    // 自身の軸で X → Y → Z と回すため、積は qX * qY * qZ にする。
    m_rotationOffset = {cx * cy * cz - sx * sy * sz, sx * cy * cz + cx * sy * sz,
        cx * sy * cz - sx * cy * sz, cx * cy * sz + sx * sy * cz};
    Log("%s 補正: pitch=%.3f yaw=%.3f roll=%.3f deg, x=%.4f y=%.4f z=%.4f m",
        SerialNumber(), pitch, yaw, roll,
        m_translationOffset[0], m_translationOffset[1], m_translationOffset[2]);
}

vr::EVRInitError ControllerDevice::Activate(uint32_t objectId) {
    auto* properties = vr::VRProperties();
    const auto container = properties->TrackedDeviceToPropertyContainer(objectId);
    const bool isLeft = m_hand == Hand::Left;
    properties->SetStringProperty(container, vr::Prop_ControllerType_String, "oculus_touch");
    properties->SetStringProperty(container, vr::Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
    properties->SetStringProperty(container, vr::Prop_RenderModelName_String,
        isLeft ? "oculus_quest_plus_controller_left" : "oculus_quest_plus_controller_right");
    properties->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32,
        isLeft ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    properties->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Oculus");
    properties->SetStringProperty(container, vr::Prop_ModelNumber_String,
        isLeft ? "Oculus Quest3 (Left Controller)" : "Oculus Quest3 (Right Controller)");
    properties->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "wnvrbridge");
    properties->SetStringProperty(container, vr::Prop_SerialNumber_String, SerialNumber());
    std::lock_guard lock(m_poseMutex);
    m_inputHandles = {};
    auto* input = vr::VRDriverInput();
    const auto createBoolean = [&](const char* path, vr::VRInputComponentHandle_t& handle) {
        vr::VRInputComponentHandle_t created = vr::k_ulInvalidInputComponentHandle;
        const auto error = input->CreateBooleanComponent(container, path, &created);
        if (error == vr::VRInputError_None) {
            handle = created;
        } else {
            Log("%s 入力コンポーネント作成失敗: %s error=%d", SerialNumber(), path, static_cast<int>(error));
        }
    };
    const auto createScalar = [&](const char* path, vr::EVRScalarUnits units, vr::VRInputComponentHandle_t& handle) {
        vr::VRInputComponentHandle_t created = vr::k_ulInvalidInputComponentHandle;
        const auto error = input->CreateScalarComponent(container, path, &created, vr::VRScalarType_Absolute, units);
        if (error == vr::VRInputError_None) {
            handle = created;
        } else {
            Log("%s 入力コンポーネント作成失敗: %s error=%d", SerialNumber(), path, static_cast<int>(error));
        }
    };
    createScalar("/input/trigger/value", vr::VRScalarUnits_NormalizedOneSided, m_inputHandles.trigger);
    createScalar("/input/grip/value", vr::VRScalarUnits_NormalizedOneSided, m_inputHandles.grip);
    createScalar("/input/joystick/x", vr::VRScalarUnits_NormalizedTwoSided, m_inputHandles.thumbstickX);
    createScalar("/input/joystick/y", vr::VRScalarUnits_NormalizedTwoSided, m_inputHandles.thumbstickY);
    createBoolean("/input/trigger/touch", m_inputHandles.triggerTouch);
    createBoolean("/input/grip/touch", m_inputHandles.gripTouch);
    createBoolean("/input/joystick/click", m_inputHandles.thumbstickClick);
    createBoolean("/input/joystick/touch", m_inputHandles.thumbstickTouch);
    createBoolean("/input/thumbrest/touch", m_inputHandles.thumbrestTouch);
    createBoolean(isLeft ? "/input/x/click" : "/input/a/click", m_inputHandles.primaryClick);
    createBoolean(isLeft ? "/input/x/touch" : "/input/a/touch", m_inputHandles.primaryTouch);
    createBoolean(isLeft ? "/input/y/click" : "/input/b/click", m_inputHandles.secondaryClick);
    createBoolean(isLeft ? "/input/y/touch" : "/input/b/touch", m_inputHandles.secondaryTouch);
    if (isLeft) {
        createBoolean("/input/system/click", m_inputHandles.systemClick);
        createBoolean("/input/system/touch", m_inputHandles.systemTouch);
    }
    m_objectId = objectId;
    UpdateInputLocked({});
    PublishPoseLocked();
    return vr::VRInitError_None;
}

void ControllerDevice::Deactivate() {
    std::lock_guard lock(m_poseMutex);
    m_objectId = vr::k_unTrackedDeviceIndexInvalid;
    m_inputHandles = {};
}

void ControllerDevice::EnterStandby() {
}

void* ControllerDevice::GetComponent(const char*) {
    return nullptr;
}

void ControllerDevice::DebugRequest(const char*, char* responseBuffer, uint32_t bufferSize) {
    if (bufferSize > 0 && responseBuffer != nullptr) {
        responseBuffer[0] = '\0';
    }
}

const char* ControllerDevice::SerialNumber() const {
    return m_hand == Hand::Left ? "WNVR-CTRL-LEFT" : "WNVR-CTRL-RIGHT";
}

vr::DriverPose_t ControllerDevice::GetPose() {
    std::lock_guard lock(m_poseMutex);
    return GetPoseLocked();
}

vr::DriverPose_t ControllerDevice::GetPoseLocked() const {
    vr::DriverPose_t pose{};
    const auto& source = m_pose.pose;
    pose.vecPosition[0] = source.position.x;
    pose.vecPosition[1] = source.position.y;
    pose.vecPosition[2] = source.position.z;
    pose.qRotation = {source.orientation.w, source.orientation.x, source.orientation.y, source.orientation.z};
    // コントローラは映像内に描かれるので、速度を渡し SteamVR の予測で表示時刻に合わせる。
    pose.vecVelocity[0] = source.linearVelocity.x;
    pose.vecVelocity[1] = source.linearVelocity.y;
    pose.vecVelocity[2] = source.linearVelocity.z;
    pose.vecAngularVelocity[0] = source.angularVelocity.x;
    pose.vecAngularVelocity[1] = source.angularVelocity.y;
    pose.vecAngularVelocity[2] = source.angularVelocity.z;
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation = m_rotationOffset;
    for (size_t index = 0; index < 3; ++index) {
        pose.vecDriverFromHeadTranslation[index] = m_translationOffset[index];
    }
    pose.poseIsValid = m_clientConnected && m_pose.valid;
    pose.deviceIsConnected = m_clientConnected;
    pose.result = pose.poseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    return pose;
}

void ControllerDevice::PublishPoseLocked() {
    // 送信までロックを維持し、古い姿勢が切断通知を追い越すのを防ぐ。
    if (m_objectId != vr::k_unTrackedDeviceIndexInvalid) {
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_objectId, GetPoseLocked(), sizeof(vr::DriverPose_t));
    }
}

void ControllerDevice::UpdatePose(const protocol::DevicePose& pose) {
    std::lock_guard lock(m_poseMutex);
    m_pose = pose;
    ++m_receivedPoseCount;
    if (pose.valid) {
        ++m_validPoseCount;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastLogTime >= std::chrono::seconds(5)) {
        Log("%s 姿勢: 受信=%llu 有効=%llu 入力受信=%llu", SerialNumber(),
            static_cast<unsigned long long>(m_receivedPoseCount), static_cast<unsigned long long>(m_validPoseCount),
            static_cast<unsigned long long>(m_receivedInputCount));
        m_receivedInputCount = 0;
        m_receivedPoseCount = 0;
        m_validPoseCount = 0;
        m_lastLogTime = now;
    }
    PublishPoseLocked();
}

void ControllerDevice::UpdateInput(const protocol::ControllerInput& input) {
    std::lock_guard lock(m_poseMutex);
    ++m_receivedInputCount;
    UpdateInputLocked(input);
}

void ControllerDevice::UpdateInputLocked(const protocol::ControllerInput& input) {
    if (m_objectId == vr::k_unTrackedDeviceIndexInvalid) {
        return;
    }
    const auto state = input.active && m_clientConnected ? input : protocol::ControllerInput{};
    auto* driverInput = vr::VRDriverInput();
    const auto updateBoolean = [&](vr::VRInputComponentHandle_t handle, bool value) {
        if (handle != vr::k_ulInvalidInputComponentHandle) {
            driverInput->UpdateBooleanComponent(handle, value, 0);
        }
    };
    const auto updateScalar = [&](vr::VRInputComponentHandle_t handle, float value) {
        if (handle != vr::k_ulInvalidInputComponentHandle) {
            driverInput->UpdateScalarComponent(handle, value, 0);
        }
    };
    updateScalar(m_inputHandles.trigger, state.trigger);
    updateScalar(m_inputHandles.grip, state.grip);
    updateScalar(m_inputHandles.thumbstickX, state.thumbstickX);
    updateScalar(m_inputHandles.thumbstickY, state.thumbstickY);
    updateBoolean(m_inputHandles.triggerTouch, (state.buttons & protocol::kButtonTriggerTouch) != 0);
    updateBoolean(m_inputHandles.gripTouch, state.grip > kGripTouchThreshold);
    updateBoolean(m_inputHandles.thumbstickClick, (state.buttons & protocol::kButtonThumbstickClick) != 0);
    updateBoolean(m_inputHandles.thumbstickTouch, (state.buttons & protocol::kButtonThumbstickTouch) != 0);
    updateBoolean(m_inputHandles.thumbrestTouch, (state.buttons & protocol::kButtonThumbrestTouch) != 0);
    updateBoolean(m_inputHandles.primaryClick, (state.buttons & protocol::kButtonPrimaryClick) != 0);
    updateBoolean(m_inputHandles.primaryTouch, (state.buttons & protocol::kButtonPrimaryTouch) != 0);
    updateBoolean(m_inputHandles.secondaryClick, (state.buttons & protocol::kButtonSecondaryClick) != 0);
    updateBoolean(m_inputHandles.secondaryTouch, (state.buttons & protocol::kButtonSecondaryTouch) != 0);
    updateBoolean(m_inputHandles.systemClick, (state.buttons & protocol::kButtonMenuClick) != 0);
    updateBoolean(m_inputHandles.systemTouch, (state.buttons & protocol::kButtonMenuClick) != 0);
}

void ControllerDevice::SetClientConnected(bool connected) {
    std::lock_guard lock(m_poseMutex);
    m_clientConnected = connected;
    if (!connected) {
        UpdateInputLocked({});
    }
    // 再接続時も、新しい相手の姿勢が来るまで以前の姿勢を有効にしない。
    m_pose.valid = false;
    PublishPoseLocked();
}

}
