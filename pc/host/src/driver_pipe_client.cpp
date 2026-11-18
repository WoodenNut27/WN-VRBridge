#include "driver_pipe_client.h"

#include "host_log.h"
#include "network_adapters.h"
#include <wnvr/host_ipc/pipe_io.h>
#include <wnvr/version.h>

#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace wnvr::host {
namespace {

using Clock = std::chrono::steady_clock;

}

struct DriverPipeClient::Impl {
    ipc::Handle m_stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    ipc::Handle m_wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::jthread m_thread;
    mutable std::mutex m_mutex;
    ipc::SettingsMessage m_settings;
    std::function<void(const ipc::PairingRequestMessage&)> m_pairingCallback;
    uint64_t m_revision = 0;
    bool m_connected = false;
    std::optional<ipc::StatsSnapshot> m_stats;

    void Disconnected() {
        std::lock_guard lock(m_mutex);
        if (m_connected) {
            LogInfo("ドライバのパイプから切断しました");
        }
        m_connected = false;
        m_stats.reset();
    }

    void Session(HANDLE pipe) {
        if (!ipc::WriteMessage(pipe, m_stop.m_value, ipc::HelloMessage{ipc::kProtocolVersion, kVersion})) {
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            m_connected = true;
        }
        LogInfo("ドライバのパイプに接続しました");
        uint64_t sentRevision = 0;
        bool first = true;
        auto lastStatus = Clock::now();
        const auto result = ipc::ReadMessages(pipe, m_stop.m_value, m_wake.m_value, [&] {
            if (Clock::now() - lastStatus >= std::chrono::seconds(3)) {
                LogWarn("ドライバの統計が 3 秒間届きません");
                return false;
            }
            ipc::SettingsMessage settings;
            uint64_t revision;
            {
                std::lock_guard lock(m_mutex);
                revision = m_revision;
                if (!first && sentRevision == revision) {
                    return true;
                }
                settings = m_settings;
            }
            first = false;
            sentRevision = revision;
            if (!ipc::WriteMessage(pipe, m_stop.m_value, settings)) {
                LogWarn("ドライバへの設定の送信に失敗しました: %lu", GetLastError());
                return false;
            }
            return true;
        }, [&](const ipc::Message& message) {
            if (const auto* status = std::get_if<ipc::StatusMessage>(&message)) {
                lastStatus = Clock::now();
                std::lock_guard lock(m_mutex);
                m_stats = status->stats;
            } else if (const auto* request = std::get_if<ipc::PairingRequestMessage>(&message)) {
                if (m_pairingCallback) {
                    m_pairingCallback(*request);
                }
            }
            return true;
        });
        // 切断の原因を後から追えるよう、自分で止めたとき以外は理由を残す。
        if (result.end != ipc::ReadEnd::Stopped) {
            LogWarn("ドライバのパイプを閉じます: %s", result.reason.c_str());
        }
    }

    void Run() {
        while (WaitForSingleObject(m_stop.m_value, 0) == WAIT_TIMEOUT) {
            try {
                ipc::Handle pipe(CreateFileW(ipc::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
                if (pipe.m_value != INVALID_HANDLE_VALUE) {
                    DWORD mode = PIPE_READMODE_MESSAGE;
                    if (SetNamedPipeHandleState(pipe.m_value, &mode, nullptr, nullptr)) {
                        Session(pipe.m_value);
                    }
                }
            } catch (const std::exception& error) {
                LogWarn("ドライバパイプで例外: %s", error.what());
            } catch (...) {
                LogWarn("ドライバパイプで予期しない例外");
            }
            Disconnected();
            WaitForSingleObject(m_stop.m_value, 1000);
        }
    }
};

DriverPipeClient::DriverPipeClient() : m_impl(std::make_unique<Impl>()) {}

DriverPipeClient::~DriverPipeClient() {
    Stop();
}

void DriverPipeClient::SetPairingCallback(std::function<void(const ipc::PairingRequestMessage&)> callback) {
    m_impl->m_pairingCallback = std::move(callback);
}

void DriverPipeClient::SetRejectedDevices(std::vector<std::string> ids) {
    {
        std::lock_guard lock(m_impl->m_mutex);
        m_impl->m_settings.rejectedDeviceIds = std::move(ids);
        ++m_impl->m_revision;
    }
    SetEvent(m_impl->m_wake.m_value);
}

void DriverPipeClient::Start() {
    if (m_impl->m_thread.joinable()) {
        return;
    }
    if (!m_impl->m_stop.m_value || !m_impl->m_wake.m_value) {
        LogError("パイプイベントを作成できません");
        return;
    }
    ResetEvent(m_impl->m_stop.m_value);
    m_impl->m_thread = std::jthread([this] {
        m_impl->Run();
    });
}

void DriverPipeClient::Stop() {
    SetEvent(m_impl->m_stop.m_value);
    if (m_impl->m_thread.joinable()) {
        m_impl->m_thread.join();
    }
    m_impl->Disconnected();
}

void DriverPipeClient::SetSettings(const HostSettings& settings) {
    ipc::SettingsMessage message;
    for (const auto& device : settings.approvedDevices) {
        message.approvedDeviceIds.push_back(device.id);
    }
    message.bitrateMbps = settings.bitrateMbps;
    message.questResolutionPercent = settings.questResolutionPercent;
    message.foveatedEncoding = settings.foveatedEncoding;
    message.foveationEdgeRatio = settings.foveationEdgeRatio;
    message.streamAudioToQuest = settings.streamAudioToQuest;
    message.micToPc = settings.micToPc;
    message.autoStopSteamVr = settings.autoStopSteamVr;
    message.muteSpeakerWhileConnected = settings.muteSpeakerWhileConnected;
    message.bindAddress = BindAddressFor(settings.adapterId);
    {
        std::lock_guard lock(m_impl->m_mutex);
        message.rejectedDeviceIds = m_impl->m_settings.rejectedDeviceIds;
        m_impl->m_settings = std::move(message);
        ++m_impl->m_revision;
    }
    SetEvent(m_impl->m_wake.m_value);
}

bool DriverPipeClient::IsConnected() const {
    std::lock_guard lock(m_impl->m_mutex);
    return m_impl->m_connected;
}

std::optional<ipc::StatsSnapshot> DriverPipeClient::LatestStats() const {
    std::lock_guard lock(m_impl->m_mutex);
    return m_impl->m_connected ? m_impl->m_stats : std::nullopt;
}

}
