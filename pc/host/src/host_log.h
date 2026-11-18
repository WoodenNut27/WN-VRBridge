#pragma once

#include <filesystem>

namespace wnvr::host {

bool InitializeLog();
void ShutdownLog();
std::filesystem::path GetLogDirectory();
void LogInfo(const char* format, ...);
void LogWarn(const char* format, ...);
void LogError(const char* format, ...);

}
