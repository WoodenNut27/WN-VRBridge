#pragma once

#include <cstdint>
#include <string>

namespace wnvr::quest {

// この Quest を PC が見分けるための ID（16 進 32 文字）。
// 初回に乱数で作ってアプリのフォルダに保存し、次からは同じ値を使う（PC での承認が続くように）。
// 保存できないときは、その起動のあいだだけ使う ID を返す。
std::string LoadDeviceId(const char* directory);

// アプリの起動ごと（とトリガーでの再開ごと）に変わる番号。0 にはならず、previous とも重ならない。
// PC は「利用者が SteamVR を終了したときにつながっていたセッション」を、これで見分ける。
uint32_t MakeSessionId(uint32_t previous);

}
