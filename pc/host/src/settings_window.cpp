#include "settings_window.h"

#include "app_icon.h"
#include "host_log.h"
#include "ui_widgets.h"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

#include <algorithm>
#include <cstddef>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace wnvr::host {
namespace {

constexpr wchar_t kSettingsWindowClass[] = L"WNVRBridgeSettingsWindow";
// 表示の倍率が 100% のときのウィンドウの大きさと、これより小さくできない大きさ。
constexpr float kInitialWidth = 880.0f;
constexpr float kInitialHeight = 880.0f;
constexpr float kMinimumWidth = 620.0f;
constexpr float kMinimumHeight = 480.0f;

struct FontFile {
    const char* path;
    int face; // .ttc の中の何番目の書体か
};
// UI 向けに字幅を詰めた書体を優先する。無ければ次の候補を使う。
constexpr FontFile kTextFonts[] = {
    {"C:\\Windows\\Fonts\\YuGothM.ttc", 1}, // Yu Gothic UI
    {"C:\\Windows\\Fonts\\meiryo.ttc", 2},  // Meiryo UI
};
constexpr FontFile kHeadingFonts[] = {
    {"C:\\Windows\\Fonts\\YuGothB.ttc", 2}, // Yu Gothic UI Semibold
    {"C:\\Windows\\Fonts\\meiryob.ttc", 2}, // Meiryo UI Bold
};

template<size_t Count>
ImFont* LoadFirstFont(const FontFile (&candidates)[Count]) {
    for (const auto& candidate : candidates) {
        if (GetFileAttributesA(candidate.path) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        ImFontConfig config;
        config.FontNo = static_cast<ImU32>(candidate.face);
        // DX11 バックエンドは動的な文字生成に対応するため、文字範囲の指定は不要。
        if (auto* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(candidate.path, 0.0f, &config)) {
            return font;
        }
        LogWarn("日本語フォントの読み込みに失敗しました: %s", candidate.path);
    }
    return nullptr;
}

}

SettingsWindow::~SettingsWindow() {
    Destroy();
}

bool SettingsWindow::Show(HINSTANCE instance) {
    if (m_window == nullptr && !Create(instance)) {
        Destroy();
        return false;
    }
    m_view.OnShown();
    ShowWindow(m_window, IsIconic(m_window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(m_window);
    LogInfo("設定ウィンドウを開きました");
    return true;
}

bool SettingsWindow::Create(HINSTANCE instance) {
    m_instance = instance;
    m_largeIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON));
    m_smallIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON));
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = m_largeIcon != nullptr ? m_largeIcon : LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = m_smallIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kSettingsWindowClass;
    if (RegisterClassExW(&windowClass) == 0) {
        LogError("設定ウィンドウのクラス登録に失敗しました: %lu", GetLastError());
        return false;
    }
    m_classRegistered = true;
    // マウスのあるモニターに、そのモニターの倍率に合った大きさで出す。
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);
    const RECT& work = monitorInfo.rcWork;
    const int width = std::min(static_cast<int>(kInitialWidth * scale), static_cast<int>(work.right - work.left));
    const int height = std::min(static_cast<int>(kInitialHeight * scale), static_cast<int>(work.bottom - work.top));
    m_window = CreateWindowExW(0, kSettingsWindowClass, L"WN-VRBridge", WS_OVERLAPPEDWINDOW,
        work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance, this);
    if (m_window == nullptr) {
        LogError("設定ウィンドウの作成に失敗しました: %lu", GetLastError());
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_contextCreated = true;
    auto& io = ImGui::GetIO();
    // 配置情報は保存対象ではないため、作業ディレクトリに imgui.ini を作らない。
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    LoadFonts();
    ApplyDpi(GetDpiForWindow(m_window));
    m_win32Initialized = ImGui_ImplWin32_Init(m_window);
    if (!m_win32Initialized) {
        LogError("ImGui Win32 バックエンドの初期化に失敗しました");
        return false;
    }
    return CreateGraphics();
}

bool SettingsWindow::LoadFonts() {
    // 最初に加えたフォントが本文の既定のフォントになる。
    ImFont* text = LoadFirstFont(kTextFonts);
    if (text == nullptr) {
        ImGui::GetIO().Fonts->AddFontDefault();
        LogWarn("日本語フォントが見つからないため既定フォントを使います");
    }
    // 見出し用の太字が無ければ、本文と同じフォントを大きくして使う。
    ui::SetHeadingFont(text != nullptr ? LoadFirstFont(kHeadingFonts) : nullptr);
    return text != nullptr;
}

void SettingsWindow::Destroy() {
    DestroyGraphics();
    if (m_win32Initialized) {
        ImGui_ImplWin32_Shutdown();
        m_win32Initialized = false;
    }
    if (m_contextCreated) {
        ui::SetHeadingFont(nullptr);
        ImGui::DestroyContext();
        m_contextCreated = false;
    }
    if (m_window != nullptr) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
    if (m_classRegistered) {
        UnregisterClassW(kSettingsWindowClass, m_instance);
        m_classRegistered = false;
    }
    for (HICON* icon : {&m_largeIcon, &m_smallIcon}) {
        if (*icon != nullptr) {
            DestroyIcon(*icon);
            *icon = nullptr;
        }
    }
}

bool SettingsWindow::CreateGraphics() {
    RECT client{};
    GetClientRect(m_window, &client);
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = static_cast<UINT>(client.right);
    description.BufferDesc.Height = static_cast<UINT>(client.bottom);
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.OutputWindow = m_window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, levels, 1, D3D11_SDK_VERSION, &description,
        m_swapChain.GetAddressOf(), m_device.GetAddressOf(), nullptr, m_deviceContext.GetAddressOf());
    if (FAILED(result)) {
        LogError("D3D11 の作成に失敗しました: 0x%08lX", static_cast<unsigned long>(result));
        return false;
    }
    // Alt+Enter による意図しない全画面化を防ぎ、通常の設定画面として扱う。
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (SUCCEEDED(m_swapChain->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
        factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
    }
    if (!CreateRenderTarget()) {
        return false;
    }
    m_dx11Initialized = ImGui_ImplDX11_Init(m_device.Get(), m_deviceContext.Get());
    if (!m_dx11Initialized) {
        LogError("ImGui DX11 バックエンドの初期化に失敗しました");
        return false;
    }
    m_resizePending = false;
    m_occluded = false;
    return true;
}

void SettingsWindow::DestroyGraphics() {
    if (m_dx11Initialized) {
        ImGui_ImplDX11_Shutdown();
        m_dx11Initialized = false;
    }
    if (m_deviceContext) {
        m_deviceContext->ClearState();
    }
    m_renderTarget.Reset();
    m_swapChain.Reset();
    if (m_deviceContext) {
        // フリップモデルの再作成前に、古いスワップチェーンの遅延破棄を完了させる。
        m_deviceContext->Flush();
    }
    m_deviceContext.Reset();
    m_device.Reset();
}

bool SettingsWindow::CreateRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (SUCCEEDED(result)) {
        result = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_renderTarget.GetAddressOf());
    }
    if (FAILED(result)) {
        LogError("描画ターゲットの作成に失敗しました: 0x%08lX", static_cast<unsigned long>(result));
        return false;
    }
    return true;
}

bool SettingsWindow::RecoverGraphics() {
    LogWarn("D3D11 を再作成します (デバイス状態: 0x%08lX)",
        static_cast<unsigned long>(m_device ? m_device->GetDeviceRemovedReason() : E_FAIL));
    DestroyGraphics();
    if (CreateGraphics()) {
        return true;
    }
    // 復旧に失敗した場合は待機に戻し、次に開くときに初期化をやり直す。
    LogError("D3D11 の復旧に失敗したため設定ウィンドウを閉じます");
    Destroy();
    return false;
}

void SettingsWindow::ApplyDpi(UINT dpi) {
    if (m_contextCreated) {
        ui::ApplyTheme(static_cast<float>(dpi) / 96.0f);
    }
}

bool SettingsWindow::IsOpen() const {
    return m_window != nullptr && IsWindowVisible(m_window);
}

bool SettingsWindow::IsRenderable() const {
    return IsOpen() && !IsIconic(m_window);
}

// 描画を続けてよければ true。大きさが 0 のあいだと、作り直しに失敗したときは false。
bool SettingsWindow::ResizeIfNeeded() {
    if (!m_resizePending) {
        return true;
    }
    RECT client{};
    GetClientRect(m_window, &client);
    if (client.right == 0 || client.bottom == 0) {
        return false;
    }
    m_deviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    m_renderTarget.Reset();
    const HRESULT result = m_swapChain->ResizeBuffers(0, static_cast<UINT>(client.right),
        static_cast<UINT>(client.bottom), DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result) || !CreateRenderTarget()) {
        LogError("スワップチェーンのリサイズに失敗しました: 0x%08lX", static_cast<unsigned long>(result));
        RecoverGraphics();
        return false;
    }
    m_resizePending = false;
    return true;
}

void SettingsWindow::Render() {
    if (!IsRenderable()) {
        return;
    }
    if (m_occluded) {
        const HRESULT result = m_swapChain->Present(0, DXGI_PRESENT_TEST);
        if (result == DXGI_STATUS_OCCLUDED) {
            // 画面ロック中などは Present が待機しないため、メッセージ付きの待機にする。
            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            return;
        }
        if (FAILED(result)) {
            LogError("表示状態の確認に失敗しました: 0x%08lX", static_cast<unsigned long>(result));
            RecoverGraphics();
            return;
        }
        m_occluded = false;
    }
    if (!ResizeIfNeeded()) {
        return;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    // ウィンドウいっぱいに 1 つの画面を敷き、枠や題名のない普通のアプリの画面に見せる。
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("Settings", nullptr, kFlags);
    m_view.Draw(m_window);
    ImGui::End();
    ImGui::Render();
    const ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const float clearColor[] = {background.x, background.y, background.z, 1.0f};
    ID3D11RenderTargetView* target = m_renderTarget.Get();
    m_deviceContext->OMSetRenderTargets(1, &target, nullptr);
    m_deviceContext->ClearRenderTargetView(target, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT result = m_swapChain->Present(1, 0);
    if (FAILED(result)) {
        LogError("Present に失敗しました: 0x%08lX", static_cast<unsigned long>(result));
        RecoverGraphics();
    } else {
        m_occluded = result == DXGI_STATUS_OCCLUDED;
    }
}

LRESULT CALLBACK SettingsWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* settings = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        settings = static_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(settings));
    }
    if (settings != nullptr) {
        switch (message) {
        case WM_CLOSE:
            // 閉じてもアプリは終了せず、タスクトレイで動き続ける。
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                // フレームの途中で描画先を破棄しないよう、再確保は次の描画前に行う。
                settings->m_resizePending = true;
            }
            return 0;
        case WM_GETMINMAXINFO: {
            // 中身が読めなくなるほど小さくはできないようにする。
            const float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {static_cast<LONG>(kMinimumWidth * scale), static_cast<LONG>(kMinimumHeight * scale)};
            return 0;
        }
        case WM_DPICHANGED: {
            settings->ApplyDpi(HIWORD(wParam));
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        default:
            break;
        }
        if (settings->m_win32Initialized && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
            return 1;
        }
        // Alt キーでメニューに入って操作が止まったように見えるのを防ぐ。
        if (message == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}
