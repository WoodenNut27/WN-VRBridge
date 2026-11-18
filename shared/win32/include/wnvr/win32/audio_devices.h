#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wnvr::win32 {

// VB-CABLE の再生側の名前（Windows の表示名に含まれる文字列）。
inline constexpr wchar_t kVbCableRenderName[] = L"CABLE Input";
struct AudioEndpoint {
    std::wstring id;
    std::wstring friendlyName;
};
// ホストとドライバで同じ見つけ方を使う。呼ぶスレッドで COM を初期化しておくこと。
std::optional<AudioEndpoint> FindRenderEndpoint(std::wstring_view nameFragment);

}
