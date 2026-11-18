#include "frame_dump.h"

#include "driver_log.h"

#include <wnvr/win32/platform.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace wnvr::driver {
namespace {

using Microsoft::WRL::ComPtr;

std::string PathText(const std::filesystem::path& path) {
    return win32::ToUtf8(path.wstring(), win32::InvalidText::Replace);
}

}

void DumpFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
        || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
        || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool bgrx = desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM
        || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    if (!rgba && !bgra && !bgrx) {
        Log("BMP 保存に未対応の DXGI フォーマットです: %u", static_cast<unsigned>(desc.Format));
        return;
    }
    constexpr DWORD kPixelOffset = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    const uint64_t imageSize = static_cast<uint64_t>(desc.Width) * desc.Height * 4;
    if (desc.Width > static_cast<UINT>(std::numeric_limits<LONG>::max())
        || desc.Height > static_cast<UINT>(std::numeric_limits<LONG>::max())
        || imageSize > std::numeric_limits<DWORD>::max() - kPixelOffset
        || desc.SampleDesc.Count != 1) {
        Log("BMP 保存できないテクスチャの大きさまたはサンプル数です。");
        return;
    }
    const auto directory = win32::AppLocalDataPath();
    if (directory.empty()) {
        Log("BMP 保存先のフォルダを取得できません。");
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        Log("BMP 保存先を作成できません: %s", error.message().c_str());
        return;
    }
    const auto path = directory / L"frame_dump.bmp";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        Log("BMP ファイルを開けません: %s", PathText(path).c_str());
        return;
    }

    // GPU のテクスチャは直接読めないので、CPU から読める写しを作って写す。
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(result)) {
        Log("BMP 用 staging テクスチャを作成できません: 0x%08lx", static_cast<unsigned long>(result));
        return;
    }
    context->CopyResource(staging.Get(), texture);
    std::vector<unsigned char> row(static_cast<size_t>(desc.Width) * 4);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        Log("BMP 用テクスチャを Map できません: 0x%08lx", static_cast<unsigned long>(result));
        return;
    }
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = kPixelOffset;
    fileHeader.bfSize = kPixelOffset + static_cast<DWORD>(imageSize);
    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = static_cast<LONG>(desc.Width);
    infoHeader.biHeight = static_cast<LONG>(desc.Height);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = static_cast<DWORD>(imageSize);
    file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    file.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    // BMP は下の行から並べ、色は B・G・R・A の順に書く。
    for (UINT y = desc.Height; y > 0; --y) {
        const auto* source = static_cast<const unsigned char*>(mapped.pData)
            + static_cast<size_t>(y - 1) * mapped.RowPitch;
        std::copy_n(source, row.size(), row.data());
        for (size_t x = 0; x < row.size(); x += 4) {
            if (rgba) {
                std::swap(row[x], row[x + 2]);
            }
            if (bgrx) {
                row[x + 3] = 255;
            }
        }
        file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    context->Unmap(staging.Get(), 0);
    file.close();
    if (!file) {
        Log("BMP の書き込みに失敗しました: %s", PathText(path).c_str());
        return;
    }
    Log("BMP を保存しました: %s", PathText(path).c_str());
}

}
