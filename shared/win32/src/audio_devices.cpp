#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <wnvr/win32/audio_devices.h>

#include <memory>
#include <utility>

namespace wnvr::win32 {

std::optional<AudioEndpoint> FindRenderEndpoint(std::wstring_view nameFragment) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)))
        || FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        return std::nullopt;
    }
    UINT count = 0;
    if (FAILED(devices->GetCount(&count))) {
        return std::nullopt;
    }
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> properties;
        if (FAILED(devices->Item(index, &device)) || FAILED(device->OpenPropertyStore(STGM_READ, &properties))) {
            continue;
        }
        struct NameScope {
            PROPVARIANT value{};
            ~NameScope() { PropVariantClear(&value); }
        } nameScope;
        auto& name = nameScope.value;
        const HRESULT result = properties->GetValue(PKEY_Device_FriendlyName, &name);
        std::wstring friendlyName;
        if (SUCCEEDED(result) && name.vt == VT_LPWSTR && name.pwszVal != nullptr) {
            friendlyName = name.pwszVal;
        }
        if (friendlyName.find(nameFragment) == std::wstring::npos) {
            continue;
        }
        LPWSTR rawId = nullptr;
        const HRESULT idResult = device->GetId(&rawId);
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> id(rawId, CoTaskMemFree);
        if (SUCCEEDED(idResult) && id) {
            return AudioEndpoint{id.get(), std::move(friendlyName)};
        }
    }
    return std::nullopt;
}

}
