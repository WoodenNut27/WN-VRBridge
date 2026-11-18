#include "wnvr/protocol/byte_buffer.h"
#include "wnvr/protocol/messages.h"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

namespace wnvr::protocol {
namespace {

constexpr size_t kPongPacketSize = kHeaderSize + 22;
constexpr size_t kHelloBasePacketSize = 75;

void CheckEqual(const Vec3& actual, const Vec3& expected) {
    CHECK(actual.x == expected.x);
    CHECK(actual.y == expected.y);
    CHECK(actual.z == expected.z);
}

void CheckEqual(const Fov& actual, const Fov& expected) {
    CHECK(actual.left == expected.left);
    CHECK(actual.right == expected.right);
    CHECK(actual.top == expected.top);
    CHECK(actual.bottom == expected.bottom);
}

void CheckEqual(const DevicePose& actual, const DevicePose& expected) {
    CHECK(actual.valid == expected.valid);
    CheckEqual(actual.pose.position, expected.pose.position);
    CHECK(actual.pose.orientation.x == expected.pose.orientation.x);
    CHECK(actual.pose.orientation.y == expected.pose.orientation.y);
    CHECK(actual.pose.orientation.z == expected.pose.orientation.z);
    CHECK(actual.pose.orientation.w == expected.pose.orientation.w);
    CheckEqual(actual.pose.linearVelocity, expected.pose.linearVelocity);
    CheckEqual(actual.pose.angularVelocity, expected.pose.angularVelocity);
}

void CheckEqual(const HelloMessage& actual, const HelloMessage& expected) {
    CHECK(actual.deviceName == expected.deviceName);
    CHECK(actual.deviceId == expected.deviceId);
    CHECK(actual.eyeWidth == expected.eyeWidth);
    CHECK(actual.eyeHeight == expected.eyeHeight);
    CHECK(actual.refreshRate == expected.refreshRate);
    CHECK(actual.ipdMeters == expected.ipdMeters);
    CheckEqual(actual.leftEyeFov, expected.leftEyeFov);
    CheckEqual(actual.rightEyeFov, expected.rightEyeFov);
    CHECK(actual.sessionId == expected.sessionId);
    CHECK(actual.startRequested == expected.startRequested);
}

void CheckEqual(const HelloAckMessage& actual, const HelloAckMessage& expected) {
    CHECK(actual.result == expected.result);
    CHECK(actual.hostProtocolVersion == expected.hostProtocolVersion);
    CheckEqual(actual.renderLeftFov, expected.renderLeftFov);
    CheckEqual(actual.renderRightFov, expected.renderRightFov);
    CHECK(actual.renderIpdMeters == expected.renderIpdMeters);
    CHECK(actual.foveation == expected.foveation);
    CHECK(actual.questEyeWidth == expected.questEyeWidth);
    CHECK(actual.questEyeHeight == expected.questEyeHeight);
}

void CheckEqual(const PoseMessage& actual, const PoseMessage& expected) {
    CHECK(actual.sampleTimeUs == expected.sampleTimeUs);
    CheckEqual(actual.head, expected.head);
    CheckEqual(actual.leftHand, expected.leftHand);
    CheckEqual(actual.rightHand, expected.rightHand);
}

void CheckEqual(const ControllerInput& actual, const ControllerInput& expected) {
    CHECK(actual.active == expected.active);
    CHECK(actual.buttons == expected.buttons);
    CHECK(actual.trigger == expected.trigger);
    CHECK(actual.grip == expected.grip);
    CHECK(actual.thumbstickX == expected.thumbstickX);
    CHECK(actual.thumbstickY == expected.thumbstickY);
}

void CheckEqual(const InputMessage& actual, const InputMessage& expected) {
    CHECK(actual.sampleTimeUs == expected.sampleTimeUs);
    CheckEqual(actual.left, expected.left);
    CheckEqual(actual.right, expected.right);
}

void CheckEqual(const AudioMessage& actual, const AudioMessage& expected) {
    CHECK(actual.frameIndex == expected.frameIndex);
    CHECK(actual.sampleRate == expected.sampleRate);
    CHECK(actual.channelCount == expected.channelCount);
    CHECK(actual.samples == expected.samples);
}

void CheckEqual(const MicAudioMessage& actual, const MicAudioMessage& expected) {
    CHECK(actual.frameIndex == expected.frameIndex);
    CHECK(actual.sampleRate == expected.sampleRate);
    CHECK(actual.samples == expected.samples);
}

void CheckEqual(const ClientStatsMessage& actual, const ClientStatsMessage& expected) {
    CHECK(actual.periodUs == expected.periodUs);
    CHECK(actual.receivedFrames == expected.receivedFrames);
    CHECK(actual.lostFrames == expected.lostFrames);
    CHECK(actual.receivedFragments == expected.receivedFragments);
    CHECK(actual.receivedVideoBytes == expected.receivedVideoBytes);
    CHECK(actual.decodedFrames == expected.decodedFrames);
    CHECK(actual.decodeAverageUs == expected.decodeAverageUs);
    CHECK(actual.decodeMaxUs == expected.decodeMaxUs);
    CHECK(actual.displayedFrames == expected.displayedFrames);
    CHECK(actual.repeatedFrames == expected.repeatedFrames);
    CHECK(actual.latencyAverageUs == expected.latencyAverageUs);
    CHECK(actual.latencyMaxUs == expected.latencyMaxUs);
    CHECK(actual.roundTripUs == expected.roundTripUs);
    CHECK(actual.audioUnderruns == expected.audioUnderruns);
    CHECK(actual.audioMissingFrames == expected.audioMissingFrames);
    CHECK(actual.audioBufferAverageUs == expected.audioBufferAverageUs);
    CHECK(actual.firstFragmentAverageUs == expected.firstFragmentAverageUs);
    CHECK(actual.assembleAverageUs == expected.assembleAverageUs);
    CHECK(actual.decoderWaitAverageUs == expected.decoderWaitAverageUs);
    CHECK(actual.displayWaitAverageUs == expected.displayWaitAverageUs);
    CHECK(actual.micState == expected.micState);
}

void CheckEqual(const PingMessage& actual, const PingMessage& expected) {
    CHECK(actual.pingId == expected.pingId);
}

void CheckEqual(const PongMessage& actual, const PongMessage& expected) {
    CHECK(actual.pingId == expected.pingId);
    CHECK(actual.pingTimestampUs == expected.pingTimestampUs);
    CHECK(actual.micRequested == expected.micRequested);
    CHECK(actual.questEyeWidth == expected.questEyeWidth);
    CHECK(actual.questEyeHeight == expected.questEyeHeight);
    CHECK(actual.foveationPending == expected.foveationPending);
}

void CheckEqual(const ByeMessage&, const ByeMessage&) {}

void CheckEqual(const DiscoverMessage& actual, const DiscoverMessage& expected) {
    CHECK(actual.deviceName == expected.deviceName);
}

void CheckEqual(const DiscoverReplyMessage& actual, const DiscoverReplyMessage& expected) {
    CHECK(actual.hostName == expected.hostName);
    CHECK(actual.port == expected.port);
}

void CheckEqual(const KeyFrameRequestMessage& actual, const KeyFrameRequestMessage& expected) {
    CHECK(actual.lastGoodFrameIndex == expected.lastGoodFrameIndex);
    CHECK(actual.reason == expected.reason);
}

constexpr PacketHeader kHeader{MessageType::Bye, 0, 0x78563412, 0x8877665544332211ULL};

void CheckEqual(const DisplayFeedbackMessage& actual, const DisplayFeedbackMessage& expected) {
    CHECK(actual.averageQueueDepth == expected.averageQueueDepth);
    CHECK(actual.repeatedFrames == expected.repeatedFrames);
    CHECK(actual.droppedFrames == expected.droppedFrames);
    CHECK(actual.displayPeriodUs == expected.displayPeriodUs);
}

std::vector<std::byte> Encode(const Message& message) {
    std::vector<std::byte> buffer(kMaxPacketSize);
    const auto size = Serialize(kHeader, message, buffer);
    CHECK(size != 0);
    CHECK(size <= kMaxPacketSize);
    buffer.resize(size);
    return buffer;
}

void CheckError(std::span<const std::byte> data, ParseError expected) {
    const auto parsed = Parse(data);
    CHECK(parsed.error == expected);
    CHECK(!parsed.packet.has_value());
}

template<typename T>
void CheckRoundTrip(const T& message, MessageType type, size_t expectedSize) {
    const auto buffer = Encode(message);
    CHECK(buffer.size() == expectedSize);
    CHECK(TypeOf(Message{message}) == type);
    const auto parsed = Parse(buffer);
    CHECK(parsed.error == ParseError::None);
    CHECK(parsed.receivedVersion == kProtocolVersion);
    CHECK(parsed.packet.has_value());
    if (parsed.packet) {
        CHECK(parsed.packet->header.type == type);
        CHECK(parsed.packet->header.flags == kHeader.flags);
        CHECK(parsed.packet->header.sequence == kHeader.sequence);
        CHECK(parsed.packet->header.timestampUs == kHeader.timestampUs);
        const auto* actual = std::get_if<T>(&parsed.packet->message);
        CHECK(actual != nullptr);
        if (actual) {
            CheckEqual(*actual, message);
        }
    }
    auto trailing = buffer;
    trailing.push_back(std::byte{0});
    CheckError(trailing, ParseError::TrailingData);
    for (size_t size = 0; size < buffer.size(); ++size) {
        CheckError(std::span<const std::byte>(buffer).first(size),
            size < kHeaderSize ? ParseError::TooShort : ParseError::Malformed);
        std::vector<std::byte> small(size);
        CHECK(Serialize(kHeader, message, small) == 0);
    }
    std::vector<std::byte> exact(buffer.size());
    CHECK(Serialize(kHeader, message, exact) == buffer.size());
    CHECK(exact == buffer);
}

TEST_CASE("バイト列の読み書き") {
    std::array<std::byte, 32> buffer{};
    ByteWriter writer(buffer);
    writer.WriteU8(0xab);
    writer.WriteU16(0x1234);
    writer.WriteU32(0x89abcdef);
    writer.WriteU64(0x0123456789abcdefULL);
    writer.WriteF32(1.0f);
    writer.WriteString("A\x80", 2);
    CHECK(writer.Ok());
    CHECK(writer.Size() == 22);
    const std::array<uint8_t, 22> expected{
        0xab, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89,
        0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
        0x00, 0x00, 0x80, 0x3f, 0x02, 0x41, 0x80};
    for (size_t index = 0; index < expected.size(); ++index) {
        CHECK(std::to_integer<uint8_t>(buffer[index]) == expected[index]);
    }
    ByteReader reader(std::span<const std::byte>(buffer).first(writer.Size()));
    CHECK(reader.ReadU8() == 0xab);
    CHECK(reader.ReadU16() == 0x1234);
    CHECK(reader.ReadU32() == 0x89abcdef);
    CHECK(reader.ReadU64() == 0x0123456789abcdefULL);
    CHECK(reader.ReadF32() == 1.0f);
    CHECK(reader.ReadString(2) == "A\x80");
    CHECK(reader.Ok());
    CHECK(reader.Remaining() == 0);
    CHECK(reader.ReadU64() == 0);
    CHECK(!reader.Ok());
    CHECK(reader.ReadF32() == 0);
    CHECK(reader.ReadString(255).empty());

    std::array<std::byte, 1> small{std::byte{0x55}};
    ByteWriter overflow(small);
    overflow.WriteU16(1);
    overflow.WriteU8(2);
    CHECK(!overflow.Ok());
    CHECK(overflow.Size() == 0);
    CHECK(small[0] == std::byte{0x55});
    ByteReader underflow(small);
    CHECK(underflow.ReadU32() == 0);
    CHECK(underflow.ReadU8() == 0);
    CHECK(!underflow.Ok());
    CHECK(underflow.Remaining() == 1);

    for (const auto limit : {size_t{0}, size_t{256}, std::numeric_limits<size_t>::max()}) {
        ByteWriter invalid(buffer);
        invalid.WriteString("a", limit);
        CHECK(!invalid.Ok());
        CHECK(invalid.Size() == 0);
    }
    std::array<std::byte, 256> stringBuffer{};
    ByteWriter stringWriter(stringBuffer);
    stringWriter.WriteString(std::string(255, 'x'), 255);
    CHECK(stringWriter.Ok());
    CHECK(stringWriter.Size() == 256);
    ByteReader stringReader(stringBuffer);
    CHECK(stringReader.ReadString(255) == std::string(255, 'x'));
    CHECK(stringReader.Ok());
    ByteReader longString(stringBuffer);
    CHECK(longString.ReadString(254).empty());
    CHECK(!longString.Ok());
    ByteReader shortString(std::span<const std::byte>(stringBuffer).first(255));
    CHECK(shortString.ReadString(255).empty());
    CHECK(!shortString.Ok());
    ByteWriter emptyWriter(buffer);
    emptyWriter.WriteString("", 0);
    ByteReader emptyReader(std::span<const std::byte>(buffer).first(emptyWriter.Size()));
    CHECK(emptyReader.ReadString(0).empty());
    CHECK(emptyReader.Ok());
}

PoseMessage MakePose() {
    PoseMessage value;
    value.sampleTimeUs = 0xfedcba9876543210ULL;
    value.head = {true, {{1, -2, 3}, {0.1f, -0.2f, 0.3f, -0.4f}, {4, -5, 6}, {-7, 8, -9}}};
    value.leftHand = {false, {{10, 11, -12}, {-0.5f, 0.6f, -0.7f, 0.8f}, {-13, 14, 15}, {16, -17, 18}}};
    value.rightHand = {true, {{-19, 20, 21}, {0.9f, -1, 1.1f, -1.2f}, {22, 23, -24}, {25, 26, -27}}};
    return value;
}

TEST_CASE("映像の断片") {
    static_assert(kVideoFragmentHeaderSize == 53);
    static_assert(kMaxVideoPayloadSize == 1327);
    VideoFragmentMessage fragment;
    fragment.frameIndex = 0xfedcba98;
    fragment.fragmentIndex = 2;
    fragment.fragmentCount = 3;
    fragment.frameSize = 3981;
    fragment.flags = 1;
    fragment.videoWidth = 4128;
    fragment.videoHeight = 2208;
    fragment.pose = {0xfedcba9876543210ULL, {1, -2, 3}, {0.1f, -0.2f, 0.3f, -0.4f}};
    for (const size_t payloadSize : {size_t{1}, kMaxVideoPayloadSize}) {
        fragment.payload.resize(payloadSize);
        for (size_t index = 0; index < payloadSize; ++index) {
            fragment.payload[index] = static_cast<uint8_t>(index);
        }
        const auto buffer = Encode(fragment);
        CHECK(buffer.size() == kHeaderSize + kVideoFragmentHeaderSize + payloadSize);
        CHECK(buffer[kHeaderSize + 13] == std::byte{0x20});
        CHECK(buffer[kHeaderSize + 14] == std::byte{0x10});
        CHECK(buffer[kHeaderSize + 15] == std::byte{0xa0});
        CHECK(buffer[kHeaderSize + 16] == std::byte{0x08});
        CHECK(TypeOf(fragment) == MessageType::VideoFragment);
        const auto parsed = Parse(buffer);
        CHECK(parsed.error == ParseError::None);
        CHECK(parsed.packet.has_value());
        if (parsed.packet) {
            CHECK(parsed.packet->header.type == MessageType::VideoFragment);
            CHECK(parsed.packet->header.sequence == kHeader.sequence);
            CHECK(parsed.packet->header.timestampUs == kHeader.timestampUs);
            const auto* actual = std::get_if<VideoFragmentMessage>(&parsed.packet->message);
            CHECK(actual != nullptr);
            if (actual) {
                CHECK(actual->frameIndex == fragment.frameIndex);
                CHECK(actual->fragmentIndex == fragment.fragmentIndex);
                CHECK(actual->fragmentCount == fragment.fragmentCount);
                CHECK(actual->frameSize == fragment.frameSize);
                CHECK(actual->flags == fragment.flags);
                CHECK(actual->videoWidth == fragment.videoWidth);
                CHECK(actual->videoHeight == fragment.videoHeight);
                CHECK(actual->pose.sampleTimeUs == fragment.pose.sampleTimeUs);
                CheckEqual(actual->pose.position, fragment.pose.position);
                CHECK(actual->pose.orientation.x == fragment.pose.orientation.x);
                CHECK(actual->pose.orientation.y == fragment.pose.orientation.y);
                CHECK(actual->pose.orientation.z == fragment.pose.orientation.z);
                CHECK(actual->pose.orientation.w == fragment.pose.orientation.w);
                CHECK(actual->payload == fragment.payload);
            }
        }
        for (size_t size = 0; size <= kHeaderSize + kVideoFragmentHeaderSize; ++size) {
            CheckError(std::span<const std::byte>(buffer).first(size),
                size < kHeaderSize ? ParseError::TooShort : ParseError::Malformed);
        }
        for (size_t size = 0; size < buffer.size(); ++size) {
            std::vector<std::byte> small(size);
            CHECK(Serialize(kHeader, fragment, small) == 0);
        }
        auto extra = buffer;
        extra.push_back(std::byte{42});
        // 末尾は常に payload であり、TrailingData にはならない。
        CHECK(Parse(extra).error == (payloadSize == kMaxVideoPayloadSize ? ParseError::Malformed : ParseError::None));
    }
    fragment.payload.resize(kMaxVideoPayloadSize + 1);
    std::array<std::byte, kMaxPacketSize + 1> oversized{};
    CHECK(Serialize(kHeader, fragment, oversized) == 0);
    fragment.payload = {1, 2};
    auto invalid = fragment;
    invalid.fragmentCount = 0;
    CheckError(Encode(invalid), ParseError::Malformed);
    invalid = fragment;
    invalid.fragmentIndex = invalid.fragmentCount;
    CheckError(Encode(invalid), ParseError::Malformed);
    invalid = fragment;
    invalid.fragmentIndex = static_cast<uint16_t>(invalid.fragmentCount + 1);
    CheckError(Encode(invalid), ParseError::Malformed);
    invalid = fragment;
    invalid.payload.clear();
    CheckError(Encode(invalid), ParseError::Malformed);
    invalid = fragment;
    invalid.frameSize = 1;
    CheckError(Encode(invalid), ParseError::Malformed);
    invalid = fragment;
    invalid.videoWidth = 0;
    CheckError(Encode(invalid), ParseError::Malformed);
    invalid = fragment;
    invalid.videoHeight = 0;
    CheckError(Encode(invalid), ParseError::Malformed);
}

TEST_CASE("中心を細かく送る写像（foveation）") {
    const Fov left{-1.376f, 0.839f, -0.966f, 1.428f};
    const Fov right{-0.839f, 1.376f, -0.966f, 1.428f};
    const auto f = MakeFoveation(left, right, 0.45f, 0.40f, 4.0f, 5.0f, 0.0f);
    CHECK(f.enabled == 1);
    CHECK(std::abs((f.left.centerX0 + f.left.centerX1) * 0.5f - 0.621f) < 0.001f);
    CHECK(std::abs((f.right.centerX0 + f.right.centerX1) * 0.5f - 0.379f) < 0.001f);
    CHECK(std::abs((f.left.centerY0 + f.left.centerY1) * 0.5f - 0.597f) < 0.001f);
    CHECK(f.left.centerY0 == f.right.centerY0);
    const float wnX = FoveationCompressedRatio(f.left.centerX0, f.left.centerX1, f.edgeRatioX);
    const float wnY = FoveationCompressedRatio(f.left.centerY0, f.left.centerY1, f.edgeRatioY);
    CHECK(std::abs(wnX - 0.5875f) < 0.00001f);
    CHECK(std::abs(wnY - 0.52f) < 0.00001f);
    CHECK(std::abs(f.left.centerY0 - 0.3965f) < 0.001f);
    CHECK(std::abs(f.left.centerY1 - 0.7965f) < 0.001f);
    const auto extended = MakeFoveation(left, right, 0.45f, 0.40f, 4.0f, 5.0f, 0.1f);
    for (const auto& eye : {extended.left, extended.right}) {
        CHECK(std::abs(eye.centerY0 - 0.2965f) < 0.001f);
        CHECK(std::abs(eye.centerY1 - 0.7965f) < 0.001f);
        CHECK(eye.centerY1 == f.left.centerY1);
        CHECK(std::abs(FoveationCompressedRatio(eye.centerY0, eye.centerY1, extended.edgeRatioY)
            - 0.60f) < 0.00001f);
    }
    CHECK(extended.left.centerX0 == f.left.centerX0);
    CHECK(extended.left.centerX1 == f.left.centerX1);
    CHECK(extended.right.centerX0 == f.right.centerX0);
    CHECK(extended.right.centerX1 == f.right.centerX1);
    CheckRoundTrip(HelloAckMessage{HelloResult::Accepted, kProtocolVersion, left, right, 0.064f, extended},
        MessageType::HelloAck, 104);
    const auto align = [](float value) {
        return (static_cast<uint32_t>(std::round(value)) + 31u) & ~31u;
    };
    CHECK(align(2064.0f * wnX) == 1216);
    CHECK(align(2208.0f * wnY) == 1152);
    const auto full = MakeFoveation(left, right, 1, 1, 1, 1, 0);
    CHECK(full.left == FoveationEye{});
    CHECK(full.right == FoveationEye{});
    const auto clamped = MakeFoveation({-0.01f, 1, -1, 0.01f}, {-1, 0.01f, -0.01f, 1},
        0.45f, 0.4f, 4, 5, 0);
    CHECK(clamped.left.centerX0 == 0);
    CHECK(clamped.left.centerY0 == 0);
    CHECK(clamped.right.centerX1 == 1);
    CHECK(clamped.right.centerY1 == 1);
    const auto extendedClamped = MakeFoveation({-0.01f, 1, -1, 0.01f}, {-1, 0.01f, -0.01f, 1},
        0.45f, 0.4f, 4, 5, 0.1f);
    CHECK(extendedClamped.left.centerY0 == 0);
    CHECK(std::abs(extendedClamped.left.centerY1 - 0.5f) < 0.00001f);
    CHECK(std::abs(extendedClamped.right.centerY0 - 0.5f) < 0.00001f);
    CHECK(extendedClamped.right.centerY1 == 1);
    const auto saturated = MakeFoveation(left, right, 0.45f, 0.8f, 4, 5, 0.5f);
    CHECK(saturated.left.centerY0 == 0);
    CHECK(saturated.left.centerY1 == 1);
    CHECK(saturated.right.centerY0 == 0);
    CHECK(saturated.right.centerY1 == 1);
    HelloAckMessage ack{HelloResult::Accepted, kProtocolVersion, left, right, 0.064f, f, 2064, 2208};
    CheckRoundTrip(ack, MessageType::HelloAck, 104);
    const auto bytes = Encode(ack);
    CHECK(bytes[59] == std::byte{1});
    const std::array<float, 10> expected{f.left.centerX0, f.left.centerX1, f.left.centerY0, f.left.centerY1,
        f.right.centerX0, f.right.centerX1, f.right.centerY0, f.right.centerY1, f.edgeRatioX, f.edgeRatioY};
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto bits = std::bit_cast<uint32_t>(expected[index]);
        for (size_t byte = 0; byte < 4; ++byte) {
            CHECK(std::to_integer<uint8_t>(bytes[60 + index * 4 + byte]) == ((bits >> (byte * 8)) & 0xff));
        }
    }
    auto invalid = bytes;
    invalid[59] = std::byte{2};
    CheckError(invalid, ParseError::Malformed);
    // 無効時も検査し、後で有効にしたときに不正値が入り込まないことを確かめる。
    for (uint8_t enabled : {uint8_t{0}, uint8_t{1}}) {
        ack.foveation = f;
        ack.foveation.enabled = enabled;
        const auto valid = ack.foveation;
        for (size_t field = 0; field < 10; ++field) {
            for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                    -0.1f, field < 8 ? 1.1f : 16.1f}) {
                ack.foveation = valid;
                const std::array fields{&ack.foveation.left.centerX0, &ack.foveation.left.centerX1,
                    &ack.foveation.left.centerY0, &ack.foveation.left.centerY1,
                    &ack.foveation.right.centerX0, &ack.foveation.right.centerX1,
                    &ack.foveation.right.centerY0, &ack.foveation.right.centerY1,
                    &ack.foveation.edgeRatioX, &ack.foveation.edgeRatioY};
                *fields[field] = bad;
                CheckError(Encode(ack), ParseError::Malformed);
            }
        }
        for (bool rightEye : {false, true}) {
            for (bool vertical : {false, true}) {
                ack.foveation = valid;
                auto& eye = rightEye ? ack.foveation.right : ack.foveation.left;
                float& c0 = vertical ? eye.centerY0 : eye.centerX0;
                float& c1 = vertical ? eye.centerY1 : eye.centerX1;
                c0 = c1;
                CheckError(Encode(ack), ParseError::Malformed);
                c0 = c1 + 0.01f;
                CheckError(Encode(ack), ParseError::Malformed);
            }
        }
        for (bool vertical : {false, true}) {
            ack.foveation = valid;
            float& ratio = vertical ? ack.foveation.edgeRatioY : ack.foveation.edgeRatioX;
            ratio = 0.99f;
            CheckError(Encode(ack), ParseError::Malformed);
            ratio = 16;
            CheckRoundTrip(ack, MessageType::HelloAck, 104);
        }
    }
}

TEST_CASE("ペアリングと Hello の検査") {
    const auto starting = Encode(HelloAckMessage{HelloResult::Starting});
    CHECK(starting[kHeaderSize] == std::byte{5});
    const auto stopped = Encode(HelloAckMessage{HelloResult::Stopped});
    CHECK(stopped[kHeaderSize] == std::byte{6});
    static_assert(kProtocolVersion == 6);
    HelloMessage hello;
    hello.deviceName = "Quest";
    hello.deviceId = std::string(64, 'a');
    CheckRoundTrip(hello, MessageType::Hello, kHelloBasePacketSize + hello.deviceName.size() + hello.deviceId.size());
    for (const uint8_t requested : {uint8_t{0}, uint8_t{1}}) {
        hello.startRequested = requested;
        CheckRoundTrip(hello, MessageType::Hello, kHelloBasePacketSize + hello.deviceName.size() + hello.deviceId.size());
    }
    hello.startRequested = 2;
    CheckError(Encode(hello), ParseError::Malformed);
    hello.startRequested = 0;
    const auto valid = Encode(hello);
    const size_t idOffset = kHeaderSize + 1 + hello.deviceName.size();
    CHECK(valid[idOffset] == std::byte{64});
    CHECK(valid[idOffset + 1] == std::byte{'a'});
    for (const auto& id : {std::string{}, std::string("bad.id"), std::string("bad id"),
            std::string("日本語"), std::string("a\0b", 3)}) {
        hello.deviceId = id;
        CheckError(Encode(hello), ParseError::Malformed);
    }
    auto tooLong = valid;
    tooLong[idOffset] = std::byte{65};
    tooLong.insert(tooLong.begin() + static_cast<std::ptrdiff_t>(idOffset + 1), std::byte{'a'});
    CheckError(tooLong, ParseError::Malformed);
    for (const auto result : {uint8_t{7}, uint8_t{255}}) {
        auto invalid = Encode(HelloAckMessage{});
        invalid[kHeaderSize] = static_cast<std::byte>(result);
        CheckError(invalid, ParseError::Malformed);
    }
}

TEST_CASE("各メッセージの往復と不正なパケット") {
    AudioMessage audio{0x8877665544332211ULL, 48000, 2, {-32768, 32767, -1, 0}};
    CheckRoundTrip(audio, MessageType::Audio, kHeaderSize + 15 + 8);
    auto audioBytes = Encode(audio);
    CHECK(audioBytes[6] == std::byte{13});
    CHECK(audioBytes[kHeaderSize] == std::byte{0x11});
    CHECK(audioBytes[kHeaderSize + 8] == std::byte{0x80});
    CHECK(audioBytes[kHeaderSize + 9] == std::byte{0xbb});
    CHECK(audioBytes[kHeaderSize + 13] == std::byte{2});
    CHECK(audioBytes[kHeaderSize + 15] == std::byte{0});
    CHECK(audioBytes[kHeaderSize + 16] == std::byte{0x80});
    for (const uint16_t frames : {uint16_t{0}, uint16_t{321}}) {
        auto invalid = audioBytes;
        invalid[kHeaderSize + 13] = static_cast<std::byte>(frames & 0xff);
        invalid[kHeaderSize + 14] = static_cast<std::byte>(frames >> 8);
        CheckError(invalid, ParseError::Malformed);
    }
    auto invalidAudio = audioBytes;
    invalidAudio[kHeaderSize + 12] = std::byte{1};
    CheckError(invalidAudio, ParseError::Malformed);
    std::array<std::byte, kMaxPacketSize> audioBuffer{};
    for (const uint32_t rate : {7999u, 192001u}) {
        audio.sampleRate = rate;
        CHECK(Serialize(kHeader, audio, audioBuffer) == 0);
        auto invalid = audioBytes;
        for (size_t index = 0; index < 4; ++index) {
            invalid[kHeaderSize + 8 + index] = static_cast<std::byte>((rate >> (index * 8)) & 0xff);
        }
        CheckError(invalid, ParseError::Malformed);
    }
    for (const uint32_t rate : {8000u, 48000u, 192000u}) {
        audio.sampleRate = rate;
        audio.samples.resize(kMaxAudioFrames * 2);
        CheckRoundTrip(audio, MessageType::Audio, kHeaderSize + 15 + kMaxAudioFrames * 4);
    }
    for (const size_t samples : {size_t{0}, size_t{3}, (kMaxAudioFrames + 1) * 2}) {
        audio.samples.resize(samples);
        CHECK(Serialize(kHeader, audio, audioBuffer) == 0);
    }
    audio.samples = {0, 0};
    for (const uint8_t channels : {uint8_t{0}, uint8_t{1}, uint8_t{3}}) {
        audio.channelCount = channels;
        CHECK(Serialize(kHeader, audio, audioBuffer) == 0);
    }
    const InputMessage input{0xfedcba9876543210ULL,
        {true, kButtonPrimaryClick | kButtonMenuClick, 0.25f, 0.75f, -1.0f, 1.0f},
        {false, kButtonSecondaryTouch | kButtonThumbrestTouch, 1.0f, 0.5f, 0.125f, -0.5f}};
    CheckRoundTrip(input, MessageType::Input, kHeaderSize + 50);
    CheckRoundTrip(InputMessage{}, MessageType::Input, kHeaderSize + 50);
    const auto inputBytes = Encode(input);
    CHECK(inputBytes[6] == std::byte{12});
    CHECK(inputBytes[kHeaderSize + 8] == std::byte{1});
    CHECK(inputBytes[kHeaderSize + 9] == std::byte{1});
    CHECK(inputBytes[kHeaderSize + 10] == std::byte{1});
    CHECK(inputBytes[kHeaderSize + 29] == std::byte{0});
    for (const size_t offset : {size_t{8}, size_t{29}}) {
        auto invalid = inputBytes;
        invalid[kHeaderSize + offset] = std::byte{2};
        CheckError(invalid, ParseError::Malformed);
    }
    CheckRoundTrip(DisplayFeedbackMessage{1.25f, 0x1234, 0x5678, 13889}, MessageType::DisplayFeedback, kHeaderSize + 12);
    const auto feedbackBytes = Encode(DisplayFeedbackMessage{1.25f, 0x1234, 0x5678, 0x12345678});
    constexpr std::array<uint8_t, 12> kFeedbackBody{
        0x00, 0x00, 0xa0, 0x3f, 0x34, 0x12, 0x78, 0x56, 0x78, 0x56, 0x34, 0x12};
    CHECK(feedbackBytes[6] == std::byte{11});
    for (size_t index = 0; index < kFeedbackBody.size(); ++index) {
        CHECK(std::to_integer<uint8_t>(feedbackBytes[kHeaderSize + index]) == kFeedbackBody[index]);
    }
    HelloMessage hello{"Quest 3 \xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88", "Quest-3_0123", 2064, 2208, 72.0f, 0.064f,
        {-1.1f, 1.2f, -1.3f, 1.4f}, {-1.5f, 1.6f, -1.7f, 1.8f}, 0xfedcba98};
    CheckRoundTrip(hello, MessageType::Hello, kHelloBasePacketSize + hello.deviceName.size() + hello.deviceId.size());
    const auto helloBytes = Encode(hello);
    constexpr std::array<std::byte, 4> kSessionBytes{
        std::byte{0x98}, std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe}};
    for (size_t index = 0; index < kSessionBytes.size(); ++index) {
        CHECK(helloBytes[helloBytes.size() - sizeof(hello.startRequested) - kSessionBytes.size() + index] == kSessionBytes[index]);
    }
    hello.deviceName = std::string(64, 'x');
    CheckRoundTrip(hello, MessageType::Hello, kHelloBasePacketSize + kMaxDeviceNameLength + hello.deviceId.size());
    hello.deviceName.clear();
    CheckRoundTrip(hello, MessageType::Hello, kHelloBasePacketSize + hello.deviceId.size());
    for (uint8_t result = 0; result <= 6; ++result) {
        CheckRoundTrip(HelloAckMessage{static_cast<HelloResult>(result), 0x1234,
            {-1.1f, 0.8f, -0.9f, 1.4f}, {-0.8f, 1.1f, -1.2f, 1.3f}, 0.064f},
            MessageType::HelloAck, 104);
    }
    // 往復だけでは送受信が同じ順序で間違っていても通るため、バイト列の配置も確認する。
    const auto ackBytes = Encode(HelloAckMessage{HelloResult::Busy, 0x1234,
        {-1.1f, 0.8f, -0.9f, 1.4f}, {-0.8f, 1.1f, -1.2f, 1.3f}, 0.064f});
    CHECK(ackBytes[kHeaderSize] == std::byte{3});
    CHECK(ackBytes[kHeaderSize + 1] == std::byte{0x34});
    CHECK(ackBytes[kHeaderSize + 2] == std::byte{0x12});
    constexpr std::array<float, 9> kAckFloats{-1.1f, 0.8f, -0.9f, 1.4f,
        -0.8f, 1.1f, -1.2f, 1.3f, 0.064f};
    for (size_t index = 0; index < kAckFloats.size(); ++index) {
        const auto bits = std::bit_cast<uint32_t>(kAckFloats[index]);
        for (size_t byte = 0; byte < 4; ++byte) {
            CHECK(std::to_integer<uint8_t>(ackBytes[kHeaderSize + 3 + index * 4 + byte])
                == ((bits >> (byte * 8)) & 0xff));
        }
    }
    for (uint8_t reason = 0; reason <= 3; ++reason) {
        CheckRoundTrip(KeyFrameRequestMessage{0xfedcba98, static_cast<KeyFrameReason>(reason)},
            MessageType::KeyFrameRequest, kHeaderSize + 5);
    }
    auto invalidRequest = Encode(KeyFrameRequestMessage{});
    invalidRequest[kHeaderSize + 4] = std::byte{4};
    CheckError(invalidRequest, ParseError::Malformed);
    CheckRoundTrip(MakePose(), MessageType::Pose, 187);
    CheckRoundTrip(PingMessage{0x123456789abcdef0ULL}, MessageType::Ping, 28);
    CheckRoundTrip(PongMessage{0xfedcba9876543210ULL, 0x1020304050607080ULL}, MessageType::Pong, kPongPacketSize);
    CheckRoundTrip(ByeMessage{}, MessageType::Bye, 20);
    for (const auto& name : {std::string{}, std::string("PC \xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"), std::string(64, 'x')}) {
        CheckRoundTrip(DiscoverMessage{name}, MessageType::Discover, 21 + name.size());
        CheckRoundTrip(DiscoverReplyMessage{name, 12345}, MessageType::DiscoverReply, 23 + name.size());
    }
    std::array<std::byte, kMaxPacketSize> discoveryBuffer{};
    CHECK(Serialize(kHeader, DiscoverMessage{std::string(65, 'x')}, discoveryBuffer) == 0);
    CHECK(Serialize(kHeader, DiscoverReplyMessage{std::string(65, 'x')}, discoveryBuffer) == 0);
    for (const Message& message : {Message{DiscoverMessage{std::string(64, 'x')}},
            Message{DiscoverReplyMessage{std::string(64, 'x')}}}) {
        auto invalidName = Encode(message);
        invalidName[kHeaderSize] = std::byte{65};
        CheckError(invalidName, ParseError::Malformed);
    }

    const ClientStatsMessage stats{0x12345600, 0x1201, 0x1202, 0x12345603, 0x12345604, 0x1205,
        0x12345606, 0x12345607, 0x1208, 0x1209, 0x1234560a, 0x1234560b, 0x1234560c, 0x120d,
        0x1234560e, 0x1234560f, 0x12345610, 0x12345611, 0x12345612, 0x12345613};
    CheckRoundTrip(stats, MessageType::ClientStats, kHeaderSize + 69);
    auto statsBytes = Encode(stats);
    CHECK(statsBytes[6] == std::byte{14});
    size_t statsOffset = kHeaderSize;
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.periodUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 2; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.receivedFrames >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 2; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.lostFrames >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.receivedFragments >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.receivedVideoBytes >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 2; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.decodedFrames >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.decodeAverageUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.decodeMaxUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 2; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.displayedFrames >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 2; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.repeatedFrames >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.latencyAverageUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.latencyMaxUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.roundTripUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 2; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.audioUnderruns >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.audioMissingFrames >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.audioBufferAverageUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.firstFragmentAverageUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.assembleAverageUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.decoderWaitAverageUs >> (byte * 8)) & 0xff));
    }
    for (size_t byte = 0; byte < 4; ++byte) {
        CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == ((stats.displayWaitAverageUs >> (byte * 8)) & 0xff));
    }
    CHECK(std::to_integer<uint8_t>(statsBytes[statsOffset++]) == stats.micState);
    CHECK(statsOffset == statsBytes.size());
    CheckError(std::span<const std::byte>(statsBytes).first(statsBytes.size() - 1), ParseError::Malformed);
    statsBytes.push_back(std::byte{0});
    CheckError(statsBytes, ParseError::TrailingData);

    const auto bye = Encode(ByeMessage{});
    const std::array<uint8_t, 20> expected{
        'W', 'N', 'V', 'R', static_cast<uint8_t>(kProtocolVersion & 0xff), static_cast<uint8_t>(kProtocolVersion >> 8), 6, 0, 0x12, 0x34, 0x56, 0x78,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    for (size_t index = 0; index < expected.size(); ++index) {
        CHECK(std::to_integer<uint8_t>(bye[index]) == expected[index]);
    }
    auto corrupt = bye;
    corrupt[0] ^= std::byte{1};
    CheckError(corrupt, ParseError::BadMagic);
    CHECK(Parse(corrupt).receivedVersion == kProtocolVersion);
    corrupt = bye;
    corrupt[4] = std::byte{1};
    CheckError(corrupt, ParseError::VersionMismatch);
    CHECK(Parse(corrupt).receivedVersion == 1);
    CHECK(Parse(std::span<const std::byte>(corrupt).first(6)).receivedVersion == 1);
    corrupt = bye;
    constexpr uint8_t kPreviousProtocolVersion = 5;
    corrupt[4] = static_cast<std::byte>(kPreviousProtocolVersion);
    CheckError(corrupt, ParseError::VersionMismatch);
    CHECK(Parse(corrupt).receivedVersion == kPreviousProtocolVersion);
    for (const auto type : {std::byte{0}, std::byte{16}, std::byte{200}}) {
        corrupt = bye;
        corrupt[6] = type;
        CheckError(corrupt, ParseError::UnknownType);
    }
    corrupt = Encode(HelloAckMessage{});
    corrupt[kHeaderSize] = std::byte{7};
    CheckError(corrupt, ParseError::Malformed);
    corrupt = Encode(MakePose());
    corrupt[kHeaderSize + 8] = std::byte{2};
    CheckError(corrupt, ParseError::Malformed);
    hello.deviceName = std::string(64, 'x');
    corrupt = Encode(hello);
    corrupt[kHeaderSize] = std::byte{65};
    CheckError(corrupt, ParseError::Malformed);
    hello.deviceName.push_back('x');
    std::array<std::byte, kMaxPacketSize> buffer{};
    CHECK(Serialize(kHeader, hello, buffer) == 0);

    // 数値の妥当性は受信側が判断するため、特殊な浮動小数点値もビットを保つ。
    hello.deviceName.clear();
    hello.refreshRate = std::bit_cast<float>(uint32_t{0x7fc12345});
    hello.ipdMeters = -0.0f;
    const auto special = Parse(Encode(hello));
    CHECK(special.error == ParseError::None);
    if (special.packet) {
        const auto& actual = std::get<HelloMessage>(special.packet->message);
        CHECK(std::bit_cast<uint32_t>(actual.refreshRate) == 0x7fc12345);
        CHECK(std::bit_cast<uint32_t>(actual.ipdMeters) == 0x80000000);
    }
    constexpr std::array errors{ParseError::None, ParseError::TooShort, ParseError::BadMagic,
        ParseError::VersionMismatch, ParseError::UnknownType, ParseError::Malformed, ParseError::TrailingData};
    constexpr std::array<std::string_view, 7> names{
        "None", "TooShort", "BadMagic", "VersionMismatch", "UnknownType", "Malformed", "TrailingData"};
    for (size_t index = 0; index < errors.size(); ++index) {
        CHECK(ToString(errors[index]) == names[index]);
    }
    CHECK(std::string_view(ToString(static_cast<ParseError>(255))) == "Unknown");
}

TEST_CASE("バイト列をまとめて読み書きする") {
    const std::array<uint8_t, 5> source{1, 2, 3, 4, 5};
    std::array<std::byte, 6> buffer{};
    ByteWriter writer(buffer);
    writer.WriteU8(0xaa);
    writer.WriteBytes(source);
    CHECK(writer.Ok());
    CHECK(writer.Size() == 6);
    // ちょうど埋まったあとは、空の書き込みだけが成功する。
    writer.WriteBytes({});
    CHECK(writer.Ok());
    writer.WriteBytes(std::span(source).first(1));
    CHECK(!writer.Ok());
    CHECK(writer.Size() == 6);

    ByteReader reader(buffer);
    CHECK(reader.ReadU8() == 0xaa);
    std::array<uint8_t, 5> copied{};
    reader.ReadBytes(copied);
    CHECK(reader.Ok());
    CHECK(copied == source);
    CHECK(reader.Remaining() == 0);
    // 足りないときは出力に触れずに失敗する。
    std::array<uint8_t, 2> untouched{7, 7};
    reader.ReadBytes(untouched);
    CHECK(!reader.Ok());
    CHECK(untouched == std::array<uint8_t, 2>{7, 7});
}

TEST_CASE("メッセージの型の番号は variant の並び順と一致する") {
    const std::array<Message, 15> messages{HelloMessage{}, HelloAckMessage{}, PoseMessage{}, PingMessage{},
        PongMessage{}, ByeMessage{}, DiscoverMessage{}, DiscoverReplyMessage{}, VideoFragmentMessage{},
        KeyFrameRequestMessage{}, DisplayFeedbackMessage{}, InputMessage{}, AudioMessage{}, ClientStatsMessage{}, MicAudioMessage{}};
    for (size_t index = 0; index < messages.size(); ++index) {
        CHECK(static_cast<size_t>(TypeOf(messages[index])) == index + 1);
    }
    // 未知の型は本文を読む前に落とす。
    auto unknown = Encode(PingMessage{1});
    for (const auto type : {uint8_t{0}, uint8_t{16}, uint8_t{255}}) {
        unknown[6] = static_cast<std::byte>(type);
        CheckError(unknown, ParseError::UnknownType);
    }
}

TEST_CASE("端末の ID に使える文字と長さ") {
    CHECK(IsValidDeviceId("AZaz09-_"));
    CHECK(IsValidDeviceId(std::string(kMaxDeviceIdLength, 'a')));
    CHECK(!IsValidDeviceId(std::string(kMaxDeviceIdLength + 1, 'a')));
    for (const auto& id : {std::string{}, std::string("a b"), std::string("a.b"), std::string("日本語"),
            std::string("a\0b", 3)}) {
        CHECK(!IsValidDeviceId(id));
    }
}

}

TEST_CASE("Quest の要求描画サイズは両方未指定か範囲内だけを受け付ける") {
    const std::array<std::array<uint16_t, 2>, 4> validSizes{{{0, 0}, {256, 256}, {8192, 8192}, {2064, 2208}}};
    for (const auto& size : validSizes) {
        HelloAckMessage ack;
        ack.questEyeWidth = size[0];
        ack.questEyeHeight = size[1];
        CheckRoundTrip(ack, MessageType::HelloAck, 104);
        for (const uint8_t pending : {uint8_t{0}, uint8_t{1}}) {
            CheckRoundTrip(PongMessage{1, 2, 0, size[0], size[1], pending},
                MessageType::Pong, kPongPacketSize);
        }
    }
    const std::array<std::array<uint16_t, 2>, 6> invalidSizes{{{0, 2208}, {2064, 0}, {255, 2208},
        {2064, 255}, {8193, 2208}, {2064, 8193}}};
    for (const auto& size : invalidSizes) {
        HelloAckMessage ack;
        ack.questEyeWidth = size[0];
        ack.questEyeHeight = size[1];
        CheckError(Encode(ack), ParseError::Malformed);
        CheckError(Encode(PongMessage{1, 2, 0, size[0], size[1], 0}), ParseError::Malformed);
    }
    CheckError(Encode(PongMessage{1, 2, 0, 2064, 2208, 2}), ParseError::Malformed);
}

}

namespace wnvr::protocol {

TEST_CASE("マイク音声の往復と確保前の範囲検査") {
    MicAudioMessage mic{0x8877665544332211ULL, 48000, {-32768, -1, 0, 32767}};
    CheckRoundTrip(mic, MessageType::MicAudio, kHeaderSize + 14 + 8);
    const auto valid = Encode(mic);
    CHECK(valid[6] == std::byte{15});
    for (const uint32_t rate : {8000u, 192000u}) {
        mic.sampleRate = rate;
        mic.samples.resize(kMaxAudioFrames);
        CheckRoundTrip(mic, MessageType::MicAudio, kHeaderSize + 14 + kMaxAudioFrames * 2);
    }
    for (const uint32_t rate : {7999u, 192001u}) {
        auto invalid = valid;
        for (size_t index = 0; index < sizeof(rate); ++index) {
            invalid[kHeaderSize + 8 + index] = static_cast<std::byte>((rate >> (index * 8)) & 0xff);
        }
        CheckError(invalid, ParseError::Malformed);
    }
    for (const uint16_t frames : {uint16_t{0}, static_cast<uint16_t>(kMaxAudioFrames + 1)}) {
        auto invalid = valid;
        invalid[kHeaderSize + 12] = static_cast<std::byte>(frames & 0xff);
        invalid[kHeaderSize + 13] = static_cast<std::byte>(frames >> 8);
        CheckError(invalid, ParseError::Malformed);
    }
    std::array<std::byte, kMaxPacketSize> buffer{};
    mic.sampleRate = 7999;
    CHECK(Serialize(kHeader, mic, buffer) == 0);
    mic.sampleRate = 48000;
    mic.samples.clear();
    CHECK(Serialize(kHeader, mic, buffer) == 0);
    mic.samples.resize(kMaxAudioFrames + 1);
    CHECK(Serialize(kHeader, mic, buffer) == 0);
}

TEST_CASE("Pong のマイク要求と Quest のマイク状態の範囲") {
    for (uint8_t requested = 0; requested <= 1; ++requested) {
        CheckRoundTrip(PongMessage{1, 2, requested}, MessageType::Pong, kPongPacketSize);
    }
    auto pong = Encode(PongMessage{});
    constexpr size_t kPongMicRequestedOffset = kHeaderSize + 16;
    pong[kPongMicRequestedOffset] = std::byte{2};
    CheckError(pong, ParseError::Malformed);
    for (uint8_t state = 0; state <= 3; ++state) {
        ClientStatsMessage stats;
        stats.micState = state;
        CheckRoundTrip(stats, MessageType::ClientStats, kHeaderSize + 69);
    }
    auto stats = Encode(ClientStatsMessage{});
    stats.back() = std::byte{4};
    CheckError(stats, ParseError::Malformed);
}

}
