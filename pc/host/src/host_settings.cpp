#include "host_settings.h"

#include "host_log.h"

#include <wnvr/win32/platform.h>

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace wnvr::host {
namespace {

using Json = nlohmann::json;

constexpr int kSettingsVersion = 1;

bool IsValidApprovalTime(std::string_view text) {
    if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != ' '
        || text[13] != ':' || text[16] != ':') {
        return false;
    }
    const auto number = [text](size_t offset, size_t count) {
        const auto part = text.substr(offset, count);
        int value = -1;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        return part.find_first_not_of("0123456789") == std::string_view::npos
            && parsed.ec == std::errc{} && parsed.ptr == part.data() + part.size() ? value : -1;
    };
    const int year = number(0, 4);
    const int month = number(5, 2);
    const int day = number(8, 2);
    const int hour = number(11, 2);
    const int minute = number(14, 2);
    const int second = number(17, 2);
    return year >= 1 && month >= 1 && day >= 1
        && std::chrono::year_month_day{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
            std::chrono::day{static_cast<unsigned>(day)}}.ok()
        && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
}

const Json* FindSetting(const Json& root, const char* section, const char* key) {
    const auto group = root.find(section);
    if (group == root.end()) {
        return nullptr;
    }
    if (!group->is_object()) {
        LogWarn("設定 %s はオブジェクトではないため既定値を使います", section);
        return nullptr;
    }
    const auto value = group->find(key);
    return value == group->end() ? nullptr : &*value;
}

void ReadBool(const Json& root, const char* section, const char* key, bool& target) {
    if (const auto* value = FindSetting(root, section, key)) {
        if (value->is_boolean()) {
            target = value->get<bool>();
        } else {
            LogWarn("%s.%s が不正なため既定値を使います", section, key);
        }
    }
}

// ファイルを開いて JSON として読む。読めなければ nullopt（壊れていれば .bad に退避する）。
std::optional<Json> ReadSettingsFile(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        LogError("設定ファイルの確認に失敗しました: %s", error.message().c_str());
        return std::nullopt;
    }
    if (!exists) {
        LogInfo("設定ファイルがないため既定値を使います");
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        LogError("設定ファイルを開けないため既定値を使います");
        return std::nullopt;
    }
    try {
        Json root;
        file >> root;
        // 後続の不正なデータも検出し、途中までの JSON を正常と扱わない。
        file >> std::ws;
        if (file.bad()) {
            LogError("設定ファイルの読み取りに失敗しました");
            return std::nullopt;
        }
        if (!file.eof() || !root.is_object()) {
            throw std::runtime_error("設定 JSON の形式が不正です");
        }
        return root;
    } catch (const std::exception& exception) {
        file.close();
        LogWarn("設定ファイルが壊れているため既定値を使います: %s", exception.what());
        auto badPath = path;
        badPath += L".bad";
        if (!MoveFileExW(path.c_str(), badPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            LogError("壊れた設定ファイルを退避できませんでした: %lu", GetLastError());
        }
        return std::nullopt;
    }
}

std::vector<ApprovedDevice> ReadApprovedDevices(const Json& entries) {
    std::vector<ApprovedDevice> devices;
    if (!entries.is_array()) {
        LogWarn("pairing.approvedDevices が配列ではないため無視します");
        return devices;
    }
    for (const auto& value : entries) {
        if (!value.is_object() || !value.contains("id") || !value["id"].is_string()
            || !value.contains("name") || !value["name"].is_string()
            || !value.contains("approvedAt") || !value["approvedAt"].is_string()) {
            LogWarn("承認した Quest の不正な項目を無視します");
            continue;
        }
        ApprovedDevice device{value["id"].get<std::string>(), value["name"].get<std::string>(),
            value["approvedAt"].get<std::string>()};
        const bool duplicate = std::any_of(devices.begin(), devices.end(),
            [&](const auto& entry) { return entry.id == device.id; });
        if (!ipc::IsValidDeviceId(device.id) || device.name.size() > ipc::kMaxDeviceNameBytes
            || device.name.find('\0') != std::string::npos
            || !IsValidApprovalTime(device.approvedAt)
            || duplicate || devices.size() >= ipc::kMaxApprovedDevices) {
            LogWarn("承認した Quest の不正・重複・上限超過の項目を無視します");
            continue;
        }
        devices.push_back(std::move(device));
    }
    return devices;
}

Json ToJson(const HostSettings& settings) {
    if (settings.bitrateMbps < ipc::kMinBitrateMbps || settings.bitrateMbps > ipc::kMaxBitrateMbps
        || settings.questResolutionPercent < ipc::kMinQuestResolutionPercent
        || settings.questResolutionPercent > ipc::kMaxQuestResolutionPercent
        || !std::isfinite(settings.foveationEdgeRatio)
        || settings.foveationEdgeRatio < ipc::kMinFoveationEdgeRatio
        || settings.foveationEdgeRatio > ipc::kMaxFoveationEdgeRatio
        || settings.adapterId.find('\0') != std::string::npos) {
        throw std::runtime_error("保存する設定の値が不正です");
    }
    if (settings.approvedDevices.size() > ipc::kMaxApprovedDevices) {
        throw std::runtime_error("承認した Quest は " + std::to_string(ipc::kMaxApprovedDevices) + " 個までです");
    }
    Json devices = Json::array();
    for (const auto& device : settings.approvedDevices) {
        if (!ipc::IsValidDeviceId(device.id)) {
            throw std::runtime_error("端末 ID が不正です");
        }
        devices.push_back({{"id", device.id}, {"name", device.name}, {"approvedAt", device.approvedAt}});
    }
    return {
        {"version", kSettingsVersion},
        {"pairing", {{"approvedDevices", devices}}},
        {"network", {{"adapterId", settings.adapterId}}},
        {"video", {{"bitrateMbps", settings.bitrateMbps},
            {"questResolutionPercent", settings.questResolutionPercent},
            {"foveatedEncoding", settings.foveatedEncoding}, {"foveationEdgeRatio", settings.foveationEdgeRatio}}},
        {"steamvr", {{"autoStart", settings.autoStartSteamVr}, {"autoStop", settings.autoStopSteamVr}}},
        {"audio", {{"streamToQuest", settings.streamAudioToQuest}, {"micToPc", settings.micToPc},
            {"muteSpeakerWhileConnected", settings.muteSpeakerWhileConnected}}}
    };
}

}

bool HostSettings::IsApproved(std::string_view deviceId) const {
    return std::any_of(approvedDevices.begin(), approvedDevices.end(),
        [deviceId](const auto& device) { return device.id == deviceId; });
}

std::filesystem::path SettingsPath() {
    const auto folder = win32::AppRoamingDataPath();
    if (folder.empty()) {
        LogError("設定フォルダを取得できませんでした");
        return {};
    }
    return folder / L"settings.json";
}

HostSettings LoadSettings() {
    HostSettings settings;
    const auto path = SettingsPath();
    if (path.empty()) {
        return settings;
    }
    const auto loaded = ReadSettingsFile(path);
    if (!loaded) {
        return settings;
    }
    const auto& root = *loaded;
    const auto version = root.find("version");
    if (version != root.end() && *version != kSettingsVersion) {
        LogWarn("未対応の設定バージョンのため既定値を使います");
        return settings;
    }
    if (const auto* value = FindSetting(root, "network", "adapterId")) {
        if (value->is_string() && value->get_ref<const std::string&>().find('\0') == std::string::npos) {
            settings.adapterId = value->get<std::string>();
        } else {
            LogWarn("network.adapterId が不正なため既定値を使います");
        }
    }
    if (const auto* value = FindSetting(root, "video", "bitrateMbps")) {
        // int に変換する前に比較し、巨大な整数の切り詰めを防ぐ。
        if (value->is_number_integer() && *value >= ipc::kMinBitrateMbps && *value <= ipc::kMaxBitrateMbps) {
            settings.bitrateMbps = value->get<int>();
        } else {
            LogWarn("video.bitrateMbps は %d〜%d の整数ではないため既定値を使います",
                ipc::kMinBitrateMbps, ipc::kMaxBitrateMbps);
        }
    }
    const auto* resolution = FindSetting(root, "video", "questResolutionPercent");
    if (resolution && resolution->is_number_integer() && *resolution >= ipc::kMinQuestResolutionPercent
        && *resolution <= ipc::kMaxQuestResolutionPercent) {
        settings.questResolutionPercent = resolution->get<int>();
    } else {
        LogWarn("video.questResolutionPercent がないか範囲外のため既定値を使います");
    }
    const auto* foveation = FindSetting(root, "video", "foveatedEncoding");
    if (foveation && foveation->is_boolean()) {
        settings.foveatedEncoding = foveation->get<bool>();
    } else {
        LogWarn("video.foveatedEncoding がないか不正なため既定値を使います");
    }
    const auto* ratio = FindSetting(root, "video", "foveationEdgeRatio");
    if (ratio && ratio->is_number() && std::isfinite(ratio->get<double>())
        && *ratio >= ipc::kMinFoveationEdgeRatio && *ratio <= ipc::kMaxFoveationEdgeRatio) {
        settings.foveationEdgeRatio = std::round(ratio->get<float>() / ipc::kFoveationEdgeRatioStep)
            * ipc::kFoveationEdgeRatioStep;
    } else {
        LogWarn("video.foveationEdgeRatio がないか範囲外のため既定値を使います");
    }
    ReadBool(root, "steamvr", "autoStart", settings.autoStartSteamVr);
    ReadBool(root, "steamvr", "autoStop", settings.autoStopSteamVr);
    ReadBool(root, "audio", "streamToQuest", settings.streamAudioToQuest);
    ReadBool(root, "audio", "micToPc", settings.micToPc);
    ReadBool(root, "audio", "muteSpeakerWhileConnected", settings.muteSpeakerWhileConnected);
    if (const auto* devices = FindSetting(root, "pairing", "approvedDevices")) {
        settings.approvedDevices = ReadApprovedDevices(*devices);
    }
    LogInfo("設定を読み込みました");
    return settings;
}

bool SaveSettings(const HostSettings& settings) {
    const auto path = SettingsPath();
    if (path.empty()) {
        return false;
    }
    // 書いている途中で止まっても元のファイルを壊さないよう、別名で書いてから置き換える。
    auto temporaryPath = path;
    temporaryPath += L".tmp";
    try {
        const std::string contents = ToJson(settings).dump(2) + '\n';
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(temporaryPath, std::ios::binary | std::ios::trunc);
        file << contents;
        file.close();
        if (!MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            LogError("設定ファイルの置き換えに失敗しました: %lu", GetLastError());
            std::error_code error;
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
        LogInfo("設定を保存しました");
        return true;
    } catch (const std::exception& exception) {
        LogError("設定の保存に失敗しました: %s", exception.what());
        std::error_code error;
        std::filesystem::remove(temporaryPath, error);
        return false;
    }
}

}
