#include "device_provider.h"

#include <cstring>

static wnvr::driver::DeviceProvider g_deviceProvider;

// SteamVR がドライバを読み込むときに呼ぶ入口。
extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (interfaceName != nullptr && std::strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version) == 0) {
        return &g_deviceProvider;
    }
    if (returnCode != nullptr) {
        *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    }
    return nullptr;
}
