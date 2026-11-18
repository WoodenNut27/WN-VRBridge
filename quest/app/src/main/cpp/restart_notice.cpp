#include "restart_notice.h"

namespace wnvr::quest {

void RestartNotice::Update(bool eyeSizePending, bool foveationPending, Clock::time_point now) {
    m_justStarted = (eyeSizePending && !m_eyeSizePending) || (foveationPending && !m_foveationPending);
    if (m_justStarted) {
        m_startedAt = now;
        m_shownEyeSize = eyeSizePending;
        m_shownFoveation = foveationPending;
    }
    m_eyeSizePending = eyeSizePending;
    m_foveationPending = foveationPending;
}

bool RestartNotice::Visible(Clock::time_point now) const {
    return m_startedAt && now >= *m_startedAt && now - *m_startedAt < kShowDuration;
}

std::pair<std::string, std::string> RestartNotice::Text() const {
    const std::string setting = m_shownEyeSize
        ? (m_shownFoveation ? "描画の解像度と foveation" : "描画の解像度") : "foveation";
    return {"設定が変わりました", setting + "の変更は、\nアプリを起動し直すと使われます"};
}

void RestartNotice::Reset() {
    *this = RestartNotice{};
}

}
