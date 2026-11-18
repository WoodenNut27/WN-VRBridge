#include "stats_collector.h"
#include "driver_log.h"
#include "video_encoder.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace wnvr::driver {
namespace {

constexpr double kWindowSeconds = 5.0;

double Ratio(double total, uint64_t count) {
    return count ? total / static_cast<double>(count) : 0.0;
}

double Mbps(uint64_t bytes, double seconds) {
    return seconds > 0 ? static_cast<double>(bytes) * 8.0 / seconds / 1000000.0 : 0.0;
}

}

void StatsCollector::StartWindowLocked(bool connected, std::string name, uint32_t sessionId) {
    const double hz = m_window.virtualVsyncHz;
    m_window = {};
    m_window.clientConnected = connected;
    m_window.clientName = std::move(name);
    m_window.clientSessionId = sessionId;
    m_window.virtualVsyncHz = hz;
    m_totals = {};
    m_windowStart = Clock::now();
}

void StatsCollector::OnClientConnected(const std::string& name, uint32_t sessionId) {
    std::lock_guard lock(m_mutex);
    // 接続前や別の端末の計測を、新しい接続へ持ち越さない。
    StartWindowLocked(true, name, sessionId);
    m_questMicState = 0;
    m_snapshot = {};
}

void StatsCollector::OnClientDisconnected() {
    std::lock_guard lock(m_mutex);
    m_window.clientConnected = false;
    m_questMicState = 0;
    m_window.clientName.clear();
    m_window.clientSessionId = 0;
    // 切断後に古い端末名や統計が表示され続けないよう、確定値も無効化する。
    m_snapshot = {};
}

void StatsCollector::OnClientStats(const protocol::ClientStatsMessage& stats) {
    std::lock_guard lock(m_mutex);
    if (!m_window.clientConnected) {
        return;
    }
    m_questMicState = stats.micState;
    if (stats.periodUs == 0) {
        return;
    }
    m_totals.clientPeriodUs += stats.periodUs;
    m_totals.received += stats.receivedFrames;
    m_totals.receivedFragments += stats.receivedFragments;
    m_totals.receivedBytes += stats.receivedVideoBytes;
    m_window.lostFrames += stats.lostFrames;
    m_totals.decoded += stats.decodedFrames;
    m_totals.decodeTotalUs += static_cast<double>(stats.decodeAverageUs) * stats.decodedFrames;
    m_window.decodeMaxMs = std::max(m_window.decodeMaxMs, stats.decodeMaxUs / 1000.0);
    m_totals.displayed += stats.displayedFrames;
    m_window.repeatedFrames += stats.repeatedFrames;
    // 遅延は同じ画像の再表示時には測らないため、新しい画像の表示回数で重み付けする。
    const auto latencyFrames = stats.displayedFrames - std::min(stats.displayedFrames, stats.repeatedFrames);
    m_totals.latencyFrames += static_cast<uint64_t>(latencyFrames);
    m_totals.latencyTotalUs += static_cast<double>(stats.latencyAverageUs) * latencyFrames;
    m_window.latencyMaxMs = std::max(m_window.latencyMaxMs, stats.latencyMaxUs / 1000.0);
    m_window.roundTripMs = stats.roundTripUs / 1000.0;
    m_window.audioUnderruns += stats.audioUnderruns;
    m_window.audioMissingFrames += stats.audioMissingFrames;
    // プロトコルには音声コールバック数がないため、期間で重み付けする。
    m_totals.audioBufferTotal += static_cast<double>(stats.audioBufferAverageUs) * stats.periodUs;
    // Quest は区間ごとの平均を送ってくるので、その区間を測ったフレーム数で重み付けして足す。
    const std::array<uint32_t, kQuestIntervalCount> averages{stats.firstFragmentAverageUs,
        stats.assembleAverageUs, stats.decoderWaitAverageUs, stats.displayWaitAverageUs};
    const std::array<uint64_t, kQuestIntervalCount> counts{stats.receivedFrames, stats.receivedFrames,
        stats.decodedFrames, static_cast<uint64_t>(latencyFrames)};
    for (size_t index = 0; index < averages.size(); ++index) {
        if (averages[index] != 0) {
            m_totals.questIntervals[index].Add(averages[index] / 1000.0, counts[index]);
        }
    }
}

void StatsCollector::OnFrameTiming(const FrameTiming& timing, Clock::time_point firstSent,
    Clock::time_point lastSent) {
    std::lock_guard lock(m_mutex);
    // 隣り合う時刻の差が、そのまま PcInterval の各区間になる。
    const std::array<Clock::time_point, kPcIntervalCount + 1> times{timing.poseReceived, timing.presented,
        timing.encoded, firstSent, lastSent};
    for (size_t index = 0; index < m_totals.pcIntervals.size(); ++index) {
        // 測れなかった時刻（既定値のまま）や、逆転した時刻は数えない。
        if (times[index] != Clock::time_point{} && times[index + 1] != Clock::time_point{}
            && times[index + 1] >= times[index]) {
            m_totals.pcIntervals[index].Add(
                std::chrono::duration<double, std::milli>(times[index + 1] - times[index]).count());
        }
    }
}

void StatsCollector::OnRenderedFrame() {
    std::lock_guard lock(m_mutex);
    ++m_totals.rendered;
}

void StatsCollector::OnEncodedFrame(double encodeMs) {
    std::lock_guard lock(m_mutex);
    ++m_totals.encoded;
    m_totals.encodeTotalMs += encodeMs;
    m_window.encodeMaxMs = std::max(m_window.encodeMaxMs, encodeMs);
}

void StatsCollector::OnVideoSent(uint64_t fragments, uint64_t payloadBytes) {
    std::lock_guard lock(m_mutex);
    m_window.sentFragments += fragments;
    m_totals.sentBytes += payloadBytes;
}

void StatsCollector::OnSendDroppedFrame() {
    std::lock_guard lock(m_mutex);
    ++m_window.sendDroppedFrames;
}

void StatsCollector::SetPcMicState(int state) {
    std::lock_guard lock(m_mutex);
    m_pcMicState = state;
}

void StatsCollector::SetVirtualVsyncHz(double hz) {
    std::lock_guard lock(m_mutex);
    if (std::isfinite(hz) && hz > 0) {
        m_window.virtualVsyncHz = hz;
    }
}

StatsSnapshot StatsCollector::GetSnapshot() const {
    std::lock_guard lock(m_mutex);
    auto snapshot = m_snapshot;
    snapshot.clientConnected = m_window.clientConnected;
    snapshot.clientName = m_window.clientName;
    snapshot.clientSessionId = m_window.clientSessionId;
    snapshot.pcMicState = m_pcMicState;
    snapshot.questMicState = m_questMicState;
    return snapshot;
}

void StatsCollector::FinishWindowLocked(double seconds) {
    const auto& totals = m_totals;
    m_window.renderFps = static_cast<double>(totals.rendered) / seconds;
    m_window.encodeFps = static_cast<double>(totals.encoded) / seconds;
    m_window.encodeAverageMs = Ratio(totals.encodeTotalMs, totals.encoded);
    m_window.sentMbps = Mbps(totals.sentBytes, seconds);
    // Quest の値は、Quest が報告した期間の合計で割る（PC の窓と境目がずれるため）。
    const double clientSeconds = static_cast<double>(totals.clientPeriodUs) / 1000000.0;
    m_window.receivedFps = clientSeconds > 0 ? static_cast<double>(totals.received) / clientSeconds : 0;
    m_window.receivedMbps = Mbps(totals.receivedBytes, clientSeconds);
    m_window.displayFps = clientSeconds > 0 ? static_cast<double>(totals.displayed) / clientSeconds : 0;
    // 損失は、Quest が報告した期間に送ったはずの数（送った数 × 報告の期間 / 窓の長さ）と比べる。
    // 報告は 1 秒ごとに届くので、1 つの窓に入る数は 4〜6 個と揺れる。送った数をそのまま比べると、
    // 報告が 1 個少ないだけで、何も失われていないのに約 20% の損失に見えてしまう。
    const double expectedFragments = static_cast<double>(m_window.sentFragments) * clientSeconds / seconds;
    m_window.fragmentLossPercent = expectedFragments > 0 ? std::max(0.0,
        1.0 - static_cast<double>(totals.receivedFragments) / expectedFragments) * 100.0 : 0;
    m_window.decodeAverageMs = Ratio(totals.decodeTotalUs, totals.decoded) / 1000.0;
    m_window.latencyAverageMs = Ratio(totals.latencyTotalUs, totals.latencyFrames) / 1000.0;
    m_window.audioBufferAverageMs = Ratio(totals.audioBufferTotal, totals.clientPeriodUs) / 1000.0;
}

void StatsCollector::LogIfDue(Clock::time_point now) {
    StatsSnapshot stats;
    std::array<double, kPcIntervalCount> pc{};
    std::array<double, kQuestIntervalCount> quest{};
    {
        std::lock_guard lock(m_mutex);
        const double seconds = std::chrono::duration<double>(now - m_windowStart).count();
        if (seconds < kWindowSeconds) {
            return;
        }
        FinishWindowLocked(seconds);
        m_snapshot = m_window.clientConnected ? m_window : StatsSnapshot{};
        stats = m_snapshot;
        stats.pcMicState = m_pcMicState;
        stats.questMicState = m_questMicState;
        for (size_t index = 0; index < pc.size(); ++index) {
            pc[index] = m_totals.pcIntervals[index].AverageMs();
        }
        for (size_t index = 0; index < quest.size(); ++index) {
            quest[index] = m_totals.questIntervals[index].AverageMs();
        }
        StartWindowLocked(m_window.clientConnected, m_window.clientName, m_window.clientSessionId);
    }
    // ログ出力は他部品のロックを取り得るため、集計のロックを保持しない。
    if (!stats.clientConnected) {
        Log("統計: 未接続 mic(pc=%d quest=%d)", stats.pcMicState, stats.questMicState);
        return;
    }
    Log("統計: 接続=%s PC[render=%.1f enc=%.1f (%.1f/%.1fms) send=%.2fMbps frag=%llu drop=%llu vsync=%.2fHz] "
        "Quest[recv=%.1f lost=%llu loss=%.2f%% %.2fMbps dec=%.1f/%.1fms disp=%.1f rep=%llu lat=%.1f/%.1fms "
        "rtt=%.1fms audio(under=%llu miss=%llu buf=%.1fms)] mic(pc=%d quest=%d)",
        stats.clientName.c_str(), stats.renderFps, stats.encodeFps, stats.encodeAverageMs, stats.encodeMaxMs,
        stats.sentMbps, static_cast<unsigned long long>(stats.sentFragments),
        static_cast<unsigned long long>(stats.sendDroppedFrames), stats.virtualVsyncHz,
        stats.receivedFps, static_cast<unsigned long long>(stats.lostFrames), stats.fragmentLossPercent,
        stats.receivedMbps, stats.decodeAverageMs, stats.decodeMaxMs, stats.displayFps,
        static_cast<unsigned long long>(stats.repeatedFrames), stats.latencyAverageMs, stats.latencyMaxMs,
        stats.roundTripMs, static_cast<unsigned long long>(stats.audioUnderruns),
        static_cast<unsigned long long>(stats.audioMissingFrames), stats.audioBufferAverageMs,
        stats.pcMicState, stats.questMicState);
    // 回線の時間は直接は測れないので、Quest の「姿勢→最初の断片」から PC の中の時間を引いて求める。
    const double pcBeforeSend = pc[kPoseToPresent] + pc[kPresentToEncoded] + pc[kEncodedToFirstSent];
    Log("遅延の内訳: PC[姿勢の受信→描画=%.1f 描画→エンコード完了=%.1f (うちエンコード=%.1f) 送信待ち=%.1f 送信=%.1f] "
        "回線(行き+帰り)=%.1f Quest[姿勢→最初の断片=%.1f 組み立て=%.1f デコード待ち=%.1f デコード=%.1f 表示待ち=%.1f 合計=%.1f] ms",
        pc[kPoseToPresent], pc[kPresentToEncoded], stats.encodeAverageMs, pc[kEncodedToFirstSent],
        pc[kFirstToLastSent], quest[kPoseToFirstFragment] - pcBeforeSend,
        quest[kPoseToFirstFragment], quest[kAssemble], quest[kDecoderWait], stats.decodeAverageMs,
        quest[kDisplayWait],
        quest[kPoseToFirstFragment] + quest[kAssemble] + quest[kDecoderWait] + stats.decodeAverageMs
            + quest[kDisplayWait]);
}

}
