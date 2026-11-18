#pragma once

#include <wnvr/host_ipc/host_ipc.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wnvr::host {

struct ApprovedDevice {
    std::string id;
    std::string name;
    std::string approvedAt; // 「YYYY-MM-DD hh:mm:ss」（PC の現地時刻）
};

struct HostSettings {
    std::vector<ApprovedDevice> approvedDevices;
    std::string adapterId; // 空なら、すべてのアダプターで待ち受ける
    int bitrateMbps = ipc::kDefaultBitrateMbps;
    int questResolutionPercent = ipc::kDefaultQuestResolutionPercent;
    bool foveatedEncoding = true;
    float foveationEdgeRatio = ipc::kDefaultFoveationEdgeRatio;
    bool autoStartSteamVr = true;
    bool autoStopSteamVr = false;
    bool streamAudioToQuest = true;
    bool micToPc = true;
    bool muteSpeakerWhileConnected = true;

    bool IsApproved(std::string_view deviceId) const;
};

// 読めない項目は既定値にして続ける。壊れたファイルは .bad に退避する。
HostSettings LoadSettings();
bool SaveSettings(const HostSettings& settings);
std::filesystem::path SettingsPath();

}
