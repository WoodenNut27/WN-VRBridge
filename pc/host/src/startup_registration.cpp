#include "startup_registration.h"

#include "host_log.h"

#include <wnvr/win32/platform.h>

#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <vector>

namespace wnvr::host {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"WN-VRBridge";

std::wstring ExecutablePath() {
    auto path = win32::ExecutablePath().wstring();
    if (path.empty()) {
        LogError("実行ファイルのパスを取得できませんでした");
    }
    return path;
}

// 登録されているコマンドを読む。登録が無いか読めなければ空。
std::wstring ReadRunCommand() {
    DWORD bytes = 0;
    LSTATUS result = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ,
        nullptr, nullptr, &bytes);
    if (result == ERROR_FILE_NOT_FOUND) {
        return {};
    }
    std::vector<wchar_t> command(static_cast<size_t>(bytes) / sizeof(wchar_t) + 1, L'\0');
    if (result == ERROR_SUCCESS) {
        result = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ,
            nullptr, command.data(), &bytes);
    }
    if (result != ERROR_SUCCESS) {
        LogError("スタートアップ登録を読み取れませんでした: %ld", result);
        return {};
    }
    return command.data();
}

}

bool IsStartupRegistered() {
    const auto command = ReadRunCommand();
    if (command.empty()) {
        return false;
    }
    int count = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(
        CommandLineToArgvW(command.c_str(), &count), LocalFree);
    if (!arguments) {
        LogError("スタートアップのコマンドを解析できませんでした: %lu", GetLastError());
        return false;
    }
    // フォルダを動かす前の登録が残っていても「登録済み」と見せないよう、今の実行ファイルと比べる。
    const auto path = ExecutablePath();
    return !path.empty() && count > 0
        && CompareStringOrdinal(arguments.get()[0], -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool SetStartupRegistered(bool registered) {
    LSTATUS result = ERROR_SUCCESS;
    if (registered) {
        const auto path = ExecutablePath();
        if (path.empty()) {
            return false;
        }
        // --startup を付けて、Windows の起動時は設定画面を開かずにタスクトレイに入るようにする。
        const std::wstring command = L"\"" + path + L"\" --startup";
        HKEY key = nullptr;
        result = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &key, nullptr);
        if (result == ERROR_SUCCESS) {
            result = RegSetValueExW(key, kValueName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
    } else {
        result = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }
    if (result != ERROR_SUCCESS) {
        LogError("スタートアップ登録の変更に失敗しました: %ld", result);
        return false;
    }
    LogInfo("スタートアップ登録を%sしました", registered ? "有効に" : "解除");
    return true;
}

}
