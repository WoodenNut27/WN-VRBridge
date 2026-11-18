#include "status_summary.h"
#include "host_listener.h"

#include <doctest/doctest.h>

#include <string>

using namespace wnvr::host;
using wnvr::ipc::StatsSnapshot;

namespace {

HostStatus Status(HostState state) {
    HostStatus status;
    status.state = state;
    status.bindAddress = "192.168.1.31";
    return status;
}

StatsSnapshot Connected(const char* name) {
    StatsSnapshot stats;
    stats.clientConnected = true;
    stats.clientName = name;
    return stats;
}

bool Contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

}

TEST_CASE("Quest とつながっているときは緑で、Quest の名前を出す") {
    const auto summary = Summarize(Status(HostState::DriverConnected), Connected("Meta Quest 3"),
        DriverSetup::Registered);
    CHECK(summary.tone == Tone::Good);
    CHECK(summary.title == "Quest とつながっています");
    CHECK(Contains(summary.detail, "Meta Quest 3"));
}

TEST_CASE("SteamVR だけが動いているときは、Quest を待っていると伝える") {
    for (const auto& stats : {std::optional<StatsSnapshot>{}, std::optional<StatsSnapshot>{StatsSnapshot{}}}) {
        const auto summary = Summarize(Status(HostState::DriverConnected), stats, DriverSetup::Registered);
        CHECK(summary.tone == Tone::Busy);
        CHECK(Contains(summary.title, "待っています"));
    }
}

TEST_CASE("ドライバのパイプが切れたら、古い統計が残っていても「つながっている」とは言わない") {
    const auto summary = Summarize(Status(HostState::Listening), Connected("Meta Quest 3"),
        DriverSetup::Registered);
    CHECK(summary.tone == Tone::Idle);
    CHECK(DescribeQuest(Status(HostState::Listening), Connected("Meta Quest 3")).tone == Tone::Idle);
}

TEST_CASE("待ち受け中は、ドライバが未登録なら先にセットアップを促す") {
    auto status = Status(HostState::Listening);
    CHECK(Summarize(status, std::nullopt, DriverSetup::Registered).tone == Tone::Idle);
    const auto missing = Summarize(status, std::nullopt, DriverSetup::Missing);
    CHECK(missing.tone == Tone::Warning);
    CHECK(Contains(missing.detail, "セットアップ"));
    // 登録の状態を確認できていないときは、起動を見送った記録があるときだけ促す。
    CHECK(Summarize(status, std::nullopt, DriverSetup::Unknown).tone == Tone::Idle);
    status.driverMissingAtLaunch = true;
    CHECK(Summarize(status, std::nullopt, DriverSetup::Unknown).tone == Tone::Warning);
    // 確認できていて登録済みなら、古い記録は無視する。
    CHECK(Summarize(status, std::nullopt, DriverSetup::Registered).tone == Tone::Idle);
}

TEST_CASE("利用者が SteamVR を終了したあとは、もう一度使う方法を伝える") {
    for (const auto state : {HostState::WaitingForDriverExit, HostState::Listening}) {
        auto status = Status(state);
        status.autoLaunchSuppressed = true;
        const auto summary = Summarize(status, std::nullopt, DriverSetup::Missing);
        CHECK(summary.title == "SteamVR は終了しました");
        CHECK(Contains(summary.detail, "トリガー"));
        CHECK(Contains(DescribeSteamVr(status).text, "終了しました"));
    }
}

TEST_CASE("SteamVR の起動中は経過した秒数を出す") {
    auto status = Status(HostState::LaunchingSteamVr);
    status.launchSeconds = 12;
    const auto summary = Summarize(status, std::nullopt, DriverSetup::Registered);
    CHECK(summary.tone == Tone::Busy);
    CHECK(Contains(summary.detail, "12 秒"));
    CHECK(Contains(DescribeSteamVr(status).text, "12 秒"));
    CHECK(DescribeQuest(status, std::nullopt).tone == Tone::Busy);
}

TEST_CASE("起動や待ち受けの失敗は、理由を添えて伝える") {
    auto failed = Status(HostState::ListenFailed);
    failed.listenError = "192.168.1.31:9944 を開けません";
    const auto listen = Summarize(failed, std::nullopt, DriverSetup::Registered);
    CHECK(listen.tone == Tone::Error);
    CHECK(Contains(listen.detail, "192.168.1.31:9944 を開けません"));
    CHECK(DescribeHost(failed).tone == Tone::Error);

    auto launch = Status(HostState::Listening);
    launch.launchError = "起動できませんでした";
    const auto summary = Summarize(launch, std::nullopt, DriverSetup::Registered);
    CHECK(summary.tone == Tone::Warning);
    CHECK(Contains(summary.detail, "起動できませんでした"));
}

TEST_CASE("待ち受けのアドレスは、すべてのアダプターのときは言葉で出す") {
    auto status = Status(HostState::Listening);
    CHECK(Contains(DescribeHost(status).text, "192.168.1.31:9944"));
    status.bindAddress = "0.0.0.0";
    CHECK(Contains(DescribeHost(status).text, "すべてのアダプター"));
    CHECK(!Contains(DescribeHost(status).text, "0.0.0.0"));
}

TEST_CASE("音が出る場所は 4 通りを言い分ける") {
    const std::string both = DescribeAudioRoute(true, false);
    const std::string questOnly = DescribeAudioRoute(true, true);
    const std::string pcOnly = DescribeAudioRoute(false, false);
    const std::string none = DescribeAudioRoute(false, true);
    CHECK(Contains(questOnly, "Quest だけ"));
    CHECK(Contains(both, "両方"));
    CHECK(Contains(pcOnly, "PC だけ"));
    CHECK(Contains(none, "どこからも"));
}

TEST_CASE("マイクの状態から今起きていることと次の操作を伝える") {
    CHECK(DescribeMic(false, std::nullopt).tone == Tone::Idle);
    CHECK(DescribeMic(false, std::nullopt).text == "使っていません");
    CHECK(DescribeMic(true, std::nullopt).text == "Quest がつながると使えます");
    auto stats = Connected("Quest");
    stats.pcMicState = 1;
    CHECK(DescribeMic(true, stats).tone == Tone::Warning);
    CHECK(DescribeMic(true, stats).text == "VB-CABLE が見つかりません。インストールしてください");
    stats.pcMicState = 2;
    stats.questMicState = 1;
    CHECK(DescribeMic(true, stats).tone == Tone::Warning);
    CHECK(DescribeMic(true, stats).text == "Quest でマイクの使用を許可してください");
    stats.questMicState = 3;
    CHECK(DescribeMic(true, stats).tone == Tone::Error);
    CHECK(DescribeMic(true, stats).text == "Quest のマイクを開けませんでした。Quest のアプリを起動し直してください");
    stats.pcMicState = 3;
    stats.questMicState = 2;
    CHECK(DescribeMic(true, stats).tone == Tone::Good);
    CHECK(DescribeMic(true, stats).text == "Quest のマイクの音を CABLE Output に送っています");
    CHECK(DescribeMic(false, stats).text == "使っていません");
    stats.clientConnected = false;
    CHECK(DescribeMic(true, stats).tone == Tone::Idle);
    CHECK(DescribeMic(true, stats).text == "Quest がつながると使えます");
    stats.clientConnected = true;
    for (const int pc : {0, 2, 3}) {
        stats.pcMicState = pc;
        stats.questMicState = 0;
        CHECK(DescribeMic(true, stats).tone == Tone::Busy);
        CHECK(DescribeMic(true, stats).text == "Quest からマイクの音が届くのを待っています");
    }
    stats.pcMicState = 2;
    stats.questMicState = 2;
    CHECK(DescribeMic(true, stats).tone == Tone::Busy);
}

TEST_CASE("自動起動の設定に合わせて待ち受け中の操作を案内する") {
    auto status = Status(HostState::Listening);
    REQUIRE(status.autoStartSteamVr);
    const auto automatic = Summarize(status, std::nullopt, DriverSetup::Registered);
    CHECK(automatic.title == "Quest の接続を待っています");
    CHECK(automatic.detail == "Quest で WN-VRBridge を起動すると、SteamVR が自動で起動してつながります。");
    CHECK(DescribeSteamVr(status).text == "起動していません（Quest がつながると自動で起動します）");

    status.autoStartSteamVr = false;
    const auto manual = Summarize(status, std::nullopt, DriverSetup::Registered);
    CHECK(manual.title == "Quest での起動操作を待っています");
    CHECK(Contains(manual.detail, "トリガー"));
    CHECK(Contains(DescribeSteamVr(status).text, "トリガー"));
    status.autoLaunchSuppressed = true;
    const auto suppressed = Summarize(status, std::nullopt, DriverSetup::Registered);
    CHECK(Contains(suppressed.detail, "トリガー"));
    CHECK_FALSE(Contains(suppressed.detail, "起動し直して"));
}

TEST_CASE("Hello の承認・拒否・抑止・自動起動・起動要求の全組み合わせ") {
    using wnvr::protocol::HelloResult;
    for (const bool approved : {false, true}) {
        for (const bool rejected : {false, true}) {
            for (const bool suppressed : {false, true}) {
                for (const bool autoStart : {false, true}) {
                    for (const bool requested : {false, true}) {
                        CAPTURE(approved);
                        CAPTURE(rejected);
                        CAPTURE(suppressed);
                        CAPTURE(autoStart);
                        CAPTURE(requested);
                        const auto actual = DecideHelloResult(approved, rejected, suppressed, autoStart, requested);
                        if (!approved && rejected) {
                            CHECK(actual == HelloResult::Rejected);
                        } else if (!approved) {
                            CHECK(actual == HelloResult::PendingApproval);
                        } else if (suppressed) {
                            CHECK(actual == HelloResult::Stopped);
                        } else if (!autoStart && !requested) {
                            CHECK(actual == HelloResult::Stopped);
                        } else {
                            CHECK(actual == HelloResult::Starting);
                        }
                    }
                }
            }
        }
    }
}
