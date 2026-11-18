#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

// ホストとドライバの両方で使う、Windows の定型処理。
// 同じ変換やフォルダの取得を部品ごとに書くと、失敗時の扱いが少しずつずれるのでここに集める。
namespace wnvr::win32 {

inline constexpr wchar_t kAppFolderName[] = L"WN-VRBridge";

// デバイスの再試行待ちでも、DLL 終了時の停止要求にはすぐ応じる。
void WaitForStop(std::stop_token token, std::chrono::milliseconds duration);

enum class InvalidText {
    Fail,    // 不正な並びがあれば空文字列を返す（パスや ID など、化けると別物になるもの）。
    Replace, // 不正な並びを U+FFFD に置き換える（画面やログに出すだけのもの）。
};

// 空の入力と変換の失敗はどちらも空文字列になる。区別が要る呼び出し側は入力が空かを先に見る。
std::wstring ToWide(std::string_view utf8, InvalidText invalid = InvalidText::Fail);
std::string ToUtf8(std::wstring_view wide, InvalidText invalid = InvalidText::Fail);

// 文字の途中で切らずに、UTF-8 で maxBytes 以下に収める。
std::string TruncateUtf8(std::string text, size_t maxBytes);

// 取得できないときは空のパスを返す。
std::filesystem::path ExecutablePath();
// %LOCALAPPDATA%\WN-VRBridge（ログ、ミュートの状態、開発用の保存ファイル）。
std::filesystem::path AppLocalDataPath();
// %APPDATA%\WN-VRBridge（設定）。
std::filesystem::path AppRoamingDataPath();
// %LOCALAPPDATA% そのもの（SteamVR の openvrpaths.vrpath を読むときに使う）。
std::filesystem::path LocalAppDataPath();

// Quest の画面とログに出す、この PC の名前。取得できないときは fallback を返す。
std::string ComputerName(size_t maxUtf8Bytes, std::string_view fallback);

}
