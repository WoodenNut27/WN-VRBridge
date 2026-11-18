#include "stats_collector.h"

#include "log_stub.h"
#include "video_encoder.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>

using namespace wnvr::driver;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

// 10 秒の窓を閉じる時刻。窓は「今」から始まるので、ちょうど 10 秒にはならない。割合は近い値で確かめる。
Clock::time_point AfterTenSeconds() {
    return Clock::now() + 10s;
}

wnvr::protocol::ClientStatsMessage QuestReport() {
    wnvr::protocol::ClientStatsMessage stats;
    stats.periodUs = 1'000'000;
    stats.receivedFrames = 72;
    stats.lostFrames = 1;
    stats.receivedFragments = 900;
    stats.receivedVideoBytes = 1'250'000; // 1 秒で 10 Mbps
    stats.decodedFrames = 70;
    stats.decodeAverageUs = 12'000;
    stats.decodeMaxUs = 20'000;
    stats.displayedFrames = 72;
    stats.repeatedFrames = 2;
    stats.latencyAverageUs = 50'000;
    stats.latencyMaxUs = 64'000;
    stats.roundTripUs = 3'000;
    stats.audioUnderruns = 1;
    stats.audioMissingFrames = 240;
    stats.audioBufferAverageUs = 30'000;
    stats.firstFragmentAverageUs = 11'000;
    stats.assembleAverageUs = 2'000;
    stats.decoderWaitAverageUs = 1'000;
    stats.displayWaitAverageUs = 25'000;
    return stats;
}

bool Contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

}

TEST_CASE("窓を閉じる前は、接続の情報だけが分かる") {
    StatsCollector collector;
    CHECK(!collector.GetSnapshot().clientConnected);
    collector.OnClientConnected("Meta Quest 3", 0x1234);
    collector.OnRenderedFrame();
    const auto snapshot = collector.GetSnapshot();
    CHECK(snapshot.clientConnected);
    CHECK(snapshot.clientName == "Meta Quest 3");
    CHECK(snapshot.clientSessionId == 0x1234);
    CHECK(snapshot.renderFps == 0);
    // 5 秒たっていなければ窓は閉じない。
    collector.LogIfDue();
    CHECK(collector.GetSnapshot().renderFps == 0);
}

TEST_CASE("窓を閉じると、平均と割合が計算される") {
    testing::LogLines().clear();
    StatsCollector collector;
    collector.SetVirtualVsyncHz(71.94);
    collector.OnClientConnected("Meta Quest 3", 7);
    for (int index = 0; index < 720; ++index) {
        collector.OnRenderedFrame();
    }
    for (int index = 0; index < 700; ++index) {
        collector.OnEncodedFrame(index == 0 ? 9.0 : 3.0);
    }
    collector.OnVideoSent(1000, 12'500'000); // 10 秒で 10 Mbps
    collector.OnSendDroppedFrame();
    collector.OnClientStats(QuestReport());
    collector.OnClientStats(QuestReport());

    collector.LogIfDue(AfterTenSeconds());
    const auto stats = collector.GetSnapshot();
    CHECK(stats.clientConnected);
    CHECK(stats.renderFps == doctest::Approx(72.0).epsilon(0.01));
    CHECK(stats.encodeFps == doctest::Approx(70.0).epsilon(0.01));
    CHECK(stats.encodeAverageMs == doctest::Approx((9.0 + 699 * 3.0) / 700));
    CHECK(stats.encodeMaxMs == 9.0);
    CHECK(stats.sentMbps == doctest::Approx(10.0).epsilon(0.01));
    CHECK(stats.sentFragments == 1000);
    CHECK(stats.sendDroppedFrames == 1);
    CHECK(stats.virtualVsyncHz == 71.94);
    // Quest の値は、Quest が報告した期間（1 秒 × 2 回）で割る。
    CHECK(stats.receivedFps == doctest::Approx(72.0));
    CHECK(stats.receivedMbps == doctest::Approx(10.0));
    CHECK(stats.displayFps == doctest::Approx(72.0));
    CHECK(stats.lostFrames == 2);
    CHECK(stats.repeatedFrames == 4);
    // 10 秒で 1000 個を送り、報告のあった 2 秒に届いたのは 1800 個。その 2 秒に送ったはずの数
    // （200 個）より多いので、損失は負にせず 0 にする。
    CHECK(stats.fragmentLossPercent == 0);
    CHECK(stats.decodeAverageMs == doctest::Approx(12.0));
    CHECK(stats.decodeMaxMs == doctest::Approx(20.0));
    CHECK(stats.latencyAverageMs == doctest::Approx(50.0));
    CHECK(stats.latencyMaxMs == doctest::Approx(64.0));
    CHECK(stats.roundTripMs == doctest::Approx(3.0));
    CHECK(stats.audioUnderruns == 2);
    CHECK(stats.audioMissingFrames == 480);
    CHECK(stats.audioBufferAverageMs == doctest::Approx(30.0));

    // ログには統計と、遅延の内訳が出る。
    REQUIRE(testing::LogLines().size() == 2);
    CHECK(Contains(testing::LogLines()[0], "統計: 接続=Meta Quest 3"));
    CHECK(Contains(testing::LogLines()[1], "姿勢→最初の断片=11.0 組み立て=2.0 デコード待ち=1.0 デコード=12.0 表示待ち=25.0 合計=51.0"));
}

TEST_CASE("断片の損失は、Quest が報告した期間に送ったはずの数と比べる") {
    // 10 秒の窓で 9000 個（1 秒に 900 個）を送り、1 秒ごとの報告が reports 個届いたときの損失。
    const auto lossWith = [](int reports, uint32_t receivedPerReport) {
        StatsCollector collector;
        collector.OnClientConnected("Quest", 1);
        collector.OnVideoSent(9000, 0);
        auto report = QuestReport();
        report.receivedFragments = receivedPerReport;
        for (int index = 0; index < reports; ++index) {
            collector.OnClientStats(report);
        }
        collector.LogIfDue(AfterTenSeconds());
        return collector.GetSnapshot().fragmentLossPercent;
    };
    // すべて届いていれば、窓に入った報告が 1〜2 個少なくても多くても、損失は 0。
    // （実機で、何も失われていないのに約 20% と出ることがあった。報告の数が窓ごとに揺れるため）
    for (const int reports : {8, 10, 12}) {
        CAPTURE(reports);
        CHECK(lossWith(reports, 900) == 0);
    }
    // 1 割が届かなければ、報告の数によらず約 10%。
    for (const int reports : {8, 10, 12}) {
        CAPTURE(reports);
        CHECK(lossWith(reports, 810) == doctest::Approx(10.0).epsilon(0.01));
    }
    // 報告が 1 つも無ければ、比べる相手が無いので 0 にする。
    CHECK(lossWith(0, 0) == 0);
}

TEST_CASE("PC の中の区間は、測れた時刻の差の平均になる") {
    testing::LogLines().clear();
    StatsCollector collector;
    collector.OnClientConnected("Quest", 1);
    collector.OnClientStats(QuestReport());
    const auto base = Clock::now();
    FrameTiming timing{base, base + 4ms, base + 7ms};
    collector.OnFrameTiming(timing, base + 8ms, base + 9ms);
    // 姿勢を受け取った時刻が無いフレームは、最初の区間だけ数えない。
    timing.poseReceived = {};
    collector.OnFrameTiming(timing, base + 10ms, base + 9ms);
    collector.LogIfDue(AfterTenSeconds());
    REQUIRE(testing::LogLines().size() == 2);
    const auto& line = testing::LogLines()[1];
    // 送信待ちは (1 + 3) / 2、送信は逆転した 2 つ目を除いた 1。回線は 11 − (4 + 3 + 2)。
    CHECK(Contains(line, "PC[姿勢の受信→描画=4.0 描画→エンコード完了=3.0 (うちエンコード=0.0) 送信待ち=2.0 送信=1.0]"));
    CHECK(Contains(line, "回線(行き+帰り)=2.0"));
}

TEST_CASE("切断すると、古い統計は残さない") {
    testing::LogLines().clear();
    StatsCollector collector;
    collector.OnClientConnected("Quest", 1);
    collector.OnRenderedFrame();
    collector.LogIfDue(AfterTenSeconds());
    collector.OnClientDisconnected();
    const auto snapshot = collector.GetSnapshot();
    CHECK(!snapshot.clientConnected);
    CHECK(snapshot.clientName.empty());
    CHECK(snapshot.clientSessionId == 0);
    CHECK(snapshot.renderFps == 0);
    // つながっていないあいだの窓は「未接続」とだけ出す。
    testing::LogLines().clear();
    collector.LogIfDue(Clock::now() + 20s);
    REQUIRE(testing::LogLines().size() == 1);
    CHECK(testing::LogLines()[0] == "統計: 未接続 mic(pc=0 quest=0)");
}

TEST_CASE("接続し直すと、前の接続の値を持ち越さない") {
    StatsCollector collector;
    collector.SetVirtualVsyncHz(72.0);
    collector.OnClientConnected("First", 1);
    collector.OnVideoSent(500, 1000);
    collector.OnClientStats(QuestReport());
    collector.OnClientConnected("Second", 2);
    collector.LogIfDue(AfterTenSeconds());
    const auto stats = collector.GetSnapshot();
    CHECK(stats.clientName == "Second");
    CHECK(stats.sentFragments == 0);
    CHECK(stats.receivedFps == 0);
    CHECK(stats.lostFrames == 0);
    // 仮想の垂直同期の周波数は、接続をまたいで引き継ぐ。
    CHECK(stats.virtualVsyncHz == 72.0);
}

TEST_CASE("マイクの状態は窓を閉じる前後とも最新値で、切断でQuest側を戻す") {
    StatsCollector collector;
    collector.SetPcMicState(1);
    CHECK(collector.GetSnapshot().pcMicState == 1);
    collector.OnClientConnected("Quest", 1);
    auto report = QuestReport();
    report.micState = 1;
    collector.OnClientStats(report);
    CHECK(collector.GetSnapshot().questMicState == 1);
    report.micState = 2;
    collector.OnClientStats(report);
    collector.SetPcMicState(3);
    collector.LogIfDue(AfterTenSeconds());
    CHECK(collector.GetSnapshot().pcMicState == 3);
    CHECK(collector.GetSnapshot().questMicState == 2);
    report.micState = 3;
    collector.OnClientStats(report);
    CHECK(collector.GetSnapshot().questMicState == 3);
    report.periodUs = 0;
    report.micState = 1;
    collector.OnClientStats(report);
    CHECK(collector.GetSnapshot().questMicState == 1);
    collector.OnClientDisconnected();
    CHECK(collector.GetSnapshot().questMicState == 0);
    CHECK(collector.GetSnapshot().pcMicState == 3);
}
