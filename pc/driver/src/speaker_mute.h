#pragma once

#include <memory>

namespace wnvr::driver {

// Quest とつながっているあいだだけ既定の再生デバイスをミュートし、切断したら元の状態に戻す。
// ミュートする前の状態をファイルに残すので、接続中に SteamVR が落ちても、次の起動で戻せる。
// 操作は専用のスレッドで行い、呼び出し側は最大 2 秒だけ結果を待つ。
class SpeakerMute {
public:
    SpeakerMute();
    ~SpeakerMute();
    // 前回の実行が残した状態ファイルがあれば、それに従って元に戻す。
    void RestoreLeftover();
    void Mute();
    void Restore();

private:
    struct State;
    enum class Operation { RestoreLeftover, Mute, Restore };
    void Submit(Operation operation);

    std::unique_ptr<State> m_state;
};

}
