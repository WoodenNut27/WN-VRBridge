#pragma once

#include <wnvr/protocol/messages.h>
#include <wnvr/host_ipc/host_ipc.h>

#include <array>
#include <chrono>
#include <mutex>
#include <string>

namespace wnvr::driver {

using StatsSnapshot = ipc::StatsSnapshot;
struct FrameTiming;

// ドライバの各部品（描画の受け取り、エンコーダー、送信、Quest からの報告）の数値を 1 か所に集め、
// 5 秒ごとの窓にまとめる。閉じた窓はログに出し、ホストの設定画面にも渡す。
// どのスレッドから呼んでもよい。
class StatsCollector {
public:
    void OnClientConnected(const std::string& name, uint32_t sessionId);
    void OnClientDisconnected();
    void OnClientStats(const protocol::ClientStatsMessage& stats);
    void OnRenderedFrame();
    void OnEncodedFrame(double encodeMs);
    void OnFrameTiming(const FrameTiming& timing, std::chrono::steady_clock::time_point firstSent,
        std::chrono::steady_clock::time_point lastSent);
    void OnVideoSent(uint64_t fragments, uint64_t payloadBytes);
    void OnSendDroppedFrame();
    void SetVirtualVsyncHz(double hz);
    void SetPcMicState(int state);
    // 数値は最後に閉じた窓、接続状態と名前は現在の値を返す。
    StatsSnapshot GetSnapshot() const;
    // 窓の長さ（5 秒）がたっていれば、窓を閉じてログに出す。SteamVR の RunFrame から呼ぶ。
    // now は単体テストで時間を進めるためのもので、ふだんは省略する。
    void LogIfDue(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

private:
    using Clock = std::chrono::steady_clock;

    struct IntervalStats {
        double totalMs = 0;
        uint64_t frames = 0;

        void Add(double ms, uint64_t count = 1) {
            totalMs += ms * static_cast<double>(count);
            frames += count;
        }

        double AverageMs() const {
            return frames ? totalMs / static_cast<double>(frames) : 0;
        }
    };

    // 遅延の区間。PC と Quest の時計は合わせていないので、それぞれの中で測れる区間だけを持つ。
    enum PcInterval { kPoseToPresent, kPresentToEncoded, kEncodedToFirstSent, kFirstToLastSent, kPcIntervalCount };
    enum QuestInterval { kPoseToFirstFragment, kAssemble, kDecoderWait, kDisplayWait, kQuestIntervalCount };

    // 窓のあいだ足し込む値。平均や割合は、窓を閉じるときにここから計算する。
    struct Totals {
        uint64_t rendered = 0;
        uint64_t encoded = 0;
        uint64_t sentBytes = 0;
        uint64_t clientPeriodUs = 0;
        uint64_t received = 0;
        uint64_t receivedFragments = 0;
        uint64_t receivedBytes = 0;
        uint64_t decoded = 0;
        uint64_t displayed = 0;
        uint64_t latencyFrames = 0;
        double encodeTotalMs = 0;
        double decodeTotalUs = 0;
        double latencyTotalUs = 0;
        double audioBufferTotal = 0;
        std::array<IntervalStats, kPcIntervalCount> pcIntervals{};
        std::array<IntervalStats, kQuestIntervalCount> questIntervals{};
    };

    // 新しい窓を始める。接続の情報と仮想の垂直同期の周波数だけを引き継ぐ。
    void StartWindowLocked(bool connected, std::string name, uint32_t sessionId);
    // 足し込んだ値から、窓の平均や割合を m_window に書き込む。
    void FinishWindowLocked(double seconds);

    mutable std::mutex m_mutex;
    Clock::time_point m_windowStart = Clock::now();
    StatsSnapshot m_snapshot; // 最後に閉じた窓
    StatsSnapshot m_window;   // 今の窓（最大値と回数は、届くたびにここへ直接入れる）
    Totals m_totals;
    int m_pcMicState = 0;
    int m_questMicState = 0;
};

}
