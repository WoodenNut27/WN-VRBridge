#include <wnvr/win32/platform.h>

#include <doctest/doctest.h>

#include <string>

using namespace wnvr::win32;

TEST_CASE("UTF-8 と UTF-16 を往復できる") {
    const std::string utf8 = "Quest 3 の名前 \xF0\x9F\x8E\xAE"; // 最後はサロゲートペアになる絵文字
    const auto wide = ToWide(utf8);
    CHECK(wide == L"Quest 3 の名前 \U0001F3AE");
    CHECK(ToUtf8(wide) == utf8);
}

TEST_CASE("空の入力は空を返す") {
    CHECK(ToWide("").empty());
    CHECK(ToUtf8(L"").empty());
}

TEST_CASE("不正な UTF-8 は、失敗させるか置き換えるかを選べる") {
    const std::string broken = "Quest \xFF 3";
    CHECK(ToWide(broken).empty());
    CHECK(ToWide(broken, InvalidText::Replace) == L"Quest � 3");
}

TEST_CASE("対になっていないサロゲートは、失敗させるか置き換えるかを選べる") {
    const std::wstring broken{L'A', static_cast<wchar_t>(0xD800), L'B'};
    CHECK(ToUtf8(broken).empty());
    CHECK(ToUtf8(broken, InvalidText::Replace) == "A\xEF\xBF\xBD" "B");
}

TEST_CASE("途中の NUL も 1 文字として変換する") {
    const std::string text("a\0b", 3);
    const auto wide = ToWide(text);
    REQUIRE(wide.size() == 3);
    CHECK(wide[1] == L'\0');
    CHECK(ToUtf8(wide) == text);
}

TEST_CASE("UTF-8 を文字の途中で切らない") {
    const std::string text = "abあい"; // あ・い は 3 バイトずつ
    CHECK(TruncateUtf8(text, 100) == text);
    CHECK(TruncateUtf8(text, 8) == text);
    CHECK(TruncateUtf8(text, 7) == "abあ");
    CHECK(TruncateUtf8(text, 6) == "abあ");
    CHECK(TruncateUtf8(text, 5) == "abあ");
    CHECK(TruncateUtf8(text, 4) == "ab");
    CHECK(TruncateUtf8(text, 2) == "ab");
    CHECK(TruncateUtf8(text, 1) == "a");
    CHECK(TruncateUtf8(text, 0).empty());
    CHECK(TruncateUtf8("あ", 2).empty());
}

TEST_CASE("実行ファイルとデータのフォルダを取得できる") {
    const auto exe = ExecutablePath();
    CHECK(exe.is_absolute());
    CHECK(exe.has_filename());
    const auto local = AppLocalDataPath();
    const auto roaming = AppRoamingDataPath();
    CHECK(local.is_absolute());
    CHECK(roaming.is_absolute());
    CHECK(local.filename() == kAppFolderName);
    CHECK(roaming.filename() == kAppFolderName);
    CHECK(local.parent_path() == LocalAppDataPath());
}

TEST_CASE("PC の名前は上限に収まり、空にならない") {
    const auto name = ComputerName(64, "fallback");
    CHECK(!name.empty());
    CHECK(name.size() <= 64);
    // 上限 0 では切り詰めた結果が空になる（代わりの名前を使うのは取得に失敗したときだけ）。
    CHECK(ComputerName(0, "fallback").empty());
}

TEST_CASE("停止済みの要求なら再試行の待機からすぐに戻る") {
    std::stop_source stop;
    stop.request_stop();
    const auto start = std::chrono::steady_clock::now();
    constexpr auto kLongRetry = std::chrono::seconds(5);
    constexpr auto kImmediateLimit = std::chrono::seconds(1);
    WaitForStop(stop.get_token(), kLongRetry);
    CHECK(std::chrono::steady_clock::now() - start < kImmediateLimit);
}
