#include "mic_packetizer.h"
#include <doctest/doctest.h>
#include <numeric>
#include <vector>

using namespace wnvr;

TEST_CASE("マイクを240フレームに区切り、端数を次の録音につなぐ") {
    MicPacketizer packetizer;
    std::vector<int16_t> samples(1000);
    std::iota(samples.begin(), samples.end(), int16_t{0});
    packetizer.Append(samples.data(), 101, 48000);
    CHECK(packetizer.Take().empty());
    packetizer.Append(samples.data() + 101, 500, 48000);
    auto packets = packetizer.Take();
    REQUIRE(packets.size() == 2);
    for (size_t index = 0; index < packets.size(); ++index) {
        CHECK(packets[index].frameIndex == index * kMicPacketFrames);
        CHECK(packets[index].sampleRate == 48000);
        CHECK(packets[index].samples == std::vector<int16_t>(samples.begin() + index * kMicPacketFrames,
            samples.begin() + (index + 1) * kMicPacketFrames));
    }
    packetizer.Append(samples.data() + 601, 119, 48000);
    packets = packetizer.Take();
    REQUIRE(packets.size() == 1);
    CHECK(packets[0].frameIndex == 480);
    CHECK(packets[0].samples.front() == 480);
    CHECK(packets[0].samples.back() == 719);
}

TEST_CASE("マイクの送信待ちは100msまでにして、捨てた分も番号を進める") {
    MicPacketizer packetizer;
    std::vector<int16_t> samples(6000, 7);
    packetizer.Append(samples.data(), samples.size(), 48000);
    const auto packets = packetizer.Take();
    REQUIRE(packets.size() == 20);
    CHECK(packets.front().frameIndex == 1200);
    CHECK(packets.back().frameIndex == 5760);
    CHECK(packetizer.DroppedFrames() == 1200);
    CHECK(packetizer.Take().empty());
    packetizer.Reset();
    CHECK(packetizer.DroppedFrames() == 0);
    packetizer.Append(samples.data(), kMicPacketFrames, 48000);
    REQUIRE(packetizer.Take().front().frameIndex == 0);
}

TEST_CASE("マイクのレート変更では端数と古い送信待ちを捨てる") {
    MicPacketizer packetizer;
    std::vector<int16_t> oldSamples(300, 1);
    std::vector<int16_t> newSamples(kMicPacketFrames, 2);
    packetizer.Append(oldSamples.data(), oldSamples.size(), 48000);
    packetizer.Append(newSamples.data(), newSamples.size(), 44100);
    const auto packets = packetizer.Take();
    REQUIRE(packets.size() == 1);
    CHECK(packets[0].sampleRate == 44100);
    CHECK(packets[0].frameIndex == 300);
    CHECK(packets[0].samples == newSamples);
    CHECK(packetizer.DroppedFrames() == 300);
}
