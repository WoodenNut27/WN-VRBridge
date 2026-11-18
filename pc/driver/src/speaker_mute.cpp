#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "speaker_mute.h"

#include "driver_log.h"

#include <wnvr/win32/platform.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace wnvr::driver {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr auto kRequestTimeout = std::chrono::seconds(2);
// 状態ファイルは 2 行（デバイスの ID と 0/1）だけなので、これより大きければ壊れている。
constexpr uintmax_t kMaxStateFileBytes = 65536;

// デバイスの ID はファイルに UTF-8 で保存する。途中に NUL がある ID は不正とみなす。
std::wstring DecodeDeviceId(const std::string& id) {
    return id.find('\0') != std::string::npos ? std::wstring{} : win32::ToWide(id);
}

}

struct SpeakerMute::State {
    struct Request {
        Operation operation;
        Clock::time_point deadline;
        bool done = false;
    };

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::shared_ptr<Request>> m_requests;
    std::filesystem::path m_path;
    std::string m_deviceId;
    BOOL m_originalMute = FALSE;
    // このドライバがミュートしていて、元に戻す責任がある。
    bool m_active = false;
    std::jthread m_thread;

    ~State() {
        if (m_thread.joinable()) {
            {
                std::lock_guard lock(m_mutex);
                m_thread.request_stop();
            }
            m_condition.notify_all();
            // DLL の解放後にコードを実行させないため、専用スレッドは切り離さない。
            m_thread.join();
        }
    }

    bool PreparePath() {
        if (!m_path.empty()) {
            return true;
        }
        const auto folder = win32::AppLocalDataPath();
        if (folder.empty()) {
            Log("ミュート状態の保存先を取得できません。");
            return false;
        }
        m_path = folder / L"speaker_mute_state.txt";
        return true;
    }

    bool DeleteState() {
        std::error_code error;
        std::filesystem::remove(m_path, error);
        if (error) {
            Log("ミュート状態ファイルを削除できません: %s", error.message().c_str());
            return false;
        }
        m_active = false;
        m_deviceId.clear();
        return true;
    }

    bool ReadState() {
        std::error_code error;
        const bool exists = std::filesystem::exists(m_path, error);
        if (error) {
            Log("ミュート状態ファイルを確認できません: %s", error.message().c_str());
            return false;
        }
        if (!exists) {
            return false;
        }
        // 壊れたファイルから際限なくメモリを確保しない。
        const auto size = std::filesystem::file_size(m_path, error);
        std::ifstream input;
        std::string id;
        std::string mute;
        if (!error && size <= kMaxStateFileBytes) {
            input.open(m_path, std::ios::binary);
        }
        if (!input.is_open() || !std::getline(input, id) || !std::getline(input, mute)
            || (mute != "0" && mute != "1") || id.empty()
            || id.find('\r') != std::string::npos || DecodeDeviceId(id).empty()
            || input.peek() != std::char_traits<char>::eof() || input.bad()) {
            input.close();
            Log("ミュート状態ファイルを読めないか形式が不正なため削除します。");
            DeleteState();
            return false;
        }
        m_deviceId = std::move(id);
        m_originalMute = mute == "1" ? TRUE : FALSE;
        m_active = true;
        return true;
    }

    bool SaveState() {
        std::error_code error;
        std::filesystem::create_directories(m_path.parent_path(), error);
        if (error) {
            Log("ミュート状態の保存先を作成できません: %s", error.message().c_str());
            return false;
        }
        // 復旧できていない以前の記録を上書きしない（CREATE_NEW は、すでにあれば失敗する）。
        const std::string contents = m_deviceId + (m_originalMute ? "\n1\n" : "\n0\n");
        const HANDLE file = CreateFileW(m_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            Log("ミュート状態ファイルを作成できません: %lu", GetLastError());
            return false;
        }
        DWORD written = 0;
        const bool saved = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr)
            && written == contents.size() && FlushFileBuffers(file);
        const DWORD writeError = saved ? ERROR_SUCCESS : GetLastError();
        const bool closed = CloseHandle(file) != FALSE;
        if (!saved || !closed) {
            Log("ミュート状態ファイルの保存に失敗しました: %lu", saved ? GetLastError() : writeError);
            DeleteState();
            return false;
        }
        return true;
    }

    HRESULT RestoreDevice(IMMDeviceEnumerator* enumerator) {
        ComPtr<IMMDevice> device;
        const auto wideId = DecodeDeviceId(m_deviceId);
        HRESULT result = enumerator->GetDevice(wideId.c_str(), &device);
        if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            Log("復元先の再生デバイスがなくなったため記録を削除します: id=%s", m_deviceId.c_str());
            DeleteState();
            return S_OK;
        }
        if (FAILED(result)) { return result; }
        ComPtr<IAudioEndpointVolume> volume;
        result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(volume.GetAddressOf()));
        if (FAILED(result)) { return result; }
        result = volume->SetMute(m_originalMute, nullptr);
        if (FAILED(result)) { return result; }
        Log("スピーカーのミュートを復元しました: id=%s, mute=%d", m_deviceId.c_str(), m_originalMute);
        DeleteState();
        return S_OK;
    }

    HRESULT MuteDefaultDevice(IMMDeviceEnumerator* enumerator, Clock::time_point deadline) {
        ComPtr<IMMDevice> device;
        HRESULT result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(result)) { return result; }
        LPWSTR rawId = nullptr;
        result = device->GetId(&rawId);
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> id(rawId, CoTaskMemFree);
        if (FAILED(result)) { return result; }
        m_deviceId = id ? win32::ToUtf8(id.get()) : std::string{};
        if (m_deviceId.empty()) { return E_FAIL; }
        ComPtr<IAudioEndpointVolume> volume;
        result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(volume.GetAddressOf()));
        if (FAILED(result)) { return result; }
        result = volume->GetMute(&m_originalMute);
        if (FAILED(result)) { return result; }
        if (Clock::now() >= deadline) { return HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
        if (!SaveState()) { return E_FAIL; }
        // 保存後に失敗しても復元できるよう、SetMute より先に所有状態を記録する。
        m_active = true;
        if (Clock::now() >= deadline) {
            DeleteState();
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        result = volume->SetMute(TRUE, nullptr);
        if (SUCCEEDED(result)) {
            Log("スピーカーをミュートしました: id=%s, originalMute=%d", m_deviceId.c_str(), m_originalMute);
        }
        return result;
    }

    HRESULT Execute(const Request& request) {
        if (request.operation == Operation::Mute && m_active) {
            return S_OK;
        }
        if (request.operation == Operation::Restore && !m_active) {
            return S_OK;
        }
        if (!PreparePath()) { return E_FAIL; }
        if (request.operation == Operation::RestoreLeftover && !m_active && !ReadState()) {
            return S_OK;
        }
        ComPtr<IMMDeviceEnumerator> enumerator;
        const HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) { return result; }
        return request.operation == Operation::Mute ? MuteDefaultDevice(enumerator.Get(), request.deadline)
            : RestoreDevice(enumerator.Get());
    }

    void Run(std::stop_token token) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) {
            Log("ミュート専用スレッドの COM 初期化に失敗しました: 0x%08lx",
                static_cast<unsigned long>(initialized));
        }
        while (true) {
            std::shared_ptr<Request> request;
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait(lock, [&] { return token.stop_requested() || !m_requests.empty(); });
                if (m_requests.empty()) { break; }
                request = m_requests.front();
                m_requests.pop_front();
            }
            try {
                // 復元は待機が切れていても実行し、切断後に遅れて届くミュートは捨てる。
                if (SUCCEEDED(initialized)
                    && (request->operation != Operation::Mute || Clock::now() < request->deadline)) {
                    const HRESULT result = Execute(*request);
                    if (FAILED(result)) {
                        Log("スピーカーのミュート操作に失敗しました: operation=%d, 0x%08lx",
                            static_cast<int>(request->operation), static_cast<unsigned long>(result));
                    }
                }
            } catch (const std::exception& error) {
                Log("スピーカーのミュート操作に失敗しました: %s", error.what());
            }
            {
                std::lock_guard lock(m_mutex);
                request->done = true;
            }
            m_condition.notify_all();
        }
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
    }
};

SpeakerMute::SpeakerMute() : m_state(std::make_unique<State>()) {}

SpeakerMute::~SpeakerMute() {
    Restore();
}

void SpeakerMute::Submit(Operation operation) {
    const auto deadline = Clock::now() + kRequestTimeout;
    try {
        auto request = std::make_shared<State::Request>(operation, deadline);
        std::unique_lock lock(m_state->m_mutex);
        // 静的なドライバ生成時には、ローダーロック内でスレッドを起動しない。
        if (!m_state->m_thread.joinable()) {
            m_state->m_thread = std::jthread([state = m_state.get()](std::stop_token token) { state->Run(token); });
        }
        m_state->m_requests.push_back(request);
        m_state->m_condition.notify_all();
        if (!m_state->m_condition.wait_until(lock, deadline, [&] { return request->done; })) {
            Log("スピーカーのミュート操作の待機が 2 秒を超えました: operation=%d", static_cast<int>(operation));
        }
    } catch (const std::exception& error) {
        Log("スピーカーのミュート操作を依頼できません: %s", error.what());
    }
}

void SpeakerMute::RestoreLeftover() { Submit(Operation::RestoreLeftover); }
void SpeakerMute::Mute() { Submit(Operation::Mute); }
void SpeakerMute::Restore() { Submit(Operation::Restore); }

}
