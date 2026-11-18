#pragma once

#include "driver_registration.h"
#include "firewall_rules.h"
#include "status_summary.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace wnvr::host {

// 初回のセットアップ（SteamVR へのドライバの登録と、ファイアウォールの許可ルール）の状態と操作。
// 状態の確認は UI スレッドで行う。変更は vrpathreg や UAC の確認を待つので、専用のスレッドで行う。
// 公開している関数は、すべて UI スレッドから呼ぶ。
class SetupTasks {
public:
    enum class Action { AddDriver, RemoveDriver, RemoveOtherDrivers, AddFirewall, RemoveFirewall };

    struct Outcome {
        Action action = Action::AddDriver;
        bool success = false;
        std::string error; // 失敗の理由（成功なら空）
        // SteamVR が動いているあいだにドライバの登録を変えたので、起動し直すまで反映されない。
        bool needsSteamVrRestart = false;
    };

    // 今の状態を読み直す。
    void Refresh();
    // ドライバの登録だけを読み直す（ファイアウォールの確認は時間がかかるので分ける）。
    void RefreshDriver();
    // 1 秒ごとに呼ぶ。終わった操作の結果を取り込み、画面が見えているあいだは定期的に読み直す。
    void Update(bool windowVisible);
    // 操作を始める。前の操作が終わっていなければ何もしない。
    void Start(Action action, HWND owner, bool steamVrRunning);

    const DriverRegistrationStatus& Driver() const { return m_driver; }
    // 一度も確認していないあいだと、確認に失敗したときは Unknown。
    DriverSetup DriverState() const;
    const FirewallStatus& Firewall() const { return m_firewall; }
    // SteamVR の vrserver.exe が見つかっていて、許可ルールを登録できるか。
    bool CanAddFirewall() const { return m_canAddFirewall; }
    bool IsBusy() const { return m_busy; }
    // 最後に行った操作の結果。次の操作を始めると消える。
    const std::optional<Outcome>& LastOutcome() const { return m_outcome; }

private:
    DriverRegistrationStatus m_driver;
    bool m_driverQueried = false;
    FirewallStatus m_firewall;
    std::wstring m_vrServerPath;
    bool m_canAddFirewall = false;
    std::chrono::steady_clock::time_point m_nextRefresh{};
    bool m_busy = false;
    std::optional<Outcome> m_outcome;

    // スレッドが書き、UI スレッドが Update で受け取る。
    std::mutex m_mutex;
    std::optional<Outcome> m_finished;
    // スレッドが使うメンバーより先に破棄（join）されるよう、最後に宣言する。
    std::jthread m_thread;
};

}
