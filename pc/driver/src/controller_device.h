#pragma once

#include <openvr_driver.h>
#include <wnvr/protocol/messages.h>

#include <chrono>
#include <mutex>

namespace wnvr::driver {

enum class Hand { Left, Right };

class ControllerDevice : public vr::ITrackedDeviceServerDriver {
public:
    explicit ControllerDevice(Hand hand);

    vr::EVRInitError Activate(uint32_t objectId) override;
    void Deactivate() override;
    void EnterStandby() override;
    void* GetComponent(const char* name) override;
    void DebugRequest(const char* request, char* responseBuffer, uint32_t bufferSize) override;
    vr::DriverPose_t GetPose() override;

    const char* SerialNumber() const;
    // ネットワークのスレッドから呼ばれる。
    void UpdatePose(const protocol::DevicePose& pose);
    void SetClientConnected(bool connected);
    void UpdateInput(const protocol::ControllerInput& input);

private:
    vr::DriverPose_t GetPoseLocked() const;
    void PublishPoseLocked();
    void UpdateInputLocked(const protocol::ControllerInput& input);

    struct InputHandles {
        vr::VRInputComponentHandle_t trigger = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t grip = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t thumbstickX = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t thumbstickY = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t triggerTouch = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t gripTouch = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t thumbstickClick = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t thumbstickTouch = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t thumbrestTouch = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t primaryClick = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t primaryTouch = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t secondaryClick = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t secondaryTouch = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t systemClick = vr::k_ulInvalidInputComponentHandle;
        vr::VRInputComponentHandle_t systemTouch = vr::k_ulInvalidInputComponentHandle;
    };
    InputHandles m_inputHandles;
    uint64_t m_receivedInputCount = 0;

    const Hand m_hand;
    vr::HmdQuaternion_t m_rotationOffset{1.0, 0.0, 0.0, 0.0};
    double m_translationOffset[3]{};
    std::mutex m_poseMutex;
    protocol::DevicePose m_pose;
    bool m_clientConnected = false;
    vr::TrackedDeviceIndex_t m_objectId = vr::k_unTrackedDeviceIndexInvalid;
    std::chrono::steady_clock::time_point m_lastLogTime = std::chrono::steady_clock::now();
    uint64_t m_receivedPoseCount = 0;
    uint64_t m_validPoseCount = 0;
};

}
