#pragma once

#include <openxr/openxr.h>

#include <wnvr/protocol/messages.h>

#include <array>
#include <functional>

namespace wnvr::quest {

// Touch コントローラの入力（OpenXR のアクション）。
// ボタンやスティックの状態を読んで InputMessage にし、手の姿勢を取るための空間を持つ。
class XrInput {
public:
    // OpenXR の呼び出しの結果を確かめる関数。失敗したらログに出して false を返す（XrApp::CheckXr）。
    using Check = std::function<bool(XrResult result, const char* call)>;

    // セッションを作った直後に呼ぶ。アクションを作り、セッションに取り付ける。
    bool Create(XrInstance instance, XrSession session, Check check);
    // 手の空間はセッションより先に、アクションはインスタンスより先に破棄する。
    void DestroySpaces();
    void DestroyActions();

    // アクションを同期して、今のボタンやスティックの状態を読む。
    // handActive には、左右のコントローラの姿勢を取れる状態かどうかを入れる。
    // focused が false のあいだ（Quest のメニューを開いているときなど）は、何も押されていない状態を返す。
    protocol::InputMessage Sync(bool focused, std::array<bool, 2>& handActive);

    XrSpace HandSpace(size_t index) const { return m_handSpaces[index]; }

private:
    bool CreateActions();
    bool SuggestBindings();
    bool CreateHandSpaces();
    protocol::ControllerInput ReadController(size_t index);
    float ReadFloat(XrAction action, XrPath hand, bool* active = nullptr);
    bool ReadBoolean(XrAction action, XrPath hand);

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSession m_session = XR_NULL_HANDLE;
    Check m_check;
    XrActionSet m_actionSet = XR_NULL_HANDLE;
    XrAction m_gripPose = XR_NULL_HANDLE;
    XrAction m_triggerValue = XR_NULL_HANDLE;
    XrAction m_triggerTouch = XR_NULL_HANDLE;
    XrAction m_squeezeValue = XR_NULL_HANDLE;
    XrAction m_thumbstick = XR_NULL_HANDLE;
    XrAction m_thumbstickClick = XR_NULL_HANDLE;
    XrAction m_thumbstickTouch = XR_NULL_HANDLE;
    XrAction m_thumbrestTouch = XR_NULL_HANDLE;
    XrAction m_primaryClick = XR_NULL_HANDLE;
    XrAction m_primaryTouch = XR_NULL_HANDLE;
    XrAction m_secondaryClick = XR_NULL_HANDLE;
    XrAction m_secondaryTouch = XR_NULL_HANDLE;
    XrAction m_menuClick = XR_NULL_HANDLE;
    // 添え字は 0 が左手、1 が右手。
    std::array<XrPath, 2> m_handPaths{};
    std::array<XrSpace, 2> m_handSpaces{};
};

}
