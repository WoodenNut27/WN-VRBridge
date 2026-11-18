#pragma once

#include "status_summary.h"

#include <windows.h>

#include <cstdint>

namespace wnvr::host {

// 状態の色（0xRRGGBB）。タスクトレイのアイコンと設定画面の表示で同じ色を使う。
constexpr uint32_t ToneRgb(Tone tone) {
    switch (tone) {
    case Tone::Good: return 0x3fb950;    // 緑
    case Tone::Busy: return 0x4c8dff;    // 青
    case Tone::Idle: return 0x8b949e;    // 灰色
    case Tone::Warning: return 0xd29922; // 黄
    case Tone::Error: return 0xf85149;   // 赤
    }
    return 0x8b949e;
}

// exe に埋め込み、表示の倍率が変わっても同じ絵を使えるようにする。
// 使い終わったら DestroyIcon で破棄する。読めなければ nullptr。
HICON LoadAppIcon(int size);

// トレイでも状態を見分けられるよう、埋め込んだアイコンの右下に色の印を重ねる。
// 使い終わったら DestroyIcon で破棄する。作れなければ nullptr。
HICON CreateStatusIcon(int size, Tone tone);

}
