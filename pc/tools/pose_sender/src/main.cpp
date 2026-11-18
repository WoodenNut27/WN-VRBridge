#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include "wnvr/protocol/messages.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace wnvr::tools {
namespace {

using Clock = std::chrono::steady_clock;
std::atomic<bool> stopRequested{false};

BOOL WINAPI HandleConsoleControl(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        stopRequested.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

struct Options {
    std::string host = "127.0.0.1";
    std::string deviceId = "pose-sender";
    uint16_t port = protocol::kDefaultPort;
    unsigned rate = 72;
    double duration = 0;
    bool help = false;
};

void PrintUsage() {
    std::cout << "使い方: pose_sender.exe [--host <IPv4>] [--port <番号>]"
                 " [--rate <Hz>] [--duration <秒>] [--device-id <id>] [--help]\n"
              << "  --host      送信先 IPv4 アドレス（既定: 127.0.0.1）\n"
              << "  --port      送信先ポート（1〜65535、既定: " << protocol::kDefaultPort << "）\n"
              << "  --rate      送信頻度（1〜1000 Hz、既定: 72）\n"
              << "  --duration  送信時間（0 以上の秒数、既定: 0 = Ctrl+C まで）\n"
              << "  --device-id 端末 ID（英数字と - _、1〜64 バイト。既定: pose-sender）\n"
              << "  --help      この使い方を表示\n";
}

template <typename T>
bool ParseNumber(std::string_view text, T& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool ParseOptions(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument != "--host" && argument != "--port"
            && argument != "--rate" && argument != "--duration" && argument != "--device-id") {
            return false;
        }
        if (++index == argc) {
            return false;
        }
        const std::string_view value = argv[index];
        if (argument == "--host") {
            options.host = value;
        } else if (argument == "--device-id") {
            options.deviceId = value;
        } else if (argument == "--port") {
            unsigned port = 0;
            if (!ParseNumber(value, port) || port == 0 || port > 65535) {
                return false;
            }
            options.port = static_cast<uint16_t>(port);
        } else if (argument == "--rate") {
            if (!ParseNumber(value, options.rate) || options.rate < 1 || options.rate > 1000) {
                return false;
            }
        } else if (!ParseNumber(value, options.duration)
            || !std::isfinite(options.duration) || options.duration < 0) {
            return false;
        }
    }
    in_addr address{};
    return inet_pton(AF_INET, options.host.c_str(), &address) == 1
        && !options.deviceId.empty() && options.deviceId.size() <= protocol::kMaxDeviceIdLength
        && options.deviceId.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}

uint64_t TimestampUs(Clock::time_point now) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

class Connection {
public:
    ~Connection() {
        if (m_handlerInstalled) {
            SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
        }
        if (m_timerStarted) {
            timeEndPeriod(1);
        }
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
        }
        if (m_winsockStarted) {
            WSACleanup();
        }
    }

    void Open(const Options& options) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("Winsock の初期化に失敗しました");
        }
        m_winsockStarted = true;
        m_destination.sin_family = AF_INET;
        m_destination.sin_port = htons(options.port);
        if (inet_pton(AF_INET, options.host.c_str(), &m_destination.sin_addr) != 1) {
            throw std::runtime_error("送信先の IPv4 アドレスが不正です");
        }
        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET) {
            throw std::runtime_error("UDP ソケットの作成に失敗しました");
        }
        u_long nonblocking = 1;
        if (ioctlsocket(m_socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            throw std::runtime_error("UDP ソケットの非ブロッキング設定に失敗しました");
        }
        if (!SetConsoleCtrlHandler(HandleConsoleControl, TRUE)) {
            throw std::runtime_error("コンソール終了ハンドラの登録に失敗しました");
        }
        m_handlerInstalled = true;
        if (timeBeginPeriod(1) != TIMERR_NOERROR) {
            throw std::runtime_error("タイマー分解能の設定に失敗しました");
        }
        m_timerStarted = true;
    }

    void Send(const protocol::Message& message) {
        std::array<std::byte, protocol::kMaxPacketSize> buffer{};
        protocol::PacketHeader header{protocol::TypeOf(message)};
        header.sequence = m_sequence;
        header.timestampUs = TimestampUs(Clock::now());
        const auto size = protocol::Serialize(header, message, buffer);
        if (size == 0) {
            throw std::runtime_error("パケットのシリアライズに失敗しました");
        }
        const int sent = sendto(m_socket, reinterpret_cast<const char*>(buffer.data()),
            static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&m_destination),
            sizeof(m_destination));
        if (sent != static_cast<int>(size)) {
            throw std::runtime_error("UDP パケットの送信に失敗しました");
        }
        ++m_sequence;
    }

    std::optional<protocol::Packet> Receive() {
        // 無関係なパケットが大量に届いても、送信と Ctrl+C の処理に戻れるよう上限を設ける。
        for (unsigned count = 0; count < 64; ++count) {
            std::array<std::byte, protocol::kMaxPacketSize> buffer{};
            sockaddr_in sender{};
            int senderSize = sizeof(sender);
            const int size = recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&sender), &senderSize);
            if (size == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) {
                    return std::nullopt;
                }
                // 未起動のドライバ宛ての ICMP 応答は Hello の再送を妨げない。
                if (error == WSAECONNRESET || error == WSAEMSGSIZE) {
                    continue;
                }
                throw std::runtime_error("UDP パケットの受信に失敗しました");
            }
            if (sender.sin_family != AF_INET || sender.sin_addr.s_addr != m_destination.sin_addr.s_addr
                || sender.sin_port != m_destination.sin_port) {
                continue;
            }
            auto parsed = protocol::Parse(
                std::span<const std::byte>(buffer.data(), static_cast<size_t>(size)));
            if (parsed.error == protocol::ParseError::None) {
                return std::move(parsed.packet);
            }
        }
        return std::nullopt;
    }

private:
    SOCKET m_socket = INVALID_SOCKET;
    sockaddr_in m_destination{};
    uint32_t m_sequence = 0;
    bool m_winsockStarted = false;
    bool m_handlerInstalled = false;
    bool m_timerStarted = false;
};

const char* ResultName(protocol::HelloResult result) {
    switch (result) {
    case protocol::HelloResult::Accepted: return "Accepted";
    case protocol::HelloResult::Busy: return "Busy";
    case protocol::HelloResult::PendingApproval: return "PendingApproval";
    case protocol::HelloResult::VersionMismatch: return "VersionMismatch";
    case protocol::HelloResult::Rejected: return "Rejected";
    default: return "不明";
    }
}

bool Connect(Connection& connection, const Options& options) {
    protocol::HelloMessage hello;
    hello.deviceName = "pose_sender";
    hello.deviceId = options.deviceId;
    hello.eyeWidth = 2064;
    hello.eyeHeight = 2208;
    hello.refreshRate = static_cast<float>(options.rate);
    hello.ipdMeters = 0.063f;
    auto nextApprovalLog = Clock::now();
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        if (stopRequested.load(std::memory_order_relaxed)) {
            return false;
        }
        connection.Send(hello);
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        while (Clock::now() < deadline && !stopRequested.load(std::memory_order_relaxed)) {
            if (const auto packet = connection.Receive()) {
                if (const auto* ack = std::get_if<protocol::HelloAckMessage>(&packet->message)) {
                    if (ack->result == protocol::HelloResult::PendingApproval) {
                        attempt = 0;
                        if (Clock::now() >= nextApprovalLog) {
                            std::cout << "PendingApproval: ホストでの承認を待っています\n";
                            nextApprovalLog = Clock::now() + std::chrono::seconds(5);
                        }
                        continue;
                    }
                    if (ack->result != protocol::HelloResult::Accepted) {
                        std::cerr << "接続が拒否されました: " << ResultName(ack->result)
                                  << "（相手のバージョン: " << ack->hostProtocolVersion << "）\n";
                        return false;
                    }
                    return true;
                }
            }
            std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::milliseconds(5)));
        }
    }
    if (!stopRequested.load(std::memory_order_relaxed)) {
        std::cerr << "ドライバから応答がありません\n";
    }
    return false;
}

protocol::PoseMessage MakePose(Clock::time_point now, Clock::time_point start) {
    const double seconds = std::chrono::duration<double>(now - start).count();
    constexpr double kAmplitude = std::numbers::pi / 4.0;
    constexpr double kFrequency = 2.0 * std::numbers::pi / 8.0;
    const double yaw = kAmplitude * std::sin(kFrequency * seconds);
    protocol::PoseMessage message;
    message.sampleTimeUs = TimestampUs(now);
    message.head.valid = true;
    message.head.pose.position = {0, 1.6f, 0};
    message.head.pose.orientation = {0, static_cast<float>(std::sin(yaw / 2.0)),
        0, static_cast<float>(std::cos(yaw / 2.0))};
    message.head.pose.angularVelocity.y = static_cast<float>(kAmplitude * kFrequency * std::cos(kFrequency * seconds));
    return message;
}

void Advance(Clock::time_point& next, Clock::duration interval) {
    const auto now = Clock::now();
    // 遅延分をまとめて飛ばし、復帰直後の連続送信を避ける。
    next += interval * ((now - next) / interval + 1);
}

void Stream(Connection& connection, const Options& options) {
    const auto start = Clock::now();
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / options.rate));
    auto nextPose = start;
    auto nextPing = start + std::chrono::seconds(1);
    auto nextReport = nextPing;
    uint64_t poseCount = 0;
    uint64_t pingId = 0;
    std::optional<double> roundTripMs;
    while (!stopRequested.load(std::memory_order_relaxed)) {
        const auto now = Clock::now();
        if (options.duration > 0 && std::chrono::duration<double>(now - start).count() >= options.duration) {
            break;
        }
        if (now >= nextPose) {
            connection.Send(MakePose(now, start));
            ++poseCount;
            Advance(nextPose, interval);
        }
        if (now >= nextPing) {
            connection.Send(protocol::PingMessage{++pingId});
            Advance(nextPing, std::chrono::seconds(1));
        }
        for (unsigned count = 0; count < 64; ++count) {
            const auto packet = connection.Receive();
            if (!packet) {
                break;
            }
            if (const auto* pong = std::get_if<protocol::PongMessage>(&packet->message)) {
                const auto receivedUs = TimestampUs(Clock::now());
                if (pong->pingId > 0 && pong->pingId <= pingId && pong->pingTimestampUs <= receivedUs) {
                    roundTripMs = static_cast<double>(receivedUs - pong->pingTimestampUs) / 1000.0;
                }
            }
        }
        if (now >= nextReport) {
            std::cout << "送信 " << poseCount << " 件 / 往復時間 ";
            if (roundTripMs) {
                std::cout << std::fixed << std::setprecision(1) << *roundTripMs << " ms";
            } else {
                std::cout << "-";
            }
            std::cout << std::endl;
            roundTripMs.reset();
            Advance(nextReport, std::chrono::seconds(1));
        }
        // 低い送信頻度でも受信と Ctrl+C を速やかに処理する。
        std::this_thread::sleep_until(std::min({nextPose, nextPing, nextReport,
            Clock::now() + std::chrono::milliseconds(5)}));
    }
}

}

int Run(int argc, char* argv[]) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::cerr << "引数が不正です\n";
        PrintUsage();
        return 1;
    }
    if (options.help) {
        PrintUsage();
        return 0;
    }
    try {
        Connection connection;
        connection.Open(options);
        if (!Connect(connection, options)) {
            if (stopRequested.load(std::memory_order_relaxed)) {
                connection.Send(protocol::ByeMessage{});
                return 0;
            }
            return 2;
        }
        Stream(connection, options);
        connection.Send(protocol::ByeMessage{});
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}

}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    return wnvr::tools::Run(argc, argv);
}
