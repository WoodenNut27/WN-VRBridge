#pragma once

#include "driver_pipe_client.h"
#include "host_listener.h"
#include "host_status.h"
#include "pairing_manager.h"
#include "settings_window.h"
#include "setup_tasks.h"
#include "tray_icon.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace wnvr::host {

inline constexpr wchar_t kMessageWindowClass[] = L"WNVRBridgeHostMessageWindow";
inline constexpr wchar_t kShowSettingsMessageName[] = L"WNVRBridgeHost.ShowSettings";

// ホストの本体。見えないウィンドウでメッセージを受け、次の 3 つをつなぐ。
//   - タスクトレイと設定画面（利用者とのやり取り）
//   - 接続の状態の管理（SteamVR が動いていないあいだは代わりに待ち受け、Quest が来たら SteamVR を起動する）
//   - ドライバとのパイプ（設定を渡し、統計を受け取る）
class HostApp {
public:
    HostApp();
    ~HostApp();
    struct StartOptions {
        // 起動したときに設定画面を開く（Windows の起動時の自動起動では開かない）。
        bool showSettings = true;
        // 最初に選ぶタブ（指定がなければ「状態」）。
        std::optional<SettingsView::Page> page;
    };
    int Run(HINSTANCE instance, UINT showSettingsMessage, const StartOptions& options);

private:
    using Clock = std::chrono::steady_clock;

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool CreateMessageWindow();
    int RunMessageLoop();
    void OnTimer();
    void ShowSettings();

    // 接続の状態
    void UpdateState();
    void ChangeState(HostState state);
    void StartListening();
    void OnApprovedHello(const std::string& deviceId); // 受信スレッドから呼ばれる
    void LaunchSteamVr();
    HostStatus CurrentStatus() const;
    void PublishStatus();

    // 設定と、承認・拒否した Quest の一覧の配布
    void SettingsChanged();
    void UpdateDeviceLists();
    void SendRejectedDevices();

    static constexpr UINT kPairingMessage = WM_APP + 2;
    static constexpr UINT kApprovedHelloMessage = WM_APP + 3;
    static constexpr UINT_PTR kTimerId = 1;

    HWND m_window = nullptr;
    HINSTANCE m_instance = nullptr;
    UINT m_showSettingsMessage = 0;
    UINT m_taskbarCreatedMessage = 0;
    bool m_classRegistered = false;
    TrayIcon m_tray;

    // 下の部品が参照するので、先に宣言する（破棄は逆順になる）。
    HostSettings m_hostSettings;
    DriverPipeClient m_driverPipe;
    SetupTasks m_setup;
    PairingManager m_pairing;
    HostListener m_listener;

    // 承認済みの Quest から Hello が届いたことを、受信スレッドから UI スレッドへ渡す。
    std::mutex m_launchMutex;
    std::optional<std::string> m_approvedHello;

    HostState m_state = HostState::WaitingForDriverExit;
    Clock::time_point m_stateSince{};
    Clock::time_point m_nextListenAttempt{};
    Clock::duration m_listenDelay{};
    std::string m_adapterId;
    std::string m_bindAddress;
    std::string m_launchError;
    bool m_driverMissingAtLaunch = false;
    std::optional<Clock::time_point> m_lastUnregisteredLog;
    // SteamVR が終了した時点でつながっていた Quest のセッション。同じセッションの Hello では
    // SteamVR を起動し直さない（利用者が自分で終了したのに、すぐ起動し直すのを防ぐ）。
    uint32_t m_clientSessionId = 0;
    uint32_t m_suppressedSessionId = 0;

    SettingsWindow m_settings;
};

}
