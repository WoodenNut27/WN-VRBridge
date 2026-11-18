#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "audio_capture.h"
#include "driver_log.h"

#include <wnvr/win32/platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>

namespace wnvr::driver {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

// 1 パケットに入れるフレーム数（48kHz で 5ms）。小さいほど遅れは減るが、パケットの数が増える。
constexpr UINT32 kPacketFrames = 240;
static_assert(kPacketFrames <= protocol::kMaxAudioFrames);
// WASAPI に頼む録音バッファの長さ（100ns 単位で 200ms）。
constexpr REFERENCE_TIME kCaptureBufferDuration = 2000000;
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kReportInterval = std::chrono::seconds(5);
constexpr auto kRetryInterval = std::chrono::seconds(1);
// 対応していない形式のデバイスは、すぐにやり直しても変わらないので長めに待つ。
constexpr auto kUnsupportedRetryInterval = std::chrono::seconds(5);

// 再生デバイスのミックス形式のうち、変換に必要な部分。
struct MixFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    bool floatingPoint = false;
    uint16_t blockAlign = 0; // 1 フレーム（全チャンネル分）のバイト数
    bool operator==(const MixFormat&) const = default;
};

// 形式を読み取る。32 bit の浮動小数点か 16 bit の整数で、変換できる形なら true。
bool ReadMixFormat(const WAVEFORMATEX& format, MixFormat& result) {
    bool floatingPoint = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool pcm = format.wFormatTag == WAVE_FORMAT_PCM;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format);
        floatingPoint = IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        pcm = IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)
            && extended->Samples.wValidBitsPerSample == 16;
    }
    result = {format.nSamplesPerSec, format.nChannels, format.wBitsPerSample, floatingPoint, format.nBlockAlign};
    const bool supportedSample = (floatingPoint && result.bits == 32) || (pcm && result.bits == 16);
    return supportedSample && result.channels != 0 && result.sampleRate >= 8000 && result.sampleRate <= 192000
        && result.blockAlign == static_cast<size_t>(result.channels) * (result.bits / 8);
}

void LogFormat(const char* label, const MixFormat& format) {
    Log("%s: rate=%u, channels=%u, bits=%u, float=%d", label, format.sampleRate,
        static_cast<unsigned>(format.channels), static_cast<unsigned>(format.bits), format.floatingPoint ? 1 : 0);
}

int16_t ReadSample(const BYTE* source, bool floatingPoint) {
    if (!floatingPoint) {
        int16_t sample = 0;
        std::memcpy(&sample, source, sizeof(sample));
        return sample;
    }
    float value = 0;
    std::memcpy(&value, source, sizeof(value));
    // NaN は丸められないため無音にし、無限大は端に収める。
    value = std::isnan(value) ? 0.0f : std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(value * 32767.0f));
}

// 録音したデータの firstFrame から count フレームを、16 bit・ステレオにして samples に入れる。
// モノラルは左右に同じ音を入れ、3 チャンネル以上は前の左右だけを使う。
void ConvertFrames(const BYTE* data, const MixFormat& format, size_t firstFrame, size_t count,
    std::vector<int16_t>& samples) {
    const size_t bytesPerSample = format.bits / 8;
    for (size_t frame = 0; frame < count; ++frame) {
        for (size_t channel = 0; channel < 2; ++channel) {
            const size_t source = (firstFrame + frame) * format.blockAlign
                + (format.channels == 1 ? 0 : channel) * bytesPerSample;
            samples[frame * 2 + channel] = ReadSample(data + source, format.floatingPoint);
        }
    }
}

// 取得したバッファを、途中で抜けても必ず返す。
struct CaptureBuffer {
    IAudioCaptureClient* client;
    UINT32 frames;
    ~CaptureBuffer() {
        if (client != nullptr) {
            client->ReleaseBuffer(frames);
        }
    }
};

// 録音のやり直しをまたいで続く値。
struct CaptureProgress {
    uint64_t frameIndex = 0; // 送ったフレームの通し番号（Quest が抜けを検出するのに使う）
    uint64_t capturedFrames = 0;
    uint64_t sentPackets = 0;
    MixFormat lastLoggedFormat;
    Clock::time_point nextReport = Clock::now() + kReportInterval;
};

// 既定の再生デバイスを開いて、止められるか失敗するまで録り続ける。
// 対応していない形式のときは unsupported を true にして失敗を返す。
HRESULT RunSession(std::stop_token token, AudioSink& sink, CaptureProgress& progress, bool& unsupported) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) { return result; }
    result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(result)) { return result; }
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(result)) { return result; }
    WAVEFORMATEX* rawFormat = nullptr;
    result = client->GetMixFormat(&rawFormat);
    if (FAILED(result)) { return result; }
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(rawFormat, CoTaskMemFree);
    MixFormat current;
    if (!ReadMixFormat(*format, current)) {
        unsupported = true;
        LogFormat("未対応の録音形式", current);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
        kCaptureBufferDuration, 0, format.get(), nullptr);
    if (FAILED(result)) { return result; }
    result = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(result)) { return result; }
    result = client->Start();
    if (FAILED(result)) { return result; }
    struct ClientScope {
        IAudioClient* client;
        ~ClientScope() { client->Stop(); }
    } clientScope{client.Get()};
    while (!token.stop_requested()) {
        UINT32 packetFrames = 0;
        result = capture->GetNextPacketSize(&packetFrames);
        if (FAILED(result)) { return result; }
        while (packetFrames != 0 && !token.stop_requested()) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) { return result; }
            CaptureBuffer buffer{capture.Get(), frames};
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
            for (UINT32 offset = 0; offset < frames && !token.stop_requested();) {
                const auto count = std::min(kPacketFrames, frames - offset);
                protocol::AudioMessage audio;
                audio.frameIndex = progress.frameIndex;
                audio.sampleRate = current.sampleRate;
                // 無音のときも、0 で埋めたパケットを同じ間隔で送る（Quest の再生を途切れさせない）。
                audio.samples.resize(static_cast<size_t>(count) * 2);
                if (!silent) {
                    ConvertFrames(data, current, offset, count, audio.samples);
                }
                sink.OnAudio(audio);
                progress.frameIndex += count;
                progress.capturedFrames += count;
                ++progress.sentPackets;
                offset += count;
            }
            result = capture->ReleaseBuffer(frames);
            buffer.client = nullptr;
            if (FAILED(result)) { return result; }
            result = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(result)) { return result; }
        }
        if (Clock::now() >= progress.nextReport) {
            if (current != progress.lastLoggedFormat) {
                LogFormat("録音形式", current);
                progress.lastLoggedFormat = current;
            }
            Log("音声録音5s: frames=%llu, packets=%llu", static_cast<unsigned long long>(progress.capturedFrames),
                static_cast<unsigned long long>(progress.sentPackets));
            progress.capturedFrames = progress.sentPackets = 0;
            progress.nextReport = Clock::now() + kReportInterval;
        }
        win32::WaitForStop(token, kPollInterval);
    }
    return S_OK;
}

}

AudioCapture::AudioCapture(AudioSink& sink) : m_sink(sink) {}

AudioCapture::~AudioCapture() {
    Stop();
}

void AudioCapture::Start() {
    std::lock_guard lock(m_mutex);
    if (m_thread.joinable()) {
        return;
    }
    try {
        m_thread = std::jthread([this](std::stop_token token) { Capture(token); });
    } catch (const std::exception& error) {
        Log("録音スレッドの開始に失敗しました: %s", error.what());
    }
}

void AudioCapture::Stop() {
    std::lock_guard lock(m_mutex);
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
}

void AudioCapture::Capture(std::stop_token token) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        Log("録音の COM 初期化に失敗しました: 0x%08lx", static_cast<unsigned long>(initialized));
        return;
    }
    struct ComScope {
        ~ComScope() { CoUninitialize(); }
    } comScope;
    CaptureProgress progress;
    while (!token.stop_requested()) {
        bool unsupported = false;
        try {
            const HRESULT result = RunSession(token, m_sink, progress, unsupported);
            if (FAILED(result) && !unsupported) {
                Log("録音に失敗しました。再試行します: 0x%08lx", static_cast<unsigned long>(result));
            }
        } catch (const std::exception& error) {
            Log("録音処理に失敗しました。再試行します: %s", error.what());
        }
        win32::WaitForStop(token, unsupported ? std::chrono::milliseconds(kUnsupportedRetryInterval)
            : std::chrono::milliseconds(kRetryInterval));
    }
}

}
