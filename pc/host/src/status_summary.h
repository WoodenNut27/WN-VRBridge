#pragma once

#include "host_status.h"

#include <wnvr/host_ipc/host_ipc.h>

#include <optional>
#include <string>

namespace wnvr::host {

// 状態の「色」。設定画面の見出しとタスクトレイのアイコンで同じ区分を使う。
enum class Tone {
    Good,    // Quest とつながって映像を送っている
    Busy,    // 進行中（SteamVR を起動している、Quest を待っている など）
    Idle,    // 待機中。問題はない
    Warning, // 使うにはユーザーの操作が要る
    Error,   // 失敗していて、自動のやり直しを待っている
};

// ドライバが SteamVR に登録されているか。確認できないときは Unknown（警告は出さない）。
enum class DriverSetup { Registered, Missing, Unknown };

struct StatusSummary {
    Tone tone = Tone::Idle;
    std::string title;  // 1 行の見出し
    std::string detail; // 今起きていること、または次にすること
};

// ホストの状態・ドライバからの統計・ドライバの登録の有無から、利用者に見せる 1 つの要約を作る。
StatusSummary Summarize(const HostStatus& status, const std::optional<ipc::StatsSnapshot>& stats,
    DriverSetup driverSetup);

// 「PC（このアプリ）」「SteamVR」「Quest」それぞれの 1 行の説明。
struct LinkStatus {
    Tone tone = Tone::Idle;
    std::string text;
};
LinkStatus DescribeHost(const HostStatus& status);
LinkStatus DescribeSteamVr(const HostStatus& status);
LinkStatus DescribeQuest(const HostStatus& status, const std::optional<ipc::StatsSnapshot>& stats);

LinkStatus DescribeMic(bool micToPc, const std::optional<ipc::StatsSnapshot>& stats);

// 2 つの音の設定から、音が出る場所を言葉にする。
const char* DescribeAudioRoute(bool streamToQuest, bool muteSpeaker);

}
