#include "host_log.h"

#include <wnvr/win32/platform.h>

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace wnvr::host {
namespace {

constexpr auto kKeepLogs = std::chrono::days(7);

std::mutex g_logMutex;
std::filesystem::path g_logDirectory;
std::ofstream g_logFile;
std::string g_logDate;

std::string DateText(const SYSTEMTIME& time) {
    char date[16]{};
    std::snprintf(date, sizeof(date), "%04u%02u%02u", time.wYear, time.wMonth, time.wDay);
    return date;
}

// 日付が変わったら、その日のファイルに切り替える。g_logMutex を取ってから呼ぶ。
void OpenLogFileLocked(const std::string& date) {
    g_logFile.close();
    g_logFile.clear();
    g_logFile.open(g_logDirectory / ("host-" + date + ".log"), std::ios::app | std::ios::binary);
    g_logDate = date;
}

// 「host-YYYYMMDD.log」の形のファイルだけを対象にして、古いものを消す。
void RemoveOldLogs() {
    const auto cutoff = std::filesystem::file_time_type::clock::now() - kKeepLogs;
    std::error_code error;
    std::filesystem::directory_iterator entry(g_logDirectory, error);
    const std::filesystem::directory_iterator end;
    while (!error && entry != end) {
        const auto filename = entry->path().filename().wstring();
        if (filename.size() == 17 && filename.starts_with(L"host-") && filename.ends_with(L".log")
            && filename.find_first_not_of(L"0123456789", 5) == 13) {
            std::error_code fileError;
            if (entry->is_regular_file(fileError) && !fileError) {
                const auto written = entry->last_write_time(fileError);
                if (!fileError && written < cutoff) {
                    std::filesystem::remove(entry->path(), fileError);
                }
            }
        }
        entry.increment(error);
    }
}

void WriteLog(const char* level, const char* format, va_list args) {
    std::lock_guard lock(g_logMutex);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const auto date = DateText(time);
    if (!g_logDirectory.empty() && (g_logDate != date || !g_logFile.is_open())) {
        OpenLogFileLocked(date);
    }
    va_list countArgs;
    va_copy(countArgs, args);
    const int length = std::vsnprintf(nullptr, 0, format, countArgs);
    va_end(countArgs);
    if (length < 0) {
        return;
    }
    std::vector<char> message(static_cast<size_t>(length) + 1);
    std::vsnprintf(message.data(), message.size(), format, args);
    // 改行を置換し、メッセージ中に改行があっても一件を一行に保つ。
    for (char& ch : message) {
        if (ch == '\r' || ch == '\n') {
            ch = ' ';
        }
    }
    char prefix[80]{};
    std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, level);
    if (g_logFile.is_open()) {
        g_logFile << prefix << message.data() << '\n';
        g_logFile.flush();
    }
    // ファイルに書けないときも、デバッガーからは追えるようにする。
    if (!g_logFile) {
        OutputDebugStringA(prefix);
        OutputDebugStringA(message.data());
        OutputDebugStringA("\n");
    }
}

}

bool InitializeLog() {
    std::lock_guard lock(g_logMutex);
    const auto folder = win32::AppLocalDataPath();
    if (folder.empty()) {
        return false;
    }
    g_logDirectory = folder / L"logs";
    std::error_code error;
    std::filesystem::create_directories(g_logDirectory, error);
    if (error) {
        return false;
    }
    RemoveOldLogs();
    SYSTEMTIME time{};
    GetLocalTime(&time);
    OpenLogFileLocked(DateText(time));
    return g_logFile.is_open();
}

void ShutdownLog() {
    std::lock_guard lock(g_logMutex);
    g_logFile.close();
    g_logDirectory.clear();
    g_logDate.clear();
}

std::filesystem::path GetLogDirectory() {
    std::lock_guard lock(g_logMutex);
    return g_logDirectory;
}

void LogInfo(const char* format, ...) {
    va_list args;
    va_start(args, format);
    WriteLog("INFO", format, args);
    va_end(args);
}

void LogWarn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    WriteLog("WARN", format, args);
    va_end(args);
}

void LogError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    WriteLog("ERROR", format, args);
    va_end(args);
}

}
