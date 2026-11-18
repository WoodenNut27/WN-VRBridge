#pragma once

#include "wnvr/protocol/messages.h"
#include "frame_assembler.h"
#include "config_file.h"
#include "audio_player.h"

#include <netinet/in.h>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>

namespace wnvr::quest {

class NetworkClient {
public:
    enum class ConnectionState { Searching, Connecting, PendingApproval, Starting, Stopped, Rejected, Busy, VersionMismatch, Connected };
    struct Status {
        ConnectionState state = ConnectionState::Searching;
        std::string hostName;
        std::string hostAddress;
        uint16_t hostProtocolVersion = 0;
    };
    Status GetStatus() const;
    struct RenderDisplay {
        protocol::Fov leftFov;
        protocol::Fov rightFov;
        float ipdMeters = 0;
    };

    struct ClientStats {
        FrameAssembler::ClientStats video;
        AudioPlayer::ClientStats audio;
        double roundTripMs = 0;
        uint64_t generation = 0;
    };
    ClientStats TakeClientStats();
    bool SendClientStats(const protocol::ClientStatsMessage& stats, uint64_t generation);
    NetworkClient() = default;
    ~NetworkClient();
    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;

    bool Open(const std::string& host, uint16_t port);
    bool OpenDiscovery();
    void Close();
    void Update(const protocol::HelloMessage& hello);
    bool SendPose(const protocol::PoseMessage& pose);
    void SendInput(const protocol::InputMessage& input);
    void SendMicAudio(const protocol::MicAudioMessage& mic);
    bool MicRequested() const;
    bool FoveationPending() const;
    void SendDisplayFeedback(const protocol::DisplayFeedbackMessage& message);
    void SendKeyFrameRequest(uint32_t lastGoodFrameIndex, protocol::KeyFrameReason reason);
    bool IsConnected() const;
    uint64_t ConnectionGeneration() const;
    std::optional<RenderDisplay> GetRenderDisplay() const;
    protocol::Foveation GetFoveation() const;
    std::optional<EyeSize> RequestedEyeSize() const;
    std::optional<AssembledFrame> PopFrame();

private:
    using Clock = std::chrono::steady_clock;
    bool OpenSocket(bool broadcast);
    bool StartReceiving();
    void Receive();
    // ここから 3 つは受信スレッドで、m_mutex を取ってから呼ぶ。
    void HandleDatagram(const sockaddr_in& sender, socklen_t senderSize, bool truncated,
        std::span<const std::byte> data, int64_t kernelTimestampNs);
    void HandleDiscoverReply(const sockaddr_in& sender, const protocol::DiscoverReplyMessage& reply,
        Clock::time_point now);
    void HandleHelloAck(const protocol::HelloAckMessage& ack, Clock::time_point now);
    void BeginDiscovery();
    void SetRequestedEyeSize(uint16_t width, uint16_t height);
    bool Send(const protocol::Message& message);
    void Disconnect(const char* reason, bool rediscover = true);
    void LogIoError(const char* operation, int error);

    int m_socket = -1;
    // Open / Close は描画スレッドから呼び、ソケットは join 完了まで維持する。
    mutable std::mutex m_mutex;
    std::atomic<bool> m_stopping{true};
    std::thread m_receiveThread;
    FrameAssembler m_assembler;
    AudioPlayer m_audioPlayer;
    sockaddr_in m_destination{};
    bool m_connected = false;
    bool m_micRequested = false;
    bool m_foveationPending = false;
    std::string m_hostName;
    std::string m_hostAddress;
    std::optional<protocol::HelloAckMessage> m_lastAck;
    Clock::time_point m_lastAckTime{};
    std::optional<RenderDisplay> m_renderDisplay;
    protocol::Foveation m_foveation;
    std::optional<EyeSize> m_requestedEyeSize;
    uint64_t m_connectionGeneration = 0;
    bool m_discovery = false;
    bool m_searching = false;
    unsigned m_helloAttempts = 0;
    uint32_t m_sequence = 0;
    uint64_t m_pingId = 0;
    uint64_t m_poseCount = 0;
    uint64_t m_inputCount = 0;
    protocol::InputMessage m_lastInput;
    std::array<uint64_t, 4> m_keyFrameRequests{};
    std::optional<double> m_roundTripMs;
    Clock::time_point m_lastReceived{};
    Clock::time_point m_nextHello{};
    Clock::time_point m_nextDiscover{};
    std::optional<Clock::time_point> m_lastDiscoveryLog;
    Clock::time_point m_nextPing{};
    Clock::time_point m_nextReport{};
    std::optional<Clock::time_point> m_lastIoLog;
    std::optional<Clock::time_point> m_lastParseLog;
    std::array<std::optional<Clock::time_point>, 7> m_lastAckLog{};
};

}
