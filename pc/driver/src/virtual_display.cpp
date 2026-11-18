#include "virtual_display.h"

#include "driver_log.h"
#include "frame_dump.h"
#include "ivf_writer.h"
#include "stats_collector.h"
#include "video_encoder.h"

#include <wnvr/win32/platform.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wnvr::driver {
namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

// エンコーダーに渡すテクスチャの数。エンコード中の 1 枚を待たずに、次のフレームを別の 1 枚へ写せる。
constexpr size_t kEncoderTextureCount = 3;
// SteamVR が使い回す共有テクスチャを覚えておく数。
constexpr size_t kMaxSharedTextures = 8;
constexpr DWORD kAcquireTimeoutMs = 100;
constexpr auto kCopyTimeout = std::chrono::milliseconds(50);
constexpr auto kLogInterval = std::chrono::seconds(5);
// dumpFrame が有効なとき、起動から何枚目を保存するか（最初の数秒は画面が安定しない）。
constexpr uint64_t kDumpFrameNumber = 300;

// 仮想の垂直同期の速さを、Quest にたまっている画像の数で合わせる。
constexpr double kTargetQueueDepth = 1.0;   // 目標は 1 枚
constexpr double kFeedbackSmoothing = 0.2;   // 報告のばらつきをならす係数
constexpr double kFeedbackGain = 0.002;      // 1 枚のずれあたりの速さの変化
constexpr double kMaxRateAdjustment = 0.005; // 速さを変える上限（±0.5%）

uint64_t PackLuid(const LUID& luid) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
}

}

struct VirtualDisplay::Impl : EncodedFrameSink {
    // 周期の変更前にも旧周期で進め、時刻と番号の連続性を維持する。
    void AdvanceVsync(Clock::time_point now) {
        const auto period = std::max(Clock::duration{1},
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(m_period)));
        const auto periods = (now - m_lastVsync) / period;
        if (periods > 0) {
            m_lastVsync += period * periods;
            m_vsyncCounter += static_cast<uint64_t>(periods);
        }
    }

    void OnEncodedFrame(const EncodedFrame& frame) override {
        if (m_config.dumpVideoSeconds > 0) {
            m_ivfWriter.OnEncodedFrame(frame);
        }
        // 解除は進行中の呼び出しも待ち、送信先の停止後に参照を残さない。
        std::lock_guard lock(m_sinkMutex);
        if (auto* sink = m_frameSink.load()) {
            sink->OnEncodedFrame(frame);
        }
    }

    ~Impl() {
        // コールバック先と D3D11 資源より先にワーカーを停止する。
        m_encoder.Shutdown();
        m_ivfWriter.Close();
    }

    struct SharedTexture {
        vr::SharedTextureHandle_t handle = 0;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> keyedMutex;
        ComPtr<ID3D11ShaderResourceView> srv;
    };

    bool InitializeFoveation() {
        constexpr char kShader[] = R"(
cbuffer FoveationConstants : register(b0) {
    float4 centers[2];
    float4 compressed[2];
    float4 ratios;
    float4 sizes;
};
Texture2D sourceImage : register(t0);
SamplerState linearClamp : register(s0);
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
float InverseAxis(float v, float c0, float c1, float r, float wn, out float footprint) {
    float s = v * wn;
    float a = c0 / r;
    float b = a + c1 - c0;
    footprint = (s < a || s >= b) ? r : 1.0;
    float u = s < a ? s * r : (s < b ? c0 + s - a : c1 + (s - b) * r);
    return saturate(u);
}
float4 PS(float4 position : SV_Position) : SV_Target {
    float2 uv = position.xy / sizes.zw;
    uint eye = uv.x < 0.5 ? 0 : 1;
    float2 v = float2(uv.x * 2.0 - eye, uv.y);
    float4 c = centers[eye];
    float2 footprint;
    float2 u;
    u.x = InverseAxis(v.x, c.x, c.y, ratios.x, compressed[eye].x, footprint.x);
    u.y = InverseAxis(v.y, c.z, c.w, ratios.y, compressed[eye].y, footprint.y);
    float2 offset = 0.25 * footprint / sizes.xy;
    float2 halfTexel = 0.5 / sizes.xy;
    float4 sum = 0.0;
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            float2 sampleUv = clamp(u + float2(x * 2 - 1, y * 2 - 1) * offset,
                halfTexel, 1.0 - halfTexel);
            sampleUv.x = (eye + sampleUv.x) * 0.5;
            sum += sourceImage.SampleLevel(linearClamp, sampleUv, 0);
        }
    }
    return sum * 0.25;
}
)";
        ComPtr<ID3DBlob> vertex;
        ComPtr<ID3DBlob> pixel;
        ComPtr<ID3DBlob> errors;
        HRESULT result = D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr,
            "VS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &vertex, &errors);
        if (SUCCEEDED(result)) {
            errors.Reset();
            result = D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr,
                "PS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &pixel, &errors);
        }
        if (FAILED(result)) {
            Log("Foveation シェーダーをコンパイルできません: 0x%08lx %s",
                static_cast<unsigned long>(result), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return false;
        }
        result = m_device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &m_vertexShader);
        if (SUCCEEDED(result)) {
            result = m_device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &m_pixelShader);
        }
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = static_cast<UINT>(sizeof(FoveationConstants));
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(result)) {
            result = m_device->CreateBuffer(&buffer, nullptr, &m_constants);
        }
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(result)) {
            result = m_device->CreateSamplerState(&sampler, &m_sampler);
        }
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        if (SUCCEEDED(result)) {
            result = m_device->CreateRasterizerState(&rasterizer, &m_rasterizer);
        }
        if (FAILED(result)) {
            Log("Foveation 描画資源を作成できません: 0x%08lx", static_cast<unsigned long>(result));
            return false;
        }
        return true;
    }

    struct FoveationConstants {
        std::array<std::array<float, 4>, 2> centers;
        std::array<std::array<float, 4>, 2> compressed;
        std::array<float, 4> ratios;
        std::array<float, 4> sizes;
    };

    void DrawFoveated(size_t index, ID3D11ShaderResourceView* source) {
        FoveationConstants constants{};
        const std::array eyes{m_foveation.left, m_foveation.right};
        for (size_t eye = 0; eye < eyes.size(); ++eye) {
            const auto& c = eyes[eye];
            constants.centers[eye] = {c.centerX0, c.centerX1, c.centerY0, c.centerY1};
            constants.compressed[eye] = {
                protocol::FoveationCompressedRatio(c.centerX0, c.centerX1, m_foveation.edgeRatioX),
                protocol::FoveationCompressedRatio(c.centerY0, c.centerY1, m_foveation.edgeRatioY), 0, 0};
        }
        constants.ratios = {m_foveation.edgeRatioX, m_foveation.edgeRatioY, 0, 0};
        constants.sizes = {static_cast<float>(m_sourceDesc.Width) * 0.5f, static_cast<float>(m_sourceDesc.Height),
            static_cast<float>(m_textureDesc.Width), static_cast<float>(m_textureDesc.Height)};
        m_context->UpdateSubresource(m_constants.Get(), 0, nullptr, &constants, 0, 0);
        auto* target = m_renderTargets[index].Get();
        auto* buffer = m_constants.Get();
        auto* sampler = m_sampler.Get();
        m_context->OMSetRenderTargets(1, &target, nullptr);
        m_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        m_context->OMSetDepthStencilState(nullptr, 0);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(m_textureDesc.Width),
            static_cast<float>(m_textureDesc.Height), 0, 1};
        m_context->RSSetViewports(1, &viewport);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
        m_context->PSSetConstantBuffers(0, 1, &buffer);
        m_context->PSSetSamplers(0, 1, &sampler);
        m_context->PSSetShaderResources(0, 1, &source);
        m_context->Draw(3, 0);
        // 次のコピーや NVENC の読み取りとバインドを競合させない。
        ID3D11ShaderResourceView* empty = nullptr;
        m_context->PSSetShaderResources(0, 1, &empty);
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    bool PrepareTextures(const D3D11_TEXTURE2D_DESC& sourceDesc) {
        if (m_textures[0] && !m_foveationDirty && m_sourceDesc.Width == sourceDesc.Width
            && m_sourceDesc.Height == sourceDesc.Height && m_sourceDesc.Format == sourceDesc.Format
            && m_sourceDesc.MipLevels == sourceDesc.MipLevels && m_sourceDesc.ArraySize == sourceDesc.ArraySize
            && m_sourceDesc.SampleDesc.Count == sourceDesc.SampleDesc.Count
            && m_sourceDesc.SampleDesc.Quality == sourceDesc.SampleDesc.Quality) {
            return true;
        }
        D3D11_TEXTURE2D_DESC desc = sourceDesc;
        if (m_foveation.enabled) {
            const auto align = [](float value) {
                return (static_cast<UINT>(std::round(value)) + 31u) & ~31u;
            };
            const auto& c = m_foveation.left;
            desc.Width = 2u * align(static_cast<float>(sourceDesc.Width) * 0.5f
                * protocol::FoveationCompressedRatio(c.centerX0, c.centerX1, m_foveation.edgeRatioX));
            desc.Height = align(static_cast<float>(sourceDesc.Height)
                * protocol::FoveationCompressedRatio(c.centerY0, c.centerY1, m_foveation.edgeRatioY));
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.SampleDesc = {1, 0};
        }
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        std::array<ComPtr<ID3D11Texture2D>, kEncoderTextureCount> textures;
        std::array<ComPtr<ID3D11Query>, kEncoderTextureCount> queries;
        std::array<ComPtr<ID3D11RenderTargetView>, kEncoderTextureCount> targets;
        const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
        for (size_t index = 0; index < textures.size(); ++index) {
            HRESULT result = m_device->CreateTexture2D(&desc, nullptr, &textures[index]);
            if (SUCCEEDED(result)) {
                result = m_device->CreateQuery(&queryDesc, &queries[index]);
            }
            if (SUCCEEDED(result) && m_foveation.enabled) {
                result = m_device->CreateRenderTargetView(textures[index].Get(), nullptr, &targets[index]);
            }
            if (FAILED(result)) {
                Log("フレームのコピー先または問い合わせを作成できません: 0x%08lx",
                    static_cast<unsigned long>(result));
                return false;
            }
        }
        m_encoder.Shutdown();
        m_ivfWriter.Close();
        m_encoderReady = false;
        m_pendingTexture.reset();
        m_lastQuery.Reset();
        m_textures = std::move(textures);
        m_queries = std::move(queries);
        m_textureDesc = desc;
        m_sourceDesc = sourceDesc;
        m_renderTargets = std::move(targets);
        m_foveationDirty = false;
        const auto& f = m_foveation;
        Log("Foveation: enabled=%u L=[%.4f %.4f %.4f %.4f] R=[%.4f %.4f %.4f %.4f] ratio=%.2f,%.2f output=%ux%u",
            static_cast<unsigned>(f.enabled),
            f.left.centerX0, f.left.centerX1, f.left.centerY0, f.left.centerY1,
            f.right.centerX0, f.right.centerX1, f.right.centerY0, f.right.centerY1,
            f.edgeRatioX, f.edgeRatioY, desc.Width, desc.Height);
        m_nextTexture = 0;
        const double frequency = std::round(static_cast<double>(m_config.displayFrequency));
        if (frequency < 1.0 || frequency > static_cast<double>(std::numeric_limits<uint32_t>::max())
            || (m_config.dumpVideoSeconds > 0
                && frequency * m_config.dumpVideoSeconds > static_cast<double>(std::numeric_limits<uint32_t>::max()))) {
            Log("NVENC に使用できない表示周波数です。エンコードなしで続けます。");
            return true;
        }
        const auto frameRate = static_cast<uint32_t>(frequency);
        if (m_config.dumpVideoSeconds > 0) {
            // 開発用: エンコードした映像の最初の数秒を、PC のプレーヤーで確かめられる形で保存する。
            const auto directory = win32::AppLocalDataPath();
            if (!directory.empty()) {
                m_ivfWriter.Open(directory / L"capture.ivf", desc.Width, desc.Height, frameRate,
                    m_config.dumpVideoSeconds * frameRate);
            } else {
                Log("IVF 保存先のフォルダを取得できません。");
            }
        }
        std::vector<void*> inputs;
        for (const auto& texture : m_textures) {
            inputs.push_back(texture.Get());
        }
        m_encoderReady = m_encoder.Initialize(m_device.Get(), inputs,
            {desc.Width, desc.Height, frameRate, m_config.bitrateMbps, m_config.statsCollector}, this);
        if (!m_encoderReady) {
            Log("NVENC を初期化できません。エンコードなしでフレームの受け取りを続けます。");
            m_ivfWriter.Close();
        }
        return true;
    }

    // SteamVR が渡すハンドルから共有テクスチャを開く。同じ数枚が繰り返し使われるので、開いたものは覚えておく。
    SharedTexture* FindSharedTexture(vr::SharedTextureHandle_t handle) {
        const auto found = std::find_if(m_sharedTextures.begin(), m_sharedTextures.end(),
            [handle](const auto& entry) { return entry.handle == handle; });
        if (found != m_sharedTextures.end()) {
            return &*found;
        }
        SharedTexture entry;
        entry.handle = handle;
        const HRESULT result = m_device->OpenSharedResource(
            reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)), IID_PPV_ARGS(&entry.texture));
        if (FAILED(result)) {
            const auto now = Clock::now();
            if (!m_lastOpenError || now - *m_lastOpenError >= kLogInterval) {
                Log("共有テクスチャを開けません: handle=0x%llx, HRESULT=0x%08lx",
                    static_cast<unsigned long long>(handle), static_cast<unsigned long>(result));
                m_lastOpenError = now;
            }
            return nullptr;
        }
        entry.texture.As(&entry.keyedMutex);
        if (m_sharedTextures.size() == kMaxSharedTextures) {
            m_sharedTextures.pop_front();
        }
        m_sharedTextures.push_back(std::move(entry));
        return &m_sharedTextures.back();
    }

    // 縮めて描くとき（foveation）は、共有テクスチャをシェーダーから読めるようにする。
    bool EnsureShaderView(SharedTexture& shared) {
        if (!m_foveation.enabled || shared.srv) {
            return true;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        const HRESULT result = m_device->CreateShaderResourceView(shared.texture.Get(), &view, &shared.srv);
        if (FAILED(result)) {
            Log("共有テクスチャの SRV を作成できません: 0x%08lx", static_cast<unsigned long>(result));
            return false;
        }
        return true;
    }

    // SteamVR の画像を、エンコーダーに渡す自前のテクスチャへ写す（foveation が有効なら縮めて描く）。
    // SteamVR が書いている途中の画像を読まないよう、写すあいだだけ共有テクスチャをロックする。
    bool CopyFrame(SharedTexture& shared, size_t index) {
        if (shared.keyedMutex) {
            const HRESULT result = shared.keyedMutex->AcquireSync(0, kAcquireTimeoutMs);
            // WAIT_TIMEOUT と WAIT_ABANDONED は FAILED では検出できない。
            if (result != S_OK) {
                Log("共有テクスチャの AcquireSync に失敗しました: 0x%08lx", static_cast<unsigned long>(result));
                return false;
            }
        }
        if (m_foveation.enabled) {
            DrawFoveated(index, shared.srv.Get());
        } else {
            m_context->CopyResource(m_textures[index].Get(), shared.texture.Get());
        }
        // コピーが GPU で終わったことを WaitForPresent で確かめるための目印を置く。
        m_context->End(m_queries[index].Get());
        m_lastQuery = m_queries[index];
        m_context->Flush();
        if (shared.keyedMutex) {
            const HRESULT result = shared.keyedMutex->ReleaseSync(0);
            if (FAILED(result)) {
                Log("共有テクスチャの ReleaseSync に失敗しました: 0x%08lx", static_cast<unsigned long>(result));
            }
        }
        return true;
    }

    void LogFrameStats(const D3D11_TEXTURE2D_DESC& desc, uint64_t frameId) {
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - m_statsTime).count();
        if (m_frameCount != 1 && elapsed < std::chrono::duration<double>(kLogInterval).count()) {
            return;
        }
        const double fps = elapsed > 0.0 ? static_cast<double>(m_statsFrameCount) / elapsed : 0.0;
        std::lock_guard vsyncLock(m_vsyncMutex);
        Log("フレーム受信: %ux%u, DXGI format=%u, %.2f fps, nFrameId=%llu, total=%llu, Hz=%.3f, queue=%.3f, repeated=%u, dropped=%u",
            desc.Width, desc.Height, static_cast<unsigned>(desc.Format), fps,
            static_cast<unsigned long long>(frameId), static_cast<unsigned long long>(m_frameCount),
            1.0 / m_period, static_cast<double>(m_feedback.averageQueueDepth),
            static_cast<unsigned>(m_feedback.repeatedFrames), static_cast<unsigned>(m_feedback.droppedFrames));
        m_statsFrameCount = 0;
        m_statsTime = now;
    }

    // Quest の表示の周期と、Quest にたまっている画像の数から、仮想の垂直同期の周期を決め直す。
    // PC と Quest の時計はわずかに速さが違うので、たまりすぎたら遅く、足りなければ速くする。
    void UpdatePeriodLocked() {
        const double frequency = m_feedback.displayPeriodUs >= 5000 && m_feedback.displayPeriodUs <= 50000
            ? 1000000.0 / static_cast<double>(m_feedback.displayPeriodUs) : m_config.displayFrequency;
        m_period = 1.0 / (frequency * m_rateMultiplier);
        if (m_config.statsCollector) {
            m_config.statsCollector->SetVirtualVsyncHz(1.0 / m_period);
        }
    }

    Config m_config;
    uint64_t m_adapterLuid = 0;
    std::mutex m_vsyncMutex;
    Clock::time_point m_lastVsync = Clock::now();
    uint64_t m_vsyncCounter = 0;
    double m_period = 1.0 / 72.0;
    double m_smoothedError = 0;
    double m_rateMultiplier = 1;
    protocol::DisplayFeedbackMessage m_feedback;
    std::mutex m_contextMutex;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    std::deque<SharedTexture> m_sharedTextures;
    std::array<ComPtr<ID3D11Texture2D>, kEncoderTextureCount> m_textures;
    std::array<ComPtr<ID3D11Query>, kEncoderTextureCount> m_queries;
    ComPtr<ID3D11Query> m_lastQuery;
    D3D11_TEXTURE2D_DESC m_textureDesc{};
    D3D11_TEXTURE2D_DESC m_sourceDesc{};
    protocol::Foveation m_foveation;
    bool m_foveationDirty = false;
    std::array<ComPtr<ID3D11RenderTargetView>, kEncoderTextureCount> m_renderTargets;
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11Buffer> m_constants;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11RasterizerState> m_rasterizer;
    size_t m_nextTexture = 0;
    std::optional<size_t> m_pendingTexture;
    uint64_t m_pendingFrameId = 0;
    protocol::FramePose m_pendingPose;
    FrameTiming m_pendingTiming;
    std::mutex m_sinkMutex;
    std::atomic<EncodedFrameSink*> m_frameSink{nullptr};
    IvfWriter m_ivfWriter;
    VideoEncoder m_encoder;
    bool m_encoderReady = false;
    uint64_t m_frameCount = 0;
    uint64_t m_statsFrameCount = 0;
    Clock::time_point m_statsTime = Clock::now();
    std::optional<Clock::time_point> m_lastOpenError;
};

// 不完全型 Impl の破棄が必要になるため、コンストラクタも実装側で定義する。
VirtualDisplay::VirtualDisplay() = default;
VirtualDisplay::~VirtualDisplay() = default;

bool VirtualDisplay::Initialize(const Config& config) {
    if (m_impl) {
        Log("仮想ディスプレイは初期化済みです。");
        return false;
    }
    if (config.adapterIndex < -1 || !std::isfinite(config.displayFrequency) || config.displayFrequency <= 0.0f) {
        Log("仮想ディスプレイの設定が不正です。");
        return false;
    }
    auto impl = std::make_unique<Impl>();
    ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        Log("DXGI factory を作成できません: 0x%08lx", static_cast<unsigned long>(result));
        return false;
    }
    ComPtr<IDXGIAdapter1> selected;
    DXGI_ADAPTER_DESC1 selectedDesc{};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result)) {
            Log("GPU の列挙に失敗しました: 0x%08lx", static_cast<unsigned long>(result));
            return false;
        }
        DXGI_ADAPTER_DESC1 desc{};
        result = adapter->GetDesc1(&desc);
        if (FAILED(result)) {
            Log("GPU の情報を取得できません: 0x%08lx", static_cast<unsigned long>(result));
            return false;
        }
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        Log("GPU [%u]: %s, LUID=0x%016llx, VRAM=%llu bytes, software=%s", index,
            win32::ToUtf8(desc.Description, win32::InvalidText::Replace).c_str(),
            static_cast<unsigned long long>(PackLuid(desc.AdapterLuid)),
            static_cast<unsigned long long>(desc.DedicatedVideoMemory), software ? "true" : "false");
        if (!software && ((config.adapterIndex >= 0 && index == static_cast<UINT>(config.adapterIndex))
                || (config.adapterIndex == -1 && (!selected || desc.DedicatedVideoMemory > selectedDesc.DedicatedVideoMemory)))) {
            selected = adapter;
            selectedDesc = desc;
        }
    }
    if (!selected) {
        Log("使用できる GPU が見つかりません: adapterIndex=%d", config.adapterIndex);
        return false;
    }
    Log("選択した GPU: %s, LUID=0x%016llx, VRAM=%llu bytes",
        win32::ToUtf8(selectedDesc.Description, win32::InvalidText::Replace).c_str(),
        static_cast<unsigned long long>(PackLuid(selectedDesc.AdapterLuid)),
        static_cast<unsigned long long>(selectedDesc.DedicatedVideoMemory));
    constexpr D3D_FEATURE_LEVEL kFeatureLevels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    result = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kFeatureLevels, static_cast<UINT>(std::size(kFeatureLevels)), D3D11_SDK_VERSION,
        &impl->m_device, nullptr, &impl->m_context);
    if (FAILED(result)) {
        Log("D3D11 デバイスを作成できません: 0x%08lx", static_cast<unsigned long>(result));
        return false;
    }
    if (!impl->InitializeFoveation()) {
        return false;
    }
    impl->m_config = config;
    impl->m_adapterLuid = PackLuid(selectedDesc.AdapterLuid);
    impl->m_period = 1.0 / static_cast<double>(config.displayFrequency);
    if (config.statsCollector) {
        config.statsCollector->SetVirtualVsyncHz(1.0 / impl->m_period);
    }
    impl->m_lastVsync = Clock::now();
    impl->m_statsTime = impl->m_lastVsync;
    m_impl = std::move(impl);
    return true;
}

uint64_t VirtualDisplay::AdapterLuid() const {
    return m_impl ? m_impl->m_adapterLuid : 0;
}

void VirtualDisplay::SetFrameSink(EncodedFrameSink* sink) {
    if (m_impl) {
        std::lock_guard lock(m_impl->m_sinkMutex);
        m_impl->m_frameSink.store(sink);
    }
}

void VirtualDisplay::SetFoveation(const protocol::Foveation& foveation) {
    if (m_impl) {
        std::lock_guard lock(m_impl->m_contextMutex);
        if (m_impl->m_foveation != foveation) {
            m_impl->m_foveation = foveation;
            m_impl->m_foveationDirty = true;
        }
    }
}

void VirtualDisplay::SetBitrate(uint32_t mbps) {
    if (m_impl) {
        std::lock_guard lock(m_impl->m_contextMutex);
        if (m_impl->m_config.bitrateMbps == mbps) {
            return;
        }
        m_impl->m_config.bitrateMbps = mbps;
        m_impl->m_encoder.SetBitrate(mbps);
    }
}

void VirtualDisplay::RequestKeyFrame() {
    if (m_impl) {
        m_impl->m_encoder.RequestKeyFrame();
    }
}

void VirtualDisplay::OnDisplayFeedback(const protocol::DisplayFeedbackMessage& feedback) {
    if (!m_impl || !std::isfinite(feedback.averageQueueDepth) || feedback.averageQueueDepth < 0.0f) {
        return;
    }
    auto& impl = *m_impl;
    std::lock_guard lock(impl.m_vsyncMutex);
    impl.AdvanceVsync(Clock::now());
    impl.m_feedback = feedback;
    const double error = static_cast<double>(feedback.averageQueueDepth) - kTargetQueueDepth;
    impl.m_smoothedError += kFeedbackSmoothing * (error - impl.m_smoothedError);
    impl.m_rateMultiplier = std::clamp(1.0 - kFeedbackGain * impl.m_smoothedError,
        1.0 - kMaxRateAdjustment, 1.0 + kMaxRateAdjustment);
    impl.UpdatePeriodLocked();
}

void VirtualDisplay::ResetDisplayFeedback() {
    if (m_impl) {
        auto& impl = *m_impl;
        std::lock_guard lock(impl.m_vsyncMutex);
        impl.AdvanceVsync(Clock::now());
        impl.m_smoothedError = 0;
        impl.m_rateMultiplier = 1;
        impl.m_feedback = {};
        impl.UpdatePeriodLocked();
    }
}

void VirtualDisplay::Present(const vr::PresentInfo_t* presentInfo, uint32_t presentInfoSize) {
    if (!m_impl || presentInfo == nullptr || presentInfoSize < sizeof(vr::PresentInfo_t)) {
        return;
    }
    auto& impl = *m_impl;
    std::lock_guard lock(impl.m_contextMutex);
    // フレームに付ける姿勢は、受け取った姿勢の履歴から選んだもの（HmdDevice::GetFramePose）。
    // SteamVR がこのフレームを描くのに使った姿勢そのものは、ドライバからは分からない。
    const auto framePose = impl.m_config.getFramePose ? impl.m_config.getFramePose() : FramePoseSample{};
    const FrameTiming timing{framePose.receivedAt, Clock::now(), {}};
    // WaitForPresent を挟まない呼び出しで古い番号を誤って Submit しない。
    impl.m_pendingTexture.reset();
    impl.m_lastQuery.Reset();
    auto* shared = impl.FindSharedTexture(presentInfo->backbufferTextureHandle);
    if (shared == nullptr) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    shared->texture->GetDesc(&desc);
    if (!impl.PrepareTextures(desc) || !impl.EnsureShaderView(*shared)) {
        return;
    }
    // エンコーダーがまだ読んでいるテクスチャには上書きしない。3 枚を順に使い、空いていなければこのフレームを捨てる。
    const size_t index = impl.m_nextTexture;
    impl.m_nextTexture = (index + 1) % impl.m_textures.size();
    if (impl.m_encoder.IsBusy(index)) {
        impl.m_encoder.RecordDroppedFrame();
        return;
    }
    if (!impl.CopyFrame(*shared, index)) {
        return;
    }
    impl.m_pendingTexture = index;
    impl.m_pendingFrameId = presentInfo->nFrameId;
    impl.m_pendingPose = framePose.pose;
    impl.m_pendingTiming = timing;
    ++impl.m_frameCount;
    ++impl.m_statsFrameCount;
    if (impl.m_config.statsCollector) {
        impl.m_config.statsCollector->OnRenderedFrame();
    }
    impl.LogFrameStats(desc, presentInfo->nFrameId);
    if (impl.m_config.dumpFrame && impl.m_frameCount == kDumpFrameNumber) {
        DumpFrame(impl.m_device.Get(), impl.m_context.Get(), impl.m_textures[index].Get());
    }
}

void VirtualDisplay::WaitForPresent() {
    if (!m_impl) {
        return;
    }
    auto& impl = *m_impl;
    {
        std::lock_guard lock(impl.m_contextMutex);
        if (impl.m_lastQuery) {
            // コピーが GPU で終わるのを待ってから、エンコーダーに渡す。
            const auto deadline = Clock::now() + kCopyTimeout;
            for (;;) {
                BOOL complete = FALSE;
                const HRESULT result = impl.m_context->GetData(impl.m_lastQuery.Get(), &complete, sizeof(complete), 0);
                if (result == S_OK && complete) {
                    if (impl.m_pendingTexture && impl.m_encoderReady) {
                        impl.m_encoder.Submit(*impl.m_pendingTexture, impl.m_pendingFrameId, impl.m_pendingPose,
                            impl.m_pendingTiming);
                    }
                    break;
                }
                if (FAILED(result)) {
                    Log("フレームコピーの完了確認に失敗しました: 0x%08lx", static_cast<unsigned long>(result));
                    impl.m_encoder.RecordDroppedFrame();
                    break;
                }
                if (Clock::now() >= deadline) {
                    Log("フレームコピーの完了待ちを 50ms で打ち切りました。");
                    impl.m_encoder.RecordDroppedFrame();
                    break;
                }
                std::this_thread::yield();
            }
        }
        impl.m_pendingTexture.reset();
        impl.m_lastQuery.Reset();
    }
    // GPU 完了後に次の境界を待ち、実ディスプレイがなくても描画周期を維持する。
    Clock::time_point nextVsync;
    {
        std::lock_guard lock(impl.m_vsyncMutex);
        impl.AdvanceVsync(Clock::now());
        nextVsync = impl.m_lastVsync
            + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(impl.m_period));
    }
    std::this_thread::sleep_until(nextVsync);
}

bool VirtualDisplay::GetTimeSinceLastVsync(float* secondsSinceLastVsync, uint64_t* frameCounter) {
    double seconds = 0.0;
    uint64_t counter = 0;
    if (m_impl) {
        std::lock_guard lock(m_impl->m_vsyncMutex);
        const auto now = Clock::now();
        m_impl->AdvanceVsync(now);
        seconds = std::chrono::duration<double>(now - m_impl->m_lastVsync).count();
        counter = m_impl->m_vsyncCounter;
    }
    if (secondsSinceLastVsync != nullptr) {
        *secondsSinceLastVsync = static_cast<float>(seconds);
    }
    if (frameCounter != nullptr) {
        *frameCounter = counter;
    }
    return true;
}

}
