#include "video_decoder.h"
#include "log.h"

#include <android/hardware_buffer.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <exception>
#include <utility>

namespace wnvr::quest {
namespace {

constexpr uint64_t kFrameRate = 72;

uint32_t FrameIndexFromTimestamp(int64_t timestampUs) {
    // 入力 PTS の整数除算による端数を丸め戻す。
    return static_cast<uint32_t>((static_cast<uint64_t>(timestampUs) * kFrameRate + 500000) / 1000000);
}

bool CheckMedia(media_status_t status, const char* operation) {
    if (status == AMEDIA_OK) {
        return true;
    }
    WNVR_LOGW("AV1 %s failed: %d", operation, static_cast<int>(status));
    return false;
}

}

VideoDecoder::~VideoDecoder() {
    Stop();
}

bool VideoDecoder::Start(std::function<std::optional<AssembledFrame>()> frameSource,
    std::function<uint64_t()> generationSource,
    std::function<void(uint32_t, protocol::KeyFrameReason)> keyFrameRequest, Config config) {
    Stop();
    if (!frameSource) {
        return false;
    }
    m_frameSource = std::move(frameSource);
    m_config = config;
    m_generationSource = std::move(generationSource);
    m_keyFrameRequest = std::move(keyFrameRequest);
    m_waitingForKeyFrame = true;
    m_failed = false;
    m_lastGoodFrame.reset();
    m_waitStarted.reset();
    m_createFailures.clear();
    m_targetWidth = m_targetHeight = 0;
    m_nextCreate = {};
    m_keyWaitMs = 0;
    m_statsTime = Clock::now();
    m_inputs = m_decoded = m_timedFrames = m_inputDrops = m_keyFrameDrops = 0;
    m_totalMs = m_maxMs = 0;
    m_droppedFrames = 0;
    m_clientStats = {};
    m_stopping.store(false);
    try {
        m_thread = std::thread([this] { Run(); });
    } catch (const std::exception& error) {
        WNVR_LOGE("AV1 thread failed: %s", error.what());
        Stop();
        return false;
    }
    return true;
}

void VideoDecoder::Stop() {
    m_stopping.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    DestroyDecoder();
    m_frameSource = {};
    m_generationSource = {};
    m_keyFrameRequest = {};
}

std::optional<DecodedImage> VideoDecoder::TakeNextImage(size_t* queueDepthBefore) {
    std::lock_guard lock(m_mutex);
    if (queueDepthBefore) {
        *queueDepthBefore = m_imageQueue.size();
    }
    while (m_imageQueue.size() > 2) {
        AImage_delete(m_imageQueue.front().image);
        m_imageQueue.pop_front();
        ++m_droppedFrames;
    }
    if (!m_imageQueue.empty()) {
        auto image = m_imageQueue.front();
        // reader 再生成後も、GL が読み取るメモリを最後の正常画像として維持する。
        AHardwareBuffer_acquire(image.buffer);
        m_renderImages.emplace(image.image, image.generation);
        m_imageQueue.pop_front();
        return image;
    }
    return std::nullopt;
}

VideoDecoder::ClientStats VideoDecoder::TakeClientStats() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_clientStats, {});
}

uint64_t VideoDecoder::TakeDroppedFrames() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_droppedFrames, 0);
}

void VideoDecoder::ReleaseImage(DecodedImage& image) {
    std::lock_guard lock(m_mutex);
    const auto found = m_renderImages.find(image.image);
    if (found != m_renderImages.end() && found->second == image.generation) {
        AImage_delete(image.image);
        m_renderImages.erase(found);
    }
    // reader 再生成後は AImage を破棄済みなので、二重に delete しない。
    if (image.buffer) {
        AHardwareBuffer_release(image.buffer);
    }
    image = {};
}

uint64_t VideoDecoder::DecoderGeneration() const {
    return m_decoderGeneration.load();
}

bool VideoDecoder::CreateDecoder(uint16_t width, uint16_t height) {
    DestroyDecoder();
    if (!CheckMedia(AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE,
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 8, &m_reader), "AImageReader_newWithUsage")) {
        return false;
    }
    ANativeWindow* window = nullptr;
    if (!CheckMedia(AImageReader_getWindow(m_reader, &window), "AImageReader_getWindow")) {
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        m_callbackReader = m_reader;
    }
    AImageReader_ImageListener listener{this, OnImageAvailable};
    if (!CheckMedia(AImageReader_setImageListener(m_reader, &listener), "AImageReader_setImageListener")) {
        return false;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        m_codec = AMediaCodec_createDecoderByType("video/av01");
        if (!m_codec) {
            WNVR_LOGE("AV1 decoder unavailable; subsequent frames will be discarded");
            return false;
        }
        AMediaFormat* format = AMediaFormat_new();
        if (!format) {
            WNVR_LOGE("AV1 AMediaFormat_new failed");
            return false;
        }
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/av01");
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
        if (attempt == 0) {
            AMediaFormat_setInt32(format, "low-latency", 1);
            if (m_config.qtiLowLatency) {
                AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
            }
        }
        AMediaFormat_setInt32(format, "priority", 0);
        if (m_config.operatingRate != 0) {
            AMediaFormat_setFloat(format, "operating-rate", static_cast<float>(m_config.operatingRate));
        }
        const auto status = AMediaCodec_configure(m_codec, format, window, nullptr, 0);
        AMediaFormat_delete(format);
        if (status == AMEDIA_OK) {
            if (!CheckMedia(AMediaCodec_start(m_codec), "start")) {
                return false;
            }
            m_started = true;
            WNVR_LOGI("AV1 decode started: %ux%u, low-latency=%d, qti=%d, operatingRate=%d",
                static_cast<unsigned>(width), static_cast<unsigned>(height), attempt == 0,
                attempt == 0 && m_config.qtiLowLatency, m_config.operatingRate);
            return true;
        }
        CheckMedia(status, "configure");
        // configure 失敗後の状態に依存せず、新しい codec で低遅延キーなしを一度だけ試す。
        CheckMedia(AMediaCodec_delete(m_codec), "delete after configure failure");
        m_codec = nullptr;
    }
    return false;
}

void VideoDecoder::DestroyDecoder() {
    if (m_codec) {
        if (m_started) {
            CheckMedia(AMediaCodec_stop(m_codec), "stop");
        }
        CheckMedia(AMediaCodec_delete(m_codec), "delete");
        m_codec = nullptr;
    }
    m_started = false;
    {
        std::lock_guard lock(m_mutex);
        m_callbackReader = nullptr;
        m_inputTimes.clear();
        for (const auto& image : m_imageQueue) {
            AImage_delete(image.image);
        }
        m_imageQueue.clear();
        if (m_reader) {
            // codec 停止後は書き込みがなく、描画側の AHardwareBuffer 参照だけで最後の画像を保持できる。
            ++m_decoderGeneration;
            for (const auto& [image, generation] : m_renderImages) {
                (void)generation;
                AImage_delete(image);
            }
            m_renderImages.clear();
        }
    }
    if (m_reader) {
        // delete はコールバックの終了を待つため、m_mutex を保持して呼ばない。
        CheckMedia(AImageReader_setImageListener(m_reader, nullptr), "clear image listener");
        AImageReader_delete(m_reader);
        m_reader = nullptr;
    }
}

void VideoDecoder::WaitForKeyFrame(protocol::KeyFrameReason reason, bool resetOutput) {
    const auto now = Clock::now();
    const bool wasWaiting = m_waitingForKeyFrame && m_waitStarted.has_value();
    if (!m_waitStarted) {
        m_waitStarted = now;
    }
    m_waitingForKeyFrame = true;
    m_waitReason = reason;
    if (!wasWaiting || resetOutput) {
        m_nextKeyRequest = now;
    }
    {
        std::lock_guard lock(m_mutex);
        m_filterOutput = true;
        m_outputBoundary = resetOutput ? std::nullopt : m_lastGoodFrame;
        m_recoveryFrame.reset();
        if (resetOutput) {
            for (const auto& image : m_imageQueue) {
                AImage_delete(image.image);
            }
            m_imageQueue.clear();
        }
    }
    RequestKeyFrame();
}

void VideoDecoder::RequestKeyFrame() {
    const auto now = Clock::now();
    if (m_waitingForKeyFrame && !m_failed && now >= m_nextKeyRequest) {
        if (m_keyFrameRequest) {
            m_keyFrameRequest(m_lastGoodFrame.value_or(0), m_waitReason);
        }
        m_nextKeyRequest = now + std::chrono::milliseconds(200);
    }
}

bool VideoDecoder::TryCreateDecoder(uint16_t width, uint16_t height) {
    m_targetWidth = width;
    m_targetHeight = height;
    if (CreateDecoder(width, height)) {
        return true;
    }
    DestroyDecoder();
    const auto now = Clock::now();
    while (!m_createFailures.empty() && now - m_createFailures.front() > std::chrono::seconds(5)) {
        m_createFailures.pop_front();
    }
    m_createFailures.push_back(now);
    m_failed = m_createFailures.size() >= 3;
    m_nextCreate = now + std::chrono::milliseconds(200);
    if (m_failed) {
        WNVR_LOGE("AV1 decoder creation failed three times within 5 seconds; decoding stopped");
    }
    return false;
}

void VideoDecoder::RecoverDecoder() {
    WaitForKeyFrame(protocol::KeyFrameReason::DecoderError, true);
    TryCreateDecoder(m_targetWidth, m_targetHeight);
}

bool VideoDecoder::Submit(const AssembledFrame& frame) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(50);
    ssize_t index = AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    while (!m_stopping.load() && Clock::now() < deadline) {
        // 出力の surface 返却を進めないと、入力バッファも空かない codec がある。
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
        index = AMediaCodec_dequeueInputBuffer(m_codec, std::max<int64_t>(0, std::min<int64_t>(2000, remaining)));
        if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            break;
        }
        if (!DrainOutput()) {
            return false;
        }
    }
    if (m_stopping.load()) {
        return true;
    }
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        ++m_inputDrops;
        WaitForKeyFrame(protocol::KeyFrameReason::DecoderDrop);
        return true;
    }
    if (index < 0) {
        WNVR_LOGE("AV1 dequeueInputBuffer failed: %zd", index);
        return false;
    }
    size_t capacity = 0;
    auto* buffer = AMediaCodec_getInputBuffer(m_codec, static_cast<size_t>(index), &capacity);
    if (!buffer || frame.data.size() > capacity) {
        WNVR_LOGE("AV1 input buffer too small: frame=%zu, capacity=%zu", frame.data.size(), capacity);
        return false;
    }
    std::memcpy(buffer, frame.data.data(), frame.data.size());
    const uint64_t pts = static_cast<uint64_t>(frame.frameIndex) * 1000000 / kFrameRate;
    Clock::time_point inputTime{};
    {
        std::lock_guard lock(m_mutex);
        // 出力されなかったフレームの記録が増え続けることを防ぐ。
        const auto now = Clock::now();
        inputTime = now;
        std::erase_if(m_inputTimes, [now](const auto& entry) {
            return now - entry.second.time >= std::chrono::seconds(5);
        });
        m_inputTimes[frame.frameIndex] = {now, frame.pose};
        if (m_waitingForKeyFrame && frame.keyFrame) {
            // queueInputBuffer 内で出力されても、回復フレームを識別できるよう先に公開する。
            m_recoveryFrame = frame.frameIndex;
        }
    }
    if (!CheckMedia(AMediaCodec_queueInputBuffer(m_codec, static_cast<size_t>(index), 0, frame.data.size(), pts, 0),
            "queueInputBuffer")) {
        return false;
    }
    if (frame.completedTime != Clock::time_point{} && inputTime >= frame.completedTime) {
        std::lock_guard lock(m_mutex);
        m_clientStats.decoderWaitTotalUs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(inputTime - frame.completedTime).count());
        ++m_clientStats.decoderWaitFrames;
    }
    ++m_inputs;
    m_lastGoodFrame = frame.frameIndex;
    if (m_waitingForKeyFrame && frame.keyFrame) {
        m_waitingForKeyFrame = false;
        if (m_waitStarted) {
            m_keyWaitMs += std::chrono::duration<double, std::milli>(Clock::now() - *m_waitStarted).count();
            m_waitStarted.reset();
        }
    }
    return true;
}

bool VideoDecoder::DrainOutput() {
    while (!m_stopping.load()) {
        AMediaCodecBufferInfo info{};
        const auto index = AMediaCodec_dequeueOutputBuffer(m_codec, &info, 0);
        if (index >= 0) {
            if (!CheckMedia(AMediaCodec_releaseOutputBuffer(m_codec, static_cast<size_t>(index), true),
                    "releaseOutputBuffer")) {
                return false;
            }
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* format = AMediaCodec_getOutputFormat(m_codec);
            if (format) {
                WNVR_LOGI("AV1 output format: %s", AMediaFormat_toString(format));
                AMediaFormat_delete(format);
            }
        } else if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        } else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            return true;
        } else {
            WNVR_LOGE("AV1 dequeueOutputBuffer failed: %zd", index);
            return false;
        }
    }
    return true;
}

void VideoDecoder::Run() {
    try {
        uint64_t generation = m_generationSource ? m_generationSource() : 0;
        WaitForKeyFrame(protocol::KeyFrameReason::DecoderStart, true);
        while (!m_stopping.load()) {
            auto frame = m_frameSource();
            const auto currentGeneration = m_generationSource ? m_generationSource() : generation;
            if (currentGeneration != generation) {
                DestroyDecoder();
                generation = currentGeneration;
                m_lastGoodFrame.reset();
                m_targetWidth = m_targetHeight = 0;
                WaitForKeyFrame(protocol::KeyFrameReason::DecoderStart, true);
                WNVR_LOGI("AV1 waiting for keyframe: connection=%" PRIu64, generation);
            }
            LogStats();
            if (frame && m_generationSource && frame->connectionGeneration != generation) {
                ++m_keyFrameDrops;
                frame.reset();
            }
            if (!m_failed && frame) {
                const bool sizeChanged = frame->videoWidth != m_targetWidth || frame->videoHeight != m_targetHeight;
                if (sizeChanged) {
                    WaitForKeyFrame(protocol::KeyFrameReason::DecoderStart, true);
                    TryCreateDecoder(frame->videoWidth, frame->videoHeight);
                }
                if (!m_codec && !m_failed && Clock::now() >= m_nextCreate) {
                    TryCreateDecoder(m_targetWidth, m_targetHeight);
                }
                if (!m_waitingForKeyFrame && m_lastGoodFrame
                    && static_cast<uint32_t>(frame->frameIndex - *m_lastGoodFrame) != 1u && !frame->keyFrame) {
                    WaitForKeyFrame(protocol::KeyFrameReason::FrameGap);
                }
                if (!m_codec || (m_waitingForKeyFrame && !frame->keyFrame)) {
                    ++m_keyFrameDrops;
                } else if (!Submit(*frame)) {
                    RecoverDecoder();
                }
            } else if (!m_failed && !m_codec && m_targetWidth != 0 && Clock::now() >= m_nextCreate) {
                TryCreateDecoder(m_targetWidth, m_targetHeight);
            }
            if (!m_failed && m_codec && !DrainOutput()) {
                RecoverDecoder();
            }
            RequestKeyFrame();
            if (!frame) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    } catch (const std::exception& error) {
        WNVR_LOGE("AV1 decode thread stopped: %s", error.what());
    } catch (...) {
        WNVR_LOGE("AV1 decode thread stopped: unexpected exception");
    }
}

void VideoDecoder::OnImageAvailable(void* context, AImageReader* reader) {
    auto& self = *static_cast<VideoDecoder*>(context);
    std::lock_guard lock(self.m_mutex);
    if (self.m_callbackReader != reader) {
        return;
    }
    for (;;) {
        AImage* image = nullptr;
        const auto status = AImageReader_acquireNextImage(reader, &image);
        if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            return;
        }
        if (!CheckMedia(status, "acquireNextImage")) {
            return;
        }
        const auto now = Clock::now();
        ++self.m_decoded;
        ++self.m_clientStats.decodedFrames;
        DecodedImage decoded;
        decoded.decodedTime = now;
        decoded.image = image;
        decoded.generation = self.m_decoderGeneration.load();
        bool timestampValid = false;
        int64_t timestampNs = 0;
        if (CheckMedia(AImage_getTimestamp(image, &timestampNs), "AImage_getTimestamp") && timestampNs >= 0) {
            const auto frameIndex = FrameIndexFromTimestamp(timestampNs / 1000);
            decoded.frameIndex = frameIndex;
            timestampValid = true;
            const auto found = self.m_inputTimes.find(frameIndex);
            if (found != self.m_inputTimes.end()) {
                const double ms = std::chrono::duration<double, std::milli>(now - found->second.time).count();
                self.m_totalMs += ms;
                self.m_clientStats.totalMs += ms;
                self.m_clientStats.maxMs = std::max(self.m_clientStats.maxMs, ms);
                ++self.m_clientStats.timedFrames;
                self.m_maxMs = std::max(self.m_maxMs, ms);
                ++self.m_timedFrames;
                decoded.pose = found->second.pose;
                decoded.hasPose = true;
                self.m_inputTimes.erase(found);
            }
        }
        if (!timestampValid || !decoded.hasPose) {
            AImage_delete(image);
            continue;
        }
        if (self.m_filterOutput) {
            const auto after = [](uint32_t value, uint32_t reference) {
                const uint32_t distance = value - reference;
                return distance != 0 && distance < 0x80000000u;
            };
            const bool afterBoundary = !self.m_outputBoundary || after(decoded.frameIndex, *self.m_outputBoundary);
            const bool beforeRecovery = !self.m_recoveryFrame || after(*self.m_recoveryFrame, decoded.frameIndex);
            if (afterBoundary && beforeRecovery) {
                AImage_delete(image);
                continue;
            }
        }
        if (!CheckMedia(AImage_getHardwareBuffer(image, &decoded.buffer), "AImage_getHardwareBuffer")) {
            AImage_delete(image);
            continue;
        }
        self.m_imageQueue.push_back(decoded);
        while (self.m_imageQueue.size() > 3) {
            AImage_delete(self.m_imageQueue.front().image);
            self.m_imageQueue.pop_front();
            ++self.m_droppedFrames;
        }
    }
}

void VideoDecoder::LogStats() {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - m_statsTime).count();
    if (elapsed < 5.0) {
        return;
    }
    if (m_waitStarted) {
        m_keyWaitMs += std::chrono::duration<double, std::milli>(now - *m_waitStarted).count();
        m_waitStarted = now;
    }
    std::lock_guard lock(m_mutex);
    WNVR_LOGI("AV1 キーフレーム待機: keyWait=%.2f ms", m_keyWaitMs);
    m_keyWaitMs = 0;
    WNVR_LOGI("AV1 デコード: input=%" PRIu64 ", decoded=%" PRIu64 " (%.2f fps), "
        "decode 平均=%.2f / 最大=%.2f ms, timed=%" PRIu64 ", inputDrop=%" PRIu64 ", keyWaitDrop=%" PRIu64,
        m_inputs, m_decoded, static_cast<double>(m_decoded) / elapsed,
        m_timedFrames ? m_totalMs / static_cast<double>(m_timedFrames) : 0.0, m_maxMs,
        m_timedFrames, m_inputDrops, m_keyFrameDrops);
    m_statsTime = now;
    m_inputs = m_decoded = m_timedFrames = m_inputDrops = m_keyFrameDrops = 0;
    m_totalMs = m_maxMs = 0;
}

}
