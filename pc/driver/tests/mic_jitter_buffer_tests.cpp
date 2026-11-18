#include "mic_jitter_buffer.h"
#include <doctest/doctest.h>
#include <array>
#include <vector>

using wnvr::driver::MicJitterBuffer;
using wnvr::protocol::MicAudioMessage;

namespace {
constexpr size_t kPacketFrames = 240;
constexpr uint32_t kRate = 48000;
constexpr size_t kStartFrames = 1920;
MicAudioMessage Packet(uint64_t frame, int16_t sample = 1) {
    return {frame, kRate, std::vector<int16_t>(kPacketFrames, sample)};
}
void Fill(MicJitterBuffer& buffer, uint64_t first, size_t packets) {
    for (size_t index = 0; index < packets; ++index) {
        buffer.Push(Packet(first + index * kPacketFrames, static_cast<int16_t>(index + 1)));
    }
}
}

TEST_CASE("マイクは40msためてから番号の順に取り出す") {
    MicJitterBuffer buffer;
    std::array<int16_t, kStartFrames> output{};
    Fill(buffer, 0, 7);
    CHECK(buffer.Pop(output.data(), output.size()) == 0);
    buffer.Push(Packet(1680, 8));
    CHECK(buffer.SampleRate() == kRate);
    CHECK(buffer.Pop(output.data(), output.size()) == kStartFrames);
    for (size_t index = 0; index < output.size(); ++index) {
        CHECK(output[index] == static_cast<int16_t>(index / kPacketFrames + 1));
    }
}

TEST_CASE("マイクの逆順と重複は捨て、抜けたフレーム数を数える") {
    MicJitterBuffer buffer;
    buffer.Push(Packet(0));
    buffer.Push(Packet(0));
    buffer.Push(Packet(480));
    buffer.Push(Packet(240));
    const auto stats = buffer.TakeStats();
    CHECK(stats.packets == 4);
    CHECK(stats.dropped == 480);
    CHECK(stats.missing == 240);
    CHECK(buffer.TakeStats().packets == 0);
}

TEST_CASE("マイクが120msを超えたら最新40msに戻す") {
    MicJitterBuffer buffer;
    Fill(buffer, 0, 25);
    const auto stats = buffer.TakeStats();
    CHECK(stats.dropped == 4080);
    std::array<int16_t, kStartFrames> output{};
    CHECK(buffer.Pop(output.data(), output.size()) == output.size());
    CHECK(output.front() == 18);
    CHECK(output.back() == 25);
    CHECK(buffer.Pop(output.data(), output.size()) == 0);
}

TEST_CASE("マイクが空になったら途切れを一度数え、また40ms待つ") {
    MicJitterBuffer buffer;
    Fill(buffer, 0, 8);
    std::array<int16_t, kStartFrames> output{};
    CHECK(buffer.Pop(output.data(), 100) == 100);
    CHECK(buffer.Pop(output.data(), output.size()) == kStartFrames - 100);
    CHECK(buffer.Pop(output.data(), output.size()) == 0);
    CHECK(buffer.TakeStats().gaps == 1);
    Fill(buffer, kStartFrames, 7);
    CHECK(buffer.Pop(output.data(), output.size()) == 0);
    buffer.Push(Packet(3600));
    CHECK(buffer.Pop(output.data(), output.size()) == output.size());
    CHECK(buffer.TakeStats().gaps == 1);
}

TEST_CASE("マイクの番号が1秒以上戻ったら録音の再開としてため直す") {
    MicJitterBuffer buffer;
    Fill(buffer, kRate, 8);
    buffer.Push(Packet(0, 9));
    std::array<int16_t, kStartFrames> output{};
    CHECK(buffer.Pop(output.data(), output.size()) == 0);
    Fill(buffer, kPacketFrames, 7);
    CHECK(buffer.Pop(output.data(), output.size()) == output.size());
    CHECK(output.front() == 9);
    const auto stats = buffer.TakeStats();
    CHECK(stats.missing == 0);
    CHECK(stats.dropped == kStartFrames);
}

TEST_CASE("マイクのレートが変わったら以前の音を混ぜずにため直す") {
    MicJitterBuffer buffer;
    Fill(buffer, 0, 8);
    auto packet = Packet(1920, 9);
    packet.sampleRate = 8000;
    packet.samples.resize(320);
    buffer.Push(packet);
    std::array<int16_t, 320> output{};
    CHECK(buffer.SampleRate() == 8000);
    CHECK(buffer.Pop(output.data(), output.size()) == output.size());
    CHECK(output.front() == 9);
    buffer.Reset();
    CHECK(buffer.SampleRate() == 0);
    CHECK(buffer.Pop(output.data(), output.size()) == 0);
}
