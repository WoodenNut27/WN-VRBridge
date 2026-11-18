#pragma once

#include "host_settings.h"

#include <wnvr/host_ipc/host_ipc.h>

#include <windows.h>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace wnvr::host {

// 初めて接続する Quest の承認。
// 要求は受信スレッド（ホストの待ち受け）とパイプのスレッド（ドライバ）から届き、
// 確認のダイアログと設定の書き換えは UI スレッドで行う。
class PairingManager {
public:
    struct Callbacks {
        // どちらも UI スレッドで呼ばれる。
        std::function<void()> onApprovedChanged; // 承認した Quest の一覧が変わった
        std::function<void()> onRejectedChanged; // 拒否中の Quest の一覧が変わった
    };

    PairingManager(HostSettings& settings, Callbacks callbacks);

    // 要求が届いたことを UI スレッドに知らせる先。Queue を呼ぶスレッドを始める前に設定する。
    void SetNotifyTarget(HWND window, UINT message);

    // どのスレッドから呼んでもよい。同じ Quest の要求は、処理が終わるまで 1 つにまとめる。
    void Queue(const ipc::PairingRequestMessage& request);

    // ここから下は UI スレッドで呼ぶ。
    void ProcessQueue();
    void ExpireRejected();
    // 拒否してからしばらくは、同じ Quest の確認を出さない。その対象の ID。
    std::vector<std::string> RejectedIds() const;

private:
    void Ask(const ipc::PairingRequestMessage& request);
    void Approve(const ipc::PairingRequestMessage& request);
    void Reject(const ipc::PairingRequestMessage& request);

    HostSettings& m_settings;
    Callbacks m_callbacks;
    HWND m_window = nullptr;
    UINT m_message = 0;

    std::mutex m_mutex;
    std::deque<ipc::PairingRequestMessage> m_queue;
    std::set<std::string> m_pendingIds;

    // UI スレッドだけが使う。
    std::map<std::string, std::chrono::steady_clock::time_point> m_rejected;
    bool m_processing = false;
};

}
