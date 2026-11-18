#include <wnvr/win32/platform.h>

#include <windows.h>
#include <shlobj.h>

#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace wnvr::win32 {
namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, 0, nullptr, &raw);
    // 失敗したときも、返されたポインタは解放する決まりになっている。
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(raw, CoTaskMemFree);
    return SUCCEEDED(result) && folder ? std::filesystem::path(folder.get()) : std::filesystem::path{};
}

std::filesystem::path AppFolder(REFKNOWNFOLDERID id) {
    const auto folder = KnownFolder(id);
    return folder.empty() ? folder : folder / kAppFolderName;
}

}

void WaitForStop(std::stop_token token, std::chrono::milliseconds duration) {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock(mutex);
    condition.wait_for(lock, token, duration, [] { return false; });
}

std::wstring ToWide(std::string_view utf8, InvalidText invalid) {
    if (utf8.empty() || utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const DWORD flags = invalid == InvalidText::Fail ? MB_ERR_INVALID_CHARS : 0;
    const int length = static_cast<int>(utf8.size());
    const int size = MultiByteToWideChar(CP_UTF8, flags, utf8.data(), length, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, flags, utf8.data(), length, wide.data(), size) != size) {
        return {};
    }
    return wide;
}

std::string ToUtf8(std::wstring_view wide, InvalidText invalid) {
    if (wide.empty() || wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const DWORD flags = invalid == InvalidText::Fail ? WC_ERR_INVALID_CHARS : 0;
    const int length = static_cast<int>(wide.size());
    const int size = WideCharToMultiByte(CP_UTF8, flags, wide.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, flags, wide.data(), length, utf8.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return utf8;
}

std::string TruncateUtf8(std::string text, size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return text;
    }
    size_t end = maxBytes;
    // 継続バイト（10xxxxxx）の上で切ると最後の文字が壊れるので、文字の先頭まで戻る。
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) {
        --end;
    }
    text.resize(end);
    return text;
}

std::filesystem::path ExecutablePath() {
    // 長いパスを有効にした環境でも収まる大きさ（NTFS のパスの上限）。
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::filesystem::path AppLocalDataPath() {
    return AppFolder(FOLDERID_LocalAppData);
}

std::filesystem::path AppRoamingDataPath() {
    return AppFolder(FOLDERID_RoamingAppData);
}

std::filesystem::path LocalAppDataPath() {
    return KnownFolder(FOLDERID_LocalAppData);
}

std::string ComputerName(size_t maxUtf8Bytes, std::string_view fallback) {
    DWORD length = 0;
    GetComputerNameExW(ComputerNameDnsHostname, nullptr, &length);
    if (length == 0) {
        return std::string(fallback);
    }
    std::vector<wchar_t> name(length);
    if (!GetComputerNameExW(ComputerNameDnsHostname, name.data(), &length) || length == 0) {
        return std::string(fallback);
    }
    auto utf8 = ToUtf8(std::wstring_view(name.data(), length));
    if (utf8.empty()) {
        return std::string(fallback);
    }
    return TruncateUtf8(std::move(utf8), maxUtf8Bytes);
}

}
