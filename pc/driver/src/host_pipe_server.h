#pragma once
#include <wnvr/host_ipc/host_ipc.h>
#include <memory>

namespace wnvr::driver {
class StatsCollector;
class HostPipeListener {
public:
    virtual ~HostPipeListener() = default;
    virtual void OnHostSettings(const ipc::SettingsMessage& settings) = 0;
};
class HostPipeServer {
public:
    HostPipeServer(HostPipeListener& listener, const StatsCollector& stats);
    ~HostPipeServer();
    void Start();
    void Send(const ipc::Message& message);
    // 停止イベントで未完了 I/O を起こし、スレッドの終了まで待つ。
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
