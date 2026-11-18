#include "frame_assembler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using namespace wnvr::quest;
using wnvr::protocol::VideoFragmentMessage;
using wnvr::protocol::kMaxVideoPayloadSize;

namespace {

// 中身が位置と番号で決まるデータを作り、組み立てた結果と比べられるようにする。
std::vector<uint8_t> MakeData(size_t size, uint32_t seed) {
    std::vector<uint8_t> data(size);
    for (size_t index = 0; index < size; ++index) {
        data[index] = static_cast<uint8_t>((index * 31 + seed * 7) & 0xff);
    }
    return data;
}

// PC の送信側と同じ分け方で、1 フレームを断片に分ける。
std::vector<VideoFragmentMessage> Split(uint32_t frameIndex, const std::vector<uint8_t>& data, bool keyFrame = false) {
    const size_t count = (data.size() - 1) / kMaxVideoPayloadSize + 1;
    std::vector<VideoFragmentMessage> fragments;
    for (size_t index = 0; index < count; ++index) {
        const size_t offset = index * kMaxVideoPayloadSize;
        const size_t size = std::min(kMaxVideoPayloadSize, data.size() - offset);
        VideoFragmentMessage fragment;
        fragment.frameIndex = frameIndex;
        fragment.fragmentIndex = static_cast<uint16_t>(index);
        fragment.fragmentCount = static_cast<uint16_t>(count);
        fragment.frameSize = static_cast<uint32_t>(data.size());
        fragment.flags = keyFrame ? 1 : 0;
        fragment.videoWidth = 2432;
        fragment.videoHeight = 1344;
        fragment.pose.sampleTimeUs = 1000 + frameIndex;
        fragment.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
            data.begin() + static_cast<std::ptrdiff_t>(offset + size));
        fragments.push_back(std::move(fragment));
    }
    return fragments;
}

void AddAll(FrameAssembler& assembler, const std::vector<VideoFragmentMessage>& fragments) {
    for (const auto& fragment : fragments) {
        assembler.AddFragment(fragment, 0);
    }
}

}

TEST_CASE("順番に届いた断片から、元のフレームを組み立てる") {
    FrameAssembler assembler;
    const auto data = MakeData(3000, 1);
    const auto fragments = Split(10, data, true);
    REQUIRE(fragments.size() == 3);
    assembler.AddFragment(fragments[0], 0);
    assembler.AddFragment(fragments[1], 0);
    CHECK(!assembler.PopFrame());
    assembler.AddFragment(fragments[2], 0);
    const auto frame = assembler.PopFrame();
    REQUIRE(frame);
    CHECK(frame->frameIndex == 10);
    CHECK(frame->keyFrame);
    CHECK(frame->videoWidth == 2432);
    CHECK(frame->videoHeight == 1344);
    CHECK(frame->pose.sampleTimeUs == 1010);
    CHECK(frame->data == data);
    CHECK(!assembler.PopFrame());

    const auto stats = assembler.TakeClientStats();
    CHECK(stats.receivedFrames == 1);
    CHECK(stats.lostFrames == 0);
    CHECK(stats.receivedFragments == 3);
    CHECK(stats.receivedVideoBytes == 3000);
    // 取り出すと、次の集計は 0 から始まる。
    CHECK(assembler.TakeClientStats().receivedFragments == 0);
}

TEST_CASE("順番が入れ替わっても、同じ断片が 2 回届いても組み立てられる") {
    FrameAssembler assembler;
    const auto data = MakeData(kMaxVideoPayloadSize * 2 + 1, 2);
    auto fragments = Split(1, data);
    REQUIRE(fragments.size() == 3);
    assembler.AddFragment(fragments[2], 0);
    assembler.AddFragment(fragments[0], 0);
    assembler.AddFragment(fragments[0], 0);
    assembler.AddFragment(fragments[1], 0);
    const auto frame = assembler.PopFrame();
    REQUIRE(frame);
    CHECK(frame->data == data);
}

TEST_CASE("ちょうど 1 個に収まるフレームと、1 バイトだけのフレーム") {
    FrameAssembler assembler;
    for (const size_t size : {size_t{1}, kMaxVideoPayloadSize}) {
        const auto data = MakeData(size, 3);
        const auto index = static_cast<uint32_t>(size);
        AddAll(assembler, Split(index, data));
        const auto frame = assembler.PopFrame();
        REQUIRE(frame);
        CHECK(frame->data == data);
    }
}

TEST_CASE("断片が欠けたフレームは出さず、次のフレームが完成したら失われたと数える") {
    FrameAssembler assembler;
    auto incomplete = Split(1, MakeData(3000, 4));
    incomplete.pop_back();
    AddAll(assembler, incomplete);
    CHECK(!assembler.PopFrame());

    const auto data = MakeData(2000, 5);
    AddAll(assembler, Split(2, data));
    const auto frame = assembler.PopFrame();
    REQUIRE(frame);
    CHECK(frame->frameIndex == 2);
    CHECK(frame->data == data);
    const auto stats = assembler.TakeClientStats();
    CHECK(stats.receivedFrames == 1);
    CHECK(stats.lostFrames == 1);
}

TEST_CASE("完成したフレームより古い断片は、遅れて届いても使わない") {
    FrameAssembler assembler;
    AddAll(assembler, Split(5, MakeData(100, 6)));
    REQUIRE(assembler.PopFrame());
    // 同じ番号と、それより前の番号。
    AddAll(assembler, Split(5, MakeData(100, 7)));
    AddAll(assembler, Split(4, MakeData(100, 8)));
    CHECK(!assembler.PopFrame());
    AddAll(assembler, Split(6, MakeData(100, 9)));
    const auto frame = assembler.PopFrame();
    REQUIRE(frame);
    CHECK(frame->frameIndex == 6);
}

TEST_CASE("フレームの番号が 32 bit を一周しても、新しい方を新しいと判断する") {
    FrameAssembler assembler;
    AddAll(assembler, Split(0xffffffffu, MakeData(100, 10)));
    REQUIRE(assembler.PopFrame());
    AddAll(assembler, Split(0, MakeData(100, 11)));
    const auto frame = assembler.PopFrame();
    REQUIRE(frame);
    CHECK(frame->frameIndex == 0);
    // 一周する前の番号は、古いものとして捨てる。
    AddAll(assembler, Split(0xfffffffeu, MakeData(100, 12)));
    CHECK(!assembler.PopFrame());
}

TEST_CASE("つじつまの合わない断片は受け付けない") {
    const auto data = MakeData(3000, 13);
    const auto valid = Split(1, data);
    const auto reject = [&](auto&& change) {
        FrameAssembler assembler;
        auto fragments = valid;
        change(fragments);
        AddAll(assembler, fragments);
        CHECK(!assembler.PopFrame());
    };
    // 断片の数とフレームの大きさが合わない。
    reject([](auto& fragments) { for (auto& fragment : fragments) { fragment.fragmentCount = 4; } });
    reject([](auto& fragments) { for (auto& fragment : fragments) { fragment.frameSize = 100000; } });
    // 途中の断片が短い。
    reject([](auto& fragments) { fragments[0].payload.pop_back(); });
    // 番号が範囲の外。
    reject([](auto& fragments) { fragments[2].fragmentIndex = 3; });
    // 大きさが 0 の映像。
    reject([](auto& fragments) { for (auto& fragment : fragments) { fragment.videoWidth = 0; } });
    // 途中から映像の大きさが変わる。
    reject([](auto& fragments) { fragments[1].videoHeight = 100; });
}

TEST_CASE("取り出されないまま 3 フレームを超えたら、古いものから捨てる") {
    FrameAssembler assembler;
    for (uint32_t index = 1; index <= 4; ++index) {
        AddAll(assembler, Split(index, MakeData(100, index)));
    }
    for (uint32_t expected = 2; expected <= 4; ++expected) {
        const auto frame = assembler.PopFrame();
        REQUIRE(frame);
        CHECK(frame->frameIndex == expected);
    }
    CHECK(!assembler.PopFrame());
    CHECK(assembler.TakeClientStats().lostFrames == 1);
}

TEST_CASE("Reset のあとは、前の接続の番号に縛られない") {
    FrameAssembler assembler;
    AddAll(assembler, Split(100, MakeData(100, 14)));
    assembler.Reset();
    CHECK(!assembler.PopFrame());
    AddAll(assembler, Split(1, MakeData(100, 15)));
    const auto frame = assembler.PopFrame();
    REQUIRE(frame);
    CHECK(frame->frameIndex == 1);
}
