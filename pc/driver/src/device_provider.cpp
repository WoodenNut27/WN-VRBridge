#include "device_provider.h"

#include "driver_log.h"
#include "driver_settings.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace wnvr::driver {
namespace {

constexpr float kMinFoveationCenterSize = 0.1f;
constexpr float kMaxFoveationCenterSize = 1.0f;
constexpr float kMinFoveationExtendUp = 0.0f;
constexpr float kMaxFoveationExtendUp = 0.5f;
constexpr float kMinSteamVrFoveationRatio = 1.0f;
constexpr float kMaxSteamVrFoveationRatio = 8.0f;

constexpr auto kAutoStopDelay = std::chrono::seconds(15);

constexpr auto kNetworkRetryInterval = std::chrono::seconds(1);
// 承認待ち・拒否の端末について、ホストへの通知とログを繰り返す間隔。
constexpr auto kPairingRequestInterval = std::chrono::seconds(2);
constexpr auto kPairingLogInterval = std::chrono::seconds(10);
// Hello がこの時間届かなければ、その端末の記録を捨てる。
constexpr auto kPairingForgetAfter = std::chrono::seconds(10);

bool IsIpv4Address(std::string_view address) {
    for (int index = 0; index < 4; ++index) {
        const auto separator = address.find('.');
        const auto part = address.substr(0, separator);
        unsigned value = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || (part.size() > 1 && part.front() == '0')
            || parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || value > 255) {
            return false;
        }
        if (index == 3) {
            return separator == std::string_view::npos;
        }
        if (separator == std::string_view::npos) {
            return false;
        }
        address.remove_prefix(separator + 1);
    }
    return false;
}

bool Contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

NetworkServer::Config ReadNetworkConfig() {
    NetworkServer::Config config;
    const auto address = settings::ReadString("bindAddress", config.bindAddress);
    if (IsIpv4Address(address)) {
        config.bindAddress = address;
    } else {
        Log("bindAddress が IPv4 アドレスではないため、既定値 %s を使います。", config.bindAddress.c_str());
    }
    config.port = static_cast<uint16_t>(settings::ReadInt("port", 1, 65535, config.port));
    config.clientTimeout = std::chrono::milliseconds(
        settings::ReadInt("clientTimeoutMs", 1, std::numeric_limits<int32_t>::max(), 2000));
    config.pacingRatio = settings::ReadFloat("pacingRatio", 0.0f, 1.0f, config.pacingRatio);
    return config;
}

}

vr::EVRInitError DeviceProvider::Init(vr::IVRDriverContext* driverContext) {
    VR_INIT_SERVER_DRIVER_CONTEXT(driverContext);
    Log("ドライバを初期化します。");
    m_speakerMute = std::make_unique<SpeakerMute>();
    // 前回、接続中のまま SteamVR が落ちていたら、スピーカーを元の状態に戻す。
    m_speakerMute->RestoreLeftover();
    m_hmd = std::make_unique<HmdDevice>(&m_statsCollector);
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(HmdDevice::kSerialNumber, vr::TrackedDeviceClass_HMD, m_hmd.get())) {
        Log("仮想 HMD の登録に失敗しました。");
        m_hmd.reset();
        m_speakerMute.reset();
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
        return vr::VRInitError_Driver_Failed;
    }
    const auto registerController = [](Hand hand) {
        auto controller = std::make_unique<ControllerDevice>(hand);
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
                controller->SerialNumber(), vr::TrackedDeviceClass_Controller, controller.get())) {
            Log("コントローラ %s の登録に失敗しました。このコントローラなしで動作を続けます。",
                controller->SerialNumber());
            controller.reset();
        }
        return controller;
    };
    m_leftController = registerController(Hand::Left);
    m_rightController = registerController(Hand::Right);
    ReadAudioSettings();
    m_micPlayback.SetEnabled(settings::ReadBool("micToPc", true));
    m_requireHostApproval = settings::ReadBool("requireHostApproval", true);
    {
        std::lock_guard lock(m_autoStopMutex);
        m_autoStopSteamVr = settings::ReadBool("autoStopSteamVr", false);
        m_autoStopAt.reset();
    }
    {
        std::lock_guard lock(m_foveationMutex);
        FoveationParams params;
        params.enabled = settings::ReadBool("foveatedEncoding", params.enabled);
        params.width = settings::ReadFloat("foveationCenterWidth", kMinFoveationCenterSize, kMaxFoveationCenterSize, params.width);
        params.height = settings::ReadFloat("foveationCenterHeight", kMinFoveationCenterSize, kMaxFoveationCenterSize, params.height);
        params.extendUp = settings::ReadFloat("foveationCenterExtendUp", kMinFoveationExtendUp, kMaxFoveationExtendUp, params.extendUp);
        params.ratioX = settings::ReadFloat("foveationEdgeRatioX", kMinSteamVrFoveationRatio, kMaxSteamVrFoveationRatio, params.ratioX);
        params.ratioY = settings::ReadFloat("foveationEdgeRatioY", kMinSteamVrFoveationRatio, kMaxSteamVrFoveationRatio, params.ratioY);
        m_foveationParams = params;
        m_pendingFoveationParams = params;
    }
    ConfigureRenderDisplay();
    m_networkConfig = ReadNetworkConfig();
    // ホストが先にパイプへ接続し、待ち受けのポートを解放できるようにする。
    m_hostPipe.Start();
    m_networkRetryPending = !m_networkServer.Start(m_networkConfig);
    if (m_networkRetryPending) {
        Log("UDP サーバーを開始できませんでした。1 秒ごとに再試行します。");
        ScheduleNetworkRetry();
    }
    m_hmd->SetFrameSink(&m_networkServer);
    return vr::VRInitError_None;
}

void DeviceProvider::ReadAudioSettings() {
    std::lock_guard lock(m_audioMutex);
    m_audioStopping = false;
    m_audioEnabled = settings::ReadBool("audioEnabled", true);
    // 次の 2 つは、ホストがつながると設定画面の値で上書きされる。
    m_streamAudioToQuest = true;
    m_muteSpeakerWhileConnected = settings::ReadBool("muteSpeakerWhileConnected", true);
}

// 描画条件が変わっても、接続時に確定した周辺の縮小率は維持する。
void DeviceProvider::ConfigureRenderDisplay() {
    std::lock_guard lock(m_foveationMutex);
    ApplyFoveation(m_foveationParams);
    m_networkServer.SetRenderDisplay(m_hmd->RenderLeftFov(), m_hmd->RenderRightFov(), m_hmd->RenderIpdMeters());
}

void DeviceProvider::ApplyFoveation(const FoveationParams& params) {
    const auto& [enabled, width, height, extendUp, ratioX, ratioY] = params;
    const auto foveation = enabled
        ? protocol::MakeFoveation(m_hmd->RenderLeftFov(), m_hmd->RenderRightFov(), width, height,
            ratioX, ratioY, extendUp)
        : protocol::Foveation{};
    Log("foveation 写像: enabled=%u width=%.2f height=%.2f extendUp=%.2f ratioX=%.2f ratioY=%.2f",
        static_cast<unsigned int>(foveation.enabled), static_cast<double>(width), static_cast<double>(height),
        static_cast<double>(extendUp), static_cast<double>(ratioX), static_cast<double>(ratioY));
    m_hmd->SetFoveation(foveation);
    m_networkServer.SetFoveation(foveation);
}

void DeviceProvider::Cleanup() {
    {
        // 停止直前の Hello や設定受信で録音・ミュートが再開されないようにする。
        std::lock_guard lock(m_audioMutex);
        m_audioStopping = true;
        if (m_speakerMute) {
            m_speakerMute->Restore();
        }
        m_audioCapture.Stop();
    }
    m_hostPipe.Stop();
    m_micPlayback.SetEnabled(false);
    if (m_hmd) {
        m_hmd->SetFrameSink(nullptr);
    }
    m_networkServer.Stop();
    {
        std::lock_guard lock(m_autoStopMutex);
        m_autoStopAt.reset();
    }
    // DLL の静的デストラクタではローダーロックがあるため、ここで専用スレッドを終了させる。
    m_speakerMute.reset();
    m_leftController.reset();
    m_rightController.reset();
    m_hmd.reset();
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

const char* const* DeviceProvider::GetInterfaceVersions() {
    return vr::k_InterfaceVersions;
}

void DeviceProvider::RunFrame() {
    {
        std::lock_guard lock(m_autoStopMutex);
        if (m_autoStopAt && Clock::now() >= *m_autoStopAt && !m_clientConnected.load()) {
            // 次のフレームで繰り返さないよう、要求する前に予定を消す。
            m_autoStopAt.reset();
            Log("SteamVR を終了します（自動終了）");
            vr::VRServerDriverHost()->VendorSpecificEvent(0, vr::VREvent_DriverRequestedQuit, {}, 0);
        }
    }
    SwitchBindAddressIfPending();
    RetryNetworkStartIfDue();
    m_networkServer.SetMicRequested(m_micPlayback.WantsAudio());
    m_statsCollector.SetPcMicState(static_cast<int>(m_micPlayback.State()));
    m_statsCollector.LogIfDue();
    if (m_hmd) {
        m_hmd->RunFrame();
    }
    vr::VREvent_t event{};
    while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
        // 使うイベントはまだ無いが、キューにためないよう読み捨てる。
    }
}

void DeviceProvider::ScheduleNetworkRetry() {
    m_nextNetworkRetry = Clock::now() + kNetworkRetryInterval;
}

// ホストの設定で待ち受けるアダプターが変わっていたら、Quest がつながっていないときに開き直す。
void DeviceProvider::SwitchBindAddressIfPending() {
    std::optional<std::string> pending;
    {
        std::lock_guard lock(m_networkConfigMutex);
        if (!m_clientConnected.load() && m_pendingBindAddress) {
            pending = std::exchange(m_pendingBindAddress, std::nullopt);
        }
        if (pending && *pending == m_networkConfig.bindAddress) {
            pending.reset();
        }
    }
    if (!pending) {
        return;
    }
    // 確認してから止めるまでのあいだに Quest がつながった場合は、次の機会に回す。
    if (m_clientConnected.load() || !m_networkServer.PauseIfDisconnected()) {
        std::lock_guard lock(m_networkConfigMutex);
        if (!m_pendingBindAddress) {
            m_pendingBindAddress = *pending;
        }
        return;
    }
    // Stop が受信スレッドを待つため、設定用ロックを保持したまま停止しない。
    m_networkServer.Stop();
    ConfigureRenderDisplay();
    NetworkServer::Config previous;
    {
        std::lock_guard lock(m_networkConfigMutex);
        previous = m_networkConfig;
    }
    auto next = previous;
    next.bindAddress = *pending;
    if (m_networkServer.Start(next)) {
        m_networkRetryPending = false;
        std::lock_guard lock(m_networkConfigMutex);
        m_networkConfig = next;
        Log("待ち受けるアドレスを切り替えました: %s", next.bindAddress.c_str());
        return;
    }
    Log("アドレスの切り替えに失敗しました。%s へ戻します", previous.bindAddress.c_str());
    ConfigureRenderDisplay();
    m_networkRetryPending = !m_networkServer.Start(previous);
    if (m_networkRetryPending) {
        Log("元のアドレスでも UDP サーバーを開始できません");
        ScheduleNetworkRetry();
    }
}

// ホストがポートを手放すのが遅れたときなどに、待ち受けの開始をやり直す。
void DeviceProvider::RetryNetworkStartIfDue() {
    if (!m_networkRetryPending || Clock::now() < m_nextNetworkRetry) {
        return;
    }
    NetworkServer::Config config;
    {
        std::lock_guard lock(m_networkConfigMutex);
        config = m_networkConfig;
    }
    ConfigureRenderDisplay();
    if (m_networkServer.Start(config)) {
        m_networkRetryPending = false;
        Log("再試行で UDP サーバーを開始しました: %s", config.bindAddress.c_str());
    } else {
        ScheduleNetworkRetry();
    }
}

bool DeviceProvider::ShouldBlockStandbyMode() {
    return false;
}

void DeviceProvider::EnterStandby() {
}

void DeviceProvider::LeaveStandby() {
}

HelloDecision DeviceProvider::OnHelloRequest(const protocol::HelloMessage& hello, const std::string& address) {
    {
        std::lock_guard lock(m_foveationMutex);
        if (m_pendingFoveationParams != m_foveationParams) {
            m_foveationParams = m_pendingFoveationParams;
            ApplyFoveation(m_foveationParams);
            Log("foveation の設定を切り替えました: enabled=%d ratio=%.1f",
                m_foveationParams.enabled ? 1 : 0, static_cast<double>(m_foveationParams.ratioX));
        }
        m_networkServer.SetFoveationPending(m_pendingFoveationParams != m_foveationParams);
    }
    if (!m_requireHostApproval) {
        return HelloDecision::Accept;
    }
    if (!ipc::IsValidDeviceId(hello.deviceId)) {
        return HelloDecision::Reject;
    }
    std::lock_guard lock(m_pairingMutex);
    if (Contains(m_approvedDeviceIds, hello.deviceId)) {
        Log("承認済み端末を受け付けます: ID %.8s %s", hello.deviceId.c_str(), hello.deviceName.c_str());
        return HelloDecision::Accept;
    }
    // Hello は 1 秒ごとに届くので、ホストへの通知とログは間隔を空ける。
    const auto now = Clock::now();
    std::erase_if(m_pairingTimes, [now](const auto& entry) {
        return now - entry.second.request >= kPairingForgetAfter;
    });
    auto [entry, inserted] = m_pairingTimes.try_emplace(hello.deviceId, PairingTimes{now, now});
    const bool logDue = inserted || now - entry->second.log >= kPairingLogInterval;
    if (Contains(m_rejectedDeviceIds, hello.deviceId)) {
        // Hello が続く間は記録を残し、削除と再登録によるログの連発を防ぐ。
        entry->second.request = now;
        if (logDue) {
            Log("端末を拒否しました: ID %.8s %s", hello.deviceId.c_str(), hello.deviceName.c_str());
            entry->second.log = now;
        }
        return HelloDecision::Reject;
    }
    if (inserted || now - entry->second.request >= kPairingRequestInterval) {
        m_hostPipe.Send(ipc::PairingRequestMessage{hello.deviceId, hello.deviceName, address});
        entry->second.request = now;
    }
    if (logDue) {
        Log("端末の承認待ち: ID %.8s %s", hello.deviceId.c_str(), hello.deviceName.c_str());
        entry->second.log = now;
    }
    return HelloDecision::PendingApproval;
}

void DeviceProvider::OnClientConnected(const protocol::HelloMessage& hello) {
    {
        std::lock_guard lock(m_autoStopMutex);
        m_clientConnected = true;
        m_autoStopAt.reset();
    }
    m_statsCollector.OnClientConnected(hello.deviceName, hello.sessionId);
    {
        std::lock_guard lock(m_audioMutex);
        if (!m_audioStopping && m_muteSpeakerWhileConnected && m_speakerMute) {
            m_speakerMute->Mute();
        }
        if (!m_audioStopping && m_audioEnabled && m_streamAudioToQuest) {
            m_audioCapture.Start();
        }
    }
    ForEachController([](ControllerDevice& controller) { controller.SetClientConnected(true); });
    if (m_hmd) {
        m_hmd->ApplyClientDisplay(hello);
        m_hmd->SetClientConnected(true);
        // 新しい相手は前のフレームを持っていないので、最初のフレームを単独で復号できる形にする。
        m_hmd->RequestKeyFrame();
    }
}

void DeviceProvider::OnClientDisconnected(DisconnectReason reason) {
    {
        std::lock_guard lock(m_autoStopMutex);
        m_clientConnected = false;
        if (reason == DisconnectReason::Bye && m_autoStopSteamVr) {
            m_autoStopAt = Clock::now() + kAutoStopDelay;
            Log("Quest のアプリが終了しました。15 秒以内につながらなければ SteamVR を終了します");
        }
    }
    m_statsCollector.OnClientDisconnected();
    {
        std::lock_guard lock(m_audioMutex);
        if (m_speakerMute) {
            m_speakerMute->Restore();
        }
        m_audioCapture.Stop();
    }
    ForEachController([](ControllerDevice& controller) { controller.SetClientConnected(false); });
    if (m_hmd) {
        m_hmd->SetClientConnected(false);
    }
}

void DeviceProvider::OnClientStats(const protocol::ClientStatsMessage& stats) {
    m_statsCollector.OnClientStats(stats);
}

void DeviceProvider::OnHostSettings(const ipc::SettingsMessage& settings) {
    {
        std::lock_guard lock(m_autoStopMutex);
        m_autoStopSteamVr = settings.autoStopSteamVr;
        if (!m_autoStopSteamVr) {
            m_autoStopAt.reset();
        }
    }
    {
        std::lock_guard lock(m_pairingMutex);
        m_approvedDeviceIds = settings.approvedDeviceIds;
        m_rejectedDeviceIds = settings.rejectedDeviceIds;
    }
    {
        std::lock_guard lock(m_foveationMutex);
        m_pendingFoveationParams.enabled = settings.foveatedEncoding;
        m_pendingFoveationParams.ratioX = settings.foveationEdgeRatio;
        m_pendingFoveationParams.ratioY = settings.foveationEdgeRatio;
        m_networkServer.SetFoveationPending(m_pendingFoveationParams != m_foveationParams);
    }
    const auto eyeSize = ipc::QuestEyeSizeForPercent(settings.questResolutionPercent);
    m_networkServer.SetQuestEyeSize(static_cast<uint16_t>(eyeSize.width), static_cast<uint16_t>(eyeSize.height));
    ApplyAudioSettings(settings);
    if (m_hmd) {
        m_hmd->SetBitrate(static_cast<uint32_t>(settings.bitrateMbps));
    }
    if (!settings.bindAddress.empty()) {
        if (!IsIpv4Address(settings.bindAddress)) {
            Log("ホストの bindAddress が不正です: %s", settings.bindAddress.c_str());
        } else {
            std::lock_guard lock(m_networkConfigMutex);
            // 実際の切り替えは RunFrame で行い、最新の保存値を優先する。
            m_pendingBindAddress = settings.bindAddress;
        }
    }
    Log("ホストの設定: bind=%s（Quest 未接続時に切り替え）bitrate=%dMbps audio=%d mute=%d questRes=%d%% foveation=%d ratio=%.1f",
        settings.bindAddress.c_str(), settings.bitrateMbps, settings.streamAudioToQuest ? 1 : 0,
        settings.muteSpeakerWhileConnected ? 1 : 0, settings.questResolutionPercent,
        settings.foveatedEncoding ? 1 : 0, static_cast<double>(settings.foveationEdgeRatio));
}

// 音の 2 つの設定は、Quest がつながっている最中に変えても、その場で録音とミュートに反映する。
void DeviceProvider::ApplyAudioSettings(const ipc::SettingsMessage& settings) {
    std::lock_guard lock(m_audioMutex);
    if (!m_audioStopping) {
        m_micPlayback.SetEnabled(settings.micToPc);
    }
    const bool streamChanged = m_streamAudioToQuest != settings.streamAudioToQuest;
    const bool muteChanged = m_muteSpeakerWhileConnected != settings.muteSpeakerWhileConnected;
    m_streamAudioToQuest = settings.streamAudioToQuest;
    m_muteSpeakerWhileConnected = settings.muteSpeakerWhileConnected;
    if (streamChanged) {
        Log("Quest への音声の送信: %s", m_streamAudioToQuest ? "有効" : "無効");
    }
    if (!m_clientConnected.load() || m_audioStopping) {
        return;
    }
    if (streamChanged && m_audioEnabled) {
        if (m_streamAudioToQuest) {
            m_audioCapture.Start();
        } else {
            m_audioCapture.Stop();
        }
    }
    if (muteChanged && m_speakerMute) {
        if (m_muteSpeakerWhileConnected) {
            m_speakerMute->Mute();
        } else {
            m_speakerMute->Restore();
        }
    }
}

void DeviceProvider::OnPose(const protocol::PoseMessage& pose) {
    if (m_leftController) {
        m_leftController->UpdatePose(pose.leftHand);
    }
    if (m_rightController) {
        m_rightController->UpdatePose(pose.rightHand);
    }
    if (m_hmd) {
        m_hmd->UpdateHeadPose(pose.head, pose.sampleTimeUs);
    }
}

void DeviceProvider::OnMicAudio(const protocol::MicAudioMessage& mic) {
    m_micPlayback.Push(mic);
}

void DeviceProvider::OnInput(const protocol::InputMessage& input) {
    if (m_leftController) {
        m_leftController->UpdateInput(input.left);
    }
    if (m_rightController) {
        m_rightController->UpdateInput(input.right);
    }
}

void DeviceProvider::OnAudio(const protocol::AudioMessage& audio) {
    m_networkServer.SendAudio(audio);
}

void DeviceProvider::OnKeyFrameNeeded() {
    if (m_hmd) {
        m_hmd->RequestKeyFrame();
    }
}

void DeviceProvider::OnDisplayFeedback(const protocol::DisplayFeedbackMessage& feedback) {
    if (m_hmd) {
        m_hmd->OnDisplayFeedback(feedback);
    }
}

}
