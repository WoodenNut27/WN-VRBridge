#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include "network_server.h"

#include "driver_log.h"
#include "stats_collector.h"

#include <wnvr/win32/platform.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace wnvr::driver {
namespace {

using Clock = std::chrono::steady_clock;

// 送る前のフレームをためておく数。これを超えたら、いちばん古いものを捨てる。
constexpr size_t kMaxQueuedFrames = 3;
// 断片をこの数ずつまとめて送り、まとまりごとに送信の間隔を空ける（ペース配分）。
constexpr size_t kFragmentBatchSize = 8;
// 1 フレーム分の断片を一度に書き込んでも、あふれない大きさ。
constexpr int kSendBufferBytes = 8 * 1024 * 1024;
// Quest からのキーフレームの要求は、この間隔より短くは受け付けない。
constexpr auto kKeyFrameRequestInterval = std::chrono::milliseconds(100);
constexpr auto kStatsLogInterval = std::chrono::seconds(5);
constexpr auto kDiscoveryLogInterval = std::chrono::seconds(10);
constexpr auto kParseErrorLogInterval = std::chrono::seconds(1);
constexpr size_t kParseErrorCount = static_cast<size_t>(protocol::ParseError::TrailingData) + 1;

bool SameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_addr.s_addr == right.sin_addr.s_addr && left.sin_port == right.sin_port;
}

std::string AddressText(const sockaddr_in& endpoint) {
    char address[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &endpoint.sin_addr, address, sizeof(address));
    return address;
}

}

struct NetworkServer::Impl {
    std::atomic<bool> m_micRequested{false};
    std::atomic<bool> m_foveationPending{false};
    Impl(NetworkListener& listener, StatsCollector* statsCollector)
        : m_listener(listener), m_statsCollector(statsCollector) {}

    void Send(const sockaddr_in& destination, const protocol::Message& message) {
        std::lock_guard lock(m_sendMutex);
        SendPacket(destination, message);
    }

    int SendPacket(const sockaddr_in& destination, const protocol::Message& message) {
        std::array<std::byte, protocol::kMaxPacketSize> buffer{};
        protocol::PacketHeader header{protocol::TypeOf(message)};
        header.sequence = m_sequence++;
        header.timestampUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
        const auto size = protocol::Serialize(header, message, buffer);
        if (size == 0) {
            Log("UDP パケットのシリアライズに失敗しました。");
            return WSAEINVAL;
        }
        if (sendto(m_socket, reinterpret_cast<const char*>(buffer.data()), static_cast<int>(size), 0,
                reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAENOBUFS) {
                Log("UDP パケットの送信に失敗しました: %d", error);
            }
            return error;
        }
        return 0;
    }

    void SendVideoFrame(const EncodedFrame& frame) {
        bool dropped = false;
        {
            std::lock_guard lock(m_clientMutex);
            if (!m_accepting || !m_client || frame.data.empty()) {
                return;
            }
            // 送信中のフレームはキュー外に置き、未送信の最古だけを捨てる。
            m_videoQueue.push_back(frame);
            if (m_videoQueue.size() > kMaxQueuedFrames) {
                m_videoQueue.pop_front();
                ++m_queueDroppedFrames;
                if (m_statsCollector) {
                    m_statsCollector->OnSendDroppedFrame();
                }
                dropped = true;
            }
        }
        if (dropped) {
            m_listener.OnKeyFrameNeeded();
        }
        m_videoCondition.notify_one();
    }

    bool TransmitVideoFrame(const EncodedFrame& frame, const sockaddr_in& client,
        uint64_t generation, Clock::time_point start, std::stop_token token) {
        const size_t count = (frame.data.size() - 1) / protocol::kMaxVideoPayloadSize + 1;
        if (frame.width == 0 || frame.height == 0 || frame.width > std::numeric_limits<uint16_t>::max()
            || frame.height > std::numeric_limits<uint16_t>::max()
            || count > std::numeric_limits<uint16_t>::max() || frame.data.size() > std::numeric_limits<uint32_t>::max()) {
            Log("映像フレームが大きすぎるため送信を破棄しました: %zu bytes", frame.data.size());
            std::lock_guard lock(m_sendMutex);
            ++m_droppedFrames;
            if (m_statsCollector) {
                m_statsCollector->OnSendDroppedFrame();
            }
            return true;
        }
        protocol::Message message = protocol::VideoFragmentMessage{};
        auto& fragment = std::get<protocol::VideoFragmentMessage>(message);
        fragment.frameIndex = static_cast<uint32_t>(frame.frameIndex);
        fragment.fragmentCount = static_cast<uint16_t>(count);
        fragment.frameSize = static_cast<uint32_t>(frame.data.size());
        fragment.flags = frame.keyFrame ? 1 : 0;
        fragment.videoWidth = static_cast<uint16_t>(frame.width);
        fragment.videoHeight = static_cast<uint16_t>(frame.height);
        fragment.pose = frame.pose;
        const double window = static_cast<double>(m_config.pacingRatio) / m_config.frameRate;
        bool pacing = window > 0.0;
        Clock::time_point firstSent{};
        Clock::time_point lastSent{};
        for (size_t batch = 0; batch < count; batch += kFragmentBatchSize) {
            {
                std::lock_guard lock(m_clientMutex);
                if (token.stop_requested() || !m_client || generation != m_clientGeneration) {
                    return false;
                }
                // 一度追いつき始めたら、このフレームの残りは待たずに送る。
                pacing = pacing && m_videoQueue.empty();
            }
            if (pacing) {
                std::this_thread::sleep_until(start + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(window * static_cast<double>(batch) / static_cast<double>(count))));
            }
            // 切断をまたいで古いフレームを送らない。待機中はどちらのロックも保持しない。
            std::lock_guard clientLock(m_clientMutex);
            if (token.stop_requested() || !m_client || generation != m_clientGeneration) {
                return false;
            }
            std::lock_guard sendLock(m_sendMutex);
            for (size_t index = batch; index < std::min(batch + kFragmentBatchSize, count); ++index) {
                const size_t offset = index * protocol::kMaxVideoPayloadSize;
                const size_t size = std::min(protocol::kMaxVideoPayloadSize, frame.data.size() - offset);
                fragment.fragmentIndex = static_cast<uint16_t>(index);
                fragment.payload.assign(frame.data.data() + offset, frame.data.data() + offset + size);
                if (SendPacket(client, message) != 0) {
                    ++m_droppedFrames;
                    if (m_statsCollector) {
                        m_statsCollector->OnSendDroppedFrame();
                    }
                    return true;
                }
                const auto sent = Clock::now();
                if (index == 0) {
                    firstSent = sent;
                }
                lastSent = sent;
                ++m_sentFragments;
                if (m_statsCollector) {
                    m_statsCollector->OnVideoSent(1, size);
                }
                m_sentBytes += protocol::kHeaderSize + protocol::kVideoFragmentHeaderSize + size;
            }
        }
        std::lock_guard lock(m_sendMutex);
        ++m_sentFrames;
        if (m_statsCollector) {
            m_statsCollector->OnFrameTiming(frame.timing, firstSent, lastSent);
        }
        return false;
    }

    void Transmit(std::stop_token token) {
        const bool timerStarted = timeBeginPeriod(1) == TIMERR_NOERROR;
        if (!timerStarted) {
            Log("送信スレッドのタイマー精度を 1ms に設定できませんでした。");
        }
        try {
            while (!token.stop_requested()) {
                EncodedFrame frame;
                sockaddr_in client{};
                uint64_t generation = 0;
                Clock::time_point start;
                {
                    std::unique_lock lock(m_clientMutex);
                    m_videoCondition.wait(lock, [this, token] {
                        return token.stop_requested() || !m_accepting || !m_videoQueue.empty();
                    });
                    if (token.stop_requested() || !m_accepting) {
                        break;
                    }
                    frame = std::move(m_videoQueue.front());
                    m_videoQueue.pop_front();
                    start = Clock::now();
                    client = *m_client;
                    generation = m_clientGeneration;
                }
                const bool keyFrameNeeded = TransmitVideoFrame(frame, client, generation, start, token);
                const double sendMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                {
                    std::lock_guard lock(m_sendMutex);
                    ++m_timedFrames;
                    m_totalSendMs += sendMs;
                    m_maxSendMs = std::max(m_maxSendMs, sendMs);
                }
                if (keyFrameNeeded) {
                    m_listener.OnKeyFrameNeeded();
                }
            }
        } catch (const std::exception& error) {
            Log("映像送信スレッドを停止します: %s", error.what());
        } catch (...) {
            Log("映像送信スレッドで予期しない例外が発生しました。");
        }
        {
            std::lock_guard lock(m_clientMutex);
            m_accepting = false;
            m_videoQueue.clear();
        }
        if (timerStarted) {
            timeEndPeriod(1);
        }
    }

    void LogVideoStats(Clock::time_point now) {
        std::lock_guard lock(m_sendMutex);
        const double elapsed = std::chrono::duration<double>(now - m_statsTime).count();
        if (elapsed < std::chrono::duration<double>(kStatsLogInterval).count()) {
            return;
        }
        const uint64_t queueDroppedFrames = m_queueDroppedFrames.exchange(0);
        if (m_client) {
            Log("映像送信: frames=%llu, fragments=%llu, %.2f Mbps, 破棄=%llu, キュー破棄=%llu, send 平均 %.2f / 最大 %.2f ms",
                static_cast<unsigned long long>(m_sentFrames), static_cast<unsigned long long>(m_sentFragments),
                static_cast<double>(m_sentBytes) * 8.0 / elapsed / 1000000.0,
                static_cast<unsigned long long>(m_droppedFrames),
                static_cast<unsigned long long>(queueDroppedFrames),
                m_timedFrames > 0 ? m_totalSendMs / static_cast<double>(m_timedFrames) : 0.0, m_maxSendMs);
        }
        Log("キーフレーム要求: received=%llu, ignored=%llu, DecoderStart=%llu, FrameGap=%llu, DecoderDrop=%llu, DecoderError=%llu",
            static_cast<unsigned long long>(m_keyFrameRequests), static_cast<unsigned long long>(m_ignoredKeyFrameRequests),
            static_cast<unsigned long long>(m_keyFrameReasons[0]), static_cast<unsigned long long>(m_keyFrameReasons[1]),
            static_cast<unsigned long long>(m_keyFrameReasons[2]), static_cast<unsigned long long>(m_keyFrameReasons[3]));
        m_keyFrameRequests = m_ignoredKeyFrameRequests = 0;
        m_keyFrameReasons.fill(0);
        m_timedFrames = 0;
        m_totalSendMs = m_maxSendMs = 0;
        m_statsTime = now;
        m_sentFrames = m_sentFragments = m_sentBytes = m_droppedFrames = 0;
    }

    void Disconnect(DisconnectReason reason, const char* text) {
        m_lastKeyFrameRequest.reset();
        {
            std::lock_guard lock(m_clientMutex);
            m_client.reset();
            ++m_clientGeneration;
            m_videoQueue.clear();
        }
        Log("クライアントを切断しました: %s", text);
        m_listener.OnClientDisconnected(reason);
    }

    void CheckTimeout(Clock::time_point now) {
        if (m_client && now - m_lastReceived >= m_config.clientTimeout) {
            Disconnect(DisconnectReason::Timeout, "タイムアウト");
        }
    }

    protocol::HelloAckMessage MakeHelloAck(protocol::HelloResult result) {
        // どの返事にも描画中の視野角を入れておく（Quest は Accepted のときだけ使う）。
        std::lock_guard lock(m_foveationMutex);
        return {result, protocol::kProtocolVersion, m_renderLeftFov, m_renderRightFov, m_renderIpdMeters,
            m_foveation, m_questEyeWidth, m_questEyeHeight};
    }

    // 発見用のポートに届いたパケット。Quest のブロードキャストに、待ち受けているポートを答える。
    void HandleDiscovery(const sockaddr_in& sender, const protocol::ParseResult& parsed, Clock::time_point now) {
        if (!parsed.packet || !std::holds_alternative<protocol::DiscoverMessage>(parsed.packet->message)) {
            return;
        }
        Send(sender, protocol::DiscoverReplyMessage{m_hostName, m_config.port});
        // 古い記録を捨て、探索端末が増えてもログ抑制用の状態が残り続けないようにする。
        std::erase_if(m_lastDiscoveryLog, [now](const auto& entry) {
            return now - entry.second >= kDiscoveryLogInterval;
        });
        const uint64_t endpoint = (static_cast<uint64_t>(sender.sin_addr.s_addr) << 16) | sender.sin_port;
        if (m_lastDiscoveryLog.emplace(endpoint, now).second) {
            Log("PC の発見要求に応答しました: %s:%u", AddressText(sender).c_str(),
                static_cast<unsigned>(ntohs(sender.sin_port)));
        }
    }

    void HandleHello(const sockaddr_in& sender, const protocol::HelloMessage& hello, bool fromClient,
        Clock::time_point now) {
        if (m_client && !fromClient) {
            Log("クライアントが接続中のため Busy を返します: device=%s, %ux%u, %.2f Hz",
                hello.deviceName.c_str(), hello.eyeWidth, hello.eyeHeight, hello.refreshRate);
            Send(sender, MakeHelloAck(protocol::HelloResult::Busy));
            return;
        }
        if (!fromClient) {
            const auto decision = m_listener.OnHelloRequest(hello, AddressText(sender));
            if (decision != HelloDecision::Accept) {
                Send(sender, MakeHelloAck(decision == HelloDecision::PendingApproval
                    ? protocol::HelloResult::PendingApproval : protocol::HelloResult::Rejected));
                return;
            }
        }
        {
            std::lock_guard lock(m_clientMutex);
            if (!m_accepting) {
                return;
            }
            m_client = sender;
        }
        m_lastReceived = now;
        // 接続中の Quest からの Hello（返事が届かなかったときの送り直し）にも、同じ返事を返す。
        Send(sender, MakeHelloAck(protocol::HelloResult::Accepted));
        if (!fromClient) {
            Log("クライアントが接続しました: device=%s, %ux%u, %.2f Hz",
                hello.deviceName.c_str(), hello.eyeWidth, hello.eyeHeight, hello.refreshRate);
            m_listener.OnClientConnected(hello);
        }
    }

    void HandleKeyFrameRequest(const protocol::KeyFrameRequestMessage& request, Clock::time_point now) {
        ++m_keyFrameRequests;
        ++m_keyFrameReasons[static_cast<size_t>(request.reason)];
        // PC 内部の破棄による要求とは独立に、Quest の連続要求だけを制限する。
        if (m_lastKeyFrameRequest && now - *m_lastKeyFrameRequest <= kKeyFrameRequestInterval) {
            ++m_ignoredKeyFrameRequests;
        } else {
            m_lastKeyFrameRequest = now;
            m_listener.OnKeyFrameNeeded();
        }
    }

    // 接続中の Quest から届いた、Hello 以外のメッセージ。
    void HandleClientMessage(const sockaddr_in& sender, const protocol::Packet& packet, Clock::time_point now) {
        const auto& message = packet.message;
        if (const auto* pose = std::get_if<protocol::PoseMessage>(&message)) {
            m_listener.OnPose(*pose);
        } else if (const auto* input = std::get_if<protocol::InputMessage>(&message)) {
            m_listener.OnInput(*input);
        } else if (const auto* mic = std::get_if<protocol::MicAudioMessage>(&message)) {
            m_listener.OnMicAudio(*mic);
        } else if (const auto* stats = std::get_if<protocol::ClientStatsMessage>(&message)) {
            m_listener.OnClientStats(*stats);
        } else if (const auto* feedback = std::get_if<protocol::DisplayFeedbackMessage>(&message)) {
            m_listener.OnDisplayFeedback(*feedback);
        } else if (const auto* request = std::get_if<protocol::KeyFrameRequestMessage>(&message)) {
            HandleKeyFrameRequest(*request, now);
        } else if (const auto* ping = std::get_if<protocol::PingMessage>(&message)) {
            protocol::PongMessage pong{ping->pingId, packet.header.timestampUs,
                static_cast<uint8_t>(m_micRequested.load())};
            {
                std::lock_guard lock(m_foveationMutex);
                pong.questEyeWidth = m_questEyeWidth;
                pong.questEyeHeight = m_questEyeHeight;
            }
            pong.foveationPending = static_cast<uint8_t>(m_foveationPending.load());
            Send(sender, pong);
        } else if (std::holds_alternative<protocol::ByeMessage>(message)) {
            Disconnect(DisconnectReason::Bye, "Bye を受信");
        }
    }

    // 接続用のポートに届いたパケット。
    void HandleDatagram(const sockaddr_in& sender, const protocol::ParseResult& parsed, Clock::time_point now) {
        if (parsed.packet && std::holds_alternative<protocol::DiscoverMessage>(parsed.packet->message)) {
            return;
        }
        // Parse に失敗した場合も、現在の相手からの受信ならタイムアウトを延長する。
        const bool fromClient = m_client && SameEndpoint(*m_client, sender);
        if (fromClient) {
            m_lastReceived = now;
        }
        if (parsed.error == protocol::ParseError::VersionMismatch) {
            Log("クライアントの VersionMismatch: version=%u", static_cast<unsigned>(parsed.receivedVersion));
            Send(sender, MakeHelloAck(protocol::HelloResult::VersionMismatch));
            return;
        }
        if (parsed.error != protocol::ParseError::None) {
            auto& lastLog = m_lastErrorLog[static_cast<size_t>(parsed.error)];
            if (!lastLog || now - *lastLog >= kParseErrorLogInterval) {
                Log("UDP パケットを破棄しました: %s", protocol::ToString(parsed.error));
                lastLog = now;
            }
            return;
        }
        const auto& packet = *parsed.packet;
        if (const auto* hello = std::get_if<protocol::HelloMessage>(&packet.message)) {
            HandleHello(sender, *hello, fromClient, now);
        } else if (fromClient) {
            HandleClientMessage(sender, packet, now);
        }
    }

    // ソケットから 1 つ読んで処理する。stop が要求されていたら false。
    bool ReceiveFrom(SOCKET socket, std::span<std::byte> buffer, std::stop_token stopToken) {
        const bool discovery = socket == m_discoverySocket;
        sockaddr_in sender{};
        int senderSize = sizeof(sender);
        const int size = recvfrom(socket, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&sender), &senderSize);
        const auto now = Clock::now();
        CheckTimeout(now);
        if (size == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            // 大きすぎるパケットも、現在の相手から届いた事実は生存確認に使う。
            if (!discovery && error == WSAEMSGSIZE && m_client && SameEndpoint(*m_client, sender)) {
                m_lastReceived = now;
            }
            if (error != WSAETIMEDOUT && error != WSAEWOULDBLOCK && error != WSAEMSGSIZE && error != WSAECONNRESET) {
                Log("UDP の受信に失敗しました: %d", error);
            }
            return true;
        }
        if (stopToken.stop_requested()) {
            return false;
        }
        const auto parsed = protocol::Parse(buffer.first(static_cast<size_t>(size)));
        if (discovery) {
            HandleDiscovery(sender, parsed, now);
        } else {
            HandleDatagram(sender, parsed, now);
        }
        return true;
    }

    void Receive(std::stop_token stopToken) {
        std::array<std::byte, 2048> buffer{};
        while (!stopToken.stop_requested()) {
            LogVideoStats(Clock::now());
            CheckTimeout(Clock::now());
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(m_socket, &readable);
            if (m_discoverySocket != INVALID_SOCKET) {
                FD_SET(m_discoverySocket, &readable);
            }
            // 何も届かなくても、タイムアウトの確認と統計のログのために 100ms ごとに起きる。
            timeval timeout{0, 100000};
            const int ready = select(0, &readable, nullptr, nullptr, &timeout);
            CheckTimeout(Clock::now());
            if (ready == SOCKET_ERROR) {
                Log("UDP の select に失敗しました: %d", WSAGetLastError());
                break;
            }
            for (const SOCKET socket : {m_socket, m_discoverySocket}) {
                if (stopToken.stop_requested()) {
                    break;
                }
                if (socket == INVALID_SOCKET || !FD_ISSET(socket, &readable)) {
                    continue;
                }
                if (!ReceiveFrom(socket, buffer, stopToken)) {
                    break;
                }
            }
        }
        if (m_client) {
            Send(*m_client, protocol::ByeMessage{});
            Disconnect(DisconnectReason::ServerStopped, "サーバーを停止");
        }
    }

    std::mutex m_foveationMutex;
    protocol::Foveation m_foveation;
    uint16_t m_questEyeWidth = 0;
    uint16_t m_questEyeHeight = 0;
    protocol::Fov m_renderLeftFov;
    protocol::Fov m_renderRightFov;
    float m_renderIpdMeters = 0.063f;
    NetworkListener& m_listener;
    StatsCollector* m_statsCollector = nullptr;
    Config m_config;
    SOCKET m_socket = INVALID_SOCKET;
    SOCKET m_discoverySocket = INVALID_SOCKET;
    std::string m_hostName;
    bool m_winsockStarted = false;
    std::jthread m_thread;
    std::jthread m_sendThread;
    std::optional<sockaddr_in> m_client;
    Clock::time_point m_lastReceived{};
    // 受信スレッドだけが更新・集計する。
    std::array<std::optional<Clock::time_point>, kParseErrorCount> m_lastErrorLog{};
    std::map<uint64_t, Clock::time_point> m_lastDiscoveryLog;
    std::optional<Clock::time_point> m_lastKeyFrameRequest;
    uint64_t m_keyFrameRequests = 0;
    uint64_t m_ignoredKeyFrameRequests = 0;
    std::array<uint64_t, 4> m_keyFrameReasons{};
    std::atomic<uint32_t> m_sequence{0};
    std::mutex m_clientMutex;
    std::condition_variable m_videoCondition;
    std::deque<EncodedFrame> m_videoQueue;
    bool m_accepting = false;
    uint64_t m_clientGeneration = 0;
    std::atomic<uint64_t> m_queueDroppedFrames{0};
    std::mutex m_sendMutex;
    Clock::time_point m_statsTime = Clock::now();
    uint64_t m_sentFrames = 0;
    uint64_t m_sentFragments = 0;
    uint64_t m_sentBytes = 0;
    uint64_t m_droppedFrames = 0;
    uint64_t m_timedFrames = 0;
    double m_totalSendMs = 0;
    double m_maxSendMs = 0;
};

NetworkServer::NetworkServer(NetworkListener& listener, StatsCollector* statsCollector)
    : m_impl(std::make_unique<Impl>(listener, statsCollector)) {}

NetworkServer::~NetworkServer() {
    Stop();
}

void NetworkServer::SetRenderDisplay(const protocol::Fov& left, const protocol::Fov& right, float ipdMeters) {
    m_impl->m_renderLeftFov = left;
    m_impl->m_renderRightFov = right;
    m_impl->m_renderIpdMeters = ipdMeters;
}

void NetworkServer::SetFoveation(const protocol::Foveation& foveation) {
    std::lock_guard lock(m_impl->m_foveationMutex);
    m_impl->m_foveation = foveation;
}

void NetworkServer::SetFoveationPending(bool pending) {
    m_impl->m_foveationPending = pending;
}

void NetworkServer::SetQuestEyeSize(uint16_t width, uint16_t height) {
    std::lock_guard lock(m_impl->m_foveationMutex);
    m_impl->m_questEyeWidth = width;
    m_impl->m_questEyeHeight = height;
}

bool NetworkServer::Start(const Config& config) {
    Stop();
    m_impl->m_config = config;
    WSADATA data{};
    const int startupError = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupError != 0) {
        Log("WSAStartup に失敗しました: %d", startupError);
        return false;
    }
    m_impl->m_winsockStarted = true;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.bindAddress.c_str(), &address.sin_addr) != 1
        || config.port == 0 || config.clientTimeout.count() <= 0 || config.frameRate == 0
        || !std::isfinite(config.pacingRatio) || config.pacingRatio < 0.0f || config.pacingRatio > 1.0f) {
        Log("UDP サーバーの設定が不正です。");
        Stop();
        return false;
    }
    m_impl->m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_impl->m_socket == INVALID_SOCKET) {
        Log("UDP ソケットの作成に失敗しました: %d", WSAGetLastError());
        Stop();
        return false;
    }
    if (bind(m_impl->m_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        Log("UDP の bind に失敗しました: %d", WSAGetLastError());
        Stop();
        return false;
    }
    if (setsockopt(m_impl->m_socket, SOL_SOCKET, SO_SNDBUF,
            reinterpret_cast<const char*>(&kSendBufferBytes), sizeof(kSendBufferBytes)) == SOCKET_ERROR) {
        Log("SO_SNDBUF の設定に失敗しました: %d", WSAGetLastError());
    }
    u_long nonblocking = 1;
    if (ioctlsocket(m_impl->m_socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        Log("UDP の非ブロッキング設定に失敗しました: %d", WSAGetLastError());
        Stop();
        return false;
    }
    m_impl->m_hostName = win32::ComputerName(protocol::kMaxHostNameLength, "WN-VRBridge PC");
    m_impl->m_lastErrorLog.fill(std::nullopt);
    m_impl->m_lastDiscoveryLog.clear();
    m_impl->m_discoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_impl->m_discoverySocket == INVALID_SOCKET) {
        Log("発見用 UDP ソケットの作成に失敗しました: %d", WSAGetLastError());
    } else {
        address.sin_port = htons(protocol::kDiscoveryPort);
        if (bind(m_impl->m_discoverySocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            Log("発見用 UDP の bind に失敗しました: %d", WSAGetLastError());
            closesocket(m_impl->m_discoverySocket);
            m_impl->m_discoverySocket = INVALID_SOCKET;
        }
    }
    try {
        {
            std::lock_guard lock(m_impl->m_clientMutex);
            m_impl->m_accepting = true;
        }
        m_impl->m_sendThread = std::jthread([impl = m_impl.get()](std::stop_token token) { impl->Transmit(token); });
        m_impl->m_thread = std::jthread([impl = m_impl.get()](std::stop_token token) { impl->Receive(token); });
    } catch (const std::exception& error) {
        Log("UDP スレッドの作成に失敗しました: %s", error.what());
        Stop();
        return false;
    }
    Log("UDP の待ち受けを開始しました: %s:%u, 発見用ポート=%u (%s)", config.bindAddress.c_str(),
        static_cast<unsigned>(config.port), static_cast<unsigned>(protocol::kDiscoveryPort),
        m_impl->m_discoverySocket != INVALID_SOCKET ? "有効" : "無効");
    return true;
}

bool NetworkServer::PauseIfDisconnected() {
    std::lock_guard lock(m_impl->m_clientMutex);
    if (m_impl->m_client) { return false; }
    m_impl->m_accepting = false;
    return true;
}

void NetworkServer::Stop() {
    {
        std::lock_guard lock(m_impl->m_clientMutex);
        m_impl->m_accepting = false;
        m_impl->m_videoQueue.clear();
    }
    if (m_impl->m_sendThread.joinable()) {
        m_impl->m_sendThread.request_stop();
        m_impl->m_videoCondition.notify_all();
        m_impl->m_sendThread.join();
    }
    if (m_impl->m_thread.joinable()) {
        m_impl->m_thread.request_stop();
        m_impl->m_thread.join();
    }
    std::lock_guard lock(m_impl->m_sendMutex);
    // Bye の送信とコールバックが完了するまでソケットと Winsock を維持する。
    if (m_impl->m_discoverySocket != INVALID_SOCKET) {
        closesocket(m_impl->m_discoverySocket);
        m_impl->m_discoverySocket = INVALID_SOCKET;
    }
    if (m_impl->m_socket != INVALID_SOCKET) {
        closesocket(m_impl->m_socket);
        m_impl->m_socket = INVALID_SOCKET;
    }
    if (m_impl->m_winsockStarted) {
        WSACleanup();
        m_impl->m_winsockStarted = false;
    }
}

void NetworkServer::SendVideoFrame(const EncodedFrame& frame) {
    m_impl->SendVideoFrame(frame);
}

void NetworkServer::SetMicRequested(bool requested) {
    m_impl->m_micRequested = requested;
}

void NetworkServer::SendAudio(const protocol::AudioMessage& audio) {
    // 映像と同じロック順で、切断・接続先変更をまたぐ送信を防ぐ。
    std::lock_guard lock(m_impl->m_clientMutex);
    if (m_impl->m_accepting && m_impl->m_client) {
        m_impl->Send(*m_impl->m_client, audio);
    }
}

}
