#pragma once

#include "status_summary.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace wnvr::host {

class TrayIcon {
public:
    static constexpr UINT kCallbackMessage = WM_APP + 1;
    static constexpr UINT kOpenSettingsCommand = 1001;
    static constexpr UINT kExitCommand = 1002;

    ~TrayIcon();
    // エクスプローラーが起動し直したとき（TaskbarCreated）にも呼び、アイコンを出し直す。
    bool Add(HWND owner);
    void Remove();
    void ShowMenu();
    // アイコンの色と、マウスを乗せたときの説明を今の状態に合わせる。変わったときだけ描き直す。
    void SetStatus(Tone tone, const std::wstring& tip);

private:
    void ApplyStatus();

    NOTIFYICONDATAW m_data{};
    bool m_added = false;
    HICON m_icon = nullptr;
    Tone m_tone = Tone::Idle;
    std::wstring m_tip = L"WN-VRBridge";
};

}
