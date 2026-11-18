#include "restart_notice.h"

#include <doctest/doctest.h>

#include <array>

using namespace wnvr::quest;
using namespace std::chrono_literals;

namespace {

constexpr auto kStart = RestartNotice::Clock::time_point{};
constexpr auto kBeforeExpiry = RestartNotice::kShowDuration - 1ms;
constexpr auto kAdditionalDelay = 2s;
constexpr auto kAfterExpiry = RestartNotice::kShowDuration + 1ms;

}

TEST_CASE("変更が起きたときだけ5秒表示し、同じ変更では再表示しない") {
    RestartNotice notice;
    notice.Update(false, false, kStart);
    CHECK_FALSE(notice.Visible(kStart));
    CHECK_FALSE(notice.JustStarted());
    notice.Update(true, false, kStart);
    CHECK(notice.JustStarted());
    CHECK(notice.Visible(kStart));
    CHECK(notice.Visible(kStart + kBeforeExpiry));
    CHECK_FALSE(notice.Visible(kStart + RestartNotice::kShowDuration));
    notice.Update(true, false, kStart + kAfterExpiry);
    CHECK_FALSE(notice.JustStarted());
    CHECK_FALSE(notice.Visible(kStart + kAfterExpiry));
    notice.Update(false, false, kStart + kAfterExpiry);
    notice.Update(true, false, kStart + kAfterExpiry + kAdditionalDelay);
    CHECK(notice.JustStarted());
    CHECK(notice.Visible(kStart + kAfterExpiry + kAdditionalDelay));
}

TEST_CASE("表示中にもう一方の変更が加わると5秒を数え直す") {
    for (const bool eyeFirst : {false, true}) {
        RestartNotice notice;
        notice.Update(eyeFirst, !eyeFirst, kStart);
        const auto addedAt = kStart + kAdditionalDelay;
        notice.Update(true, true, addedAt);
        CHECK(notice.JustStarted());
        CHECK(notice.Visible(kStart + RestartNotice::kShowDuration));
        CHECK(notice.Visible(addedAt + kBeforeExpiry));
        CHECK_FALSE(notice.Visible(addedAt + RestartNotice::kShowDuration));
        CHECK(notice.Text().second == "描画の解像度と foveationの変更は、\nアプリを起動し直すと使われます");
    }
}

TEST_CASE("通知の文言は変わった設定を示す") {
    RestartNotice notice;
    struct ExpectedText {
        bool eye;
        bool foveation;
        const char* detail;
    };
    const std::array<ExpectedText, 3> cases{{
        {true, false, "描画の解像度の変更は、\nアプリを起動し直すと使われます"},
        {false, true, "foveationの変更は、\nアプリを起動し直すと使われます"},
        {true, true, "描画の解像度と foveationの変更は、\nアプリを起動し直すと使われます"},
    }};
    for (const auto& expected : cases) {
        notice.Reset();
        notice.Update(expected.eye, expected.foveation, kStart);
        const auto [title, detail] = notice.Text();
        CHECK(title == "設定が変わりました");
        CHECK(detail == expected.detail);
    }
    notice.Reset();
    CHECK_FALSE(notice.Visible(kStart));
    CHECK_FALSE(notice.JustStarted());
    notice.Update(false, true, kStart);
    CHECK(notice.Visible(kStart));
}
