#include "driver_log.h"

#include <openvr_driver.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace wnvr::driver {

void Log(const char* fmt, ...) {
    auto* driverLog = vr::VRDriverLog();
    if (driverLog == nullptr) {
        return;
    }

    constexpr char kPrefix[] = "[WN-VRBridge] ";
    char buffer[2048]{};
    std::memcpy(buffer, kPrefix, sizeof(kPrefix) - 1);
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer + sizeof(kPrefix) - 1, sizeof(buffer) - sizeof(kPrefix) + 1, fmt, args);
    va_end(args);
    driverLog->Log(buffer);
}

}
