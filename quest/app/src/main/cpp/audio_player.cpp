#include "audio_player.h"
#include "log.h"

#include <algorithm>
#include <cinttypes>
#include <limits>
#include <utility>

namespace wnvr {

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() {
    Stop();
}

void AudioPlayer::OnError(AAudioStream*, void* context, aaudio_result_t error) {
    // コールバックからストリームに触らず、受信スレッドへ復旧を委ねる。
    static_cast<AudioPlayer*>(context)->m_error.store(error);
}

aaudio_data_callback_result_t AudioPlayer::OnData(AAudioStream*, void* context, void* data, int32_t frames) {
    if (frames <= 0) {
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    auto& player = *static_cast<AudioPlayer*>(context);
    auto* output = static_cast<int16_t*>(data);
    const size_t requested = static_cast<size_t>(frames) * 2;
    std::fill_n(output, requested, int16_t{0});
    std::lock_guard lock(player.m_bufferMutex);
    if (player.m_buffer.empty() || player.m_sampleRate == 0) {
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    player.m_bufferTotalMs += static_cast<double>(player.m_size / 2) * 1000.0 / player.m_sampleRate;
    ++player.m_callbacks;
    ++player.m_clientStats.callbacks;
    player.m_clientStats.bufferTotalMs += static_cast<double>(player.m_size / 2) * 1000.0 / player.m_sampleRate;
    const size_t target = static_cast<size_t>(player.m_sampleRate) * 40 / 1000 * 2;
    if (player.m_preparing && player.m_size < target) {
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    player.m_preparing = false;
    const size_t count = std::min(requested, player.m_size);
    const size_t first = std::min(count, player.m_buffer.size() - player.m_readOffset);
    std::copy_n(player.m_buffer.data() + player.m_readOffset, first, output);
    std::copy_n(player.m_buffer.data(), count - first, output + first);
    player.m_readOffset = (player.m_readOffset + count) % player.m_buffer.size();
    player.m_size -= count;
    if (player.m_size == 0) {
        ++player.m_underruns;
        ++player.m_clientStats.underruns;
        player.m_preparing = true;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioPlayer::CloseStream() {
    // m_controlMutex のみを保持する。close 完了まではコールバック用のメモリを維持する。
    if (m_stream != nullptr) {
        AAudioStream_requestStop(m_stream);
        AAudioStream_close(m_stream);
        m_stream = nullptr;
    }
    m_error.store(AAUDIO_OK);
    std::lock_guard lock(m_bufferMutex);
    m_buffer.clear();
    m_readOffset = m_size = 0;
    m_sampleRate = 0;
    m_preparing = true;
}

bool AudioPlayer::Open(uint32_t sampleRate) {
    AAudioStreamBuilder* builder = nullptr;
    auto result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        WNVR_LOGW("AAudio builder failed: %s", AAudio_convertResultToText(result));
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(sampleRate));
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, OnData, this);
    AAudioStreamBuilder_setErrorCallback(builder, OnError, this);
    result = AAudioStreamBuilder_openStream(builder, &m_stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        WNVR_LOGW("AAudio open failed: %s", AAudio_convertResultToText(result));
        CloseStream();
        return false;
    }
    if (AAudioStream_getFormat(m_stream) != AAUDIO_FORMAT_PCM_I16
        || AAudioStream_getChannelCount(m_stream) != 2
        || AAudioStream_getSampleRate(m_stream) != static_cast<int32_t>(sampleRate)) {
        WNVR_LOGW("AAudio output format differs from PCM request");
        CloseStream();
        return false;
    }
    {
        std::lock_guard lock(m_bufferMutex);
        m_buffer.assign(static_cast<size_t>(sampleRate) / 2 * 2, 0);
        m_sampleRate = sampleRate;
    }
    result = AAudioStream_requestStart(m_stream);
    if (result != AAUDIO_OK) {
        WNVR_LOGW("AAudio start failed: %s", AAudio_convertResultToText(result));
        CloseStream();
        return false;
    }
    return true;
}

void AudioPlayer::Push(const protocol::AudioMessage& audio) {
    if (audio.channelCount != 2 || audio.sampleRate < 8000 || audio.sampleRate > 192000
        || audio.samples.empty() || audio.samples.size() % 2 != 0
        || audio.samples.size() / 2 > protocol::kMaxAudioFrames) {
        return;
    }
    std::lock_guard controlLock(m_controlMutex);
    const auto now = std::chrono::steady_clock::now();
    if (m_nextReport == std::chrono::steady_clock::time_point{}) {
        m_nextReport = now + std::chrono::seconds(5);
    }
    ++m_packets;
    m_receivedSampleRate = audio.sampleRate;
    const uint64_t frames = audio.samples.size() / 2;
    if (m_nextFrame && audio.frameIndex != *m_nextFrame) {
        if (audio.frameIndex > *m_nextFrame) {
            m_missingFrames += audio.frameIndex - *m_nextFrame;
            std::lock_guard lock(m_bufferMutex);
            m_clientStats.missingFrames += audio.frameIndex - *m_nextFrame;
        } else if (*m_nextFrame - audio.frameIndex >= audio.sampleRate) {
            // 録音の再開で番号が戻るため、以前の音を捨てて 40ms の準備待ちに戻す。
            std::lock_guard lock(m_bufferMutex);
            m_readOffset = 0;
            m_size = 0;
            m_preparing = true;
            m_nextFrame = audio.frameIndex;
            WNVR_LOGI("音声送信の再開を検出: frame=%" PRIu64, audio.frameIndex);
        } else {
            // UDP の逆順・重複を再生すると時間が戻るので、古いパケットを破棄する。
            {
                std::lock_guard lock(m_bufferMutex);
                m_droppedFrames += frames;
            }
            LogStats();
            return;
        }
    }
    m_nextFrame = audio.frameIndex <= std::numeric_limits<uint64_t>::max() - frames
        ? std::optional<uint64_t>{audio.frameIndex + frames} : std::nullopt;
    const auto error = m_error.load();
    if (m_stream == nullptr || m_sampleRate != audio.sampleRate || error != AAUDIO_OK) {
        if (error != AAUDIO_OK) {
            WNVR_LOGW("AAudio callback error: %s", AAudio_convertResultToText(error));
        }
        CloseStream();
        if (!Open(audio.sampleRate)) {
            LogStats();
            return;
        }
    }
    {
        std::lock_guard lock(m_bufferMutex);
        const size_t count = audio.samples.size();
        // 最低レートでも 1 パケットは最大 40ms なので、500ms の容量を超えない。
        const size_t writeOffset = (m_readOffset + m_size) % m_buffer.size();
        const size_t first = std::min(count, m_buffer.size() - writeOffset);
        std::copy_n(audio.samples.data(), first, m_buffer.data() + writeOffset);
        std::copy_n(audio.samples.data() + first, count - first, m_buffer.data());
        m_size += count;
        const size_t target = static_cast<size_t>(m_sampleRate) * 40 / 1000 * 2;
        const size_t maximum = static_cast<size_t>(m_sampleRate) * 120 / 1000 * 2;
        if (m_size > maximum) {
            const size_t dropped = m_size - target;
            m_readOffset = (m_readOffset + dropped) % m_buffer.size();
            m_size = target;
            m_droppedFrames += dropped / 2;
        }
    }
    LogStats();
}

void AudioPlayer::LogStats() {
    if (m_nextReport == std::chrono::steady_clock::time_point{}
        || std::chrono::steady_clock::now() < m_nextReport) {
        return;
    }
    uint64_t underruns = 0;
    uint64_t dropped = 0;
    double averageMs = 0;
    {
        std::lock_guard lock(m_bufferMutex);
        underruns = m_underruns;
        dropped = m_droppedFrames;
        averageMs = m_callbacks == 0 ? 0.0 : m_bufferTotalMs / static_cast<double>(m_callbacks);
        m_underruns = m_droppedFrames = m_callbacks = 0;
        m_bufferTotalMs = 0;
    }
    WNVR_LOGI("音声再生5s: packets=%" PRIu64 ", rate=%" PRIu32 ", missing=%" PRIu64
        ", underruns=%" PRIu64 ", dropped=%" PRIu64 ", average=%.2f ms",
        m_packets, m_receivedSampleRate, m_missingFrames, underruns, dropped, averageMs);
    m_packets = m_missingFrames = 0;
    m_nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

AudioPlayer::ClientStats AudioPlayer::TakeClientStats() {
    std::lock_guard lock(m_bufferMutex);
    return std::exchange(m_clientStats, {});
}

void AudioPlayer::ReportStats() {
    std::lock_guard controlLock(m_controlMutex);
    LogStats();
}

void AudioPlayer::Stop() {
    std::lock_guard controlLock(m_controlMutex);
    CloseStream();
    m_nextFrame.reset();
    m_packets = m_missingFrames = 0;
    m_nextReport = {};
    m_receivedSampleRate = 0;
    std::lock_guard lock(m_bufferMutex);
    m_clientStats = {};
    m_underruns = m_droppedFrames = m_callbacks = 0;
    m_bufferTotalMs = 0;
}

}
