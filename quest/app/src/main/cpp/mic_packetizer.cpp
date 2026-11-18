#include "mic_packetizer.h"

#include <algorithm>
#include <utility>

namespace wnvr {

static_assert(kMicPacketFrames <= protocol::kMaxAudioFrames);

void MicPacketizer::Append(const int16_t* samples, size_t frames, uint32_t sampleRate) {
    if (samples == nullptr || frames == 0 || sampleRate == 0) {
        return;
    }
    if (m_sampleRate != sampleRate) {
        // 異なるレートの端数や送信待ちの音を混ぜると、再生時間が変わる。
        m_droppedFrames += m_partial.size() + m_packets.size() * kMicPacketFrames;
        m_nextFrame += m_partial.size();
        m_partial.clear();
        m_packets.clear();
        m_sampleRate = sampleRate;
    }
    const size_t maximum = static_cast<size_t>(sampleRate) * kMaxQueuedMs / kMillisecondsPerSecond;
    while (frames != 0) {
        const size_t count = std::min(frames, kMicPacketFrames - m_partial.size());
        m_partial.insert(m_partial.end(), samples, samples + count);
        samples += count;
        frames -= count;
        if (m_partial.size() == kMicPacketFrames) {
            m_packets.push_back({m_nextFrame, sampleRate, std::move(m_partial)});
            m_partial.clear();
            m_nextFrame += kMicPacketFrames;
        }
        // 送信が止まっても遅れた声をまとめて送らないよう、古いパケットを捨てる。
        while (!m_packets.empty() && m_packets.size() * kMicPacketFrames + m_partial.size() > maximum) {
            m_droppedFrames += m_packets.front().samples.size();
            m_packets.pop_front();
        }
    }
}

std::vector<protocol::MicAudioMessage> MicPacketizer::Take() {
    std::vector<protocol::MicAudioMessage> packets;
    packets.reserve(m_packets.size());
    while (!m_packets.empty()) {
        packets.push_back(std::move(m_packets.front()));
        m_packets.pop_front();
    }
    return packets;
}

void MicPacketizer::Reset() {
    m_packets.clear();
    m_partial.clear();
    m_sampleRate = 0;
    m_nextFrame = m_droppedFrames = 0;
}

}
