#include "driver_settings.h"

#include "driver_log.h"

#include <openvr_driver.h>

#include <cmath>

namespace wnvr::driver::settings {

bool ReadBool(const char* key, bool fallback) {
    auto* settings = vr::VRSettings();
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const bool value = settings ? settings->GetBool(kSection, key, &error) : fallback;
    if (settings && error == vr::VRSettingsError_None) {
        return value;
    }
    Log("%s を読み込めないため、既定値 %s を使います。", key, fallback ? "true" : "false");
    return fallback;
}

int32_t ReadInt(const char* key, int32_t minimum, int32_t maximum, int32_t fallback) {
    auto* settings = vr::VRSettings();
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const int32_t value = settings ? settings->GetInt32(kSection, key, &error) : fallback;
    if (settings && error == vr::VRSettingsError_None && value >= minimum && value <= maximum) {
        return value;
    }
    Log("%s を読み込めないか範囲外のため、既定値 %d を使います。", key, static_cast<int>(fallback));
    return fallback;
}

float ReadFloat(const char* key, float minimum, float maximum, float fallback) {
    auto* settings = vr::VRSettings();
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const float value = settings ? settings->GetFloat(kSection, key, &error) : fallback;
    if (settings && error == vr::VRSettingsError_None && std::isfinite(value)
        && value >= minimum && value <= maximum) {
        return value;
    }
    Log("%s を読み込めないか範囲外のため、既定値 %.4g を使います。", key, static_cast<double>(fallback));
    return fallback;
}

std::string ReadString(const char* key, const std::string& fallback) {
    auto* settings = vr::VRSettings();
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    char value[256]{};
    if (settings) {
        settings->GetString(kSection, key, value, sizeof(value), &error);
    }
    if (settings && error == vr::VRSettingsError_None) {
        return value;
    }
    Log("%s を読み込めないため、既定値 %s を使います。", key, fallback.c_str());
    return fallback;
}

std::optional<float> TryReadFloat(const char* key) {
    auto* settings = vr::VRSettings();
    if (!settings) {
        return std::nullopt;
    }
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const float value = settings->GetFloat(kSection, key, &error);
    return error == vr::VRSettingsError_None ? std::optional<float>(value) : std::nullopt;
}

bool WriteFloat(const char* key, float value) {
    auto* settings = vr::VRSettings();
    if (!settings) {
        Log("設定を保存できません: VRSettings がありません。");
        return false;
    }
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    settings->SetFloat(kSection, key, value, &error);
    if (error != vr::VRSettingsError_None) {
        Log("設定の保存に失敗しました: %s, error=%d", key, static_cast<int>(error));
        return false;
    }
    return true;
}

}
