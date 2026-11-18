#include "host_app.h"

#include "host_log.h"
#include "network_adapters.h"

#include <wnvr/win32/platform.h>

#include <shellapi.h>

#include <cinttypes>
#include <utility>

namespace wnvr::host {
namespace {

// 起動した直後は、SteamVR がすでに動いていないか（ドライバのパイプがつながるか）を少し待つ。
constexpr auto kStartupListenDelay = std::chrono::seconds(2);
// SteamVR が終了してから、ドライバが使っていたポートが空くまで待つ。
constexpr auto kDriverExitListenDelay = std::chrono::seconds(3);
constexpr auto kListenRetryInterval = std::chrono::seconds(5);
constexpr auto kLaunchTimeout = std::chrono::seconds(90);
constexpr auto kUnregisteredLogInterval = std::chrono::seconds(30);
constexpr UINT kTimerIntervalMs = 1000;
// ドライバに渡せる拒否中の端末の数（パイプのメッセージの上限）。
constexpr size_t kMaxRejectedIdsForDriver = ipc::kMaxApprovedDevices;
// Steam に SteamVR（アプリ ID 250820）を起動させる。
constexpr wchar_t kSteamVrLaunchUrl[] = L"steam://rungameid/250820";

const char* StateName(HostState state) {
    switch (state) {
    case HostState::WaitingForDriverExit: return "ドライバの終了待ち";
    case HostState::Listening: return "待ち受け中";
    case HostState::ListenFailed: return "待ち受けに失敗";
    case HostState::LaunchingSteamVr: return "SteamVR を起動中";
    case HostState::DriverConnected: return "SteamVR に接続中";
    }
    return "";
}

}

HostApp::HostApp()
    : m_pairing(m_hostSettings, PairingManager::Callbacks{
          [this] {
              m_driverPipe.SetSettings(m_hostSettings);
              UpdateDeviceLists();
          },
          [this] { SendRejectedDevices(); }}),
      m_listener(HostListener::Callbacks{
          [this](const ipc::PairingRequestMessage& request) { m_pairing.Queue(request); },
          [this](const std::string& deviceId) { OnApprovedHello(deviceId); }}),
      m_settings(SettingsView::Context{m_hostSettings, m_driverPipe, m_setup,
          [this] { SettingsChanged(); },
          [] { PostQuitMessage(0); }}) {}

HostApp::~HostApp() {
    m_listener.Stop();
    m_driverPipe.Stop();
    m_tray.Remove();
    if (m_window != nullptr) {
        DestroyWindow(m_window);
    }
    if (m_classRegistered) {
        UnregisterClassW(kMessageWindowClass, m_instance);
    }
}

int HostApp::Run(HINSTANCE instance, UINT showSettingsMessage, const StartOptions& options) {
    m_instance = instance;
    m_showSettingsMessage = showSettingsMessage;
    m_hostSettings = LoadSettings();
    m_driverPipe.SetSettings(m_hostSettings);
    m_listener.SetAutoStart(m_hostSettings.autoStartSteamVr);
    m_adapterId = m_hostSettings.adapterId;
    UpdateDeviceLists();
    // タスクトレイの表示に使うので、設定画面を開く前に一度確かめておく。
    m_setup.RefreshDriver();
    if (!CreateMessageWindow() || !m_tray.Add(m_window)) {
        return 1;
    }
    m_pairing.SetNotifyTarget(m_window, kPairingMessage);
    m_driverPipe.SetPairingCallback([this](const ipc::PairingRequestMessage& request) {
        m_pairing.Queue(request);
    });
    if (!SetTimer(m_window, kTimerId, kTimerIntervalMs, nullptr)) {
        LogError("定期処理のタイマーを設定できません: %lu", GetLastError());
        return 1;
    }
    m_driverPipe.Start();
    m_stateSince = Clock::now();
    m_listenDelay = kStartupListenDelay;
    PublishStatus();
    if (options.page) {
        m_settings.View().ShowPage(*options.page);
    }
    if (options.showSettings) {
        ShowSettings();
    }
    return RunMessageLoop();
}

bool HostApp::CreateMessageWindow() {
    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (m_taskbarCreatedMessage == 0) {
        LogError("TaskbarCreated の登録に失敗しました: %lu", GetLastError());
        return false;
    }
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = m_instance;
    windowClass.lpszClassName = kMessageWindowClass;
    if (RegisterClassExW(&windowClass) == 0) {
        LogError("メッセージウィンドウのクラス登録に失敗しました: %lu", GetLastError());
        return false;
    }
    m_classRegistered = true;
    // HWND_MESSAGE では TaskbarCreated のブロードキャストを受け取れない。
    m_window = CreateWindowExW(0, kMessageWindowClass, L"WN-VRBridge Host", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, m_instance, this);
    if (m_window == nullptr) {
        LogError("メッセージウィンドウの作成に失敗しました: %lu", GetLastError());
        return false;
    }
    return true;
}

int HostApp::RunMessageLoop() {
    MSG message{};
    for (;;) {
        if (m_settings.IsRenderable()) {
            // 設定画面が見えているあいだは、たまったメッセージを処理してから 1 フレーム描く
            // （描画は垂直同期で待つので、CPU を使い切らない）。
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    return static_cast<int>(message.wParam);
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (m_settings.IsRenderable()) {
                m_settings.Render();
            }
        } else {
            // 見えていないあいだは、メッセージが来るまで眠る。
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result == -1) {
                LogError("GetMessageW に失敗しました: %lu", GetLastError());
                return 1;
            }
            if (result == 0) {
                return static_cast<int>(message.wParam);
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void HostApp::OnTimer() {
    m_setup.Update(m_settings.IsOpen());
    m_pairing.ExpireRejected();
    UpdateState();
}

void HostApp::ShowSettings() {
    m_settings.View().SetHostStatus(CurrentStatus());
    m_settings.Show(m_instance);
}

// ---- 設定と、承認・拒否した Quest の一覧 ----

void HostApp::UpdateDeviceLists() {
    std::vector<std::string> approved;
    for (const auto& device : m_hostSettings.approvedDevices) {
        approved.push_back(device.id);
    }
    m_listener.SetDeviceLists(std::move(approved), m_pairing.RejectedIds());
}

void HostApp::SendRejectedDevices() {
    auto ids = m_pairing.RejectedIds();
    // 上限を超えた分も、ホストでは期限まで保持して確認のダイアログを抑止する。
    if (ids.size() > kMaxRejectedIdsForDriver) {
        ids.resize(kMaxRejectedIdsForDriver);
    }
    m_driverPipe.SetRejectedDevices(std::move(ids));
    UpdateDeviceLists();
}

void HostApp::SettingsChanged() {
    m_driverPipe.SetSettings(m_hostSettings);
    m_listener.SetAutoStart(m_hostSettings.autoStartSteamVr);
    UpdateDeviceLists();
    if (m_adapterId != m_hostSettings.adapterId) {
        m_adapterId = m_hostSettings.adapterId;
        // ホストが待ち受けているあいだなら、すぐに新しいアダプターで開き直す。
        // SteamVR が動いているあいだは、ドライバが Quest の切断後に切り替える。
        if (m_state == HostState::Listening || m_state == HostState::ListenFailed) {
            m_listener.Stop();
            StartListening();
        }
    }
    PublishStatus();
}

// ---- 接続の状態 ----

void HostApp::ChangeState(HostState state) {
    if (state == HostState::DriverConnected && m_suppressedSessionId != 0) {
        m_listener.SetSuppressedSession(0);
        m_suppressedSessionId = 0;
        LogInfo("SteamVR の自動起動の抑止を解除しました（ドライバに接続）");
    }
    if (m_state != state) {
        m_state = state;
        m_stateSince = Clock::now();
        PublishStatus();
        LogInfo("ホストの状態: %s", StateName(state));
    }
}

void HostApp::StartListening() {
    // 古い待ち受けから届いた通知を、新しいアダプターの接続と誤認しない。
    m_listener.Stop();
    if (m_driverPipe.IsConnected()) {
        ChangeState(HostState::DriverConnected);
        return;
    }
    {
        std::lock_guard lock(m_launchMutex);
        m_approvedHello.reset();
    }
    m_bindAddress = BindAddressFor(m_hostSettings.adapterId);
    // 選んだアダプターが見つからないときは、すべてのアダプターで待ち受ける。
    if (m_bindAddress.empty()) {
        m_bindAddress = "0.0.0.0";
    }
    if (m_listener.Start(m_bindAddress)) {
        ChangeState(HostState::Listening);
    } else {
        if (m_state != HostState::ListenFailed) {
            LogWarn("ホストの待ち受けに失敗しました: %s", m_listener.LastError().c_str());
        }
        ChangeState(HostState::ListenFailed);
        m_nextListenAttempt = Clock::now() + kListenRetryInterval;
    }
    PublishStatus();
}

void HostApp::UpdateState() {
    const auto now = Clock::now();
    if (m_driverPipe.IsConnected()) {
        // SteamVR が動いているあいだは、ドライバが同じポートで待ち受ける。
        m_listener.Stop();
        ChangeState(HostState::DriverConnected);
        const auto stats = m_driverPipe.LatestStats();
        m_clientSessionId = stats && stats->clientConnected ? stats->clientSessionId : 0;
    } else if (m_state == HostState::DriverConnected) {
        // SteamVR が終了した。つながっていた Quest には、自動で起動し直さないことを伝える。
        m_suppressedSessionId = m_clientSessionId;
        m_listener.SetSuppressedSession(m_suppressedSessionId);
        m_clientSessionId = 0;
        m_launchError.clear();
        m_driverMissingAtLaunch = false;
        if (m_suppressedSessionId != 0) {
            LogInfo("SteamVR の自動起動を抑止します（セッション ID: %08" PRIx32 "）", m_suppressedSessionId);
        }
        m_listenDelay = kDriverExitListenDelay;
        ChangeState(HostState::WaitingForDriverExit);
    } else if (m_state == HostState::WaitingForDriverExit && now - m_stateSince >= m_listenDelay) {
        StartListening();
    } else if (m_state == HostState::LaunchingSteamVr && now - m_stateSince >= kLaunchTimeout) {
        LogWarn("SteamVR の起動から 90 秒経過してもドライバに接続できません");
        m_launchError = "SteamVR を起動しましたが、90 秒たってもドライバとつながりませんでした。";
        StartListening();
    } else if (m_state == HostState::ListenFailed && now >= m_nextListenAttempt) {
        StartListening();
    } else if (m_state == HostState::Listening && !m_listener.IsRunning()) {
        LogWarn("ホストの待ち受けが停止しました: %s", m_listener.LastError().c_str());
        ChangeState(HostState::ListenFailed);
        m_nextListenAttempt = now + kListenRetryInterval;
    }
    PublishStatus();
}

void HostApp::OnApprovedHello(const std::string& deviceId) {
    std::lock_guard lock(m_launchMutex);
    // UI スレッドが前の通知を処理するまでは、重ねて通知しない。
    if (m_approvedHello) {
        return;
    }
    m_approvedHello = deviceId;
    if (!PostMessageW(m_window, kApprovedHelloMessage, 0, 0)) {
        m_approvedHello.reset();
        LogWarn("起動要求を UI に通知できません: %lu", GetLastError());
    }
}

void HostApp::LaunchSteamVr() {
    std::optional<std::string> deviceId;
    {
        std::lock_guard lock(m_launchMutex);
        deviceId = std::exchange(m_approvedHello, std::nullopt);
    }
    // 通知が届くまでのあいだに、状態や承認が変わっていることがある。
    if (!deviceId || m_state != HostState::Listening || !m_hostSettings.IsApproved(*deviceId)) {
        return;
    }
    if (m_driverPipe.IsConnected()) {
        UpdateState();
        return;
    }
    m_setup.RefreshDriver();
    const auto& driver = m_setup.Driver();
    if (driver.error.empty() && !driver.registered) {
        // 登録されていないドライバは SteamVR に読み込まれないので、起動しても Quest はつながらない。
        m_driverMissingAtLaunch = true;
        const auto now = Clock::now();
        if (!m_lastUnregisteredLog || now - *m_lastUnregisteredLog >= kUnregisteredLogInterval) {
            LogWarn("ドライバが SteamVR に登録されていないため、SteamVR を起動しません");
            m_lastUnregisteredLog = now;
        }
        PublishStatus();
        return;
    }
    // SteamVR を起動する前に join と closesocket を完了し、ポートを引き渡す。
    m_listener.Stop();
    m_listener.SetSuppressedSession(0);
    m_suppressedSessionId = 0;
    m_launchError.clear();
    m_driverMissingAtLaunch = false;
    ChangeState(HostState::LaunchingSteamVr);
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
        kSteamVrLaunchUrl, nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        m_launchError = "SteamVR を起動できませんでした（Steam が入っているか確かめてください。ShellExecute "
            + std::to_string(result) + "）。";
        LogError("%s", m_launchError.c_str());
        StartListening();
    }
}

HostStatus HostApp::CurrentStatus() const {
    HostStatus status;
    status.state = m_state;
    status.autoStartSteamVr = m_hostSettings.autoStartSteamVr;
    status.bindAddress = m_bindAddress;
    status.listenError = m_state == HostState::ListenFailed ? m_listener.LastError() : std::string{};
    status.launchError = m_launchError;
    status.driverMissingAtLaunch = m_driverMissingAtLaunch;
    status.launchSeconds = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - m_stateSince).count());
    status.autoLaunchSuppressed = m_suppressedSessionId != 0;
    return status;
}

void HostApp::PublishStatus() {
    auto status = CurrentStatus();
    const auto summary = Summarize(status, m_driverPipe.LatestStats(), m_setup.DriverState());
    m_tray.SetStatus(summary.tone, L"WN-VRBridge — " + win32::ToWide(summary.title, win32::InvalidText::Replace));
    m_settings.View().SetHostStatus(std::move(status));
}

LRESULT CALLBACK HostApp::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<HostApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<HostApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app != nullptr) {
        if (message == kApprovedHelloMessage) {
            app->LaunchSteamVr();
            return 0;
        }
        if (message == kPairingMessage) {
            app->m_pairing.ProcessQueue();
            return 0;
        }
        if (message == WM_TIMER && wParam == kTimerId) {
            app->OnTimer();
            return 0;
        }
        // 2 つ目を起動したときは、動いている方の設定画面を前に出す。
        if (message == app->m_showSettingsMessage) {
            app->ShowSettings();
            return 0;
        }
        // エクスプローラーが起動し直すとアイコンが消えるので、出し直す。
        if (message == app->m_taskbarCreatedMessage) {
            app->m_tray.Add(window);
            return 0;
        }
        if (message == TrayIcon::kCallbackMessage) {
            if (lParam == WM_LBUTTONDBLCLK) {
                app->ShowSettings();
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                app->m_tray.ShowMenu();
            }
            return 0;
        }
        if (message == WM_COMMAND) {
            if (LOWORD(wParam) == TrayIcon::kOpenSettingsCommand) {
                app->ShowSettings();
            } else if (LOWORD(wParam) == TrayIcon::kExitCommand) {
                PostQuitMessage(0);
            }
            return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}
