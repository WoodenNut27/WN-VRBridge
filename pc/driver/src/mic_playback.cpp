#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "mic_playback.h"
#include "driver_log.h"

#include <wnvr/win32/audio_devices.h>
#include <wnvr/win32/platform.h>

#include <algorithm>
#include <cstring>
#include <exception>

namespace wnvr::driver {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kDefaultSampleRate = 48000;
constexpr uint32_t kMillisecondsPerSecond = 1000;
constexpr uint32_t kTargetPaddingMs = 20;
constexpr WORD kMonoChannels = 1;
constexpr WORD kBitsPerSample = 16;
constexpr WORD kBytesPerSample = sizeof(int16_t);
constexpr REFERENCE_TIME kPlaybackBufferDuration = 1000000;
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr auto kMissingRetryInterval = std::chrono::seconds(5);
constexpr auto kFailureRetryInterval = std::chrono::seconds(1);
constexpr auto kRecentAudioInterval = std::chrono::seconds(1);
constexpr auto kReportInterval = std::chrono::seconds(5);

}

MicPlayback::~MicPlayback() {
    SetEnabled(false);
}

void MicPlayback::SetEnabled(bool enabled) {
    std::lock_guard controlLock(m_controlMutex);
    if (m_enabled.load() == enabled) {
        return;
    }
    m_enabled = enabled;
    if (!enabled) {
        m_open = false;
        m_thread.request_stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        std::lock_guard lock(m_bufferMutex);
        m_buffer.Reset();
        m_lastReceived = {};
        m_state = PcMicState::Disabled;
        return;
    }
    m_state = PcMicState::Waiting;
    try {
        m_thread = std::jthread([this](std::stop_token token) { Playback(token); });
    } catch (const std::exception& error) {
        m_enabled = false;
        m_state = PcMicState::Disabled;
        Log("マイク再生スレッドを開始できません: %s", error.what());
    }
}

void MicPlayback::Push(const protocol::MicAudioMessage& mic) {
    std::lock_guard lock(m_bufferMutex);
    if (m_enabled.load() && m_open.load()) {
        m_buffer.Push(mic);
        m_lastReceived = Clock::now();
    }
}

long MicPlayback::RunSession(std::stop_token token, const win32::AudioEndpoint& endpoint) {
    uint32_t rate = kDefaultSampleRate;
    {
        std::lock_guard lock(m_bufferMutex);
        if (m_buffer.SampleRate() != 0) {
            rate = m_buffer.SampleRate();
        }
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) { return result; }
    result = enumerator->GetDevice(endpoint.id.c_str(), &device);
    if (FAILED(result)) { return result; }
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(result)) { return result; }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kMonoChannels;
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = kBitsPerSample;
    format.nBlockAlign = kMonoChannels * kBytesPerSample;
    format.nAvgBytesPerSec = rate * format.nBlockAlign;
    // CABLE 側の形式への変換は Windows に任せ、独自の変換による音質差を避ける。
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        kPlaybackBufferDuration, 0, &format, nullptr);
    if (FAILED(result)) { return result; }
    UINT32 capacity = 0;
    result = client->GetBufferSize(&capacity);
    if (FAILED(result)) { return result; }
    result = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(result)) { return result; }
    result = client->Start();
    if (FAILED(result)) { return result; }
    struct ClientScope {
        IAudioClient* client;
        ~ClientScope() { client->Stop(); }
    } scope{client.Get()};
    m_open = true;
    m_state = PcMicState::Waiting;
    auto lastPlayed = Clock::time_point{};
    auto nextReport = Clock::now() + kReportInterval;
    const auto deviceName = win32::ToUtf8(endpoint.friendlyName, win32::InvalidText::Replace);
    std::vector<int16_t> samples(capacity);
    while (!token.stop_requested()) {
        UINT32 padding = 0;
        result = client->GetCurrentPadding(&padding);
        if (FAILED(result)) { return result; }
        const UINT32 target = std::min(capacity, rate * kTargetPaddingMs / kMillisecondsPerSecond);
        if (padding < target) {
            const UINT32 requested = target - padding;
            std::fill_n(samples.data(), requested, int16_t{0});
            size_t count = 0;
            {
                std::lock_guard lock(m_bufferMutex);
                if (m_buffer.SampleRate() != 0 && m_buffer.SampleRate() != rate) {
                    return S_FALSE;
                }
                // 到着が止まった古い声を、再接続の声に持ち越さない。
                if (m_lastReceived != Clock::time_point{} && Clock::now() - m_lastReceived > kRecentAudioInterval) {
                    m_buffer.Reset();
                    m_lastReceived = {};
                }
                count = m_buffer.Pop(samples.data(), requested);
            }
            BYTE* data = nullptr;
            result = render->GetBuffer(requested, &data);
            if (FAILED(result)) { return result; }
            std::memcpy(data, samples.data(), static_cast<size_t>(requested) * sizeof(int16_t));
            result = render->ReleaseBuffer(requested, 0);
            if (FAILED(result)) { return result; }
            if (count != 0) {
                lastPlayed = Clock::now();
            }
        }
        const auto now = Clock::now();
        m_state = lastPlayed != Clock::time_point{} && now - lastPlayed <= kRecentAudioInterval
            ? PcMicState::Playing : PcMicState::Waiting;
        if (now >= nextReport) {
            MicJitterBuffer::Stats stats;
            {
                std::lock_guard lock(m_bufferMutex);
                stats = m_buffer.TakeStats();
            }
            Log("マイク再生5s: packets=%llu missing=%llu dropped=%llu gaps=%llu rate=%u device=%s",
                static_cast<unsigned long long>(stats.packets), static_cast<unsigned long long>(stats.missing),
                static_cast<unsigned long long>(stats.dropped), static_cast<unsigned long long>(stats.gaps),
                rate, deviceName.c_str());
            nextReport = now + kReportInterval;
        }
        win32::WaitForStop(token, kPollInterval);
    }
    return S_OK;
}

void MicPlayback::Playback(std::stop_token token) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        Log("マイク再生の COM 初期化に失敗しました: 0x%08lx", static_cast<unsigned long>(initialized));
        return;
    }
    struct ComScope { ~ComScope() { CoUninitialize(); } } scope;
    std::optional<bool> lastFound;
    while (!token.stop_requested()) {
        auto retry = std::chrono::milliseconds(kFailureRetryInterval);
        try {
            const auto endpoint = win32::FindRenderEndpoint(win32::kVbCableRenderName);
            if (!lastFound || *lastFound != endpoint.has_value()) {
                Log("マイク再生: %s", endpoint ? "VB-CABLE が見つかりました" : "VB-CABLE が見つかりません");
                lastFound = endpoint.has_value();
            }
            if (!endpoint) {
                m_state = PcMicState::CableMissing;
                retry = kMissingRetryInterval;
            } else {
                const HRESULT result = RunSession(token, *endpoint);
                if (FAILED(result)) {
                    Log("マイク再生に失敗しました。再試行します: 0x%08lx", static_cast<unsigned long>(result));
                } else if (result == S_FALSE) {
                    // 新しいレートのパケットを保ったまま、待たずに開き直す。
                    m_open = false;
                    m_state = PcMicState::Waiting;
                    continue;
                }
            }
        } catch (const std::exception& error) {
            Log("マイク再生に失敗しました。再試行します: %s", error.what());
        }
        m_open = false;
        if (m_state.load() != PcMicState::CableMissing) {
            m_state = PcMicState::Waiting;
        }
        win32::WaitForStop(token, retry);
    }
    m_open = false;
}

}
