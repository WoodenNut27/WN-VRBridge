#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wnvr::host {

struct NetworkAdapter {
    std::string id;
    std::string name;
    std::string ipv4;
    uint64_t speedBps = 0;
};

std::vector<NetworkAdapter> EnumerateAdapters();
std::string BindAddressFor(const std::string& adapterId);

}
