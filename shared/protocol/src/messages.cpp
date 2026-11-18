#include "wnvr/protocol/messages.h"

#include "wnvr/protocol/byte_buffer.h"

#include <bit>
#include <cmath>
#include <type_traits>
#include <utility>

namespace wnvr::protocol {
namespace {

// MessageType の値は Message の並び順 + 1。対応がずれたらここでコンパイルを止める。
template<typename T, MessageType Type>
constexpr bool kIsAlternative =
    std::is_same_v<std::variant_alternative_t<static_cast<size_t>(Type) - 1, Message>, T>;
static_assert(kIsAlternative<HelloMessage, MessageType::Hello>);
static_assert(kIsAlternative<HelloAckMessage, MessageType::HelloAck>);
static_assert(kIsAlternative<PoseMessage, MessageType::Pose>);
static_assert(kIsAlternative<PingMessage, MessageType::Ping>);
static_assert(kIsAlternative<PongMessage, MessageType::Pong>);
static_assert(kIsAlternative<ByeMessage, MessageType::Bye>);
static_assert(kIsAlternative<DiscoverMessage, MessageType::Discover>);
static_assert(kIsAlternative<DiscoverReplyMessage, MessageType::DiscoverReply>);
static_assert(kIsAlternative<VideoFragmentMessage, MessageType::VideoFragment>);
static_assert(kIsAlternative<KeyFrameRequestMessage, MessageType::KeyFrameRequest>);
static_assert(kIsAlternative<DisplayFeedbackMessage, MessageType::DisplayFeedback>);
static_assert(kIsAlternative<InputMessage, MessageType::Input>);
static_assert(kIsAlternative<AudioMessage, MessageType::Audio>);
static_assert(kIsAlternative<ClientStatsMessage, MessageType::ClientStats>);
static_assert(kIsAlternative<MicAudioMessage, MessageType::MicAudio>);
static_assert(static_cast<size_t>(MessageType::MicAudio) == std::variant_size_v<Message>);

constexpr uint32_t kMinAudioSampleRate = 8000;
constexpr uint32_t kMaxAudioSampleRate = 192000;

bool IsValidAudioFrames(uint32_t sampleRate, size_t frameCount) {
    return sampleRate >= kMinAudioSampleRate && sampleRate <= kMaxAudioSampleRate
        && frameCount != 0 && frameCount <= kMaxAudioFrames;
}

bool IsValidAudio(uint32_t sampleRate, uint8_t channelCount, size_t frameCount) {
    return channelCount == 2 && IsValidAudioFrames(sampleRate, frameCount);
}

// ---- 部品 ----

void Write(ByteWriter& writer, const Vec3& value) {
    writer.WriteF32(value.x);
    writer.WriteF32(value.y);
    writer.WriteF32(value.z);
}

void Read(ByteReader& reader, Vec3& value) {
    value.x = reader.ReadF32();
    value.y = reader.ReadF32();
    value.z = reader.ReadF32();
}

void Write(ByteWriter& writer, const Quat& value) {
    writer.WriteF32(value.x);
    writer.WriteF32(value.y);
    writer.WriteF32(value.z);
    writer.WriteF32(value.w);
}

void Read(ByteReader& reader, Quat& value) {
    value.x = reader.ReadF32();
    value.y = reader.ReadF32();
    value.z = reader.ReadF32();
    value.w = reader.ReadF32();
}

void Write(ByteWriter& writer, const Fov& value) {
    writer.WriteF32(value.left);
    writer.WriteF32(value.right);
    writer.WriteF32(value.top);
    writer.WriteF32(value.bottom);
}

void Read(ByteReader& reader, Fov& value) {
    value.left = reader.ReadF32();
    value.right = reader.ReadF32();
    value.top = reader.ReadF32();
    value.bottom = reader.ReadF32();
}

void Write(ByteWriter& writer, const FoveationEye& value) {
    writer.WriteF32(value.centerX0);
    writer.WriteF32(value.centerX1);
    writer.WriteF32(value.centerY0);
    writer.WriteF32(value.centerY1);
}

void Read(ByteReader& reader, FoveationEye& value) {
    value.centerX0 = reader.ReadF32();
    value.centerX1 = reader.ReadF32();
    value.centerY0 = reader.ReadF32();
    value.centerY1 = reader.ReadF32();
}

// 真偽値は 0 と 1 だけを受け付け、壊れたパケットを有効な値として読まない。
bool ReadFlag(ByteReader& reader, bool& value) {
    const auto raw = reader.ReadU8();
    value = raw == 1;
    return raw <= 1;
}

void Write(ByteWriter& writer, const DevicePose& value) {
    writer.WriteU8(value.valid ? 1 : 0);
    Write(writer, value.pose.position);
    Write(writer, value.pose.orientation);
    Write(writer, value.pose.linearVelocity);
    Write(writer, value.pose.angularVelocity);
}

bool Read(ByteReader& reader, DevicePose& value) {
    const bool flagValid = ReadFlag(reader, value.valid);
    Read(reader, value.pose.position);
    Read(reader, value.pose.orientation);
    Read(reader, value.pose.linearVelocity);
    Read(reader, value.pose.angularVelocity);
    return flagValid && reader.Ok();
}

void Write(ByteWriter& writer, const ControllerInput& value) {
    writer.WriteU8(value.active ? 1 : 0);
    writer.WriteU32(value.buttons);
    writer.WriteF32(value.trigger);
    writer.WriteF32(value.grip);
    writer.WriteF32(value.thumbstickX);
    writer.WriteF32(value.thumbstickY);
}

bool Read(ByteReader& reader, ControllerInput& value) {
    const bool flagValid = ReadFlag(reader, value.active);
    value.buttons = reader.ReadU32();
    value.trigger = reader.ReadF32();
    value.grip = reader.ReadF32();
    value.thumbstickX = reader.ReadF32();
    value.thumbstickY = reader.ReadF32();
    return flagValid && reader.Ok();
}

// ---- メッセージ（書く側と読む側を並べて、項目の順番のずれに気づけるようにする）----

void WriteBody(ByteWriter& writer, const HelloMessage& value) {
    writer.WriteString(value.deviceName, kMaxDeviceNameLength);
    writer.WriteString(value.deviceId, kMaxDeviceIdLength);
    writer.WriteU32(value.eyeWidth);
    writer.WriteU32(value.eyeHeight);
    writer.WriteF32(value.refreshRate);
    writer.WriteF32(value.ipdMeters);
    Write(writer, value.leftEyeFov);
    Write(writer, value.rightEyeFov);
    writer.WriteU32(value.sessionId);
    writer.WriteU8(value.startRequested);
}

bool ReadBody(ByteReader& reader, HelloMessage& value) {
    value.deviceName = reader.ReadString(kMaxDeviceNameLength);
    value.deviceId = reader.ReadString(kMaxDeviceIdLength);
    if (!IsValidDeviceId(value.deviceId)) {
        return false;
    }
    value.eyeWidth = reader.ReadU32();
    value.eyeHeight = reader.ReadU32();
    value.refreshRate = reader.ReadF32();
    value.ipdMeters = reader.ReadF32();
    Read(reader, value.leftEyeFov);
    Read(reader, value.rightEyeFov);
    value.sessionId = reader.ReadU32();
    value.startRequested = reader.ReadU8();
    return value.startRequested <= 1;
}

void WriteBody(ByteWriter& writer, const HelloAckMessage& value) {
    writer.WriteU8(static_cast<uint8_t>(value.result));
    writer.WriteU16(value.hostProtocolVersion);
    Write(writer, value.renderLeftFov);
    Write(writer, value.renderRightFov);
    writer.WriteF32(value.renderIpdMeters);
    writer.WriteU8(value.foveation.enabled);
    Write(writer, value.foveation.left);
    Write(writer, value.foveation.right);
    writer.WriteF32(value.foveation.edgeRatioX);
    writer.WriteF32(value.foveation.edgeRatioY);
    writer.WriteU16(value.questEyeWidth);
    writer.WriteU16(value.questEyeHeight);
}

bool IsValidQuestEyeSize(uint16_t width, uint16_t height) {
    return (width == 0 && height == 0)
        || (width >= kMinQuestEyeSize && width <= kMaxQuestEyeSize
            && height >= kMinQuestEyeSize && height <= kMaxQuestEyeSize);
}

bool ReadBody(ByteReader& reader, HelloAckMessage& value) {
    const auto result = reader.ReadU8();
    if (result > static_cast<uint8_t>(HelloResult::Stopped)) {
        return false;
    }
    value.result = static_cast<HelloResult>(result);
    value.hostProtocolVersion = reader.ReadU16();
    Read(reader, value.renderLeftFov);
    Read(reader, value.renderRightFov);
    value.renderIpdMeters = reader.ReadF32();
    auto& foveation = value.foveation;
    foveation.enabled = reader.ReadU8();
    Read(reader, foveation.left);
    Read(reader, foveation.right);
    foveation.edgeRatioX = reader.ReadF32();
    foveation.edgeRatioY = reader.ReadF32();
    // Quest はこの値でシェーダーの写像を作るので、範囲外や NaN をここで落とす。
    const auto validInterval = [](float a, float b) {
        return std::isfinite(a) && std::isfinite(b) && a >= 0 && a < b && b <= 1;
    };
    for (const auto& eye : {foveation.left, foveation.right}) {
        if (!validInterval(eye.centerX0, eye.centerX1) || !validInterval(eye.centerY0, eye.centerY1)) {
            return false;
        }
    }
    for (const float ratio : {foveation.edgeRatioX, foveation.edgeRatioY}) {
        if (!std::isfinite(ratio) || ratio < 1 || ratio > 16) {
            return false;
        }
    }
    value.questEyeWidth = reader.ReadU16();
    value.questEyeHeight = reader.ReadU16();
    return foveation.enabled <= 1 && IsValidQuestEyeSize(value.questEyeWidth, value.questEyeHeight);
}

void WriteBody(ByteWriter& writer, const PoseMessage& value) {
    writer.WriteU64(value.sampleTimeUs);
    Write(writer, value.head);
    Write(writer, value.leftHand);
    Write(writer, value.rightHand);
}

bool ReadBody(ByteReader& reader, PoseMessage& value) {
    value.sampleTimeUs = reader.ReadU64();
    return Read(reader, value.head) && Read(reader, value.leftHand) && Read(reader, value.rightHand);
}

void WriteBody(ByteWriter& writer, const PingMessage& value) {
    writer.WriteU64(value.pingId);
}

bool ReadBody(ByteReader& reader, PingMessage& value) {
    value.pingId = reader.ReadU64();
    return true;
}

void WriteBody(ByteWriter& writer, const PongMessage& value) {
    writer.WriteU64(value.pingId);
    writer.WriteU64(value.pingTimestampUs);
    writer.WriteU8(value.micRequested);
    writer.WriteU16(value.questEyeWidth);
    writer.WriteU16(value.questEyeHeight);
    writer.WriteU8(value.foveationPending);
}

bool ReadBody(ByteReader& reader, PongMessage& value) {
    value.pingId = reader.ReadU64();
    value.pingTimestampUs = reader.ReadU64();
    value.micRequested = reader.ReadU8();
    value.questEyeWidth = reader.ReadU16();
    value.questEyeHeight = reader.ReadU16();
    value.foveationPending = reader.ReadU8();
    return value.micRequested <= 1 && value.foveationPending <= 1
        && IsValidQuestEyeSize(value.questEyeWidth, value.questEyeHeight);
}

void WriteBody(ByteWriter&, const ByeMessage&) {}

bool ReadBody(ByteReader&, ByeMessage&) {
    return true;
}

void WriteBody(ByteWriter& writer, const DiscoverMessage& value) {
    writer.WriteString(value.deviceName, kMaxDeviceNameLength);
}

bool ReadBody(ByteReader& reader, DiscoverMessage& value) {
    value.deviceName = reader.ReadString(kMaxDeviceNameLength);
    return true;
}

void WriteBody(ByteWriter& writer, const DiscoverReplyMessage& value) {
    writer.WriteString(value.hostName, kMaxHostNameLength);
    writer.WriteU16(value.port);
}

bool ReadBody(ByteReader& reader, DiscoverReplyMessage& value) {
    value.hostName = reader.ReadString(kMaxHostNameLength);
    value.port = reader.ReadU16();
    return true;
}

void WriteBody(ByteWriter& writer, const VideoFragmentMessage& value) {
    writer.WriteU32(value.frameIndex);
    writer.WriteU16(value.fragmentIndex);
    writer.WriteU16(value.fragmentCount);
    writer.WriteU32(value.frameSize);
    writer.WriteU8(value.flags);
    writer.WriteU16(value.videoWidth);
    writer.WriteU16(value.videoHeight);
    writer.WriteU64(value.pose.sampleTimeUs);
    Write(writer, value.pose.position);
    Write(writer, value.pose.orientation);
    writer.WriteBytes(value.payload);
}

bool ReadBody(ByteReader& reader, VideoFragmentMessage& value) {
    value.frameIndex = reader.ReadU32();
    value.fragmentIndex = reader.ReadU16();
    value.fragmentCount = reader.ReadU16();
    value.frameSize = reader.ReadU32();
    value.flags = reader.ReadU8();
    value.videoWidth = reader.ReadU16();
    value.videoHeight = reader.ReadU16();
    value.pose.sampleTimeUs = reader.ReadU64();
    Read(reader, value.pose.position);
    Read(reader, value.pose.orientation);
    // 長さの項目は無く、パケットの残りがすべて映像のデータになる。
    const auto payloadSize = reader.Remaining();
    if (!reader.Ok() || value.fragmentCount == 0 || value.fragmentIndex >= value.fragmentCount
        || value.videoWidth == 0 || value.videoHeight == 0
        || payloadSize == 0 || payloadSize > kMaxVideoPayloadSize || value.frameSize < payloadSize) {
        return false;
    }
    value.payload.resize(payloadSize);
    reader.ReadBytes(value.payload);
    return true;
}

void WriteBody(ByteWriter& writer, const KeyFrameRequestMessage& value) {
    writer.WriteU32(value.lastGoodFrameIndex);
    writer.WriteU8(static_cast<uint8_t>(value.reason));
}

bool ReadBody(ByteReader& reader, KeyFrameRequestMessage& value) {
    value.lastGoodFrameIndex = reader.ReadU32();
    const auto reason = reader.ReadU8();
    if (reason > static_cast<uint8_t>(KeyFrameReason::DecoderError)) {
        return false;
    }
    value.reason = static_cast<KeyFrameReason>(reason);
    return true;
}

void WriteBody(ByteWriter& writer, const DisplayFeedbackMessage& value) {
    writer.WriteF32(value.averageQueueDepth);
    writer.WriteU16(value.repeatedFrames);
    writer.WriteU16(value.droppedFrames);
    writer.WriteU32(value.displayPeriodUs);
}

bool ReadBody(ByteReader& reader, DisplayFeedbackMessage& value) {
    value.averageQueueDepth = reader.ReadF32();
    value.repeatedFrames = reader.ReadU16();
    value.droppedFrames = reader.ReadU16();
    value.displayPeriodUs = reader.ReadU32();
    return true;
}

void WriteBody(ByteWriter& writer, const InputMessage& value) {
    writer.WriteU64(value.sampleTimeUs);
    Write(writer, value.left);
    Write(writer, value.right);
}

bool ReadBody(ByteReader& reader, InputMessage& value) {
    value.sampleTimeUs = reader.ReadU64();
    return Read(reader, value.left) && Read(reader, value.right);
}

void WriteBody(ByteWriter& writer, const AudioMessage& value) {
    writer.WriteU64(value.frameIndex);
    writer.WriteU32(value.sampleRate);
    writer.WriteU8(value.channelCount);
    writer.WriteU16(static_cast<uint16_t>(value.samples.size() / value.channelCount));
    for (const auto sample : value.samples) {
        writer.WriteU16(std::bit_cast<uint16_t>(sample));
    }
}

bool ReadBody(ByteReader& reader, AudioMessage& value) {
    value.frameIndex = reader.ReadU64();
    value.sampleRate = reader.ReadU32();
    value.channelCount = reader.ReadU8();
    const auto frameCount = reader.ReadU16();
    // 件数を信じて確保する前に、範囲を確かめる。
    if (!reader.Ok() || !IsValidAudio(value.sampleRate, value.channelCount, frameCount)) {
        return false;
    }
    value.samples.resize(static_cast<size_t>(frameCount) * value.channelCount);
    for (auto& sample : value.samples) {
        sample = std::bit_cast<int16_t>(reader.ReadU16());
    }
    return true;
}

void WriteBody(ByteWriter& writer, const MicAudioMessage& value) {
    writer.WriteU64(value.frameIndex);
    writer.WriteU32(value.sampleRate);
    writer.WriteU16(static_cast<uint16_t>(value.samples.size()));
    for (const auto sample : value.samples) {
        writer.WriteU16(std::bit_cast<uint16_t>(sample));
    }
}

bool ReadBody(ByteReader& reader, MicAudioMessage& value) {
    value.frameIndex = reader.ReadU64();
    value.sampleRate = reader.ReadU32();
    const auto frames = reader.ReadU16();
    // 確保前に検査し、壊れた件数で受信スレッドを圧迫しない。
    if (!reader.Ok() || !IsValidAudioFrames(value.sampleRate, frames)) {
        return false;
    }
    value.samples.resize(frames);
    for (auto& sample : value.samples) {
        sample = std::bit_cast<int16_t>(reader.ReadU16());
    }
    return true;
}

void WriteBody(ByteWriter& writer, const ClientStatsMessage& value) {
    writer.WriteU32(value.periodUs);
    writer.WriteU16(value.receivedFrames);
    writer.WriteU16(value.lostFrames);
    writer.WriteU32(value.receivedFragments);
    writer.WriteU32(value.receivedVideoBytes);
    writer.WriteU16(value.decodedFrames);
    writer.WriteU32(value.decodeAverageUs);
    writer.WriteU32(value.decodeMaxUs);
    writer.WriteU16(value.displayedFrames);
    writer.WriteU16(value.repeatedFrames);
    writer.WriteU32(value.latencyAverageUs);
    writer.WriteU32(value.latencyMaxUs);
    writer.WriteU32(value.roundTripUs);
    writer.WriteU16(value.audioUnderruns);
    writer.WriteU32(value.audioMissingFrames);
    writer.WriteU32(value.audioBufferAverageUs);
    writer.WriteU32(value.firstFragmentAverageUs);
    writer.WriteU32(value.assembleAverageUs);
    writer.WriteU32(value.decoderWaitAverageUs);
    writer.WriteU32(value.displayWaitAverageUs);
    writer.WriteU8(value.micState);
}

bool ReadBody(ByteReader& reader, ClientStatsMessage& value) {
    value.periodUs = reader.ReadU32();
    value.receivedFrames = reader.ReadU16();
    value.lostFrames = reader.ReadU16();
    value.receivedFragments = reader.ReadU32();
    value.receivedVideoBytes = reader.ReadU32();
    value.decodedFrames = reader.ReadU16();
    value.decodeAverageUs = reader.ReadU32();
    value.decodeMaxUs = reader.ReadU32();
    value.displayedFrames = reader.ReadU16();
    value.repeatedFrames = reader.ReadU16();
    value.latencyAverageUs = reader.ReadU32();
    value.latencyMaxUs = reader.ReadU32();
    value.roundTripUs = reader.ReadU32();
    value.audioUnderruns = reader.ReadU16();
    value.audioMissingFrames = reader.ReadU32();
    value.audioBufferAverageUs = reader.ReadU32();
    value.firstFragmentAverageUs = reader.ReadU32();
    value.assembleAverageUs = reader.ReadU32();
    value.decoderWaitAverageUs = reader.ReadU32();
    value.displayWaitAverageUs = reader.ReadU32();
    value.micState = reader.ReadU8();
    return value.micState <= static_cast<uint8_t>(QuestMicState::Failed);
}

template<typename T>
bool ReadAs(ByteReader& reader, Message& message) {
    T value;
    if (!ReadBody(reader, value)) {
        return false;
    }
    message = std::move(value);
    return true;
}

bool ReadMessage(ByteReader& reader, MessageType type, Message& message) {
    switch (type) {
    case MessageType::Hello: return ReadAs<HelloMessage>(reader, message);
    case MessageType::HelloAck: return ReadAs<HelloAckMessage>(reader, message);
    case MessageType::Pose: return ReadAs<PoseMessage>(reader, message);
    case MessageType::Ping: return ReadAs<PingMessage>(reader, message);
    case MessageType::Pong: return ReadAs<PongMessage>(reader, message);
    case MessageType::Bye: return ReadAs<ByeMessage>(reader, message);
    case MessageType::Discover: return ReadAs<DiscoverMessage>(reader, message);
    case MessageType::DiscoverReply: return ReadAs<DiscoverReplyMessage>(reader, message);
    case MessageType::VideoFragment: return ReadAs<VideoFragmentMessage>(reader, message);
    case MessageType::KeyFrameRequest: return ReadAs<KeyFrameRequestMessage>(reader, message);
    case MessageType::DisplayFeedback: return ReadAs<DisplayFeedbackMessage>(reader, message);
    case MessageType::Input: return ReadAs<InputMessage>(reader, message);
    case MessageType::Audio: return ReadAs<AudioMessage>(reader, message);
    case MessageType::ClientStats: return ReadAs<ClientStatsMessage>(reader, message);
    case MessageType::MicAudio: return ReadAs<MicAudioMessage>(reader, message);
    }
    return false;
}

}

bool IsValidDeviceId(std::string_view id) {
    return !id.empty() && id.size() <= kMaxDeviceIdLength && id.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string_view::npos;
}

MessageType TypeOf(const Message& message) {
    return static_cast<MessageType>(message.index() + 1);
}

size_t Serialize(const PacketHeader& header, const Message& message, std::span<std::byte> buffer) {
    if (const auto* audio = std::get_if<AudioMessage>(&message); audio
        && (audio->samples.size() % 2 != 0
            || !IsValidAudio(audio->sampleRate, audio->channelCount, audio->samples.size() / 2))) {
        return 0;
    }
    if (const auto* fragment = std::get_if<VideoFragmentMessage>(&message);
        fragment && fragment->payload.size() > kMaxVideoPayloadSize) {
        return 0;
    }
    if (const auto* mic = std::get_if<MicAudioMessage>(&message); mic
        && !IsValidAudioFrames(mic->sampleRate, mic->samples.size())) {
        return 0;
    }
    ByteWriter writer(buffer);
    writer.WriteU32(kMagic);
    writer.WriteU16(kProtocolVersion);
    writer.WriteU8(static_cast<uint8_t>(TypeOf(message)));
    writer.WriteU8(header.flags);
    writer.WriteU32(header.sequence);
    writer.WriteU64(header.timestampUs);
    std::visit([&writer](const auto& value) { WriteBody(writer, value); }, message);
    return writer.Ok() ? writer.Size() : 0;
}

ParseResult Parse(std::span<const std::byte> data) {
    ParseResult result;
    ByteReader reader(data);
    const auto magic = reader.ReadU32();
    // 版が違っても「何版から来たか」は返せるよう、ヘッダの長さを確かめる前に読む。
    if (data.size() >= 6) {
        result.receivedVersion = reader.ReadU16();
    }
    if (data.size() < kHeaderSize) {
        result.error = ParseError::TooShort;
        return result;
    }
    if (magic != kMagic) {
        result.error = ParseError::BadMagic;
        return result;
    }
    if (result.receivedVersion != kProtocolVersion) {
        result.error = ParseError::VersionMismatch;
        return result;
    }
    const auto type = reader.ReadU8();
    if (type < static_cast<uint8_t>(MessageType::Hello) || type > std::variant_size_v<Message>) {
        result.error = ParseError::UnknownType;
        return result;
    }
    Packet packet;
    packet.header.type = static_cast<MessageType>(type);
    packet.header.flags = reader.ReadU8();
    packet.header.sequence = reader.ReadU32();
    packet.header.timestampUs = reader.ReadU64();
    if (!ReadMessage(reader, packet.header.type, packet.message) || !reader.Ok()) {
        result.error = ParseError::Malformed;
        return result;
    }
    if (reader.Remaining() != 0) {
        result.error = ParseError::TrailingData;
        return result;
    }
    result.packet = std::move(packet);
    return result;
}

const char* ToString(ParseError error) {
    switch (error) {
    case ParseError::None: return "None";
    case ParseError::TooShort: return "TooShort";
    case ParseError::BadMagic: return "BadMagic";
    case ParseError::VersionMismatch: return "VersionMismatch";
    case ParseError::UnknownType: return "UnknownType";
    case ParseError::Malformed: return "Malformed";
    case ParseError::TrailingData: return "TrailingData";
    default: return "Unknown";
    }
}

}
