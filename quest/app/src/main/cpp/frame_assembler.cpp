#include "frame_assembler.h"
#include "log.h"

#include <algorithm>
#include <cinttypes>
#include <utility>

namespace wnvr::quest {
namespace {

bool IsOlder(uint32_t index, uint32_t reference) {
    // 半周未満の差で順序を決め、32 bit の周回後も古い断片を区別する。
    const uint32_t distance = reference - index;
    return distance != 0 && distance < 0x80000000u;
}

}

void FrameAssembler::AddFragment(const protocol::VideoFragmentMessage& fragment, int64_t kernelTimestampNs) {
    const auto receivedTime = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);
    ++m_fragments;
    ++m_clientStats.receivedFragments;
    m_clientStats.receivedVideoBytes += fragment.payload.size();
    m_bytes += protocol::kHeaderSize + protocol::kVideoFragmentHeaderSize + fragment.payload.size();
    LogStatsLocked();
    if (fragment.fragmentCount == 0 || fragment.fragmentIndex >= fragment.fragmentCount
        || fragment.payload.empty() || fragment.payload.size() > protocol::kMaxVideoPayloadSize
        || fragment.frameSize == 0 || fragment.videoWidth == 0 || fragment.videoHeight == 0) {
        return;
    }
    const size_t frameSize = fragment.frameSize;
    const size_t offset = static_cast<size_t>(fragment.fragmentIndex) * protocol::kMaxVideoPayloadSize;
    // 隙間のあるフレームを完成扱いにせず、未検証の巨大サイズの確保も防ぐ。
    if ((frameSize - 1) / protocol::kMaxVideoPayloadSize + 1 != fragment.fragmentCount
        || offset >= frameSize || fragment.payload.size() != std::min(protocol::kMaxVideoPayloadSize, frameSize - offset)) {
        return;
    }
    if (m_lastCompleted && (fragment.frameIndex == *m_lastCompleted || IsOlder(fragment.frameIndex, *m_lastCompleted))) {
        return;
    }
    auto found = std::find_if(m_pending.begin(), m_pending.end(), [&fragment](const auto& pending) {
        return pending.frame.frameIndex == fragment.frameIndex;
    });
    if (found == m_pending.end()) {
        if (m_pending.size() == 4) {
            const auto oldest = std::min_element(m_pending.begin(), m_pending.end(), [](const auto& left, const auto& right) {
                return IsOlder(left.frame.frameIndex, right.frame.frameIndex);
            });
            m_pending.erase(oldest);
            ++m_lostFrames;
            ++m_clientStats.lostFrames;
        }
        PendingFrame pending;
        pending.firstKernelTimestampNs = kernelTimestampNs;
        pending.frame.firstFragmentTime = receivedTime;
        pending.frame.frameIndex = fragment.frameIndex;
        pending.frame.keyFrame = (fragment.flags & 1) != 0;
        pending.frame.pose = fragment.pose;
        pending.frame.videoWidth = fragment.videoWidth;
        pending.frame.videoHeight = fragment.videoHeight;
        pending.frame.data.resize(frameSize);
        pending.fragmentCount = fragment.fragmentCount;
        pending.received.resize(fragment.fragmentCount, false);
        m_pending.push_back(std::move(pending));
        found = std::prev(m_pending.end());
    }
    if (found->fragmentCount != fragment.fragmentCount || found->frame.data.size() != frameSize
        || found->frame.videoWidth != fragment.videoWidth || found->frame.videoHeight != fragment.videoHeight
        || found->frame.keyFrame != ((fragment.flags & 1) != 0) || found->received[fragment.fragmentIndex]) {
        return;
    }
    std::copy(fragment.payload.begin(), fragment.payload.end(), found->frame.data.data() + offset);
    found->received[fragment.fragmentIndex] = true;
    found->lastKernelTimestampNs = kernelTimestampNs;
    if (++found->receivedCount != found->fragmentCount) {
        return;
    }
    if (m_completed.size() == 3) {
        m_completed.pop_front();
        ++m_lostFrames;
        ++m_clientStats.lostFrames;
    }
    ++m_assembledFrames;
    ++m_clientStats.receivedFrames;
    // REALTIME の補正で時刻が逆転した場合は、負の所要時間を集計しない。
    if (found->firstKernelTimestampNs != 0 && found->lastKernelTimestampNs != 0
        && found->lastKernelTimestampNs >= found->firstKernelTimestampNs) {
        m_kernelAssembleTotalNs += static_cast<uint64_t>(
            found->lastKernelTimestampNs - found->firstKernelTimestampNs);
        ++m_kernelAssembleFrames;
    }
    auto& frame = found->frame;
    frame.completedTime = std::chrono::steady_clock::now();
    const auto firstUs = std::chrono::duration_cast<std::chrono::microseconds>(
        frame.firstFragmentTime.time_since_epoch()).count();
    if (firstUs >= 0 && frame.pose.sampleTimeUs != 0
        && static_cast<uint64_t>(firstUs) >= frame.pose.sampleTimeUs
        && static_cast<uint64_t>(firstUs) - frame.pose.sampleTimeUs <= 1000000) {
        m_clientStats.firstFragmentTotalUs += static_cast<uint64_t>(firstUs) - frame.pose.sampleTimeUs;
        ++m_clientStats.firstFragmentFrames;
    }
    const auto assembleUs = std::chrono::duration_cast<std::chrono::microseconds>(
        frame.completedTime - frame.firstFragmentTime).count();
    if (assembleUs >= 0) {
        m_clientStats.assembleTotalUs += static_cast<uint64_t>(assembleUs);
        ++m_clientStats.assembleFrames;
    }
    m_keyFrames += found->frame.keyFrame ? 1u : 0u;
    m_lastCompleted = fragment.frameIndex;
    m_completed.push_back(std::move(found->frame));
    m_pending.erase(found);
    const auto lost = std::erase_if(m_pending, [&fragment](const auto& pending) {
        return IsOlder(pending.frame.frameIndex, fragment.frameIndex);
    });
    m_lostFrames += lost;
    m_clientStats.lostFrames += lost;
}

std::optional<AssembledFrame> FrameAssembler::PopFrame() {
    std::lock_guard lock(m_mutex);
    LogStatsLocked();
    if (m_completed.empty()) {
        return std::nullopt;
    }
    auto frame = std::move(m_completed.front());
    m_completed.pop_front();
    return frame;
}

FrameAssembler::ClientStats FrameAssembler::TakeClientStats() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_clientStats, {});
}

double FrameAssembler::TakeKernelAssembleAverageMs() {
    std::lock_guard lock(m_mutex);
    const double averageMs = m_kernelAssembleFrames == 0 ? 0.0
        : static_cast<double>(m_kernelAssembleTotalNs) / static_cast<double>(m_kernelAssembleFrames) / 1000000.0;
    m_kernelAssembleTotalNs = 0;
    m_kernelAssembleFrames = 0;
    return averageMs;
}

void FrameAssembler::Reset() {
    std::lock_guard lock(m_mutex);
    m_pending.clear();
    m_completed.clear();
    m_lastCompleted.reset();
    m_clientStats = {};
    m_kernelAssembleTotalNs = 0;
    m_kernelAssembleFrames = 0;
    m_assembledFrames = m_lostFrames = m_fragments = m_bytes = m_keyFrames = 0;
    m_statsTime = std::chrono::steady_clock::now();
}

void FrameAssembler::LogStatsLocked() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - m_statsTime).count();
    if (elapsed < 5.0) {
        return;
    }
    WNVR_LOGI("映像受信: frames=%" PRIu64 " (%.2f fps), lost=%" PRIu64 ", fragments=%" PRIu64
        ", %.2f Mbps, key=%" PRIu64, m_assembledFrames, static_cast<double>(m_assembledFrames) / elapsed,
        m_lostFrames, m_fragments, static_cast<double>(m_bytes) * 8.0 / elapsed / 1000000.0, m_keyFrames);
    m_assembledFrames = m_lostFrames = m_fragments = m_bytes = m_keyFrames = 0;
    m_statsTime = now;
}

}
