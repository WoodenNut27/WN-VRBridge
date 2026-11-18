#pragma once

#include <string>
#include <vector>

namespace wnvr::host {

struct DriverRegistrationStatus {
    std::wstring driverPath;              // このアプリと一緒に置かれているドライバのフォルダ
    std::wstring runtimePath;             // SteamVR のフォルダ
    bool registered = false;              // driverPath が SteamVR に登録されているか
    std::vector<std::wstring> otherPaths; // 別の場所にある同じ名前のドライバの登録
    std::string error;                    // 確認できなかった理由（空なら確認できた）

    // 登録・解除のボタンを出せるか（ドライバと SteamVR の両方が見つかっているか）。
    bool CanChange() const { return error.empty() && !driverPath.empty() && !runtimePath.empty(); }
};

DriverRegistrationStatus QueryDriverRegistration();
// SteamVR の vrpathreg で登録・解除する。最大 15 秒待つため、UI スレッドから呼ばない。
bool RunVrPathReg(bool add, const std::wstring& runtimePath, const std::wstring& driverPath, std::string& error);
// ファイアウォールの許可の対象にする、SteamVR の vrserver.exe の場所。
std::wstring VrServerPath(const std::wstring& runtimePath);

}
