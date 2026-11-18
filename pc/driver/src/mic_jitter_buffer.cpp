#include "mic_jitter_buffer.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace wnvr::driver {

void MicJitterBuffer::Push(const protocol::MicAudioMessage& mic) {
    constexpr uint32_t kMinSampleRate = 8000;
    constexpr uint32_t kMaxSampleRate = 192000;
    if (mic.sampleRate < kMinSampleRate || mic.sampleRate > kMaxSampleRate
        || mic.samples.empty() || mic.samples.size() > protocol::kMaxAudioFrames) {
        return;
    }
    ++m_stats.packets;
    if (m_sampleRate != mic.sampleRate) {
        m_stats.dropped += m_samples.size();
        m_samples.clear();
        m_nextFrame.reset();
        m_preparing = true;
        m_sampleRate = mic.sampleRate;
    }
    if (m_nextFrame && mic.frameIndex < *m_nextFrame) {
        if (*m_nextFrame - mic.frameIndex >= mic.sampleRate) {
            // 録音をやり直した声に、前の録音の末尾を混ぜない。
            m_stats.dropped += m_samples.size();
            m_samples.clear();
            m_nextFrame.reset();
            m_preparing = true;
        } else {
            m_stats.dropped += mic.samples.size();
            return;
        }
    }
    if (m_nextFrame && mic.frameIndex > *m_nextFrame) {
        // 無音で抜けを埋めると、そのぶん会話の遅れが増える。
        m_stats.missing += mic.frameIndex - *m_nextFrame;
    }
    m_nextFrame = mic.frameIndex <= std::numeric_limits<uint64_t>::max() - mic.samples.size()
        ? std::optional<uint64_t>{mic.frameIndex + mic.samples.size()} : std::nullopt;
    m_samples.insert(m_samples.end(), mic.samples.begin(), mic.samples.end());
    const size_t maximum = static_cast<size_t>(m_sampleRate) * kMaxMs / kMillisecondsPerSecond;
    if (m_samples.size() > maximum) {
        const size_t target = static_cast<size_t>(m_sampleRate) * kStartMs / kMillisecondsPerSecond;
        const size_t dropped = m_samples.size() - target;
        m_samples.erase(m_samples.begin(), m_samples.begin() + static_cast<ptrdiff_t>(dropped));
        m_stats.dropped += dropped;
    }
}

size_t MicJitterBuffer::Pop(int16_t* destination, size_t frames) {
    const size_t target = static_cast<size_t>(m_sampleRate) * kStartMs / kMillisecondsPerSecond;
    if (destination == nullptr || frames == 0 || m_samples.empty() || (m_preparing && m_samples.size() < target)) {
        return 0;
    }
    m_preparing = false;
    const size_t count = std::min(frames, m_samples.size());
    for (size_t index = 0; index < count; ++index) {
        destination[index] = m_samples.front();
        m_samples.pop_front();
    }
    if (m_samples.empty()) {
        ++m_stats.gaps;
        m_preparing = true;
    }
    return count;
}

MicJitterBuffer::Stats MicJitterBuffer::TakeStats() {
    return std::exchange(m_stats, {});
}

void MicJitterBuffer::Reset() {
    m_samples.clear();
    m_nextFrame.reset();
    m_sampleRate = 0;
    m_preparing = true;
    m_stats = {};
}

}
