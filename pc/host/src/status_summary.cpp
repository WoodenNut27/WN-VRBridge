#include "status_summary.h"

#include <wnvr/protocol/messages.h>

namespace wnvr::host {
namespace {

bool IsQuestConnected(const HostStatus& status, const std::optional<ipc::StatsSnapshot>& stats) {
    return status.state == HostState::DriverConnected && stats && stats->clientConnected;
}

std::string QuestName(const ipc::StatsSnapshot& stats) {
    return stats.clientName.empty() ? std::string("Quest") : stats.clientName;
}

std::string ListeningText(const HostStatus& status) {
    const auto port = std::to_string(protocol::kDefaultPort);
    // 0.0.0.0 は利用者には意味が伝わらないので、言葉に置き換える。
    if (status.bindAddress.empty() || status.bindAddress == "0.0.0.0") {
        return "すべてのアダプターで待ち受けています（UDP " + port + "）";
    }
    return status.bindAddress + ":" + port + " で待ち受けています";
}

}

StatusSummary Summarize(const HostStatus& status, const std::optional<ipc::StatsSnapshot>& stats,
    DriverSetup driverSetup) {
    // 登録の状態を確認できていれば、それを優先する（起動を見送ったときの記録は古いことがある）。
    const bool driverMissing = driverSetup == DriverSetup::Missing
        || (driverSetup == DriverSetup::Unknown && status.driverMissingAtLaunch);
    switch (status.state) {
    case HostState::DriverConnected:
        if (IsQuestConnected(status, stats)) {
            return {Tone::Good, "Quest とつながっています",
                QuestName(*stats) + " に SteamVR の映像と音を送っています。"};
        }
        return {Tone::Busy, "Quest の接続を待っています",
            "SteamVR は起動しています。Quest で WN-VRBridge を起動してください。"};
    case HostState::LaunchingSteamVr:
        return {Tone::Busy, "SteamVR を起動しています",
            "Quest から接続の要求が届きました。SteamVR の準備ができるとつながります（"
                + std::to_string(status.launchSeconds) + " 秒経過）。"};
    case HostState::ListenFailed:
        return {Tone::Error, "待ち受けを始められません",
            status.listenError + "\n5 秒ごとにやり直しています。ほかのアプリが同じポート（UDP "
                + std::to_string(protocol::kDiscoveryPort) + "・" + std::to_string(protocol::kDefaultPort)
                + "）を使っていないか確かめてください。"};
    case HostState::WaitingForDriverExit:
    case HostState::Listening:
        break;
    }
    if (status.autoLaunchSuppressed) {
        return {Tone::Idle, "SteamVR は終了しました",
            status.autoStartSteamVr
                ? "もう一度使うときは、Quest でトリガーを引くか、Quest のアプリを起動し直してください。"
                : "もう一度使うときは、Quest でトリガーを引いてください。"};
    }
    if (status.state == HostState::WaitingForDriverExit) {
        return {Tone::Busy, "待ち受けの準備をしています", "SteamVR の終了を確認しています。数秒お待ちください。"};
    }
    if (driverMissing) {
        return {Tone::Warning, "セットアップが終わっていません",
            status.driverMissingAtLaunch
                ? "Quest から接続の要求が届きましたが、SteamVR にドライバが登録されていないため起動できません。"
                  "「セットアップ」のタブで登録してください。"
                : "SteamVR にドライバが登録されていません。「セットアップ」のタブで登録してください。"};
    }
    if (!status.launchError.empty()) {
        return {Tone::Warning, "SteamVR を起動できませんでした",
            status.launchError + "\nQuest が接続を求めているあいだは、自動でもう一度試します。"};
    }
    if (!status.autoStartSteamVr) {
        return {Tone::Idle, "Quest での起動操作を待っています",
            "Quest で WN-VRBridge を起動し、トリガーを引くと SteamVR が起動してつながります。"};
    }
    return {Tone::Idle, "Quest の接続を待っています",
        "Quest で WN-VRBridge を起動すると、SteamVR が自動で起動してつながります。"};
}

LinkStatus DescribeHost(const HostStatus& status) {
    switch (status.state) {
    case HostState::Listening:
        return {Tone::Good, ListeningText(status)};
    case HostState::ListenFailed:
        return {Tone::Error, "待ち受けに失敗しました（" + status.listenError + "）"};
    case HostState::WaitingForDriverExit:
        return {Tone::Busy, "待ち受けの準備をしています"};
    case HostState::LaunchingSteamVr:
        return {Tone::Busy, "待ち受けを SteamVR に引き継いでいます"};
    case HostState::DriverConnected:
        return {Tone::Good, "SteamVR のドライバが待ち受けています"};
    }
    return {};
}

LinkStatus DescribeSteamVr(const HostStatus& status) {
    switch (status.state) {
    case HostState::DriverConnected:
        return {Tone::Good, "起動しています"};
    case HostState::LaunchingSteamVr:
        return {Tone::Busy, "起動しています…（" + std::to_string(status.launchSeconds) + " 秒経過）"};
    case HostState::WaitingForDriverExit:
    case HostState::Listening:
    case HostState::ListenFailed:
        break;
    }
    if (status.autoLaunchSuppressed) {
        return {Tone::Idle, "終了しました（Quest で操作するまで、自動では起動しません）"};
    }
    return {Tone::Idle, status.autoStartSteamVr
        ? "起動していません（Quest がつながると自動で起動します）"
        : "起動していません（Quest でトリガーを引くと起動します）"};
}

LinkStatus DescribeQuest(const HostStatus& status, const std::optional<ipc::StatsSnapshot>& stats) {
    if (IsQuestConnected(status, stats)) {
        return {Tone::Good, QuestName(*stats) + " とつながっています"};
    }
    if (status.state == HostState::LaunchingSteamVr) {
        return {Tone::Busy, "接続の要求が届いています"};
    }
    return {Tone::Idle, "つながっていません"};
}

LinkStatus DescribeMic(bool micToPc, const std::optional<ipc::StatsSnapshot>& stats) {
    if (!micToPc) {
        return {Tone::Idle, "使っていません"};
    }
    if (!stats || !stats->clientConnected) {
        return {Tone::Idle, "Quest がつながると使えます"};
    }
    const auto pc = static_cast<ipc::PcMicState>(stats->pcMicState);
    const auto quest = static_cast<protocol::QuestMicState>(stats->questMicState);
    if (pc == ipc::PcMicState::CableMissing) {
        return {Tone::Warning, "VB-CABLE が見つかりません。インストールしてください"};
    }
    if (quest == protocol::QuestMicState::PermissionNeeded) {
        return {Tone::Warning, "Quest でマイクの使用を許可してください"};
    }
    if (quest == protocol::QuestMicState::Failed) {
        return {Tone::Error, "Quest のマイクを開けませんでした。Quest のアプリを起動し直してください"};
    }
    if (pc == ipc::PcMicState::Playing && quest == protocol::QuestMicState::Capturing) {
        return {Tone::Good, "Quest のマイクの音を CABLE Output に送っています"};
    }
    return {Tone::Busy, "Quest からマイクの音が届くのを待っています"};
}

const char* DescribeAudioRoute(bool streamToQuest, bool muteSpeaker) {
    if (streamToQuest) {
        return muteSpeaker ? "接続中は、音は Quest だけから出ます。" : "接続中は、音は PC と Quest の両方から出ます。";
    }
    return muteSpeaker ? "接続中は、どこからも音が出ません。" : "接続中も、音は PC だけから出ます。";
}

}
