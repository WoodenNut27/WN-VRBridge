#pragma once

#include <wnvr/host_ipc/host_ipc.h>

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wnvr::host {

// 承認・拒否を優先し、終了したセッションは明示的な起動要求でも抑止する。
inline protocol::HelloResult DecideHelloResult(bool approved, bool rejected, bool suppressed,
    bool autoStart, bool startRequested) {
    if (!approved) {
        return rejected ? protocol::HelloResult::Rejected : protocol::HelloResult::PendingApproval;
    }
    return !suppressed && (autoStart || startRequested)
        ? protocol::HelloResult::Starting : protocol::HelloResult::Stopped;
}

class HostListener {
public:
    struct Callbacks {
        // 受信スレッドを止めないよう、UI には通知だけを送る。
        std::function<void(const ipc::PairingRequestMessage&)> onPairingRequest;
        std::function<void(const std::string& deviceId)> onApprovedHello;
    };
    explicit HostListener(Callbacks callbacks);
    ~HostListener();
    bool Start(const std::string& bindAddress);
    void Stop();
    bool IsRunning() const;
    std::string LastError() const;
    void SetDeviceLists(std::vector<std::string> approved, std::vector<std::string> rejected);
    // 待ち受けを再開しても、ユーザーが終了した SteamVR をすぐ起動しない。
    void SetSuppressedSession(uint32_t sessionId);
    void SetAutoStart(bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
