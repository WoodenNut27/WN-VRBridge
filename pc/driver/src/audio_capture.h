#pragma once

#include <wnvr/protocol/messages.h>

#include <mutex>
#include <thread>

namespace wnvr::driver {

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // 録音スレッドから呼ばれる。
    virtual void OnAudio(const protocol::AudioMessage& audio) = 0;
};

// PC で鳴っている音（既定の再生デバイスのループバック）を録り、16 bit・ステレオの小さなパケットにして渡す。
// 再生デバイスが変わったり一時的に使えなくなったりしたときは、自動でやり直す。
class AudioCapture {
public:
    explicit AudioCapture(AudioSink& sink);
    ~AudioCapture();
    void Start();
    void Stop();

private:
    void Capture(std::stop_token token);

    AudioSink& m_sink;
    std::mutex m_mutex;
    std::jthread m_thread;
};

}
