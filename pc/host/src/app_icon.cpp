#include "app_icon.h"
#include "resource.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace wnvr::host {
namespace {

constexpr int kMaxIconSize = 512;
constexpr int kSamples = 4;
constexpr float kTotalSamples = static_cast<float>(kSamples * kSamples);
constexpr float kSampleCenter = 0.5f;
constexpr float kStatusDiameterRatio = 0.5f;
constexpr float kBorderWidthRatio = 0.06f;
constexpr float kMinBorderWidth = 1.0f;
constexpr uint32_t kBorderRgb = 0x1c1c1a;
constexpr uint32_t kMaxChannel = 255;
constexpr WORD kIconBitCount = 32;

uint32_t ToByte(float value) {
    return static_cast<uint32_t>(std::clamp(std::lround(value), 0L, static_cast<long>(kMaxChannel)));
}

}

HICON LoadAppIcon(int size) {
    if (size <= 0 || size > kMaxIconSize) {
        return nullptr;
    }
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

HICON CreateStatusIcon(int size, Tone tone) {
    const HICON baseIcon = LoadAppIcon(size);
    if (baseIcon == nullptr) {
        return nullptr;
    }
    ICONINFO baseInfo{};
    const bool hasInfo = GetIconInfo(baseIcon, &baseInfo) != FALSE;
    DestroyIcon(baseIcon);

    std::vector<uint32_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size));
    BITMAPINFO readInfo{};
    readInfo.bmiHeader.biSize = sizeof(readInfo.bmiHeader);
    readInfo.bmiHeader.biWidth = size;
    readInfo.bmiHeader.biHeight = -size;
    readInfo.bmiHeader.biPlanes = 1;
    readInfo.bmiHeader.biBitCount = kIconBitCount;
    readInfo.bmiHeader.biCompression = BI_RGB;
    const HDC screen = GetDC(nullptr);
    const bool hasPixels = hasInfo && baseInfo.hbmColor != nullptr && screen != nullptr
        && GetDIBits(screen, baseInfo.hbmColor, 0, static_cast<UINT>(size), pixels.data(), &readInfo,
            DIB_RGB_COLORS) == size;
    if (baseInfo.hbmColor != nullptr) {
        DeleteObject(baseInfo.hbmColor);
    }
    if (baseInfo.hbmMask != nullptr) {
        DeleteObject(baseInfo.hbmMask);
    }
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }
    if (!hasPixels) {
        return nullptr;
    }

    const float radius = static_cast<float>(size) * kStatusDiameterRatio / 2.0f;
    const float center = static_cast<float>(size) - radius;
    const float borderWidth = std::max(static_cast<float>(size) * kBorderWidthRatio, kMinBorderWidth);
    const float innerRadius = std::max(radius - borderWidth, 0.0f);
    const uint32_t rgb = ToneRgb(tone);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            float sum[3]{};
            int covered = 0;
            // 小さいトレイ表示でも縁が欠けないよう、画素内の覆う割合を求める。
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const float x = static_cast<float>(px) + (static_cast<float>(sx) + kSampleCenter) / kSamples;
                    const float y = static_cast<float>(py) + (static_cast<float>(sy) + kSampleCenter) / kSamples;
                    const float distance = std::hypot(x - center, y - center);
                    if (distance >= radius) {
                        continue;
                    }
                    const uint32_t color = distance < innerRadius ? rgb : kBorderRgb;
                    sum[0] += static_cast<float>((color >> 16) & kMaxChannel);
                    sum[1] += static_cast<float>((color >> 8) & kMaxChannel);
                    sum[2] += static_cast<float>(color & kMaxChannel);
                    ++covered;
                }
            }
            auto& pixel = pixels[static_cast<size_t>(py) * static_cast<size_t>(size) + static_cast<size_t>(px)];
            const float alpha = static_cast<float>(covered) / kTotalSamples;
            const float remaining = 1.0f - alpha;
            // 元画像もアルファを掛けた色なので、そのまま「上に重ねる」合成をする。
            pixel = (ToByte(alpha * kMaxChannel + static_cast<float>(pixel >> 24) * remaining) << 24)
                | (ToByte(sum[0] / kTotalSamples + static_cast<float>((pixel >> 16) & kMaxChannel) * remaining) << 16)
                | (ToByte(sum[1] / kTotalSamples + static_cast<float>((pixel >> 8) & kMaxChannel) * remaining) << 8)
                | ToByte(sum[2] / kTotalSamples + static_cast<float>(pixel & kMaxChannel) * remaining);
        }
    }
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size; // 上の行から並べる
    header.bV5Planes = 1;
    header.bV5BitCount = kIconBitCount;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    const HDC outputScreen = GetDC(nullptr);
    const HBITMAP color = CreateDIBSection(outputScreen, reinterpret_cast<const BITMAPINFO*>(&header), DIB_RGB_COLORS,
        &bits, nullptr, 0);
    if (outputScreen != nullptr) {
        ReleaseDC(nullptr, outputScreen);
    }
    // アルファ付きのアイコンでもマスクは必要（中身は使われない）。
    const HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HICON icon = nullptr;
    if (color != nullptr && mask != nullptr && bits != nullptr) {
        std::copy(pixels.begin(), pixels.end(), static_cast<uint32_t*>(bits));
        ICONINFO info{};
        info.fIcon = TRUE;
        info.hbmMask = mask;
        info.hbmColor = color;
        icon = CreateIconIndirect(&info);
    }
    if (color != nullptr) {
        DeleteObject(color);
    }
    if (mask != nullptr) {
        DeleteObject(mask);
    }
    return icon;
}

}
