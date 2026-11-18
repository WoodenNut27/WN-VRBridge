#include "mic_capture.h"
#include "log.h"

#include <cinttypes>

namespace wnvr {
namespace {
constexpr int32_t kRequestedSampleRate = 48000;
constexpr int32_t kMonoChannels = 1;
constexpr int32_t kMinSampleRate = 8000;
constexpr int32_t kMaxSampleRate = 192000;
constexpr auto kReportInterval = std::chrono::seconds(5);
}

MicCapture::~MicCapture() {
    Stop();
}

aaudio_data_callback_result_t MicCapture::OnData(AAudioStream*, void* context, void* data, int32_t frames) {
    if (frames > 0) {
        auto& capture = *static_cast<MicCapture*>(context);
        std::lock_guard lock(capture.m_bufferMutex);
        capture.m_packetizer.Append(static_cast<const int16_t*>(data), static_cast<size_t>(frames), capture.m_sampleRate);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void MicCapture::OnError(AAudioStream*, void* context, aaudio_result_t) {
    // コールバックから close すると終了待ちが自分自身を待つため、描画側に任せる。
    static_cast<MicCapture*>(context)->m_failed = true;
}

void MicCapture::CloseStream() {
    if (m_stream != nullptr) {
        AAudioStream_requestStop(m_stream);
        AAudioStream_close(m_stream);
        m_stream = nullptr;
    }
    std::lock_guard lock(m_bufferMutex);
    m_packetizer.Reset();
    m_sampleRate = 0;
    m_packets = m_lastDropped = 0;
    m_nextReport = {};
}

bool MicCapture::Start() {
    std::lock_guard controlLock(m_controlMutex);
    if (m_stream != nullptr && !m_failed.load()) {
        return true;
    }
    CloseStream();
    m_failed = false;
    AAudioStreamBuilder* builder = nullptr;
    auto result = AAudio_createStreamBuilder(&builder);
    if (result == AAUDIO_OK) {
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(builder, kMonoChannels);
        AAudioStreamBuilder_setSampleRate(builder, kRequestedSampleRate);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        // Quest のスピーカーで鳴る PC の音が相手に戻らないよう、エコーキャンセルを使う。
        AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
        AAudioStreamBuilder_setDataCallback(builder, OnData, this);
        AAudioStreamBuilder_setErrorCallback(builder, OnError, this);
        result = AAudioStreamBuilder_openStream(builder, &m_stream);
        AAudioStreamBuilder_delete(builder);
    }
    if (result == AAUDIO_OK) {
        const int32_t rate = AAudioStream_getSampleRate(m_stream);
        if (AAudioStream_getFormat(m_stream) != AAUDIO_FORMAT_PCM_I16
            || AAudioStream_getChannelCount(m_stream) != kMonoChannels
            || rate < kMinSampleRate || rate > kMaxSampleRate) {
            result = AAUDIO_ERROR_INVALID_FORMAT;
        } else {
            {
                std::lock_guard lock(m_bufferMutex);
                m_sampleRate = static_cast<uint32_t>(rate);
                m_nextReport = std::chrono::steady_clock::now() + kReportInterval;
            }
            result = AAudioStream_requestStart(m_stream);
        }
    }
    if (result != AAUDIO_OK) {
        WNVR_LOGW("マイクを開けません: %s", AAudio_convertResultToText(result));
        CloseStream();
        m_failed = true;
        return false;
    }
    return !m_failed.load();
}

void MicCapture::Stop() {
    std::lock_guard controlLock(m_controlMutex);
    CloseStream();
    // 失敗は次の Start まで保持し、再試行待ちも Failed として報告する。
}

bool MicCapture::IsRunning() const {
    std::lock_guard controlLock(m_controlMutex);
    return m_stream != nullptr && !m_failed.load();
}

std::vector<protocol::MicAudioMessage> MicCapture::Take() {
    std::vector<protocol::MicAudioMessage> packets;
    uint64_t dropped = 0;
    uint32_t rate = 0;
    uint64_t packetCount = 0;
    bool report = false;
    {
        std::lock_guard lock(m_bufferMutex);
        packets = m_packetizer.Take();
        m_packets += packets.size();
        const auto now = std::chrono::steady_clock::now();
        if (m_nextReport != std::chrono::steady_clock::time_point{} && now >= m_nextReport) {
            report = true;
            dropped = m_packetizer.DroppedFrames() - m_lastDropped;
            m_lastDropped = m_packetizer.DroppedFrames();
            packetCount = m_packets;
            m_packets = 0;
            rate = m_sampleRate;
            m_nextReport = now + kReportInterval;
        }
    }
    if (report) {
        WNVR_LOGI("マイク録音5s: packets=%" PRIu64 " dropped=%" PRIu64 " rate=%" PRIu32, packetCount, dropped, rate);
    }
    return packets;
}

}
