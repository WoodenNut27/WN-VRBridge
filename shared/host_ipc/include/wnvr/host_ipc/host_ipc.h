#pragma once

#include <wnvr/protocol/messages.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ホスト（設定画面）とドライバ（SteamVR の中）のあいだの、名前付きパイプのメッセージ。
// 1 メッセージを 1 つの JSON にして送る。
namespace wnvr::ipc {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\WN-VRBridge.Driver";
inline constexpr int kProtocolVersion = 5;
inline constexpr size_t kMaxMessageBytes = 64 * 1024;

// 設定の範囲。設定ファイル・設定画面・パイプの検査で同じ値を使う。
inline constexpr int kMinBitrateMbps = 10;
inline constexpr int kMaxBitrateMbps = 200;
inline constexpr int kDefaultBitrateMbps = 80;
inline constexpr size_t kMaxApprovedDevices = 64;
inline constexpr size_t kMaxDeviceNameBytes = protocol::kMaxDeviceNameLength;

// Quest 3 の画面の片目の画素数。描画の解像度の基準をそろえる。
inline constexpr int kQuestPanelEyeWidth = 2064;
inline constexpr int kQuestPanelEyeHeight = 2208;
inline constexpr int kMinQuestResolutionPercent = 70;
inline constexpr int kMaxQuestResolutionPercent = 150;
inline constexpr int kDefaultQuestResolutionPercent = 100;
inline constexpr int kQuestResolutionPercentStep = 5;
// 1 は縮めないのと同じなので、下限は 1.5 にする。
inline constexpr float kMinFoveationEdgeRatio = 1.5f;
inline constexpr float kMaxFoveationEdgeRatio = 5.0f;
inline constexpr float kDefaultFoveationEdgeRatio = 4.0f;
inline constexpr float kFoveationEdgeRatioStep = 0.5f;

struct EyeSize {
    int width = 0;
    int height = 0;
    bool operator==(const EyeSize&) const = default;
};
EyeSize QuestEyeSizeForPercent(int percent);

// 端末の ID の決まりは Quest との通信と同じ。
using protocol::IsValidDeviceId;

// ドライバの「Quest のマイクの音を CABLE Input に再生する」部分の状態。ホストの表示にも使う。
enum class PcMicState { Disabled, CableMissing, Waiting, Playing };

// 直近の確定した 5 秒窓。
struct StatsSnapshot {
    bool clientConnected = false;
    std::string clientName;
    uint32_t clientSessionId = 0;
    double renderFps = 0;
    double encodeFps = 0;
    double encodeAverageMs = 0;
    double encodeMaxMs = 0;
    double sentMbps = 0; // 映像ペイロードのみ。
    uint64_t sentFragments = 0;
    uint64_t sendDroppedFrames = 0;
    double virtualVsyncHz = 0;
    // この窓に届いた Quest の報告を集約する。
    double receivedFps = 0;
    uint64_t lostFrames = 0;
    double fragmentLossPercent = 0; // 同じ窓の送信断片数と比較し、負値は 0。
    double receivedMbps = 0;
    double decodeAverageMs = 0;
    double decodeMaxMs = 0;
    double displayFps = 0;
    uint64_t repeatedFrames = 0;
    double latencyAverageMs = 0;
    double latencyMaxMs = 0;
    double roundTripMs = 0;
    uint64_t audioUnderruns = 0;
    uint64_t audioMissingFrames = 0;
    double audioBufferAverageMs = 0;
    int pcMicState = 0;
    int questMicState = 0;
};

struct HelloMessage {
    int protocolVersion = kProtocolVersion;
    std::string version;
};
struct StatusMessage {
    StatsSnapshot stats;
};
struct SettingsMessage {
    std::string bindAddress;
    int bitrateMbps = kDefaultBitrateMbps;
    bool streamAudioToQuest = true;
    bool muteSpeakerWhileConnected = true;
    std::vector<std::string> approvedDeviceIds;
    std::vector<std::string> rejectedDeviceIds;
    int questResolutionPercent = kDefaultQuestResolutionPercent;
    bool foveatedEncoding = true;
    float foveationEdgeRatio = kDefaultFoveationEdgeRatio;
    bool micToPc = true;
    bool autoStopSteamVr = false;
};
// 未承認の端末の情報を UI スレッドへ渡す。
struct PairingRequestMessage {
    std::string deviceId;
    std::string deviceName;
    std::string address;
};

using Message = std::variant<HelloMessage, StatusMessage, SettingsMessage, PairingRequestMessage>;
std::string Encode(const Message& message);
std::optional<Message> Decode(std::string_view text, std::string* error = nullptr);

}
