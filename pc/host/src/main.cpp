#include "firewall_rules.h"
#include "host_app.h"
#include "host_log.h"

#include <wnvr/version.h>

#include <shellapi.h>

#include <exception>
#include <memory>
#include <optional>
#include <string_view>

namespace {

using namespace wnvr::host;

constexpr wchar_t kInstanceMutexName[] = L"Local\\WN-VRBridge.Host";
constexpr wchar_t kDialogTitle[] = L"WN-VRBridge";

// すでに動いているホストを探して、設定画面を開いてもらう。
void ActivateRunningInstance(UINT showSettingsMessage, bool silent) {
    // 同時に起動したとき、先のプロセスがウィンドウを作るまでの短い時間だけ待つ。
    HWND window = nullptr;
    for (int attempt = 0; attempt < 20 && window == nullptr; ++attempt) {
        window = FindWindowW(kMessageWindowClass, nullptr);
        if (window == nullptr) {
            Sleep(25);
        }
    }
    if (window != nullptr && !silent) {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        // 相手のプロセスが自分のウィンドウを前に出せるようにする。
        AllowSetForegroundWindow(processId);
        PostMessageW(window, showSettingsMessage, 0, 0);
    }
}

// 「--page setup」のように、最初に開くタブを指定できる（画面の確認を自動で行うときに使う）。
std::optional<SettingsView::Page> ParsePage(std::wstring_view name) {
    using Page = SettingsView::Page;
    if (name == L"status") {
        return Page::Status;
    }
    if (name == L"settings") {
        return Page::Settings;
    }
    if (name == L"setup") {
        return Page::Setup;
    }
    if (name == L"about") {
        return Page::About;
    }
    return std::nullopt;
}

int RunHost(HINSTANCE instance, UINT showSettingsMessage, const HostApp::StartOptions& options, bool startup,
    bool dpiAware, DWORD dpiError) {
    int exitCode = 1;
    try {
        if (!InitializeLog()) {
            MessageBoxW(nullptr, L"ログファイルを開けませんでした。", kDialogTitle, MB_OK | MB_ICONERROR);
        } else {
            LogInfo("ホストを起動しました (%s)", wnvr::kVersion);
            if (startup) {
                LogInfo("--startup による起動です");
            }
            if (!dpiAware) {
                LogWarn("DPI awareness の設定に失敗しました: %lu", dpiError);
            }
            {
                HostApp app;
                exitCode = app.Run(instance, showSettingsMessage, options);
            }
            LogInfo("ホストを終了します (終了コード: %d)", exitCode);
        }
    } catch (const std::exception& error) {
        LogError("ホストで例外が発生しました: %s", error.what());
    }
    ShutdownLog();
    return exitCode;
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argumentCount = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(
        CommandLineToArgvW(GetCommandLineW(), &argumentCount), LocalFree);
    if (!arguments) {
        return 2;
    }
    // 管理者として起動し直された場合は、ファイアウォールの変更だけを行って終わる。
    if (const auto firewallResult = RunFirewallCommand(argumentCount, arguments.get())) {
        return *firewallResult;
    }
    bool startup = false;
    HostApp::StartOptions options;
    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring_view argument = arguments.get()[index];
        if (argument == L"--startup") {
            startup = true;
        } else if (argument == L"--page" && index + 1 < argumentCount) {
            options.page = ParsePage(arguments.get()[++index]);
        }
    }
    // 自分で起動したときは、何が起きているか分かるように設定画面を開く。
    // Windows の起動時（--startup）は、じゃまにならないようタスクトレイだけにする。
    options.showSettings = !startup;
    const BOOL dpiAware = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD dpiError = dpiAware ? ERROR_SUCCESS : GetLastError();
    // 二重に起動すると、同じポートとパイプを取り合ってしまう。
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
    const DWORD mutexError = GetLastError();
    if (mutex == nullptr) {
        MessageBoxW(nullptr, L"二重起動防止の初期化に失敗しました。", kDialogTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    int exitCode = 1;
    const UINT showSettingsMessage = RegisterWindowMessageW(kShowSettingsMessageName);
    if (showSettingsMessage != 0) {
        if (mutexError == ERROR_ALREADY_EXISTS) {
            ActivateRunningInstance(showSettingsMessage, startup);
            exitCode = 0;
        } else {
            exitCode = RunHost(instance, showSettingsMessage, options, startup, dpiAware != FALSE, dpiError);
        }
    }
    CloseHandle(mutex);
    return exitCode;
}
