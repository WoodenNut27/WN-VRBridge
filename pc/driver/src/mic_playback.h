#pragma once

#include "mic_jitter_buffer.h"

#include <wnvr/host_ipc/host_ipc.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stop_token>
#include <thread>
namespace wnvr::win32 { struct AudioEndpoint; }

namespace wnvr::driver {

using ipc::PcMicState;

class MicPlayback {
public:
    ~MicPlayback();
    void SetEnabled(bool enabled);
    void Push(const protocol::MicAudioMessage& mic);
    PcMicState State() const { return m_state.load(); }
    bool WantsAudio() const { return m_enabled.load() && m_open.load(); }

private:
    void Playback(std::stop_token token);
    long RunSession(std::stop_token token, const win32::AudioEndpoint& endpoint);
    std::mutex m_controlMutex;
    std::mutex m_bufferMutex;
    std::jthread m_thread;
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_open{false};
    std::atomic<PcMicState> m_state{PcMicState::Disabled};
    MicJitterBuffer m_buffer;
    std::chrono::steady_clock::time_point m_lastReceived{};
};

}
