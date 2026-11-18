#pragma once

#include <wnvr/protocol/messages.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace wnvr {

inline constexpr size_t kMicPacketFrames = 240;

class MicPacketizer {
public:
    void Append(const int16_t* samples, size_t frames, uint32_t sampleRate);
    std::vector<protocol::MicAudioMessage> Take();
    void Reset();
    uint64_t DroppedFrames() const { return m_droppedFrames; }

private:
    static constexpr size_t kMaxQueuedMs = 100;
    static constexpr size_t kMillisecondsPerSecond = 1000;
    uint32_t m_sampleRate = 0;
    uint64_t m_nextFrame = 0;
    uint64_t m_droppedFrames = 0;
    std::vector<int16_t> m_partial;
    std::deque<protocol::MicAudioMessage> m_packets;
};

}
