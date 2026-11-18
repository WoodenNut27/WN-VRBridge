#include "tray_icon.h"

#include "app_icon.h"
#include "host_log.h"

namespace wnvr::host {

TrayIcon::~TrayIcon() {
    Remove();
    if (m_icon != nullptr) {
        DestroyIcon(m_icon);
    }
}

void TrayIcon::ApplyStatus() {
    const HICON previous = m_icon;
    m_icon = CreateStatusIcon(GetSystemMetrics(SM_CXSMICON), m_tone);
    // 描けなかったときは、Windows の既定のアイコンで代用する。
    m_data.hIcon = m_icon != nullptr ? m_icon : LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(m_data.szTip, m_tip.c_str(), _TRUNCATE);
    if (m_added) {
        Shell_NotifyIconW(NIM_MODIFY, &m_data);
    }
    // 差し替えが済んでから、前のアイコンを破棄する。
    if (previous != nullptr) {
        DestroyIcon(previous);
    }
}

bool TrayIcon::Add(HWND owner) {
    m_data = {};
    m_data.cbSize = sizeof(m_data);
    m_data.hWnd = owner;
    m_data.uID = 1;
    m_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_data.uCallbackMessage = kCallbackMessage;
    m_added = false;
    ApplyStatus();
    m_added = Shell_NotifyIconW(NIM_ADD, &m_data) != FALSE;
    if (!m_added) {
        LogError("トレイアイコンの追加に失敗しました: %lu", GetLastError());
    }
    return m_added;
}

void TrayIcon::Remove() {
    if (m_added) {
        Shell_NotifyIconW(NIM_DELETE, &m_data);
        m_added = false;
    }
}

void TrayIcon::SetStatus(Tone tone, const std::wstring& tip) {
    if (tone == m_tone && tip == m_tip) {
        return;
    }
    m_tone = tone;
    m_tip = tip;
    ApplyStatus();
}

void TrayIcon::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        LogError("トレイメニューの作成に失敗しました: %lu", GetLastError());
        return;
    }
    AppendMenuW(menu, MF_STRING, kOpenSettingsCommand, L"WN-VRBridge を開く");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"終了");
    // ダブルクリックと同じ動作の項目を太字にする。
    SetMenuDefaultItem(menu, kOpenSettingsCommand, FALSE);
    POINT position{};
    GetCursorPos(&position);
    // 前面にしないと、メニューの外をクリックしたときに閉じなくなる。
    SetForegroundWindow(m_data.hWnd);
    const UINT command = static_cast<UINT>(TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        position.x, position.y, 0, m_data.hWnd, nullptr));
    PostMessageW(m_data.hWnd, WM_NULL, 0, 0);
    if (command != 0) {
        PostMessageW(m_data.hWnd, WM_COMMAND, command, 0);
    }
    DestroyMenu(menu);
}

}
