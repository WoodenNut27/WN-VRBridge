#pragma once

#include <string>

namespace wnvr::host {

// ホストが今どの段階にいるか。
enum class HostState {
    WaitingForDriverExit, // SteamVR が終わった直後。ドライバが使っていたポートが空くのを待っている。
    Listening,            // SteamVR は動いていない。ホストが Quest からの接続を待ち受けている。
    ListenFailed,         // 待ち受けのポートを開けなかった。数秒ごとにやり直す。
    LaunchingSteamVr,     // Quest が来たので SteamVR を起動した。ドライバがつながるのを待っている。
    DriverConnected,      // SteamVR が動いていて、ドライバとパイプでつながっている。
};

// 設定画面とタスクトレイに出す文言のもとになる値。
// 文言そのものではなく値で渡し、表示する側（status_summary）が 1 か所で言葉にする。
struct HostStatus {
    HostState state = HostState::WaitingForDriverExit;
    std::string bindAddress;  // 待ち受けている IPv4 アドレス（Listening のとき）
    std::string listenError;  // ListenFailed の理由
    std::string launchError;  // SteamVR を起動できなかった理由（次の起動まで残す）
    // Quest が来たのに、ドライバが SteamVR に登録されていないので起動を見送った。
    bool driverMissingAtLaunch = false;
    int launchSeconds = 0;    // LaunchingSteamVr になってからの秒数
    // ユーザーが SteamVR を終了したので、同じ Quest のセッションでは自動で起動し直さない。
    bool autoLaunchSuppressed = false;
    bool autoStartSteamVr = true;
};

}
