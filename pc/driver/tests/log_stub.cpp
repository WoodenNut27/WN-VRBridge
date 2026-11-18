#include "log_stub.h"

#include "driver_log.h"

#include <cstdarg>
#include <cstdio>

// 本物の Log は SteamVR のログに書くので、SteamVR の外では動かない。
// テストでは、同じ名前の関数でログを配列にためる。
namespace wnvr::driver {

namespace testing {

std::vector<std::string>& LogLines() {
    static std::vector<std::string> lines;
    return lines;
}

}

void Log(const char* fmt, ...) {
    char buffer[2048]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    testing::LogLines().push_back(buffer);
}

}
