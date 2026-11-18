#pragma once

#include <wnvr/protocol/messages.h>
#include "video_encoder.h"

#include <chrono>
#include <memory>
#include <string>

namespace wnvr::driver {

// 受信・送信・エンコーダーのスレッドから呼ばれる。実装側でスレッドの安全性を保つこと。
class StatsCollector;

enum class DisconnectReason { Bye, Timeout, ServerStopped };

enum class HelloDecision { Accept, PendingApproval, Reject };

class NetworkListener {
public:
    virtual ~NetworkListener() = default;
    // 受信スレッドを止めないよう、承認の UI はここでは待たない。
    virtual HelloDecision OnHelloRequest(const protocol::HelloMessage& hello, const std::string& address) = 0;
    virtual void OnClientConnected(const protocol::HelloMessage& hello) = 0;
    virtual void OnClientDisconnected(DisconnectReason reason) = 0;
    virtual void OnClientStats(const protocol::ClientStatsMessage& stats) = 0;
    virtual void OnPose(const protocol::PoseMessage& pose) = 0;
    virtual void OnInput(const protocol::InputMessage& input) = 0;
    virtual void OnMicAudio(const protocol::MicAudioMessage& mic) = 0;
    virtual void OnKeyFrameNeeded() = 0;
    virtual void OnDisplayFeedback(const protocol::DisplayFeedbackMessage& feedback) = 0;
};

class NetworkServer : public EncodedFrameSink {
public:
    struct Config {
        std::string bindAddress = "0.0.0.0";
        uint16_t port = protocol::kDefaultPort;
        std::chrono::milliseconds clientTimeout{2000};
        uint32_t frameRate = 72;
        float pacingRatio = 0.0f;
    };

    explicit NetworkServer(NetworkListener& listener, StatsCollector* statsCollector = nullptr);
    ~NetworkServer();
    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;

    bool Start(const Config& config);
    // Start 前に設定し、稼働中は描画条件を固定する。
    void SetRenderDisplay(const protocol::Fov& left, const protocol::Fov& right, float ipdMeters);
    void Stop();
    void SetFoveation(const protocol::Foveation& foveation);
    void SetQuestEyeSize(uint16_t width, uint16_t height);
    void SetFoveationPending(bool pending);
    // 未接続の確認と新規接続の抑止を一度に行い、アドレス変更直前の Hello を防ぐ。
    bool PauseIfDisconnected();
    // 未接続なら何もしない。フレームをコピーしてキューに入れ、送信完了を待たずに戻る。
    void SendVideoFrame(const EncodedFrame& frame);
    void SendAudio(const protocol::AudioMessage& audio);
    void SetMicRequested(bool requested);
    void OnEncodedFrame(const EncodedFrame& frame) override { SendVideoFrame(frame); }

private:
    // Windows の型を公開ヘッダに持ち込まないため、ソケットを実装側に隠す。
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
