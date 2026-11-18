#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace wnvr::driver {

// 開発用。エンコードに渡す直前の画像を 1 枚、BMP で保存する
// （%LOCALAPPDATA%\WN-VRBridge\frame_dump.bmp。SteamVR の設定の dumpFrame で有効にする）。
// 画像の向きや色、中心を細かく送る写像の結果を目で確かめるために使う。
void DumpFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture);

}
