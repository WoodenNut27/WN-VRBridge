#include "network_adapters.h"

#include "host_log.h"

#include <wnvr/win32/platform.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <utility>

namespace wnvr::host {

std::string BindAddressFor(const std::string& adapterId) {
    if (adapterId.empty()) {
        return "0.0.0.0";
    }
    for (const auto& adapter : EnumerateAdapters()) {
        if (adapter.id == adapterId) {
            return adapter.ipv4;
        }
    }
    LogWarn("選択したアダプターが見つかりません: %s", adapterId.c_str());
    return {};
}

std::vector<NetworkAdapter> EnumerateAdapters() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    // 列挙中に構成が変わる場合もあるため、必要なサイズで再試行する。
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    std::vector<NetworkAdapter> adapters;
    if (result == ERROR_NO_DATA) {
        return adapters;
    }
    if (result != NO_ERROR) {
        LogError("ネットワークアダプターの列挙に失敗しました: %lu", result);
        return adapters;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        adapter != nullptr; adapter = adapter->Next) {
        // つながっていないアダプターと、自分自身にしか届かないループバックは選ばせない。
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK
            || adapter->AdapterName == nullptr) {
            continue;
        }
        for (auto* address = adapter->FirstUnicastAddress; address != nullptr; address = address->Next) {
            if (address->Address.lpSockaddr == nullptr || address->Address.lpSockaddr->sa_family != AF_INET
                || address->Address.iSockaddrLength < static_cast<int>(sizeof(sockaddr_in))) {
                continue;
            }
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr);
            char text[INET_ADDRSTRLEN]{};
            if (InetNtopA(AF_INET, &ipv4->sin_addr, text, sizeof(text)) != nullptr) {
                auto name = adapter->FriendlyName ? win32::ToUtf8(adapter->FriendlyName) : std::string{};
                // 名前を取れないときは、設定に保存している ID（GUID）をそのまま見せる。
                if (name.empty()) {
                    name = adapter->AdapterName;
                }
                adapters.push_back({adapter->AdapterName, std::move(name), text, adapter->ReceiveLinkSpeed});
                break;
            }
        }
    }
    return adapters;
}

}
