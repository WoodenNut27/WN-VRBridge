#pragma once

#include "settings_view.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace wnvr::host {

// 設定画面のウィンドウ。Win32 のウィンドウ、Direct3D 11、Dear ImGui の準備と後始末を受け持ち、
// 中身の描画は SettingsView に任せる。
class SettingsWindow {
public:
    explicit SettingsWindow(SettingsView::Context context) : m_view(std::move(context)) {}
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // ウィンドウを作って（すでにあれば）前に出す。
    bool Show(HINSTANCE instance);
    // 描く必要があるか（表示中で、最小化されていない）。
    bool IsRenderable() const;
    // ウィンドウが開いているか（最小化も含む）。定期的な状態の読み直しを続けるかどうかに使う。
    bool IsOpen() const;
    void Render();
    SettingsView& View() { return m_view; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool Create(HINSTANCE instance);
    void Destroy();
    bool LoadFonts();
    bool CreateGraphics();
    void DestroyGraphics();
    bool CreateRenderTarget();
    bool RecoverGraphics();
    bool ResizeIfNeeded();
    void ApplyDpi(UINT dpi);

    SettingsView m_view;
    HWND m_window = nullptr;
    HINSTANCE m_instance = nullptr;
    HICON m_largeIcon = nullptr;
    HICON m_smallIcon = nullptr;
    bool m_classRegistered = false;
    bool m_contextCreated = false;
    bool m_win32Initialized = false;
    bool m_dx11Initialized = false;
    bool m_resizePending = false;
    bool m_occluded = false;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deviceContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTarget;
};

}
