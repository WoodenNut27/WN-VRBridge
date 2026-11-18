#include "video_encoder.h"

#include "driver_log.h"
#include "stats_collector.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <nvEncodeAPI.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace wnvr::driver {
namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

// 入力テクスチャの数と、エンコード待ちにためておけるフレームの数の上限。
constexpr size_t kMaxInputs = 3;
constexpr auto kStatsLogInterval = std::chrono::seconds(5);

NV_ENC_BUFFER_FORMAT BufferFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return NV_ENC_BUFFER_FORMAT_ABGR;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return NV_ENC_BUFFER_FORMAT_ARGB;
    default:
        return NV_ENC_BUFFER_FORMAT_UNDEFINED;
    }
}

}

struct VideoEncoder::Impl {
    struct Input {
        ComPtr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registered = nullptr;
        NV_ENC_INPUT_PTR mapped = nullptr;
        bool busy = false;
    };
    struct Job {
        size_t textureIndex;
        uint64_t sourceFrameId;
        protocol::FramePose pose;
        FrameTiming timing;
    };

    ~Impl() {
        Shutdown();
    }

    bool Check(NVENCSTATUS status, const char* operation) const {
        if (status == NV_ENC_SUCCESS) {
            return true;
        }
        const char* detail = m_encoder && m_api.nvEncGetLastErrorString
            ? m_api.nvEncGetLastErrorString(m_encoder) : nullptr;
        Log("NVENC %s に失敗しました: status=%d, %s", operation, static_cast<int>(status), detail ? detail : "");
        return false;
    }

    // 固定ビットレート（CBR）の設定。バッファを 1 フレーム分にして、フレームごとの大きさをそろえる
    // （大きなフレームが続くと、送信と受信の時間が延びて遅延が揺れる）。
    static void ApplyBitrate(NV_ENC_RC_PARAMS& rc, uint32_t mbps, uint32_t frameRate) {
        rc.averageBitRate = mbps * 1000000u;
        rc.maxBitRate = rc.averageBitRate;
        rc.vbvBufferSize = rc.averageBitRate / frameRate;
        rc.vbvInitialDelay = rc.vbvBufferSize;
    }

    // NVIDIA のドライバに入っている NVENC の DLL を読み込み、関数の一覧を受け取る。
    bool LoadApi() {
        m_library = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!m_library) {
            Log("nvEncodeAPI64.dll を読み込めません: %lu", GetLastError());
            return false;
        }
        using GetMaxVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);
        using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
        // FARPROC から異なる関数型へのキャストによる MSVC の警告を避ける。
        const auto versionAddress = GetProcAddress(m_library, "NvEncodeAPIGetMaxSupportedVersion");
        const auto createAddress = GetProcAddress(m_library, "NvEncodeAPICreateInstance");
        GetMaxVersion getMaxVersion = nullptr;
        CreateInstance createInstance = nullptr;
        static_assert(sizeof(getMaxVersion) == sizeof(versionAddress));
        static_assert(sizeof(createInstance) == sizeof(createAddress));
        std::memcpy(&getMaxVersion, &versionAddress, sizeof(getMaxVersion));
        std::memcpy(&createInstance, &createAddress, sizeof(createInstance));
        if (!getMaxVersion || !createInstance) {
            Log("NVENC の公開関数が見つかりません。");
            return false;
        }
        uint32_t version = 0;
        if (!Check(getMaxVersion(&version), "GetMaxSupportedVersion")) {
            return false;
        }
        // 照会値は major << 4 | minor。NVENCAPI_VERSION は別のビット配置を使う。
        constexpr uint32_t kRequiredVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (version < kRequiredVersion) {
            Log("NVENC ドライバの API 版が不足しています: 対応=%u.%u, 必要=%u.%u",
                version >> 4, version & 15, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
            return false;
        }
        m_api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        return Check(createInstance(&m_api), "CreateInstance");
    }

    // 描画に使っている D3D11 デバイスでセッションを開き、AV1 に対応した GPU かを確かめる。
    bool OpenSession() {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
        session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        session.device = m_device.Get();
        session.apiVersion = NVENCAPI_VERSION;
        if (!Check(m_api.nvEncOpenEncodeSessionEx(&session, &m_encoder), "OpenEncodeSessionEx")) {
            return false;
        }
        uint32_t guidCount = 0;
        if (!Check(m_api.nvEncGetEncodeGUIDCount(m_encoder, &guidCount), "GetEncodeGUIDCount")) {
            return false;
        }
        std::vector<GUID> guids(guidCount);
        if (!Check(m_api.nvEncGetEncodeGUIDs(m_encoder, guids.data(), guidCount, &guidCount), "GetEncodeGUIDs")) {
            return false;
        }
        if (std::none_of(guids.begin(), guids.end(), [](const GUID& guid) { return guid == NV_ENC_CODEC_AV1_GUID; })) {
            Log("選択した GPU は NVENC の AV1 に対応していません。");
            return false;
        }
        return true;
    }

    // 遅延を最小にする設定で AV1 のエンコーダーを初期化する。
    bool ConfigureEncoder(const Config& config) {
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        if (!Check(m_api.nvEncGetEncodePresetConfigEx(m_encoder, NV_ENC_CODEC_AV1_GUID, NV_ENC_PRESET_P2_GUID,
                NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset), "GetEncodePresetConfigEx")) {
            return false;
        }
        auto& encodeConfig = preset.presetCfg;
        // キーフレームは定期的には入れず、接続時と Quest から要求されたときだけ入れる
        // （キーフレームは大きいので、入れるたびに遅延が跳ねる）。
        encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
        encodeConfig.frameIntervalP = 1;
        auto& rc = encodeConfig.rcParams;
        rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        ApplyBitrate(rc, config.bitrateMbps, config.frameRate);
        // 単一の出力バッファを同期的に回収するため、先読みと並べ替えを無効にする。
        rc.enableLookahead = 0;
        rc.lookaheadDepth = 0;
        rc.zeroReorderDelay = 1;
        auto& av1 = encodeConfig.encodeCodecConfig.av1Config;
        av1.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        av1.repeatSeqHdr = 1;
        av1.outputAnnexBFormat = 0;
        NV_ENC_INITIALIZE_PARAMS initialize{};
        initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
        initialize.encodeGUID = NV_ENC_CODEC_AV1_GUID;
        initialize.presetGUID = NV_ENC_PRESET_P2_GUID;
        initialize.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        initialize.encodeWidth = initialize.darWidth = initialize.maxEncodeWidth = config.width;
        initialize.encodeHeight = initialize.darHeight = initialize.maxEncodeHeight = config.height;
        initialize.frameRateNum = config.frameRate;
        initialize.frameRateDen = 1;
        initialize.enablePTD = 1;
        initialize.enableEncodeAsync = 0;
        initialize.encodeConfig = &encodeConfig;
        if (!Check(m_api.nvEncInitializeEncoder(m_encoder, &initialize), "InitializeEncoder")) {
            return false;
        }
        // ビットレートを変えるとき（ReconfigureEncoder）に同じ設定を渡すので、写しを持っておく。
        m_initialize = initialize;
        m_encodeConfig = encodeConfig;
        m_initialize.encodeConfig = &m_encodeConfig;
        return true;
    }

    // エンコードの入力にするテクスチャを NVENC に登録し、出力用のバッファを作る。
    bool RegisterInputs(const std::vector<void*>& textures, const Config& config) {
        m_inputs.resize(textures.size());
        DXGI_FORMAT firstFormat = DXGI_FORMAT_UNKNOWN;
        for (size_t index = 0; index < textures.size(); ++index) {
            if (!textures[index]) {
                Log("NVENC 入力テクスチャが null です。");
                return false;
            }
            auto& input = m_inputs[index];
            input.texture = static_cast<ID3D11Texture2D*>(textures[index]);
            D3D11_TEXTURE2D_DESC desc{};
            input.texture->GetDesc(&desc);
            ComPtr<ID3D11Device> textureDevice;
            input.texture->GetDevice(&textureDevice);
            m_format = BufferFormat(desc.Format);
            if (index == 0) {
                firstFormat = desc.Format;
            }
            if (m_format == NV_ENC_BUFFER_FORMAT_UNDEFINED || desc.Format != firstFormat
                || desc.Width != config.width || desc.Height != config.height || desc.MipLevels != 1
                || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || textureDevice.Get() != m_device.Get()) {
                Log("NVENC に登録できない入力テクスチャです: index=%zu, DXGI format=%u", index,
                    static_cast<unsigned>(desc.Format));
                return false;
            }
            NV_ENC_REGISTER_RESOURCE resource{};
            resource.version = NV_ENC_REGISTER_RESOURCE_VER;
            resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
            resource.resourceToRegister = input.texture.Get();
            resource.width = config.width;
            resource.height = config.height;
            resource.bufferFormat = m_format;
            resource.bufferUsage = NV_ENC_INPUT_IMAGE;
            if (!Check(m_api.nvEncRegisterResource(m_encoder, &resource), "RegisterResource")) {
                return false;
            }
            input.registered = resource.registeredResource;
        }
        NV_ENC_CREATE_BITSTREAM_BUFFER output{};
        output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (!Check(m_api.nvEncCreateBitstreamBuffer(m_encoder, &output), "CreateBitstreamBuffer")) {
            return false;
        }
        m_output = output.bitstreamBuffer;
        return true;
    }

    bool Initialize(void* devicePointer, const std::vector<void*>& textures, const Config& config, EncodedFrameSink* sink) {
        if (!devicePointer || textures.empty() || textures.size() > kMaxInputs || config.width == 0 || config.height == 0
            || config.frameRate == 0 || config.bitrateMbps < 1 || config.bitrateMbps > 1000) {
            Log("NVENC の設定または入力テクスチャが不正です。");
            return false;
        }
        m_config = config;
        m_requestedBitrate = config.bitrateMbps;
        m_attemptedBitrate = config.bitrateMbps;
        m_sink = sink;
        m_device = static_cast<ID3D11Device*>(devicePointer);
        // エンコードのスレッドと SteamVR の描画のスレッドが、同じデバイスコンテキストを使う。
        ComPtr<ID3D11DeviceContext> context;
        m_device->GetImmediateContext(&context);
        ComPtr<ID3D11Multithread> multithread;
        if (FAILED(context.As(&multithread))) {
            Log("D3D11 のマルチスレッド保護を取得できません。");
            return false;
        }
        multithread->SetMultithreadProtected(TRUE);
        if (!LoadApi() || !OpenSession() || !ConfigureEncoder(config) || !RegisterInputs(textures, config)) {
            return false;
        }
        m_statsTime = Clock::now();
        m_keyFrame = true;
        {
            std::lock_guard lock(m_mutex);
            m_accepting = true;
        }
        m_thread = std::jthread([this](std::stop_token token) { Run(token); });
        Log("NVENC AV1 を開始しました: %ux%u, %u fps, %u Mbps", config.width, config.height,
            config.frameRate, config.bitrateMbps);
        return true;
    }

    // 設定画面でビットレートが変わっていたら、エンコーダーを止めずに設定し直す。
    void ApplyRequestedBitrate() {
        const uint32_t requested = m_requestedBitrate.load();
        if (requested == m_attemptedBitrate) {
            return;
        }
        // 失敗しても、同じ値で毎フレームやり直さない。
        m_attemptedBitrate = requested;
        if (requested == m_config.bitrateMbps) {
            return;
        }
        auto config = m_encodeConfig;
        ApplyBitrate(config.rcParams, requested, m_config.frameRate);
        NV_ENC_RECONFIGURE_PARAMS reconfigure{};
        reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        reconfigure.reInitEncodeParams = m_initialize;
        reconfigure.reInitEncodeParams.encodeConfig = &config;
        reconfigure.resetEncoder = 1;
        reconfigure.forceIDR = 1;
        if (Check(m_api.nvEncReconfigureEncoder(m_encoder, &reconfigure), "ReconfigureEncoder")) {
            m_encodeConfig = config;
            m_config.bitrateMbps = requested;
            Log("NVENC のビットレートを %u Mbps に変更しました", requested);
        } else {
            Log("NVENC のビットレートは %u Mbps のまま続けます", m_config.bitrateMbps);
        }
    }

    bool Encode(const Job& job) {
        ApplyRequestedBitrate();
        const auto start = Clock::now();
        NV_ENC_MAP_INPUT_RESOURCE mapped{};
        mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapped.registeredResource = m_inputs[job.textureIndex].registered;
        if (!Check(m_api.nvEncMapInputResource(m_encoder, &mapped), "MapInputResource")) {
            std::lock_guard lock(m_mutex);
            m_inputs[job.textureIndex].busy = false;
            return false;
        }
        m_inputs[job.textureIndex].mapped = mapped.mappedResource;
        EncodedFrame frame;
        frame.frameIndex = m_nextFrame++;
        frame.sourceFrameId = job.sourceFrameId;
        frame.width = m_config.width;
        frame.height = m_config.height;
        frame.pose = job.pose;
        NV_ENC_PIC_PARAMS picture{};
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.inputBuffer = mapped.mappedResource;
        picture.bufferFmt = m_format;
        picture.inputWidth = m_config.width;
        picture.inputHeight = m_config.height;
        picture.outputBitstream = m_output;
        picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        picture.inputTimeStamp = frame.frameIndex;
        if (m_keyFrame.exchange(false)) {
            picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }
        bool success = Check(m_api.nvEncEncodePicture(m_encoder, &picture), "EncodePicture");
        if (success) {
            NV_ENC_LOCK_BITSTREAM output{};
            output.version = NV_ENC_LOCK_BITSTREAM_VER;
            output.outputBitstream = m_output;
            output.doNotWait = 0;
            success = Check(m_api.nvEncLockBitstream(m_encoder, &output), "LockBitstream");
            if (success) {
                // 例外時にも GPU 資源のロックを残さない。
                try {
                    const auto* bytes = static_cast<const uint8_t*>(output.bitstreamBufferPtr);
                    frame.data.assign(bytes, bytes + output.bitstreamSizeInBytes);
                    frame.keyFrame = output.pictureType == NV_ENC_PIC_TYPE_IDR;
                } catch (const std::exception& error) {
                    Log("エンコード結果をコピーできません: %s", error.what());
                    success = false;
                }
                success = Check(m_api.nvEncUnlockBitstream(m_encoder, m_output), "UnlockBitstream") && success;
            }
        }
        const bool unmapped = Check(m_api.nvEncUnmapInputResource(m_encoder, mapped.mappedResource), "UnmapInputResource");
        {
            std::lock_guard lock(m_mutex);
            // unmap 失敗時は Present がこの入力を上書きしないよう保持する。
            m_inputs[job.textureIndex].busy = !unmapped;
            if (unmapped) {
                m_inputs[job.textureIndex].mapped = nullptr;
            }
        }
        if (!success || !unmapped) {
            return false;
        }
        frame.timing = job.timing;
        frame.timing.encoded = Clock::now();
        frame.encodeMs = std::chrono::duration<double, std::milli>(frame.timing.encoded - start).count();
        if (m_config.statsCollector) {
            m_config.statsCollector->OnEncodedFrame(frame.encodeMs);
        }
        ++m_statsFrames;
        m_statsBytes += frame.data.size();
        m_statsEncodeMs += frame.encodeMs;
        m_statsMaxMs = std::max(m_statsMaxMs, frame.encodeMs);
        m_statsKeyFrames += frame.keyFrame ? 1u : 0u;
        if (m_sink) {
            m_sink->OnEncodedFrame(frame);
        }
        return true;
    }

    void LogStats() {
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - m_statsTime).count();
        if (elapsed < std::chrono::duration<double>(kStatsLogInterval).count()) {
            return;
        }
        const double frames = static_cast<double>(m_statsFrames);
        const double bytes = static_cast<double>(m_statsBytes);
        Log("NVENC 統計: %.2f fps, 平均 %.2f KB, %.2f Mbps, encode 平均 %.2f / 最大 %.2f ms, 破棄=%llu, key=%llu",
            frames / elapsed, frames > 0 ? bytes / frames / 1000.0 : 0.0, bytes * 8 / elapsed / 1000000.0,
            frames > 0 ? m_statsEncodeMs / frames : 0.0, m_statsMaxMs,
            static_cast<unsigned long long>(m_dropped.exchange(0)), static_cast<unsigned long long>(m_statsKeyFrames));
        m_statsTime = now;
        m_statsFrames = m_statsBytes = m_statsKeyFrames = 0;
        m_statsEncodeMs = m_statsMaxMs = 0;
    }

    void Run(std::stop_token token) {
        try {
            for (;;) {
                Job job{};
                {
                    std::unique_lock lock(m_mutex);
                    m_condition.wait_for(lock, std::chrono::seconds(1), [this, token] {
                        return token.stop_requested() || !m_queue.empty();
                    });
                    if (m_queue.empty()) {
                        if (token.stop_requested()) {
                            break;
                        }
                        lock.unlock();
                        LogStats();
                        continue;
                    }
                    job = m_queue.front();
                    m_queue.pop_front();
                }
                if (!Encode(job)) {
                    ++m_dropped;
                    break;
                }
                LogStats();
            }
        } catch (const std::exception& error) {
            Log("NVENC スレッドを停止します: %s", error.what());
            ++m_dropped;
        } catch (...) {
            Log("NVENC スレッドで予期しない例外が発生しました。");
            ++m_dropped;
        }
        std::lock_guard lock(m_mutex);
        m_accepting = false;
        m_dropped += m_queue.size();
        for (const auto& job : m_queue) {
            m_inputs[job.textureIndex].busy = false;
        }
        m_queue.clear();
    }

    void Shutdown() {
        {
            std::lock_guard lock(m_mutex);
            m_accepting = false;
        }
        if (m_thread.joinable()) {
            m_thread.request_stop();
            m_condition.notify_all();
            m_thread.join();
        }
        if (m_encoder) {
            for (auto& input : m_inputs) {
                // エラー経路で unmap に失敗していた場合も、登録解除前に再試行する。
                if (input.mapped) {
                    Check(m_api.nvEncUnmapInputResource(m_encoder, input.mapped), "UnmapInputResource (Shutdown)");
                }
                if (input.registered) {
                    Check(m_api.nvEncUnregisterResource(m_encoder, input.registered), "UnregisterResource");
                }
            }
            if (m_output) {
                Check(m_api.nvEncDestroyBitstreamBuffer(m_encoder, m_output), "DestroyBitstreamBuffer");
            }
            Check(m_api.nvEncDestroyEncoder(m_encoder), "DestroyEncoder");
            m_encoder = nullptr;
        }
        {
            std::lock_guard lock(m_mutex);
            m_inputs.clear();
        }
        m_output = nullptr;
        m_device.Reset();
        m_sink = nullptr;
        m_api = {};
        if (m_library) {
            FreeLibrary(m_library);
            m_library = nullptr;
        }
        m_nextFrame = 0;
        m_statsFrames = m_statsBytes = m_statsKeyFrames = 0;
        m_statsEncodeMs = m_statsMaxMs = 0;
        m_dropped = 0;
    }

    Config m_config;
    NV_ENC_INITIALIZE_PARAMS m_initialize{};
    NV_ENC_CONFIG m_encodeConfig{};
    std::atomic<uint32_t> m_requestedBitrate{80};
    uint32_t m_attemptedBitrate = 80;
    EncodedFrameSink* m_sink = nullptr;
    HMODULE m_library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_api{};
    void* m_encoder = nullptr;
    ComPtr<ID3D11Device> m_device;
    std::vector<Input> m_inputs;
    NV_ENC_BUFFER_FORMAT m_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
    NV_ENC_OUTPUT_PTR m_output = nullptr;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<Job> m_queue;
    bool m_accepting = false;
    std::jthread m_thread;
    std::atomic<bool> m_keyFrame{true};
    std::atomic<uint64_t> m_dropped{0};
    uint64_t m_nextFrame = 0;
    Clock::time_point m_statsTime{};
    uint64_t m_statsFrames = 0;
    uint64_t m_statsBytes = 0;
    uint64_t m_statsKeyFrames = 0;
    double m_statsEncodeMs = 0;
    double m_statsMaxMs = 0;
};

VideoEncoder::VideoEncoder() : m_impl(std::make_unique<Impl>()) {}

VideoEncoder::~VideoEncoder() {
    Shutdown();
}

bool VideoEncoder::Initialize(void* device, const std::vector<void*>& textures, const Config& config, EncodedFrameSink* sink) {
    Shutdown();
    try {
        if (m_impl->Initialize(device, textures, config, sink)) {
            return true;
        }
    } catch (const std::exception& error) {
        Log("NVENC の初期化に失敗しました: %s", error.what());
    }
    Shutdown();
    return false;
}

void VideoEncoder::Shutdown() {
    m_impl->Shutdown();
}

bool VideoEncoder::Submit(size_t textureIndex, uint64_t sourceFrameId, const protocol::FramePose& pose,
    const FrameTiming& timing) {
    std::lock_guard lock(m_impl->m_mutex);
    if (!m_impl->m_accepting || textureIndex >= m_impl->m_inputs.size()
        || m_impl->m_inputs[textureIndex].busy || m_impl->m_queue.size() >= kMaxInputs) {
        ++m_impl->m_dropped;
        return false;
    }
    m_impl->m_queue.push_back({textureIndex, sourceFrameId, pose, timing});
    m_impl->m_inputs[textureIndex].busy = true;
    m_impl->m_condition.notify_one();
    return true;
}

bool VideoEncoder::IsBusy(size_t textureIndex) const {
    std::lock_guard lock(m_impl->m_mutex);
    return textureIndex < m_impl->m_inputs.size() && m_impl->m_inputs[textureIndex].busy;
}

void VideoEncoder::SetBitrate(uint32_t mbps) {
    m_impl->m_requestedBitrate = mbps;
}

void VideoEncoder::RequestKeyFrame() {
    m_impl->m_keyFrame = true;
}

void VideoEncoder::RecordDroppedFrame() {
    ++m_impl->m_dropped;
}

}
