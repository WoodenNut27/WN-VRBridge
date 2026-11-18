#pragma once
#include "host_settings.h"
#include <wnvr/host_ipc/host_ipc.h>
#include <memory>
#include <functional>

namespace wnvr::host {
class DriverPipeClient {
public:
    DriverPipeClient();
    ~DriverPipeClient();
    // Start より前に設定し、Stop が返るまでコールバックの参照先を保持する。
    void SetPairingCallback(std::function<void(const ipc::PairingRequestMessage&)> callback);
    void SetRejectedDevices(std::vector<std::string> ids);
    void Start();
    void Stop();
    // アダプターは呼び出し時に解決し、再接続時にも最新の設定を送る。
    void SetSettings(const HostSettings& settings);
    bool IsConnected() const;
    std::optional<ipc::StatsSnapshot> LatestStats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
