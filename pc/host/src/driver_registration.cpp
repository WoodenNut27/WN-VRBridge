#include "driver_registration.h"

#include <wnvr/win32/platform.h>

#include <windows.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace wnvr::host {
namespace {

constexpr wchar_t kDriverName[] = L"wnvrbridge";
constexpr DWORD kVrPathRegTimeoutMs = 15000;

std::filesystem::path SteamVrTool(const std::filesystem::path& runtimePath, const wchar_t* name) {
    // 区切りが / と \ で混ざると、ファイアウォールの API がパスを受け付けない。
    return (runtimePath / L"bin" / L"win64" / name).make_preferred();
}

std::filesystem::path JsonPath(const nlohmann::json& value) {
    const auto text = value.get<std::string>();
    if (text.empty() || text.find('\0') != std::string::npos) {
        throw std::runtime_error("SteamVR のパスが空または不正です");
    }
    const auto wide = win32::ToWide(text);
    if (wide.empty()) {
        throw std::runtime_error("SteamVR のパスが UTF-8 ではありません");
    }
    return std::filesystem::weakly_canonical(wide);
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// 配布フォルダ（exe の隣の driver）と開発用のビルド（exe の 1 つ上の driver）の両方を探す。
std::wstring FindBundledDriver() {
    const auto exe = win32::ExecutablePath();
    if (exe.empty()) {
        throw std::runtime_error("ホストのパスを取得できません");
    }
    for (const auto& relative : {L"driver\\wnvrbridge", L"..\\driver\\wnvrbridge"}) {
        const auto candidate = std::filesystem::weakly_canonical(exe.parent_path() / relative);
        if (std::filesystem::is_regular_file(candidate / L"driver.vrdrivermanifest")) {
            return candidate.wstring();
        }
    }
    return {};
}

nlohmann::json ReadOpenVrPaths() {
    const auto localAppData = win32::LocalAppDataPath();
    if (localAppData.empty()) {
        throw std::runtime_error("LOCALAPPDATA を取得できません");
    }
    std::ifstream file(localAppData / L"openvr" / L"openvrpaths.vrpath", std::ios::binary);
    if (!file) {
        throw std::runtime_error("SteamVR の openvrpaths.vrpath を開けません");
    }
    return nlohmann::json::parse(file);
}

}

std::wstring VrServerPath(const std::wstring& runtimePath) {
    return runtimePath.empty() ? std::wstring{} : SteamVrTool(runtimePath, L"vrserver.exe").wstring();
}

DriverRegistrationStatus QueryDriverRegistration() {
    DriverRegistrationStatus status;
    try {
        status.driverPath = FindBundledDriver();
        const auto paths = ReadOpenVrPaths();
        const auto& runtime = paths.at("runtime");
        if (!runtime.is_array() || runtime.empty()) {
            throw std::runtime_error("SteamVR のランタイムが見つかりません");
        }
        const auto runtimePath = JsonPath(runtime.at(0));
        status.runtimePath = runtimePath.wstring();
        if (!std::filesystem::is_regular_file(SteamVrTool(runtimePath, L"vrpathreg.exe"))) {
            throw std::runtime_error("SteamVR の vrpathreg.exe が見つかりません");
        }
        if (paths.contains("external_drivers")) {
            const auto& drivers = paths.at("external_drivers");
            if (!drivers.is_array()) {
                throw std::runtime_error("external_drivers が配列ではありません");
            }
            for (const auto& entry : drivers) {
                auto path = JsonPath(entry);
                // 末尾の区切り文字があっても最後のフォルダを比較する。
                if (path.filename().empty()) {
                    path = path.parent_path();
                }
                if (!status.driverPath.empty() && SamePath(path.wstring(), status.driverPath)) {
                    status.registered = true;
                } else if (SamePath(path.filename().wstring(), kDriverName)) {
                    status.otherPaths.push_back(path.wstring());
                }
            }
        }
        if (status.driverPath.empty()) {
            status.error = "ドライバのフォルダが見つかりません（このアプリと同じ場所に driver フォルダが必要です）";
        }
    } catch (const nlohmann::json::exception&) {
        status.error = "SteamVR のパス設定 JSON が不正です";
    } catch (const std::filesystem::filesystem_error&) {
        status.error = "SteamVR またはドライバのパスを確認できません";
    } catch (const std::exception& exception) {
        status.error = exception.what();
    } catch (...) {
        status.error = "ドライバ登録の確認で予期しないエラーが発生しました";
    }
    return status;
}

bool RunVrPathReg(bool add, const std::wstring& runtimePath, const std::wstring& driverPath, std::string& error) {
    error.clear();
    try {
        const auto exe = SteamVrTool(runtimePath, L"vrpathreg.exe").wstring();
        // コマンド ラインに埋め込むので、引用符と改行を含むパスは受け付けない。
        if (driverPath.empty() || driverPath.find_first_of(L"\"\r\n") != std::wstring::npos) {
            error = "ドライバのパスが不正です";
            return false;
        }
        std::wstring command = L"\"" + exe + (add ? L"\" adddriver \"" : L"\" removedriver \"") + driverPath + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &process)) {
            error = "vrpathreg を起動できません（Windows " + std::to_string(GetLastError()) + "）";
            return false;
        }
        CloseHandle(process.hThread);
        const DWORD wait = WaitForSingleObject(process.hProcess, kVrPathRegTimeoutMs);
        DWORD exitCode = 0;
        bool success = false;
        if (wait != WAIT_OBJECT_0) {
            error = wait == WAIT_TIMEOUT ? "vrpathreg が 15 秒以内に終了しませんでした"
                : "vrpathreg の終了を待機できませんでした";
            TerminateProcess(process.hProcess, 1);
        } else if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
            error = "vrpathreg の終了コードを取得できません";
        } else if (exitCode != 0) {
            error = "vrpathreg に失敗しました（終了コード " + std::to_string(exitCode) + "）";
        } else {
            success = true;
        }
        CloseHandle(process.hProcess);
        return success;
    } catch (...) {
        error = "vrpathreg の実行で予期しないエラーが発生しました";
        return false;
    }
}

}
