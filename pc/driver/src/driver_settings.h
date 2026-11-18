#pragma once

#include <cstdint>
#include <optional>
#include <string>

// SteamVR の設定（steamvr.vrsettings の driver_wnvrbridge。既定値は resources/settings/default.vrsettings）
// の読み書き。値の検査と、読めなかったときのログの出し方をここでそろえる。
namespace wnvr::driver::settings {

inline constexpr char kSection[] = "driver_wnvrbridge";

// 読めないか範囲外（両端を含む）のときは fallback を返し、その旨をログに残す。
bool ReadBool(const char* key, bool fallback);
int32_t ReadInt(const char* key, int32_t minimum, int32_t maximum, int32_t fallback);
float ReadFloat(const char* key, float minimum, float maximum, float fallback);
std::string ReadString(const char* key, const std::string& fallback);

// 無くても普通のキー（Quest から受け取って保存する視野角など）を読む。無ければ何も言わずに nullopt。
std::optional<float> TryReadFloat(const char* key);

// 保存できなかったときはログに残して false を返す。
bool WriteFloat(const char* key, float value);

}
