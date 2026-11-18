#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace wnvr::quest {

// 設定が待っている間ずっと視界を遮らないよう、変わったときだけ知らせる。
class RestartNotice {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kShowDuration = std::chrono::seconds(5);

    void Update(bool eyeSizePending, bool foveationPending, Clock::time_point now);
    bool Visible(Clock::time_point now) const;
    std::pair<std::string, std::string> Text() const;
    // 呼び出し側も同じ変化を判定せずに、表示開始をログに残せるようにする。
    bool JustStarted() const { return m_justStarted; }
    void Reset();

private:
    bool m_eyeSizePending = false;
    bool m_foveationPending = false;
    bool m_shownEyeSize = false;
    bool m_shownFoveation = false;
    bool m_justStarted = false;
    std::optional<Clock::time_point> m_startedAt;
};

}
