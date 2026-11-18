#pragma once

#include "host_settings.h"
#include "host_status.h"
#include "network_adapters.h"
#include "setup_tasks.h"
#include "status_summary.h"

#include <windows.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wnvr::host {

class DriverPipeClient;

// 設定画面の中身（何をどこに並べるか）。ウィンドウや描画の準備は SettingsWindow が行い、
// ここは Dear ImGui の部品を並べるだけにする。すべて UI スレッドで呼ぶ。
class SettingsView {
public:
    struct Context {
        HostSettings& settings;        // 変更したら、すぐに保存して onSettingsChanged を呼ぶ
        DriverPipeClient& driverPipe;  // SteamVR が動いているかと、統計の取得
        SetupTasks& setup;             // ドライバの登録とファイアウォール
        std::function<void()> onSettingsChanged;
        std::function<void()> onExit;  // 「終了する」ボタン
    };

    enum class Page { Status, Settings, Setup, About };

    explicit SettingsView(Context context);

    void SetHostStatus(HostStatus status) { m_status = std::move(status); }
    // 次に描くときに、指定したタブを選ぶ。
    void ShowPage(Page page) { m_requestedPage = page; }
    // ウィンドウを開くたびに呼ぶ。開いているあいだに変わりうる一覧を読み直す。
    void OnShown();
    // 1 フレーム分を描く。owner はフォルダを開くときなどの親ウィンドウ。
    void Draw(HWND owner);

private:
    void DrawHeader(const StatusSummary& summary);
    void DrawStatusPage(const std::optional<ipc::StatsSnapshot>& stats);
    void DrawStatistics(const ipc::StatsSnapshot& stats);
    void DrawSettingsPage();
    void DrawAdapterSelector();
    void DrawApprovedDevices();
    void DrawSetupPage();
    void DrawDriverSetup();
    void DrawFirewallSetup();
    void DrawSetupOutcome(bool driverActions);
    void DrawAboutPage();
    void DrawFooter();

    // 設定を保存して、動いている部品（ドライバ、待ち受け）に反映する。
    void Commit();
    void OpenFolder(const std::filesystem::path& folder);
    bool NeedsSetup() const;

    Context m_context;
    HostStatus m_status;
    HWND m_owner = nullptr;
    std::optional<Page> m_requestedPage;

    std::vector<NetworkAdapter> m_adapters;
    int m_bitrateEdit = ipc::kDefaultBitrateMbps;
    int m_questResolutionEdit = ipc::kDefaultQuestResolutionPercent;
    float m_foveationRatioEdit = ipc::kDefaultFoveationEdgeRatio;
    bool m_startupRegistered = false;
    bool m_vbCableFound = false;
    std::string m_startupError;
    std::string m_saveError;
    std::chrono::steady_clock::time_point m_savedNoticeUntil{};
};

}
