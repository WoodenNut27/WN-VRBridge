#pragma once

#include "status_summary.h"

#include <imgui.h>

// 設定画面の見た目（配色・余白）と、画面のあちこちで使う小さな部品。
// 各タブのコードが「何を並べるか」だけを書けるよう、描き方はここにまとめる。
namespace wnvr::host::ui {

// 太字の見出し用のフォント。読み込めなかったときは nullptr（本文と同じフォントで描く）。
void SetHeadingFont(ImFont* font);

// 配色と余白を設定する。表示の倍率が変わるたびに、基準の値から作り直す。
void ApplyTheme(float dpiScale);

ImVec4 ToneColor(Tone tone);

// ---- 文字 ----
void PageTitle(const char* text);    // 画面のいちばん上の題
void SectionTitle(const char* text); // グループの見出し（太字）
void HelpText(const char* text);     // 補足の説明（薄い色、折り返す）
void BulletHelp(const char* text);   // 箇条書きの補足の説明
void ErrorText(const char* text);    // 失敗の説明（赤、折り返す）

// ---- 状態 ----
// 色の付いた丸を描き、同じ行に文字を続けられるようにする。
void StatusDot(Tone tone);
// 丸 + 文字（折り返す）。
void StatusLine(Tone tone, const char* text);
// 角の丸い小さなラベル（「登録済み」など）。
void Badge(const char* text, Tone tone);

// ---- 操作 ----
// 行の高さに対して四角が大きくなりすぎないようにしたチェックボックス。
bool Checkbox(const char* label, bool* value);

// ---- グループ ----
// 枠で囲んだグループ。中身の高さに合わせて伸びる。BeginCard が true のときだけ中身を描き、
// 戻り値にかかわらず EndCard を呼ぶ。
bool BeginCard(const char* id);
void EndCard();

// 「項目名 | 値」を縦にそろえて並べる 2 列の表。
bool BeginFields(const char* id, float labelWidthEm);
// 項目名を描き、値の列に移る。続けて値を描く。
void Field(const char* label);
void EndFields();

// グループとグループのあいだの空き。
void Gap(float em = 0.6f);

}
