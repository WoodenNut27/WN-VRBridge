#include "pairing_manager.h"

#include "host_log.h"

#include <wnvr/win32/platform.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace wnvr::host {
namespace {

constexpr size_t kMaxQueuedRequests = 16;
constexpr auto kRejectDuration = std::chrono::minutes(5);
constexpr wchar_t kDialogTitle[] = L"WN-VRBridge — Quest の接続の確認";

// Quest から届いた文字列をダイアログに出すので、改行などの制御文字で文面を崩されないようにする。
std::wstring DisplayText(const std::string& text) {
    auto wide = win32::ToWide(text, win32::InvalidText::Replace);
    for (auto& ch : wide) {
        if (ch < L' ') {
            ch = L' ';
        }
    }
    return wide;
}

std::string LocalTimeText() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return text;
}

}

PairingManager::PairingManager(HostSettings& settings, Callbacks callbacks)
    : m_settings(settings), m_callbacks(std::move(callbacks)) {}

void PairingManager::SetNotifyTarget(HWND window, UINT message) {
    m_window = window;
    m_message = message;
}

void PairingManager::Queue(const ipc::PairingRequestMessage& request) {
    std::lock_guard lock(m_mutex);
    if (m_pendingIds.contains(request.deviceId) || m_queue.size() >= kMaxQueuedRequests) {
        return;
    }
    m_pendingIds.insert(request.deviceId);
    m_queue.push_back(request);
    if (!PostMessageW(m_window, m_message, 0, 0)) {
        m_queue.pop_back();
        m_pendingIds.erase(request.deviceId);
        LogWarn("承認要求を UI に通知できません: %lu", GetLastError());
    }
}

void PairingManager::ProcessQueue() {
    // MessageBox のメッセージループから再入されても、ダイアログは直列に扱う。
    if (m_processing) {
        return;
    }
    m_processing = true;
    for (;;) {
        ipc::PairingRequestMessage request;
        {
            std::lock_guard lock(m_mutex);
            if (m_queue.empty()) {
                break;
            }
            request = std::move(m_queue.front());
            m_queue.pop_front();
        }
        ExpireRejected();
        // 待っているあいだに承認・拒否が済んでいれば、もう一度は聞かない。
        if (!m_settings.IsApproved(request.deviceId) && !m_rejected.contains(request.deviceId)) {
            Ask(request);
        }
        std::lock_guard lock(m_mutex);
        m_pendingIds.erase(request.deviceId);
    }
    m_processing = false;
}

void PairingManager::Ask(const ipc::PairingRequestMessage& request) {
    const auto body = L"新しい Quest から接続の要求が届きました。\n\n"
        L"　名前: " + DisplayText(request.deviceName) + L"\n"
        L"　ID: " + DisplayText(request.deviceId.substr(0, 8)) + L"\n"
        L"　アドレス: " + DisplayText(request.address) + L"\n\n"
        L"この Quest の接続を許可しますか？\n\n"
        L"「はい」: 許可します。次からは確認なしでつながります。\n"
        L"「いいえ」: 拒否します。5 分間は、この Quest の確認を出しません。";
    // 設定画面を閉じていても気づけるよう、最前面に出す。
    const int result = MessageBoxW(nullptr, body.c_str(), kDialogTitle,
        MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
    if (result == IDYES) {
        Approve(request);
    } else if (result == IDNO) {
        Reject(request);
    }
}

void PairingManager::Approve(const ipc::PairingRequestMessage& request) {
    auto& devices = m_settings.approvedDevices;
    auto found = std::find_if(devices.begin(), devices.end(),
        [&](const auto& device) { return device.id == request.deviceId; });
    if (found == devices.end() && devices.size() >= ipc::kMaxApprovedDevices) {
        LogWarn("承認した Quest が上限の数あるため追加できません");
        const auto message = L"接続を許可できる Quest は " + std::to_wstring(ipc::kMaxApprovedDevices)
            + L" 台までです。\n設定画面の「接続を許可した Quest」で、使わないものを取り消してください。";
        MessageBoxW(nullptr, message.c_str(), kDialogTitle, MB_OK | MB_ICONERROR);
        return;
    }
    ApprovedDevice device{request.deviceId, request.deviceName, LocalTimeText()};
    if (found == devices.end()) {
        devices.push_back(std::move(device));
    } else {
        *found = std::move(device);
    }
    const bool saved = SaveSettings(m_settings);
    if (m_callbacks.onApprovedChanged) {
        m_callbacks.onApprovedChanged();
    }
    LogInfo("Quest を承認しました: ID %.8s %s", request.deviceId.c_str(), request.deviceName.c_str());
    if (!saved) {
        LogError("Quest の承認を保存できませんでした");
        MessageBoxW(nullptr, L"許可した内容を設定ファイルに保存できませんでした。\n"
            L"このアプリを終了するまでは接続できますが、次に起動したときにもう一度確認が出ます。",
            kDialogTitle, MB_OK | MB_ICONERROR);
    }
}

void PairingManager::Reject(const ipc::PairingRequestMessage& request) {
    m_rejected[request.deviceId] = std::chrono::steady_clock::now() + kRejectDuration;
    if (m_callbacks.onRejectedChanged) {
        m_callbacks.onRejectedChanged();
    }
    LogInfo("Quest を拒否しました: ID %.8s %s", request.deviceId.c_str(), request.deviceName.c_str());
}

void PairingManager::ExpireRejected() {
    const auto now = std::chrono::steady_clock::now();
    if (std::erase_if(m_rejected, [now](const auto& entry) { return entry.second <= now; }) > 0
        && m_callbacks.onRejectedChanged) {
        m_callbacks.onRejectedChanged();
    }
}

std::vector<std::string> PairingManager::RejectedIds() const {
    std::vector<std::string> ids;
    ids.reserve(m_rejected.size());
    for (const auto& [id, expiry] : m_rejected) {
        (void)expiry;
        ids.push_back(id);
    }
    return ids;
}

}
