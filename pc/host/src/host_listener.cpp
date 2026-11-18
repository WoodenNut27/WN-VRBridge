#include <winsock2.h>
#include <ws2tcpip.h>

#include "host_listener.h"
#include "host_log.h"

#include <wnvr/protocol/messages.h>
#include <wnvr/win32/platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace wnvr::host {
namespace {

using Clock = std::chrono::steady_clock;

// 同じ相手のログや通知を、この間隔より短くは繰り返さない。
constexpr auto kDiscoveryLogInterval = std::chrono::seconds(10);
constexpr auto kHelloNotifyInterval = std::chrono::seconds(2);
constexpr auto kParseLogInterval = std::chrono::seconds(5);

std::string AddressText(const sockaddr_in& sender) {
    char address[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address));
    return address;
}

}

struct HostListener::Impl {
    explicit Impl(Callbacks callbacks) : m_callbacks(std::move(callbacks)) {}

    Callbacks m_callbacks;
    SOCKET m_discovery = INVALID_SOCKET;
    SOCKET m_hello = INVALID_SOCKET;
    bool m_winsockStarted = false;
    std::atomic<bool> m_running{false};
    std::jthread m_thread;
    mutable std::mutex m_mutex;
    std::vector<std::string> m_approved;
    std::vector<std::string> m_rejected;
    std::string m_error;
    std::string m_hostName;
    uint32_t m_suppressedSessionId = 0;
    bool m_autoStart = true;
    uint32_t m_sequence = 0;

    // 受信スレッドだけが使う、ログと通知の間引きの記録。
    std::map<uint32_t, Clock::time_point> m_discoveryLogs;
    // 承認前の通知が、承認後の起動通知を遅らせないよう別々に抑制する。
    std::map<std::string, Clock::time_point> m_pairingTimes;
    std::map<std::string, Clock::time_point> m_approvedTimes;
    std::optional<Clock::time_point> m_lastParseLog;

    void Error(std::string reason) {
        std::lock_guard lock(m_mutex);
        m_error = std::move(reason);
    }

    bool Open(SOCKET& target, const std::string& bindAddress, uint16_t port) {
        target = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const BOOL exclusive = TRUE;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
            Error("IPv4 アドレスが不正です: " + bindAddress);
            return false;
        }
        u_long nonblocking = 1;
        // ドライバと同じポートを使うので、二重に開いて片方にしか届かない状態を作らない。
        if (target == INVALID_SOCKET
            || setsockopt(target, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR
            || bind(target, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
            || ioctlsocket(target, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            Error(bindAddress + ":" + std::to_string(port) + " を開けません（Winsock "
                + std::to_string(WSAGetLastError()) + "）");
            return false;
        }
        return true;
    }

    void Send(SOCKET socket, const sockaddr_in& sender, const protocol::Message& message) {
        std::array<std::byte, protocol::kMaxPacketSize> buffer{};
        protocol::PacketHeader header{protocol::TypeOf(message)};
        header.sequence = m_sequence++;
        header.timestampUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
        const auto size = protocol::Serialize(header, message, buffer);
        if (size > 0) {
            sendto(socket, reinterpret_cast<const char*>(buffer.data()), static_cast<int>(size), 0,
                reinterpret_cast<const sockaddr*>(&sender), sizeof(sender));
        }
    }

    // その相手について、間隔を空けて最初の 1 回だけ true を返す。
    template<typename Key>
    static bool ShouldNotify(std::map<Key, Clock::time_point>& times, const Key& key, Clock::time_point now,
        Clock::duration interval) {
        std::erase_if(times, [now, interval](const auto& entry) { return now - entry.second >= interval; });
        return times.emplace(key, now).second;
    }

    void HandleDiscover(const sockaddr_in& sender, Clock::time_point now) {
        Send(m_discovery, sender, protocol::DiscoverReplyMessage{m_hostName, protocol::kDefaultPort});
        if (ShouldNotify(m_discoveryLogs, static_cast<uint32_t>(sender.sin_addr.s_addr), now, kDiscoveryLogInterval)) {
            LogInfo("ホストが PC の発見要求に応答しました: %s", AddressText(sender).c_str());
        }
    }

    // SteamVR が動いていないあいだ、ドライバの代わりに Hello に答える。
    void HandleHello(const sockaddr_in& sender, const protocol::HelloMessage& hello, Clock::time_point now) {
        bool approved;
        bool rejected;
        bool suppressed;
        bool autoStart;
        {
            std::lock_guard lock(m_mutex);
            approved = std::find(m_approved.begin(), m_approved.end(), hello.deviceId) != m_approved.end();
            rejected = std::find(m_rejected.begin(), m_rejected.end(), hello.deviceId) != m_rejected.end();
            suppressed = m_suppressedSessionId != 0 && hello.sessionId == m_suppressedSessionId;
            autoStart = m_autoStart;
        }
        const auto result = DecideHelloResult(approved, rejected, suppressed, autoStart, hello.startRequested != 0);
        Send(m_hello, sender, protocol::HelloAckMessage{result});
        if (result == protocol::HelloResult::Stopped || result == protocol::HelloResult::Rejected) {
            return;
        }
        if (!ShouldNotify(approved ? m_approvedTimes : m_pairingTimes, hello.deviceId, now, kHelloNotifyInterval)) {
            return;
        }
        if (approved && m_callbacks.onApprovedHello) {
            m_callbacks.onApprovedHello(hello.deviceId);
        } else if (!approved && m_callbacks.onPairingRequest) {
            m_callbacks.onPairingRequest({hello.deviceId, hello.deviceName, AddressText(sender)});
        }
    }

    void HandlePacket(SOCKET socket, const sockaddr_in& sender, std::span<const std::byte> data) {
        const auto now = Clock::now();
        const auto parsed = protocol::Parse(data);
        if (parsed.error != protocol::ParseError::None) {
            // 古いプロトコルは本文を読めないため、ヘッダの Hello 型だけを確認する。
            if (socket == m_hello && parsed.error == protocol::ParseError::VersionMismatch
                && data.size() >= protocol::kHeaderSize
                && data[6] == static_cast<std::byte>(protocol::MessageType::Hello)) {
                Send(socket, sender, protocol::HelloAckMessage{protocol::HelloResult::VersionMismatch});
            }
            if (!m_lastParseLog || now - *m_lastParseLog >= kParseLogInterval) {
                LogWarn("待ち受け UDP パケットを破棄しました: %s", protocol::ToString(parsed.error));
                m_lastParseLog = now;
            }
            return;
        }
        const auto& message = parsed.packet->message;
        if (socket == m_discovery) {
            if (std::holds_alternative<protocol::DiscoverMessage>(message)) {
                HandleDiscover(sender, now);
            }
        } else if (const auto* hello = std::get_if<protocol::HelloMessage>(&message)) {
            HandleHello(sender, *hello, now);
        }
    }

    void Receive(std::stop_token token) {
        std::array<std::byte, 65536> buffer{};
        while (!token.stop_requested()) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(m_discovery, &readable);
            FD_SET(m_hello, &readable);
            timeval timeout{0, 100000};
            if (select(0, &readable, nullptr, nullptr, &timeout) == SOCKET_ERROR) {
                Error("UDP select に失敗しました: " + std::to_string(WSAGetLastError()));
                break;
            }
            for (const SOCKET socket : {m_discovery, m_hello}) {
                if (token.stop_requested()) {
                    break;
                }
                if (!FD_ISSET(socket, &readable)) {
                    continue;
                }
                sockaddr_in sender{};
                int senderSize = sizeof(sender);
                const int size = recvfrom(socket, reinterpret_cast<char*>(buffer.data()),
                    static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&sender), &senderSize);
                if (size == SOCKET_ERROR || senderSize != sizeof(sender) || sender.sin_family != AF_INET) {
                    continue;
                }
                HandlePacket(socket, sender, std::span<const std::byte>(buffer.data(), static_cast<size_t>(size)));
            }
        }
    }
};

HostListener::HostListener(Callbacks callbacks) : m_impl(std::make_unique<Impl>(std::move(callbacks))) {}

HostListener::~HostListener() {
    Stop();
}

bool HostListener::Start(const std::string& bindAddress) {
    if (IsRunning()) {
        return true;
    }
    Stop();
    m_impl->Error({});
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error != 0) {
        m_impl->Error("WSAStartup に失敗しました: " + std::to_string(error));
        return false;
    }
    m_impl->m_winsockStarted = true;
    if (!m_impl->Open(m_impl->m_discovery, bindAddress, protocol::kDiscoveryPort)
        || !m_impl->Open(m_impl->m_hello, bindAddress, protocol::kDefaultPort)) {
        Stop();
        return false;
    }
    try {
        m_impl->m_hostName = win32::ComputerName(protocol::kMaxHostNameLength, "WN-VRBridge PC");
        m_impl->m_running = true;
        m_impl->m_thread = std::jthread([impl = m_impl.get()](std::stop_token token) {
            try {
                impl->Receive(token);
            } catch (const std::exception& exception) {
                impl->Error(std::string("待ち受けスレッドで例外: ") + exception.what());
            }
            impl->m_running = false;
        });
    } catch (const std::exception& exception) {
        m_impl->Error(std::string("待ち受けの開始に失敗しました: ") + exception.what());
        Stop();
        return false;
    }
    return true;
}

void HostListener::Stop() {
    // select の上限 100ms だけ待ち、受信中のソケットを別スレッドから閉じない。
    if (m_impl->m_thread.joinable()) {
        m_impl->m_thread.request_stop();
        m_impl->m_thread.join();
    }
    for (auto* socket : {&m_impl->m_discovery, &m_impl->m_hello}) {
        if (*socket != INVALID_SOCKET) {
            closesocket(*socket);
            *socket = INVALID_SOCKET;
        }
    }
    if (m_impl->m_winsockStarted) {
        WSACleanup();
        m_impl->m_winsockStarted = false;
    }
    m_impl->m_running = false;
    // 次の待ち受けに、前回の間引きの記録を持ち越さない。
    m_impl->m_discoveryLogs.clear();
    m_impl->m_pairingTimes.clear();
    m_impl->m_approvedTimes.clear();
    m_impl->m_lastParseLog.reset();
}

bool HostListener::IsRunning() const {
    return m_impl->m_running.load();
}

std::string HostListener::LastError() const {
    std::lock_guard lock(m_impl->m_mutex);
    return m_impl->m_error;
}

void HostListener::SetDeviceLists(std::vector<std::string> approved, std::vector<std::string> rejected) {
    std::lock_guard lock(m_impl->m_mutex);
    m_impl->m_approved = std::move(approved);
    m_impl->m_rejected = std::move(rejected);
}

void HostListener::SetAutoStart(bool enabled) {
    std::lock_guard lock(m_impl->m_mutex);
    m_impl->m_autoStart = enabled;
}

void HostListener::SetSuppressedSession(uint32_t sessionId) {
    std::lock_guard lock(m_impl->m_mutex);
    m_impl->m_suppressedSessionId = sessionId;
}

}
