#pragma once

#include "audio_capture.h"
#include "mic_playback.h"
#include "controller_device.h"
#include "hmd_device.h"
#include "host_pipe_server.h"
#include "network_server.h"
#include "speaker_mute.h"
#include "stats_collector.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wnvr::driver {

// ドライバの本体。SteamVR に HMD とコントローラを登録し、部品どうしをつなぐ。
//   - NetworkServer（Quest との UDP）からの通知を、HMD・コントローラ・音声・統計へ配る
//   - HostPipeServer（ホストとのパイプ）から届いた設定を、動いている部品へ反映する
// 通知は受信スレッドやパイプのスレッドから届くので、状態はそれぞれのミューテックスで守る。
class DeviceProvider : public vr::IServerTrackedDeviceProvider, public NetworkListener, public AudioSink, public HostPipeListener {
public:
    DeviceProvider() = default;
    DeviceProvider(const DeviceProvider&) = delete;
    DeviceProvider& operator=(const DeviceProvider&) = delete;

    // SteamVR から呼ばれる。
    vr::EVRInitError Init(vr::IVRDriverContext* driverContext) override;
    void Cleanup() override;
    const char* const* GetInterfaceVersions() override;
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override;
    void EnterStandby() override;
    void LeaveStandby() override;

    // Quest との通信（受信スレッド）から呼ばれる。
    HelloDecision OnHelloRequest(const protocol::HelloMessage& hello, const std::string& address) override;
    void OnClientConnected(const protocol::HelloMessage& hello) override;
    void OnClientDisconnected(DisconnectReason reason) override;
    void OnClientStats(const protocol::ClientStatsMessage& stats) override;
    void OnPose(const protocol::PoseMessage& pose) override;
    void OnInput(const protocol::InputMessage& input) override;
    void OnMicAudio(const protocol::MicAudioMessage& mic) override;
    void OnKeyFrameNeeded() override;
    void OnDisplayFeedback(const protocol::DisplayFeedbackMessage& feedback) override;

    // 録音スレッドから呼ばれる。
    void OnAudio(const protocol::AudioMessage& audio) override;

    // ホストとのパイプのスレッドから呼ばれる。
    void OnHostSettings(const ipc::SettingsMessage& settings) override;

private:
    using Clock = std::chrono::steady_clock;

    struct FoveationParams {
        bool enabled = true;
        float width = 0.45f;
        float height = 0.40f;
        float extendUp = 0.10f;
        float ratioX = 4.0f;
        float ratioY = 5.0f;
        bool operator==(const FoveationParams&) const = default;
    };
    void ApplyFoveation(const FoveationParams& params);
    void ReadAudioSettings();
    void ConfigureRenderDisplay();
    void SwitchBindAddressIfPending();
    void RetryNetworkStartIfDue();
    void ScheduleNetworkRetry();
    void ApplyAudioSettings(const ipc::SettingsMessage& settings);

    template<typename Function>
    void ForEachController(Function&& function) {
        for (auto* controller : {m_leftController.get(), m_rightController.get()}) {
            if (controller) {
                function(*controller);
            }
        }
    }

    // 参照するワーカーより長く保持するため、先に構築する。
    StatsCollector m_statsCollector;
    std::unique_ptr<HmdDevice> m_hmd;
    std::unique_ptr<ControllerDevice> m_leftController;
    std::unique_ptr<ControllerDevice> m_rightController;
    NetworkServer m_networkServer{*this, &m_statsCollector};
    std::atomic<bool> m_clientConnected{false};
    // 再接続や設定変更と終了要求が競合しないよう、予定と接続の更新を同じロックで守る。
    std::mutex m_autoStopMutex;
    bool m_autoStopSteamVr = false;
    std::optional<Clock::time_point> m_autoStopAt;
    // 接続中の写像を維持し、HelloAck とエンコーダーの切り替えをそろえる。
    std::mutex m_foveationMutex;
    FoveationParams m_foveationParams;
    FoveationParams m_pendingFoveationParams;

    // 待ち受けるアドレス。切り替えは RunFrame（SteamVR のスレッド）で、Quest がつながっていないときに行う。
    std::mutex m_networkConfigMutex;
    NetworkServer::Config m_networkConfig;
    std::optional<std::string> m_pendingBindAddress;
    bool m_networkRetryPending = false;
    Clock::time_point m_nextNetworkRetry{};

    // 音声（録音と、接続中のスピーカーのミュート）。
    std::mutex m_audioMutex;
    AudioCapture m_audioCapture{*this};
    MicPlayback m_micPlayback;
    std::unique_ptr<SpeakerMute> m_speakerMute;
    bool m_audioEnabled = true;  // SteamVR の設定（audioEnabled）。false なら録音しない
    bool m_audioStopping = false; // Cleanup が始まったあとは、録音もミュートも始めない
    bool m_streamAudioToQuest = true;
    bool m_muteSpeakerWhileConnected = true;

    // ペアリング（承認済み・拒否中の端末の一覧はホストから届く）。
    bool m_requireHostApproval = true;
    std::mutex m_pairingMutex;
    std::vector<std::string> m_approvedDeviceIds;
    std::vector<std::string> m_rejectedDeviceIds;
    struct PairingTimes {
        Clock::time_point request; // 最後にホストへ確認を頼んだ時刻
        Clock::time_point log;     // 最後にログへ出した時刻
    };
    std::map<std::string, PairingTimes> m_pairingTimes;

    HostPipeServer m_hostPipe{*this, m_statsCollector};
};

}
