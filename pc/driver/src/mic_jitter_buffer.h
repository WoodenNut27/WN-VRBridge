#pragma once

#include <wnvr/protocol/messages.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace wnvr::driver {

class MicJitterBuffer {
public:
    struct Stats {
        uint64_t packets = 0;
        uint64_t missing = 0;
        uint64_t dropped = 0;
        uint64_t gaps = 0;
    };
    void Push(const protocol::MicAudioMessage& mic);
    size_t Pop(int16_t* destination, size_t frames);
    uint32_t SampleRate() const { return m_sampleRate; }
    Stats TakeStats();
    void Reset();

private:
    static constexpr size_t kStartMs = 40;
    static constexpr size_t kMaxMs = 120;
    static constexpr size_t kMillisecondsPerSecond = 1000;
    std::deque<int16_t> m_samples;
    std::optional<uint64_t> m_nextFrame;
    uint32_t m_sampleRate = 0;
    bool m_preparing = true;
    Stats m_stats;
};

}
