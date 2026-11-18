// NDK の拡張ソケット API を、ビルド環境の既定マクロに依存せず宣言させる。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "network_client.h"
#include "log.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <exception>

#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace wnvr::quest {
namespace {

// 1 回の recvmmsg で読む数と、1 個あたりのバッファの大きさ（正しいパケットは 1400 バイト以下）。
constexpr unsigned kReceiveBatch = 32;
constexpr size_t kReceiveBufferSize = 2048;
// 送信待ちがあふれても取りこぼさないよう、カーネルの受信バッファを大きく取る。
constexpr int kSocketReceiveBufferBytes = 8 * 1024 * 1024;
constexpr int kReceiveThreadNice = -10;
constexpr int kPollTimeoutMs = 100;
constexpr auto kLogInterval = std::chrono::seconds(5);
constexpr auto kReceiveTimeout = std::chrono::seconds(3);
constexpr auto kRetryInterval = std::chrono::seconds(1);
constexpr auto kDiscoveryLogInterval = std::chrono::seconds(10);
// 見つけた PC に Hello を送っても返事が無いとき、この回数であきらめて探し直す。
constexpr unsigned kMaxHelloAttempts = 5;
// 拒否や承認待ちの返事は、この時間だけ状態の表示に使う（それより古ければ「接続中」に戻す）。
constexpr auto kAckDisplayDuration = std::chrono::seconds(3);

int64_t ToNanoseconds(const timespec& time) {
    return static_cast<int64_t>(time.tv_sec) * 1000000000LL + time.tv_nsec;
}

// カーネルがパケットを受け取った時刻（SO_TIMESTAMPNS）。取れなければ 0。
// アプリが読むまでの遅れを測り、受信が追いついているかを見るのに使う。
int64_t KernelTimestampNs(msghdr& header) {
    if ((header.msg_flags & MSG_CTRUNC) != 0) {
        return 0;
    }
    for (auto* control = CMSG_FIRSTHDR(&header); control != nullptr; control = CMSG_NXTHDR(&header, control)) {
        if (control->cmsg_level == SOL_SOCKET && control->cmsg_type == SCM_TIMESTAMPNS
            && control->cmsg_len >= CMSG_LEN(sizeof(timespec))) {
            timespec timestamp{};
            std::memcpy(&timestamp, CMSG_DATA(control), sizeof(timestamp));
            return ToNanoseconds(timestamp);
        }
    }
    return 0;
}

int64_t ReadSocketDrops(int socket) {
    sockaddr_in local{};
    socklen_t localSize = sizeof(local);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &localSize) == -1
        || localSize != sizeof(local) || local.sin_family != AF_INET) {
        return -1;
    }
    std::ifstream table("/proc/net/udp");
    std::string line;
    while (std::getline(table, line)) {
        std::istringstream row(line);
        std::string slot;
        std::string address;
        if (!(row >> slot >> address)) {
            continue;
        }
        const auto colon = address.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        unsigned port = 0;
        const auto portResult = std::from_chars(address.data() + colon + 1,
            address.data() + address.size(), port, 16);
        if (portResult.ec != std::errc{} || portResult.ptr != address.data() + address.size()
            || port != static_cast<unsigned>(ntohs(local.sin_port))) {
            continue;
        }
        std::string column;
        std::string lastColumn;
        size_t columns = 2;
        while (row >> column) {
            lastColumn = column;
            ++columns;
        }
        // 列が欠けた行を drops と誤認せず、読み取り不能として扱う。
        if (table.bad() || columns < 13) {
            return -1;
        }
        int64_t drops = -1;
        const auto result = std::from_chars(lastColumn.data(), lastColumn.data() + lastColumn.size(), drops);
        return result.ec == std::errc{} && result.ptr == lastColumn.data() + lastColumn.size()
            && drops >= 0 ? drops : -1;
    }
    return -1;
}

uint64_t TimestampUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* ResultName(protocol::HelloResult result) {
    switch (result) {
    case protocol::HelloResult::Accepted: return "Accepted";
    case protocol::HelloResult::Busy: return "Busy";
    case protocol::HelloResult::PendingApproval: return "PendingApproval";
    case protocol::HelloResult::Starting: return "Starting";
    case protocol::HelloResult::Stopped: return "Stopped";
    case protocol::HelloResult::VersionMismatch: return "VersionMismatch";
    case protocol::HelloResult::Rejected: return "Rejected";
    }
    return "Unknown";
}

}

NetworkClient::~NetworkClient() {
    Close();
}

bool NetworkClient::Open(const std::string& host, uint16_t port) {
    Close();
    m_destination = {};
    m_destination.sin_family = AF_INET;
    m_destination.sin_port = htons(port);
    if (port == 0 || host.find('\0') != std::string::npos
        || inet_pton(AF_INET, host.c_str(), &m_destination.sin_addr) != 1) {
        WNVR_LOGW("Invalid UDP destination: %s:%u", host.c_str(), static_cast<unsigned>(port));
        return false;
    }
    if (!OpenSocket(false)) {
        return false;
    }
    m_nextHello = Clock::now();
    m_hostAddress = host + ":" + std::to_string(port);
    WNVR_LOGI("UDP destination: %s:%u", host.c_str(), static_cast<unsigned>(port));
    return StartReceiving();
}

bool NetworkClient::OpenDiscovery() {
    Close();
    if (!OpenSocket(true)) {
        return false;
    }
    m_discovery = true;
    m_lastDiscoveryLog.reset();
    BeginDiscovery();
    return StartReceiving();
}

bool NetworkClient::OpenSocket(bool broadcast) {
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == -1) {
        LogIoError("socket", errno);
        return false;
    }
    const int flags = fcntl(m_socket, F_GETFL, 0);
    if (flags == -1 || fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        LogIoError("fcntl", errno);
        Close();
        return false;
    }
    if (setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &kSocketReceiveBufferBytes,
            sizeof(kSocketReceiveBufferBytes)) == -1) {
        LogIoError("SO_RCVBUF", errno);
    }
    // 最初の Hello より前から poll できるよう、受信ポートを確保する。
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == -1) {
        LogIoError("bind", errno);
        Close();
        return false;
    }
    const int enabled = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_TIMESTAMPNS, &enabled, sizeof(enabled)) == -1) {
        const int error = errno;
        WNVR_LOGW("UDP SO_TIMESTAMPNS failed: %s (%d)", std::strerror(error), error);
    }
    if (broadcast && setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == -1) {
        LogIoError("SO_BROADCAST", errno);
        Close();
        return false;
    }
    return true;
}

void NetworkClient::BeginDiscovery() {
    m_lastAck.reset();
    m_hostName.clear();
    m_hostAddress.clear();
    m_searching = true;
    m_helloAttempts = 0;
    m_destination = {};
    m_destination.sin_family = AF_INET;
    m_destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    m_destination.sin_port = htons(protocol::kDiscoveryPort);
    m_nextDiscover = Clock::now();
}

bool NetworkClient::StartReceiving() {
    m_stopping.store(false);
    try {
        m_receiveThread = std::thread([this] { Receive(); });
    } catch (const std::exception& error) {
        WNVR_LOGW("UDP receive thread failed: %s", error.what());
        Close();
        return false;
    }
    return true;
}

bool NetworkClient::IsConnected() const {
    std::lock_guard lock(m_mutex);
    return m_connected;
}

NetworkClient::Status NetworkClient::GetStatus() const {
    std::lock_guard lock(m_mutex);
    Status status;
    status.hostName = m_hostName;
    status.hostAddress = m_hostAddress;
    if (m_connected) {
        status.state = ConnectionState::Connected;
    } else if (m_lastAck && Clock::now() - m_lastAckTime <= kAckDisplayDuration) {
        switch (m_lastAck->result) {
        case protocol::HelloResult::PendingApproval:
            status.state = ConnectionState::PendingApproval;
            break;
        case protocol::HelloResult::Starting:
            status.state = ConnectionState::Starting;
            break;
        case protocol::HelloResult::Stopped:
            status.state = ConnectionState::Stopped;
            break;
        case protocol::HelloResult::Rejected:
            status.state = ConnectionState::Rejected;
            break;
        case protocol::HelloResult::Busy:
            status.state = ConnectionState::Busy;
            break;
        case protocol::HelloResult::VersionMismatch:
            status.state = ConnectionState::VersionMismatch;
            status.hostProtocolVersion = m_lastAck->hostProtocolVersion;
            break;
        case protocol::HelloResult::Accepted: break;
        }
    } else if (!m_searching && !m_hostAddress.empty()) {
        status.state = ConnectionState::Connecting;
    }
    return status;
}

uint64_t NetworkClient::ConnectionGeneration() const {
    std::lock_guard lock(m_mutex);
    return m_connectionGeneration;
}

std::optional<EyeSize> NetworkClient::RequestedEyeSize() const {
    std::lock_guard lock(m_mutex);
    return m_requestedEyeSize;
}

protocol::Foveation NetworkClient::GetFoveation() const {
    std::lock_guard lock(m_mutex);
    return m_foveation;
}

std::optional<NetworkClient::RenderDisplay> NetworkClient::GetRenderDisplay() const {
    std::lock_guard lock(m_mutex);
    return m_renderDisplay;
}

std::optional<AssembledFrame> NetworkClient::PopFrame() {
    // 世代とフレームを同じロックで取得し、再接続をまたいだ誤認を防ぐ。
    std::lock_guard lock(m_mutex);
    auto frame = m_assembler.PopFrame();
    if (frame) {
        frame->connectionGeneration = m_connectionGeneration;
    }
    return frame;
}

void NetworkClient::Close() {
    m_stopping.store(true);
    if (m_receiveThread.joinable()) {
        m_receiveThread.join();
    }
    std::lock_guard lock(m_mutex);
    m_audioPlayer.Stop();
    m_assembler.Reset();
    if (m_socket != -1) {
        if (m_connected) {
            Send(protocol::ByeMessage{});
        }
        close(m_socket);
        m_socket = -1;
    }
    m_connected = false;
    m_micRequested = false;
    m_foveationPending = false;
    m_renderDisplay.reset();
    m_foveation = {};
    m_requestedEyeSize.reset();
    m_keyFrameRequests.fill(0);
    m_lastAck.reset();
    m_hostName.clear();
    m_hostAddress.clear();
    m_discovery = false;
    m_searching = false;
    m_helloAttempts = 0;
    m_roundTripMs.reset();
}

void NetworkClient::LogIoError(const char* operation, int error) {
    const auto now = Clock::now();
    if (!m_lastIoLog || now - *m_lastIoLog >= kLogInterval) {
        WNVR_LOGW("UDP %s failed: %s (%d)", operation, std::strerror(error), error);
        m_lastIoLog = now;
    }
}

bool NetworkClient::Send(const protocol::Message& message) {
    std::array<std::byte, protocol::kMaxPacketSize> buffer{};
    protocol::PacketHeader header{protocol::TypeOf(message)};
    header.sequence = m_sequence++;
    header.timestampUs = TimestampUs();
    const auto size = protocol::Serialize(header, message, buffer);
    if (size == 0) {
        LogIoError("Serialize", EINVAL);
        return false;
    }
    const auto sent = sendto(m_socket, buffer.data(), size, 0,
        reinterpret_cast<const sockaddr*>(&m_destination), sizeof(m_destination));
    if (sent != static_cast<ssize_t>(size)) {
        const int error = sent < 0 ? errno : EIO;
        if (error != EAGAIN && error != EWOULDBLOCK) {
            LogIoError("sendto", error);
        }
        return false;
    }
    return true;
}

void NetworkClient::Disconnect(const char* reason, bool rediscover) {
    m_audioPlayer.Stop();
    if (m_connected) {
        WNVR_LOGW("UDP disconnected: %s", reason);
        m_nextHello = Clock::now();
    }
    m_connected = false;
    m_micRequested = false;
    m_foveationPending = false;
    m_renderDisplay.reset();
    m_foveation = {};
    m_requestedEyeSize.reset();
    m_roundTripMs.reset();
    m_assembler.Reset();
    if (m_discovery && rediscover) {
        BeginDiscovery();
    }
}

// PC の発見の返事。待ち受けているポートが分かったので、次の Update から Hello を送る。
void NetworkClient::HandleDiscoverReply(const sockaddr_in& sender, const protocol::DiscoverReplyMessage& reply,
    Clock::time_point now) {
    if (reply.port == 0) {
        return;
    }
    m_destination = sender;
    m_destination.sin_port = htons(reply.port);
    m_searching = false;
    m_helloAttempts = 0;
    m_nextHello = now;
    char address[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address));
    m_hostName = reply.hostName;
    m_hostAddress = std::string(address) + ":" + std::to_string(reply.port);
    WNVR_LOGI("PC found: %s (%s:%u)", reply.hostName.c_str(), address, static_cast<unsigned>(reply.port));
}

void NetworkClient::HandleHelloAck(const protocol::HelloAckMessage& ack, Clock::time_point now) {
    m_helloAttempts = 0;
    if (ack.result != protocol::HelloResult::Accepted) {
        if (m_connected) {
            // 応答した PC は生きているので、名前と接続先を保って理由を表示する。
            Disconnect("HelloAck refused", false);
        }
        // 状態の表示（承認待ち、SteamVR を起動中 など）に使う。
        m_lastAck = ack;
        m_lastAckTime = now;
        auto& lastLog = m_lastAckLog[static_cast<size_t>(ack.result)];
        if (!lastLog || now - *lastLog >= kLogInterval) {
            WNVR_LOGW("HelloAck %s (host version=%u)", ResultName(ack.result),
                static_cast<unsigned>(ack.hostProtocolVersion));
            lastLog = now;
        }
        return;
    }
    m_lastAck.reset();
    // PC が描画に使っている視野角と IPD。Quest はこれに合わせて映像を表示する。
    m_renderDisplay.reset();
    if (protocol::IsValidFov(ack.renderLeftFov) && protocol::IsValidFov(ack.renderRightFov)
        && protocol::IsValidIpd(ack.renderIpdMeters)) {
        m_renderDisplay = RenderDisplay{ack.renderLeftFov, ack.renderRightFov, ack.renderIpdMeters};
    } else {
        WNVR_LOGW("HelloAck render display invalid; using located views");
    }
    m_foveation = ack.foveation;
    SetRequestedEyeSize(ack.questEyeWidth, ack.questEyeHeight);
    if (m_connected) {
        return;
    }
    const auto& f = m_foveation;
    WNVR_LOGI("Foveation: enabled=%u L=[%.4f %.4f %.4f %.4f] R=[%.4f %.4f %.4f %.4f] ratio=%.2f,%.2f",
        static_cast<unsigned>(f.enabled),
        f.left.centerX0, f.left.centerX1, f.left.centerY0, f.left.centerY1,
        f.right.centerX0, f.right.centerX1, f.right.centerY0, f.right.centerY1,
        f.edgeRatioX, f.edgeRatioY);
    m_connected = true;
    ++m_connectionGeneration;
    m_poseCount = 0;
    m_inputCount = 0;
    m_lastInput = {};
    m_roundTripMs.reset();
    m_nextPing = now;
    m_nextReport = now + kLogInterval;
    WNVR_LOGI("UDP connected: HelloAck Accepted");
}

// 受け取った 1 個のパケットを処理する。m_mutex を取ってから呼ぶ。
void NetworkClient::HandleDatagram(const sockaddr_in& sender, socklen_t senderSize, bool truncated,
    std::span<const std::byte> data, int64_t kernelTimestampNs) {
    // つないでいる PC 以外から届いたものは捨てる（探しているあいだは、どの PC の返事も受け付ける）。
    if (senderSize != sizeof(sender) || sender.sin_family != AF_INET
        || (!m_searching && (sender.sin_addr.s_addr != m_destination.sin_addr.s_addr
            || sender.sin_port != m_destination.sin_port))) {
        return;
    }
    const auto now = Clock::now();
    const auto logIgnored = [&](const char* reason) {
        if (!m_lastParseLog || now - *m_lastParseLog >= kLogInterval) {
            WNVR_LOGW("UDP packet ignored: %s", reason);
            m_lastParseLog = now;
        }
    };
    if (truncated) {
        logIgnored("truncated");
        return;
    }
    const auto parsed = protocol::Parse(data);
    if (m_searching) {
        if (parsed.packet) {
            if (const auto* reply = std::get_if<protocol::DiscoverReplyMessage>(&parsed.packet->message)) {
                HandleDiscoverReply(sender, *reply, now);
            }
        }
        return;
    }
    if (parsed.error != protocol::ParseError::None) {
        logIgnored(protocol::ToString(parsed.error));
        return;
    }
    m_lastReceived = now;
    const auto& message = parsed.packet->message;
    if (const auto* ack = std::get_if<protocol::HelloAckMessage>(&message)) {
        HandleHelloAck(*ack, now);
    } else if (const auto* pong = std::get_if<protocol::PongMessage>(&message)) {
        if (m_connected) {
            m_micRequested = pong->micRequested != 0;
            m_foveationPending = pong->foveationPending != 0;
            SetRequestedEyeSize(pong->questEyeWidth, pong->questEyeHeight);
        }
        const auto receivedUs = TimestampUs();
        if (m_connected && pong->pingId > 0 && pong->pingId <= m_pingId
            && pong->pingTimestampUs <= receivedUs) {
            m_roundTripMs = static_cast<double>(receivedUs - pong->pingTimestampUs) / 1000.0;
        }
    } else if (const auto* audio = std::get_if<protocol::AudioMessage>(&message)) {
        if (m_connected) {
            m_audioPlayer.Push(*audio);
        }
    } else if (const auto* fragment = std::get_if<protocol::VideoFragmentMessage>(&message)) {
        if (m_connected) {
            m_assembler.AddFragment(*fragment, kernelTimestampNs);
        }
    } else if (std::holds_alternative<protocol::ByeMessage>(message)) {
        Disconnect("Bye received");
    }
}

void NetworkClient::Receive() {
    // 映像の断片は 1 秒に 1 万個を超える。描画のスレッドに押しのけられて受信が遅れないよう、優先度を上げる。
    if (setpriority(PRIO_PROCESS, gettid(), kReceiveThreadNice) == -1) {
        const int error = errno;
        WNVR_LOGW("UDP receive priority failed: %s (%d)", std::strerror(error), error);
    } else {
        WNVR_LOGI("UDP receive priority: nice=%d", kReceiveThreadNice);
    }
    static_assert(protocol::kMaxPacketSize <= kReceiveBufferSize);
    // recvmmsg で、1 回のシステムコールで最大 kReceiveBatch 個をまとめて読む。
    struct ControlBuffer {
        alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(timespec))> data;
    };
    std::array<std::array<std::byte, kReceiveBufferSize>, kReceiveBatch> buffers;
    std::array<ControlBuffer, kReceiveBatch> controls;
    std::array<sockaddr_in, kReceiveBatch> senders{};
    std::array<iovec, kReceiveBatch> vectors{};
    std::array<mmsghdr, kReceiveBatch> messages{};
    for (size_t index = 0; index < kReceiveBatch; ++index) {
        vectors[index].iov_base = buffers[index].data();
        vectors[index].iov_len = buffers[index].size();
        auto& header = messages[index].msg_hdr;
        header.msg_name = &senders[index];
        header.msg_iov = &vectors[index];
        header.msg_iovlen = 1;
        header.msg_control = controls[index].data.data();
    }
    // 5 秒ごとのログ用の集計（このスレッドだけが使う）。
    uint64_t receiveCalls = 0;
    uint64_t packets = 0;
    uint64_t delaySamples = 0;
    uint64_t delayTotalNs = 0;
    uint64_t delayMaxNs = 0;
    auto previousDrops = ReadSocketDrops(m_socket);
    auto nextReport = Clock::now() + kLogInterval;
    const auto reportStats = [&] {
        const auto now = Clock::now();
        if (now < nextReport) {
            return;
        }
        const auto drops = ReadSocketDrops(m_socket);
        const int64_t dropped = drops >= 0 && previousDrops >= 0 && drops >= previousDrops
            ? drops - previousDrops : -1;
        previousDrops = drops;
        const double averageBatch = receiveCalls == 0 ? 0.0
            : static_cast<double>(packets) / static_cast<double>(receiveCalls);
        const double averageDelayMs = delaySamples == 0 ? 0.0
            : static_cast<double>(delayTotalNs) / static_cast<double>(delaySamples) / 1000000.0;
        const double kernelAssembleMs = m_assembler.TakeKernelAssembleAverageMs();
        WNVR_LOGI("受信: recvmmsg=%" PRIu64 " 回, パケット=%" PRIu64
            " (平均 %.1f 個/回), カーネル→アプリ: 平均=%.2f / 最大=%.2f ms, "
            "カーネルでの組み立て=%.1f ms, ソケットで落ちた数=%" PRId64,
            receiveCalls, packets, averageBatch, averageDelayMs,
            static_cast<double>(delayMaxNs) / 1000000.0, kernelAssembleMs, dropped);
        receiveCalls = 0;
        packets = 0;
        delaySamples = 0;
        delayTotalNs = 0;
        delayMaxNs = 0;
        nextReport = now + kLogInterval;
    };
    while (!m_stopping.load()) {
        reportStats();
        pollfd descriptor{m_socket, POLLIN, 0};
        const int ready = poll(&descriptor, 1, kPollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error = errno;
            std::lock_guard lock(m_mutex);
            LogIoError("poll", error);
            Disconnect("poll failed");
            return;
        }
        if (ready == 0) {
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::lock_guard lock(m_mutex);
            LogIoError("poll events", EIO);
            Disconnect("poll events failed");
            return;
        }
        // 読めるものが無くなるまで、まとめて読み続ける。
        while (!m_stopping.load()) {
            for (auto& message : messages) {
                message.msg_hdr.msg_namelen = sizeof(sockaddr_in);
                message.msg_hdr.msg_controllen = CMSG_SPACE(sizeof(timespec));
                message.msg_hdr.msg_flags = 0;
                message.msg_len = 0;
            }
            const int count = recvmmsg(m_socket, messages.data(), kReceiveBatch, MSG_DONTWAIT, nullptr);
            const int error = count < 0 ? errno : 0;
            // バッチを読んだ直後に採時し、ロック待ちや Parse の時間を混ぜない。
            timespec readTime{};
            const bool hasReadTime = clock_gettime(CLOCK_REALTIME, &readTime) == 0;
            const int64_t readTimestampNs = hasReadTime ? ToNanoseconds(readTime) : 0;
            // 空読みも含め、実際のシステムコール数に対する平均を出す。
            ++receiveCalls;
            if (count > 0) {
                packets += static_cast<uint64_t>(count);
                // ロックは 1 個ごとではなく、まとめて読んだ 1 回につき 1 回だけ取る。
                std::lock_guard lock(m_mutex);
                for (int index = 0; index < count; ++index) {
                    auto& header = messages[index].msg_hdr;
                    const int64_t kernelTimestampNs = KernelTimestampNs(header);
                    if (kernelTimestampNs > 0 && hasReadTime && readTimestampNs >= kernelTimestampNs) {
                        const auto delayNs = static_cast<uint64_t>(readTimestampNs - kernelTimestampNs);
                        delayTotalNs += delayNs;
                        delayMaxNs = std::max(delayMaxNs, delayNs);
                        ++delaySamples;
                    }
                    HandleDatagram(senders[index], header.msg_namelen, (header.msg_flags & MSG_TRUNC) != 0,
                        std::span<const std::byte>(buffers[index].data(), messages[index].msg_len),
                        kernelTimestampNs);
                }
            } else if (count < 0 && error != EAGAIN && error != EWOULDBLOCK && error != EINTR) {
                std::lock_guard lock(m_mutex);
                LogIoError("recvmmsg", error);
            }
            // 入力が途切れない場合も、5 秒ごとの集計を止めない。
            reportStats();
            if (count == 0 || (count < 0 && error != EINTR)) {
                break;
            }
        }
    }
}

void NetworkClient::Update(const protocol::HelloMessage& hello) {
    std::lock_guard lock(m_mutex);
    m_audioPlayer.ReportStats();
    if (m_socket == -1) {
        return;
    }
    const auto now = Clock::now();
    if (m_connected && now - m_lastReceived >= kReceiveTimeout) {
        Disconnect("receive timeout (3 seconds)");
    }
    if (!m_connected) {
        if (m_discovery && !m_searching && m_helloAttempts >= kMaxHelloAttempts && now >= m_nextHello) {
            BeginDiscovery();
        }
        if (m_searching) {
            if (now >= m_nextDiscover) {
                Send(protocol::DiscoverMessage{hello.deviceName});
                m_nextDiscover = now + kRetryInterval;
                if (!m_lastDiscoveryLog || now - *m_lastDiscoveryLog >= kDiscoveryLogInterval) {
                    WNVR_LOGI("PC を探しています");
                    m_lastDiscoveryLog = now;
                }
            }
            return;
        }
        if (now >= m_nextHello) {
            Send(hello);
            if (m_discovery) {
                ++m_helloAttempts;
            }
            m_nextHello = now + kRetryInterval;
        }
        return;
    }
    if (now >= m_nextPing) {
        Send(protocol::PingMessage{++m_pingId});
        m_nextPing = now + kRetryInterval;
    }
    if (now >= m_nextReport) {
        if (m_roundTripMs) {
            WNVR_LOGI("UDP: RTT=%.2f ms, poses=%" PRIu64, *m_roundTripMs, m_poseCount);
        } else {
            WNVR_LOGI("UDP: RTT=pending, poses=%" PRIu64, m_poseCount);
        }
        WNVR_LOGI("入力送信5s=%" PRIu64
            " L: buttons=0x%08" PRIx32 " trigger=%.3f grip=%.3f stick=(%.3f, %.3f)"
            " R: buttons=0x%08" PRIx32 " trigger=%.3f grip=%.3f stick=(%.3f, %.3f)",
            m_inputCount, m_lastInput.left.buttons, m_lastInput.left.trigger, m_lastInput.left.grip,
            m_lastInput.left.thumbstickX, m_lastInput.left.thumbstickY,
            m_lastInput.right.buttons, m_lastInput.right.trigger, m_lastInput.right.grip,
            m_lastInput.right.thumbstickX, m_lastInput.right.thumbstickY);
        m_inputCount = 0;
        WNVR_LOGI("キーフレーム要求送信: DecoderStart=%" PRIu64 ", FrameGap=%" PRIu64
            ", DecoderDrop=%" PRIu64 ", DecoderError=%" PRIu64,
            m_keyFrameRequests[0], m_keyFrameRequests[1], m_keyFrameRequests[2], m_keyFrameRequests[3]);
        m_keyFrameRequests.fill(0);
        m_nextReport = now + kLogInterval;
    }
}

void NetworkClient::SendKeyFrameRequest(uint32_t lastGoodFrameIndex, protocol::KeyFrameReason reason) {
    std::lock_guard lock(m_mutex);
    const auto reasonIndex = static_cast<size_t>(reason);
    if (m_connected && reasonIndex < m_keyFrameRequests.size()
        && Send(protocol::KeyFrameRequestMessage{lastGoodFrameIndex, reason})) {
        ++m_keyFrameRequests[reasonIndex];
    }
}

bool NetworkClient::SendPose(const protocol::PoseMessage& pose) {
    std::lock_guard lock(m_mutex);
    if (m_connected && Send(pose)) {
        ++m_poseCount;
        return true;
    }
    return false;
}

void NetworkClient::SetRequestedEyeSize(uint16_t width, uint16_t height) {
    m_requestedEyeSize = width == 0 && height == 0
        ? std::nullopt : std::optional<EyeSize>{{width, height}};
}

bool NetworkClient::FoveationPending() const {
    std::lock_guard lock(m_mutex);
    return m_connected && m_foveationPending;
}

bool NetworkClient::MicRequested() const {
    std::lock_guard lock(m_mutex);
    return m_connected && m_micRequested;
}

void NetworkClient::SendMicAudio(const protocol::MicAudioMessage& mic) {
    std::lock_guard lock(m_mutex);
    if (m_connected && m_micRequested) {
        Send(mic);
    }
}

void NetworkClient::SendInput(const protocol::InputMessage& input) {
    std::lock_guard lock(m_mutex);
    if (m_connected && Send(input)) {
        ++m_inputCount;
        m_lastInput = input;
    }
}

NetworkClient::ClientStats NetworkClient::TakeClientStats() {
    std::lock_guard lock(m_mutex);
    return {m_assembler.TakeClientStats(), m_audioPlayer.TakeClientStats(),
        m_roundTripMs.value_or(0.0), m_connectionGeneration};
}

bool NetworkClient::SendClientStats(const protocol::ClientStatsMessage& stats, uint64_t generation) {
    std::lock_guard lock(m_mutex);
    // 集計と送信の間の再接続を検出し、別の接続の値を送らない。
    return m_connected && generation == m_connectionGeneration && Send(stats);
}

void NetworkClient::SendDisplayFeedback(const protocol::DisplayFeedbackMessage& message) {
    std::lock_guard lock(m_mutex);
    if (m_connected) {
        Send(message);
    }
}

}
