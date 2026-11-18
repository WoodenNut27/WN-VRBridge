#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wnvr::protocol {

inline constexpr uint32_t kMagic = 0x52564E57;
inline constexpr uint16_t kProtocolVersion = 6;
inline constexpr uint16_t kDefaultPort = 9944;
inline constexpr uint16_t kDiscoveryPort = 9943;
// 断片化を避けるため、イーサネットの MTU より小さくする。
inline constexpr size_t kMaxPacketSize = 1400;
inline constexpr size_t kHeaderSize = 20;
inline constexpr size_t kVideoFragmentHeaderSize = 53;
inline constexpr size_t kMaxVideoPayloadSize = kMaxPacketSize - kHeaderSize - kVideoFragmentHeaderSize;
inline constexpr size_t kMaxDeviceNameLength = 64;
inline constexpr size_t kMaxDeviceIdLength = 64;
inline constexpr size_t kMaxHostNameLength = 64;
inline constexpr size_t kMaxAudioFrames = 320;

enum class MessageType : uint8_t {
    Hello = 1,
    HelloAck = 2,
    Pose = 3,
    Ping = 4,
    Pong = 5,
    Bye = 6,
    Discover = 7,
    DiscoverReply = 8,
    VideoFragment = 9,
    KeyFrameRequest = 10,
    DisplayFeedback = 11,
    Input = 12,
    Audio = 13,
    ClientStats = 14,
    MicAudio = 15,
};

struct PacketHeader {
    MessageType type;
    uint8_t flags = 0;
    uint32_t sequence = 0;
    uint64_t timestampUs = 0;
};

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };

struct Pose {
    Vec3 position;
    Quat orientation;
    Vec3 linearVelocity;
    Vec3 angularVelocity;
};

struct DevicePose {
    bool valid = false;
    Pose pose;
};

// OpenVR の GetProjectionRaw と同じ tan 表現を使い、変換による差を避ける。
struct Fov { float left = -1, right = 1, top = -1, bottom = 1; };

// 相手から受け取った視野角と IPD（瞳孔間距離）を、描画に使う前に確かめる。
inline bool IsValidFov(const Fov& fov) {
    return std::isfinite(fov.left) && std::isfinite(fov.right)
        && std::isfinite(fov.top) && std::isfinite(fov.bottom)
        && fov.left < fov.right && fov.top < fov.bottom;
}

inline constexpr float kMinIpdMeters = 0.04f;
inline constexpr float kMaxIpdMeters = 0.09f;

inline bool IsValidIpd(float meters) {
    return std::isfinite(meters) && meters >= kMinIpdMeters && meters <= kMaxIpdMeters;
}

// 縦はテクスチャの先頭行を 0 とし、PC と Quest の向きをそろえる。
struct FoveationEye {
    float centerX0 = 0;
    float centerX1 = 1;
    float centerY0 = 0;
    float centerY1 = 1;
    bool operator==(const FoveationEye&) const = default;
};

// 無効時は恒等写像にして、従来のフレームも同じ経路で扱えるようにする。
struct Foveation {
    uint8_t enabled = 0;
    FoveationEye left;
    FoveationEye right;
    float edgeRatioX = 1;
    float edgeRatioY = 1;
    bool operator==(const Foveation&) const = default;
};

inline float FoveationCompressedRatio(float c0, float c1, float ratio) {
    return c0 / ratio + (c1 - c0) + (1.0f - c1) / ratio;
}

inline Foveation MakeFoveation(const Fov& left, const Fov& right, float width, float height,
    float ratioX, float ratioY, float extendUp) {
    const auto makeEye = [width, height, extendUp](const Fov& fov) {
        const float cx = -fov.left / (fov.right - fov.left);
        const float cy = fov.bottom / (fov.bottom - fov.top);
        const float x0 = std::clamp(cx - width * 0.5f, 0.0f, 1.0f - width);
        const float total = std::min(1.0f, height + extendUp);
        // 先頭行が見た目の上なので、下端を保って上端側に広げる。
        const float y1 = std::clamp(cy + height * 0.5f, total, 1.0f);
        return FoveationEye{x0, x0 + width, y1 - total, y1};
    };
    return {1, makeEye(left), makeEye(right), ratioX, ratioY};
}

struct HelloMessage {
    std::string deviceName;
    std::string deviceId;
    uint32_t eyeWidth = 0;
    uint32_t eyeHeight = 0;
    float refreshRate = 0;
    float ipdMeters = 0;
    Fov leftEyeFov;
    Fov rightEyeFov;
    uint32_t sessionId = 0;
    uint8_t startRequested = 0;
};

enum class HelloResult : uint8_t {
    Accepted = 0,
    VersionMismatch = 1,
    Rejected = 2,
    Busy = 3,
    PendingApproval = 4,
    Starting = 5,
    Stopped = 6,
};

inline constexpr int kMinQuestEyeSize = 256;
inline constexpr int kMaxQuestEyeSize = 8192;

struct HelloAckMessage {
    HelloResult result = HelloResult::Accepted;
    uint16_t hostProtocolVersion = kProtocolVersion;
    Fov renderLeftFov;
    Fov renderRightFov;
    float renderIpdMeters = 0;
    Foveation foveation;
    // 次のアプリ起動で使うために保存する。0×0 は Quest が決める。
    uint16_t questEyeWidth = 0;
    uint16_t questEyeHeight = 0;
};

struct PoseMessage {
    uint64_t sampleTimeUs = 0;
    DevicePose head;
    DevicePose leftHand;
    DevicePose rightHand;
};

// 状態をまるごと送り、取りこぼしても次の入力で回復できるようにする。
enum ControllerButton : uint32_t {
    kButtonPrimaryClick = 1u << 0,    // A（右）/ X（左）
    kButtonPrimaryTouch = 1u << 1,
    kButtonSecondaryClick = 1u << 2,  // B（右）/ Y（左）
    kButtonSecondaryTouch = 1u << 3,
    kButtonThumbstickClick = 1u << 4,
    kButtonThumbstickTouch = 1u << 5,
    kButtonTriggerTouch = 1u << 6,
    kButtonThumbrestTouch = 1u << 7,
    kButtonMenuClick = 1u << 8,       // 左手のメニューを system に対応させる。
};

struct ControllerInput {
    bool active = false;
    uint32_t buttons = 0;
    float trigger = 0;
    float grip = 0;
    float thumbstickX = 0;
    float thumbstickY = 0; // 上が +。
};

struct InputMessage {
    uint64_t sampleTimeUs = 0; // steady_clock のマイクロ秒。
    ControllerInput left;
    ControllerInput right;
};

struct PingMessage { uint64_t pingId = 0; };
struct PongMessage {
    uint64_t pingId = 0;
    uint64_t pingTimestampUs = 0;
    uint8_t micRequested = 0;
    // 接続中の変更も次の起動に備えて保存できるよう、HelloAck と同じ指定を送る。
    uint16_t questEyeWidth = 0;
    uint16_t questEyeHeight = 0;
    // 今の接続では切り替えず、次の接続から使う設定の違いを知らせる。
    uint8_t foveationPending = 0;
};
struct ByeMessage {};

struct DiscoverMessage {
    std::string deviceName;
};

struct DiscoverReplyMessage {
    std::string hostName;
    uint16_t port = kDefaultPort;
};

// Quest の時計での採取時刻を残し、後段で表示姿勢に対応付ける。
struct FramePose {
    uint64_t sampleTimeUs = 0;
    Vec3 position;
    Quat orientation;
};

struct VideoFragmentMessage {
    uint32_t frameIndex = 0;
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 0;
    uint32_t frameSize = 0;
    uint8_t flags = 0;
    uint16_t videoWidth = 0;
    uint16_t videoHeight = 0;
    FramePose pose;
    std::vector<uint8_t> payload;
};

enum class KeyFrameReason : uint8_t {
    DecoderStart = 0,  // デコーダーの生成・再生成後は参照画像がない。
    FrameGap = 1,      // 参照フレームの欠落により復号できなくなる。
    DecoderDrop = 2,   // 入力できなかったフレームの参照を避ける。
    DecoderError = 3,  // codec の状態を破棄して回復する。
};

struct KeyFrameRequestMessage {
    uint32_t lastGoodFrameIndex = 0;
    KeyFrameReason reason = KeyFrameReason::FrameGap;
};

struct DisplayFeedbackMessage {
    float averageQueueDepth = 0;
    uint16_t repeatedFrames = 0;
    uint16_t droppedFrames = 0;
    uint32_t displayPeriodUs = 0;
};

struct AudioMessage {
    uint64_t frameIndex = 0;
    uint32_t sampleRate = 0;
    uint8_t channelCount = 2;
    std::vector<int16_t> samples;
};

// Quest のマイクの音（16 bit・モノラル）。frameIndex は録音の通し番号にして、
// PC が抜けと逆順を見分けられるようにする。
struct MicAudioMessage {
    uint64_t frameIndex = 0;
    uint32_t sampleRate = 0;
    std::vector<int16_t> samples;
};

enum class QuestMicState : uint8_t {
    NotRequested = 0,     // PC が求めていない
    PermissionNeeded = 1, // Quest でマイクの使用が許可されていない
    Capturing = 2,        // 録音して送っている
    Failed = 3,           // マイクを開けなかった
};

// Quest の直近約 1 秒の統計。既存のログとは独立して集計する。
struct ClientStatsMessage {
    uint32_t periodUs = 0; // 集計した実際の期間（約 1 秒）
    uint16_t receivedFrames = 0; // 組み立てられた映像フレーム
    uint16_t lostFrames = 0; // 失われた映像フレーム
    uint32_t receivedFragments = 0; // 受け取った映像断片
    uint32_t receivedVideoBytes = 0; // 映像ペイロードのバイト数
    uint16_t decodedFrames = 0;
    uint32_t decodeAverageUs = 0;
    uint32_t decodeMaxUs = 0;
    uint16_t displayedFrames = 0; // 映像を描いた表示の回数
    uint16_t repeatedFrames = 0; // 同じ画像を再表示した回数
    uint32_t latencyAverageUs = 0; // 姿勢の採取から表示まで
    uint32_t latencyMaxUs = 0;
    uint32_t roundTripUs = 0; // 最新の往復時間。未測定なら 0
    uint16_t audioUnderruns = 0;
    uint32_t audioMissingFrames = 0;
    uint32_t audioBufferAverageUs = 0;
    uint32_t firstFragmentAverageUs = 0; // Q0: 姿勢の採取から最初の断片の受信まで
    uint32_t assembleAverageUs = 0; // Q1: 最初の断片から組み立て完了まで
    uint32_t decoderWaitAverageUs = 0; // Q2: 組み立て完了からデコーダへの入力まで
    uint32_t displayWaitAverageUs = 0; // Q4: デコーダの出力から表示予定時刻まで
    uint8_t micState = 0;
};

// 並び順は MessageType の値の順（1 から）と同じにする。TypeOf と Parse がこの対応に頼っている
// （ずれるとコンパイルが通らないよう、messages.cpp で確かめている）。
using Message = std::variant<HelloMessage, HelloAckMessage, PoseMessage, PingMessage, PongMessage, ByeMessage,
    DiscoverMessage, DiscoverReplyMessage, VideoFragmentMessage, KeyFrameRequestMessage, DisplayFeedbackMessage,
    InputMessage, AudioMessage, ClientStatsMessage, MicAudioMessage>;

struct Packet {
    PacketHeader header;
    Message message;
};

size_t Serialize(const PacketHeader& header, const Message& message, std::span<std::byte> buffer);

enum class ParseError : uint8_t {
    None = 0,
    TooShort,
    BadMagic,
    VersionMismatch,
    UnknownType,
    Malformed,
    TrailingData,
};

struct ParseResult {
    ParseError error = ParseError::None;
    std::optional<Packet> packet;
    uint16_t receivedVersion = 0;
};

ParseResult Parse(std::span<const std::byte> data);
MessageType TypeOf(const Message& message);
const char* ToString(ParseError error);

// 端末の ID に使える文字と長さ。設定ファイルやパイプで受け取った ID も同じ決まりで確かめる。
bool IsValidDeviceId(std::string_view id);

}
