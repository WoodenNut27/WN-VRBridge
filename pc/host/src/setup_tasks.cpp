#include "setup_tasks.h"

#include "host_log.h"

#include <wnvr/win32/platform.h>

#include <exception>
#include <filesystem>
#include <utility>

namespace wnvr::host {
namespace {

constexpr auto kRefreshInterval = std::chrono::seconds(5);

const char* ActionLabel(SetupTasks::Action action) {
    switch (action) {
    case SetupTasks::Action::AddDriver: return "ドライバを登録する";
    case SetupTasks::Action::RemoveDriver: return "ドライバの登録を解除する";
    case SetupTasks::Action::RemoveOtherDrivers: return "別の場所の登録を解除する";
    case SetupTasks::Action::AddFirewall: return "許可ルールを登録する";
    case SetupTasks::Action::RemoveFirewall: return "許可ルールを削除する";
    }
    return "";
}

bool IsDriverAction(SetupTasks::Action action) {
    return action == SetupTasks::Action::AddDriver || action == SetupTasks::Action::RemoveDriver
        || action == SetupTasks::Action::RemoveOtherDrivers;
}

// 専用のスレッドで実行する。失敗の理由は error に入れる。
bool Run(SetupTasks::Action action, const DriverRegistrationStatus& driver, const std::wstring& vrServerPath,
    HWND owner, std::string& error) {
    switch (action) {
    case SetupTasks::Action::AddDriver:
    case SetupTasks::Action::RemoveDriver:
        return RunVrPathReg(action == SetupTasks::Action::AddDriver, driver.runtimePath, driver.driverPath, error);
    case SetupTasks::Action::RemoveOtherDrivers: {
        bool success = true;
        for (const auto& path : driver.otherPaths) {
            std::string reason;
            if (!RunVrPathReg(false, driver.runtimePath, path, reason)) {
                success = false;
                error += win32::ToUtf8(path, win32::InvalidText::Replace) + ": " + reason + "\n";
            }
        }
        return success;
    }
    case SetupTasks::Action::AddFirewall:
    case SetupTasks::Action::RemoveFirewall:
        return ChangeFirewallRulesElevated(owner, action == SetupTasks::Action::AddFirewall, vrServerPath, error);
    }
    return false;
}

}

void SetupTasks::RefreshDriver() {
    m_driver = QueryDriverRegistration();
    m_driverQueried = true;
}

DriverSetup SetupTasks::DriverState() const {
    if (!m_driverQueried || !m_driver.error.empty()) {
        return DriverSetup::Unknown;
    }
    return m_driver.registered ? DriverSetup::Registered : DriverSetup::Missing;
}

void SetupTasks::Refresh() {
    RefreshDriver();
    const auto hostExePath = win32::ExecutablePath().wstring();
    m_vrServerPath = VrServerPath(m_driver.runtimePath);
    std::error_code error;
    m_canAddFirewall = !hostExePath.empty() && !m_vrServerPath.empty()
        && std::filesystem::is_regular_file(m_vrServerPath, error);
    m_firewall = hostExePath.empty() ? FirewallStatus{FirewallRuleState::Unknown, "ホストのパスを取得できません"}
        : QueryFirewallRules(hostExePath, m_vrServerPath);
    m_nextRefresh = std::chrono::steady_clock::now() + kRefreshInterval;
}

void SetupTasks::Update(bool windowVisible) {
    std::optional<Outcome> finished;
    {
        std::lock_guard lock(m_mutex);
        finished = std::exchange(m_finished, std::nullopt);
    }
    if (finished) {
        m_thread.join();
        m_busy = false;
        m_outcome = std::move(finished);
        Refresh();
    } else if (!m_busy && windowVisible && std::chrono::steady_clock::now() >= m_nextRefresh) {
        // SteamVR の入れ直しや、ほかのツールでの変更に追従する。
        Refresh();
    }
}

void SetupTasks::Start(Action action, HWND owner, bool steamVrRunning) {
    if (m_busy) {
        return;
    }
    const char* label = ActionLabel(action);
    LogInfo("設定画面のボタン: %s", label);
    m_outcome.reset();
    m_busy = true;
    const bool needsSteamVrRestart = IsDriverAction(action) && steamVrRunning;
    try {
        m_thread = std::jthread([this, action, owner, label, needsSteamVrRestart,
            driver = m_driver, vrServerPath = m_vrServerPath] {
            Outcome outcome{action, false, {}, needsSteamVrRestart};
            try {
                outcome.success = Run(action, driver, vrServerPath, owner, outcome.error);
            } catch (const std::exception&) {
                outcome.error = "登録処理で例外が発生しました";
            } catch (...) {
                outcome.error = "登録処理で予期しないエラーが発生しました";
            }
            if (outcome.success) {
                LogInfo("%s: 成功", label);
            } else {
                LogWarn("%s: %s", label, outcome.error.c_str());
            }
            std::lock_guard lock(m_mutex);
            m_finished = std::move(outcome);
        });
    } catch (const std::exception&) {
        m_busy = false;
        m_outcome = Outcome{action, false, "処理スレッドを開始できません", false};
        LogWarn("%s: %s", label, m_outcome->error.c_str());
    }
}

}
