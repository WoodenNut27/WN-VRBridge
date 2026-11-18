#include "config_file.h"

#include <doctest/doctest.h>

#include <sstream>

using namespace wnvr::quest;

namespace {

struct Values {
    int maxMs = -1;
    int fixedMs = -1;
};

std::vector<std::string> Read(const std::string& text, Values& values) {
    const IntSetting settings[]{
        {"max_ms", 0, 200, [&](int value) { values.maxMs = value; }},
        {"fixed_ms", 0, 100, [&](int value) { values.fixedMs = value; }},
    };
    std::istringstream input(text);
    return ReadIntSettings(input, "prediction.txt", settings);
}

}

TEST_CASE("キー=値を読み、空行とコメントと前後の空白を読み飛ばす") {
    Values values;
    const auto warnings = Read("# 見比べ用\n\n  max_ms = 150 \r\nfixed_ms=0\n", values);
    CHECK(warnings.empty());
    CHECK(values.maxMs == 150);
    CHECK(values.fixedMs == 0);
}

TEST_CASE("範囲の両端は受け付け、外側は理由を返して無視する") {
    Values values;
    CHECK(Read("max_ms=200\nfixed_ms=100\n", values).empty());
    CHECK(values.maxMs == 200);
    CHECK(values.fixedMs == 100);

    values = {};
    const auto warnings = Read("max_ms=201\nfixed_ms=-1\n", values);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0] == "prediction.txt の 1 行目を無視: 0〜200 の整数が必要です");
    CHECK(warnings[1] == "prediction.txt の 2 行目を無視: 0〜100 の整数が必要です");
    CHECK(values.maxMs == -1);
    CHECK(values.fixedMs == -1);
}

TEST_CASE("読めない行は、行番号と理由を返して続ける") {
    Values values;
    const auto warnings = Read("max_ms\nunknown=1\nmax_ms=\nmax_ms=12abc\nmax_ms=1.5\nmax_ms=99999999999\nfixed_ms=7\n", values);
    REQUIRE(warnings.size() == 6);
    CHECK(warnings[0] == "prediction.txt の 1 行目を無視: キー=値が必要です");
    CHECK(warnings[1] == "prediction.txt の 2 行目を無視: 不明なキーです");
    CHECK(warnings[2] == "prediction.txt の 3 行目を無視: 整数が必要です");
    for (size_t index = 3; index < warnings.size(); ++index) {
        CHECK(warnings[index].find("0〜200 の整数が必要です") != std::string::npos);
    }
    // 途中に読めない行があっても、あとの行は読む。
    CHECK(values.maxMs == -1);
    CHECK(values.fixedMs == 7);
}

TEST_CASE("同じキーが 2 回あれば、あとの値が使われる") {
    Values values;
    CHECK(Read("max_ms=10\nmax_ms=20\n", values).empty());
    CHECK(values.maxMs == 20);
}

TEST_CASE("host.txt はアドレスだけでも、ポート付きでも読める") {
    const auto plain = ParseHostLine(" 192.168.1.31 \r", 9944);
    REQUIRE(plain);
    CHECK(plain->host == "192.168.1.31");
    CHECK(plain->port == 9944);

    const auto withPort = ParseHostLine("192.168.1.31:12345", 9944);
    REQUIRE(withPort);
    CHECK(withPort->host == "192.168.1.31");
    CHECK(withPort->port == 12345);

    CHECK(ParseHostLine("10.0.0.1:65535", 9944)->port == 65535);
}

TEST_CASE("host.txt の形が合わなければ読まない") {
    for (const auto* line : {"", "   ", ":9944", "192.168.1.31:", "192.168.1.31:0", "192.168.1.31:65536",
            "192.168.1.31:abc", "192.168.1.31:99 9", "192.168.1.31:-1"}) {
        CAPTURE(line);
        CHECK(!ParseHostLine(line, 9944));
    }
    CHECK(!ParseHostLine(std::string_view("1.2.3.4\0x", 9), 9944));
}

TEST_CASE("描画先の大きさは両方の項目が必要で範囲外を拒否する") {
    for (const auto text : {"width=2064\nheight=2208\n", "width=256\nheight=8192\n"}) {
        std::istringstream input(text);
        std::vector<std::string> warnings;
        CHECK(ReadEyeSize(input, &warnings));
        CHECK(warnings.empty());
    }
    for (const auto text : {"", "width=2064\n", "height=2208\n", "width=255\nheight=2208\n",
            "width=8193\nheight=2208\n", "width=2064\nheight=255\n", "width=2064\nheight=8193\n"}) {
        std::istringstream input(text);
        std::vector<std::string> warnings;
        CHECK_FALSE(ReadEyeSize(input, &warnings));
        CHECK_FALSE(warnings.empty());
    }
    const EyeSize size{2064, 2208};
    CHECK(FormatEyeSize(size) == "width=2064\nheight=2208\n");
    std::istringstream input(FormatEyeSize(size));
    const auto restored = ReadEyeSize(input, nullptr);
    REQUIRE(restored);
    CHECK(*restored == size);
}

TEST_CASE("描画先の上限を超えたときは縦横比を保って縮める") {
    const EyeSize recommended{1680, 1760};
    const EyeSize maximum{4096, 4096};
    CHECK(ChooseEyeSize(recommended, maximum, std::nullopt) == recommended);
    CHECK(ChooseEyeSize(recommended, maximum, EyeSize{2064, 2208}) == EyeSize{2064, 2208});
    CHECK(ChooseEyeSize(recommended, EyeSize{2000, 3000}, EyeSize{4000, 4000}) == EyeSize{2000, 2000});
    CHECK(ChooseEyeSize(recommended, EyeSize{3000, 1500}, EyeSize{4000, 3000}) == EyeSize{2000, 1500});
    CHECK(ChooseEyeSize(recommended, EyeSize{840, 1000}, std::nullopt) == EyeSize{840, 880});
    CHECK(ChooseEyeSize(recommended, EyeSize{1000, 840}, EyeSize{2064, 2208}) == EyeSize{785, 840});
}
