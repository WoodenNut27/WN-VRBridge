#include "xr_input.h"

#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace wnvr::quest {
namespace {

// 全フィールドをゼロ初期化し、拡張構造体の next も確実に空にする。
template<typename T>
T XrInfo(XrStructureType type) {
    T value{};
    value.type = type;
    return value;
}

}

bool XrInput::Create(XrInstance instance, XrSession session, Check check) {
    m_instance = instance;
    m_session = session;
    m_check = std::move(check);
    return CreateActions() && SuggestBindings() && CreateHandSpaces();
}

bool XrInput::CreateActions() {
    auto setInfo = XrInfo<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
    std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "wnvr");
    std::snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "WN-VRBridge");
    if (!m_check(xrCreateActionSet(m_instance, &setInfo, &m_actionSet), "xrCreateActionSet")
        || !m_check(xrStringToPath(m_instance, "/user/hand/left", &m_handPaths[0]), "xrStringToPath(left)")
        || !m_check(xrStringToPath(m_instance, "/user/hand/right", &m_handPaths[1]), "xrStringToPath(right)")) {
        return false;
    }
    const auto createAction = [&](const char* name, XrActionType type, XrAction& action, bool leftOnly = false) {
        auto info = XrInfo<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
        std::snprintf(info.actionName, sizeof(info.actionName), "%s", name);
        std::snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", name);
        info.actionType = type;
        info.countSubactionPaths = leftOnly ? 1u : static_cast<uint32_t>(m_handPaths.size());
        info.subactionPaths = m_handPaths.data();
        return m_check(xrCreateAction(m_actionSet, &info, &action), "xrCreateAction");
    };
    return createAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT, m_gripPose)
        && createAction("trigger_value", XR_ACTION_TYPE_FLOAT_INPUT, m_triggerValue)
        && createAction("trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, m_triggerTouch)
        && createAction("squeeze_value", XR_ACTION_TYPE_FLOAT_INPUT, m_squeezeValue)
        && createAction("thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, m_thumbstick)
        && createAction("thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, m_thumbstickClick)
        && createAction("thumbstick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, m_thumbstickTouch)
        && createAction("thumbrest_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, m_thumbrestTouch)
        && createAction("primary_click", XR_ACTION_TYPE_BOOLEAN_INPUT, m_primaryClick)
        && createAction("primary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, m_primaryTouch)
        && createAction("secondary_click", XR_ACTION_TYPE_BOOLEAN_INPUT, m_secondaryClick)
        && createAction("secondary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, m_secondaryTouch)
        // メニューボタンは左手にしかない（右手の同じ位置は Meta ボタンで、アプリからは読めない）。
        && createAction("menu_click", XR_ACTION_TYPE_BOOLEAN_INPUT, m_menuClick, true);
}

bool XrInput::SuggestBindings() {
    XrPath profile = XR_NULL_PATH;
    if (!m_check(xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &profile), "xrStringToPath(profile)")) {
        return false;
    }
    std::vector<XrActionSuggestedBinding> suggested;
    const auto bindAction = [&](XrAction action, const char* left, const char* right) {
        const std::array<const char*, 2> paths{left, right};
        for (const char* path : paths) {
            if (path == nullptr) {
                continue;
            }
            XrPath binding = XR_NULL_PATH;
            if (!m_check(xrStringToPath(m_instance, path, &binding), "xrStringToPath(binding)")) {
                return false;
            }
            suggested.push_back({action, binding});
        }
        return true;
    };
    // 「primary」は右手の A・左手の X、「secondary」は右手の B・左手の Y。
    if (!bindAction(m_gripPose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose")
        || !bindAction(m_triggerValue, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value")
        || !bindAction(m_triggerTouch, "/user/hand/left/input/trigger/touch", "/user/hand/right/input/trigger/touch")
        || !bindAction(m_squeezeValue, "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value")
        || !bindAction(m_thumbstick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick")
        || !bindAction(m_thumbstickClick, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click")
        || !bindAction(m_thumbstickTouch, "/user/hand/left/input/thumbstick/touch", "/user/hand/right/input/thumbstick/touch")
        || !bindAction(m_thumbrestTouch, "/user/hand/left/input/thumbrest/touch", "/user/hand/right/input/thumbrest/touch")
        || !bindAction(m_primaryClick, "/user/hand/left/input/x/click", "/user/hand/right/input/a/click")
        || !bindAction(m_primaryTouch, "/user/hand/left/input/x/touch", "/user/hand/right/input/a/touch")
        || !bindAction(m_secondaryClick, "/user/hand/left/input/y/click", "/user/hand/right/input/b/click")
        || !bindAction(m_secondaryTouch, "/user/hand/left/input/y/touch", "/user/hand/right/input/b/touch")
        || !bindAction(m_menuClick, "/user/hand/left/input/menu/click", nullptr)) {
        return false;
    }
    auto bindingInfo = XrInfo<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
    bindingInfo.interactionProfile = profile;
    bindingInfo.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
    bindingInfo.suggestedBindings = suggested.data();
    auto attachInfo = XrInfo<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &m_actionSet;
    return m_check(xrSuggestInteractionProfileBindings(m_instance, &bindingInfo), "xrSuggestInteractionProfileBindings")
        && m_check(xrAttachSessionActionSets(m_session, &attachInfo), "xrAttachSessionActionSets");
}

bool XrInput::CreateHandSpaces() {
    for (size_t index = 0; index < m_handSpaces.size(); ++index) {
        auto spaceInfo = XrInfo<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
        spaceInfo.action = m_gripPose;
        spaceInfo.subactionPath = m_handPaths[index];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!m_check(xrCreateActionSpace(m_session, &spaceInfo, &m_handSpaces[index]), "xrCreateActionSpace")) {
            return false;
        }
    }
    return true;
}

void XrInput::DestroySpaces() {
    for (auto& space : m_handSpaces) {
        if (space != XR_NULL_HANDLE) {
            m_check(xrDestroySpace(space), "xrDestroySpace(hand)");
            space = XR_NULL_HANDLE;
        }
    }
}

void XrInput::DestroyActions() {
    for (auto* action : {&m_triggerValue, &m_triggerTouch, &m_squeezeValue, &m_thumbstick,
            &m_thumbstickClick, &m_thumbstickTouch, &m_thumbrestTouch, &m_primaryClick, &m_primaryTouch,
            &m_secondaryClick, &m_secondaryTouch, &m_menuClick, &m_gripPose}) {
        if (*action != XR_NULL_HANDLE) {
            m_check(xrDestroyAction(*action), "xrDestroyAction");
            *action = XR_NULL_HANDLE;
        }
    }
    if (m_actionSet != XR_NULL_HANDLE) {
        m_check(xrDestroyActionSet(m_actionSet), "xrDestroyActionSet");
        m_actionSet = XR_NULL_HANDLE;
    }
    m_handPaths = {};
    m_instance = XR_NULL_HANDLE;
    m_session = XR_NULL_HANDLE;
}

float XrInput::ReadFloat(XrAction action, XrPath hand, bool* active) {
    auto info = XrInfo<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    info.action = action;
    info.subactionPath = hand;
    auto state = XrInfo<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
    const bool valid = m_check(xrGetActionStateFloat(m_session, &info, &state), "xrGetActionStateFloat")
        && state.isActive;
    if (active != nullptr) {
        *active = valid;
    }
    return valid ? state.currentState : 0.0f;
}

bool XrInput::ReadBoolean(XrAction action, XrPath hand) {
    auto info = XrInfo<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    info.action = action;
    info.subactionPath = hand;
    auto state = XrInfo<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
    return m_check(xrGetActionStateBoolean(m_session, &info, &state), "xrGetActionStateBoolean")
        && state.isActive && state.currentState;
}

protocol::ControllerInput XrInput::ReadController(size_t index) {
    protocol::ControllerInput hand;
    const XrPath path = m_handPaths[index];
    // トリガーを読めるかどうかで、コントローラが使える状態かを判断する。
    hand.trigger = ReadFloat(m_triggerValue, path, &hand.active);
    if (!hand.active) {
        return hand;
    }
    hand.grip = ReadFloat(m_squeezeValue, path);
    auto getInfo = XrInfo<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    getInfo.action = m_thumbstick;
    getInfo.subactionPath = path;
    auto stick = XrInfo<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F);
    if (m_check(xrGetActionStateVector2f(m_session, &getInfo, &stick), "xrGetActionStateVector2f")
        && stick.isActive) {
        hand.thumbstickX = stick.currentState.x;
        hand.thumbstickY = stick.currentState.y;
    }
    struct Button {
        XrAction action;
        uint32_t flag;
    };
    const Button buttons[]{
        {m_triggerTouch, protocol::kButtonTriggerTouch},
        {m_thumbstickClick, protocol::kButtonThumbstickClick},
        {m_thumbstickTouch, protocol::kButtonThumbstickTouch},
        {m_thumbrestTouch, protocol::kButtonThumbrestTouch},
        {m_primaryClick, protocol::kButtonPrimaryClick},
        {m_primaryTouch, protocol::kButtonPrimaryTouch},
        {m_secondaryClick, protocol::kButtonSecondaryClick},
        {m_secondaryTouch, protocol::kButtonSecondaryTouch},
    };
    for (const auto& button : buttons) {
        if (ReadBoolean(button.action, path)) {
            hand.buttons |= button.flag;
        }
    }
    if (index == 0 && ReadBoolean(m_menuClick, path)) {
        hand.buttons |= protocol::kButtonMenuClick;
    }
    return hand;
}

protocol::InputMessage XrInput::Sync(bool focused, std::array<bool, 2>& handActive) {
    handActive.fill(false);
    protocol::InputMessage input;
    input.sampleTimeUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!focused) {
        return input;
    }
    const XrActiveActionSet active{m_actionSet, XR_NULL_PATH};
    auto syncInfo = XrInfo<XrActionsSyncInfo>(XR_TYPE_ACTIONS_SYNC_INFO);
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    const XrResult syncResult = xrSyncActions(m_session, &syncInfo);
    // フォーカスを失う直前のイベント未処理期間にも、古い手の姿勢を送らない。
    if (!m_check(syncResult, "xrSyncActions") || syncResult != XR_SUCCESS) {
        return input;
    }
    const std::array<protocol::ControllerInput*, 2> hands{&input.left, &input.right};
    for (size_t index = 0; index < handActive.size(); ++index) {
        auto getInfo = XrInfo<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
        getInfo.action = m_gripPose;
        getInfo.subactionPath = m_handPaths[index];
        auto state = XrInfo<XrActionStatePose>(XR_TYPE_ACTION_STATE_POSE);
        if (m_check(xrGetActionStatePose(m_session, &getInfo, &state), "xrGetActionStatePose") && state.isActive) {
            handActive[index] = true;
        }
        *hands[index] = ReadController(index);
    }
    return input;
}

}
