#include "device_identity.h"
#include "log.h"

#include <array>
#include <fstream>
#include <limits>
#include <random>

namespace wnvr::quest {
namespace {

constexpr size_t kDeviceIdLength = 32;
constexpr char kHexDigits[] = "0123456789abcdef";

std::string ReadSavedId(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    // 1 文字多く読み、長すぎるファイルも不正として見分ける。
    std::array<char, kDeviceIdLength + 1> buffer{};
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::string id(buffer.data(), static_cast<size_t>(file.gcount()));
    if (!file.bad() && id.size() == kDeviceIdLength && id.find_first_not_of(kHexDigits) == std::string::npos) {
        return id;
    }
    WNVR_LOGW("device_id.txt の形式が不正なため ID を作り直します");
    return {};
}

std::string GenerateId() {
    std::array<unsigned char, kDeviceIdLength / 2> bytes{};
    std::ifstream random("/dev/urandom", std::ios::binary);
    random.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!random || random.gcount() != static_cast<std::streamsize>(bytes.size())) {
        WNVR_LOGW("/dev/urandom を読めないため random_device を使います");
        std::random_device source;
        for (auto& byte : bytes) {
            byte = static_cast<unsigned char>(source());
        }
    }
    std::string id;
    id.reserve(kDeviceIdLength);
    for (const auto byte : bytes) {
        id.push_back(kHexDigits[byte >> 4]);
        id.push_back(kHexDigits[byte & 15]);
    }
    return id;
}

}

std::string LoadDeviceId(const char* directory) {
    const std::string path = directory && directory[0] != '\0'
        ? std::string(directory) + "/device_id.txt" : "";
    if (!path.empty()) {
        auto saved = ReadSavedId(path);
        if (!saved.empty()) {
            return saved;
        }
    }
    const auto id = GenerateId();
    bool saved = false;
    if (!path.empty()) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << id;
        file.close();
        saved = !file.fail();
    }
    if (!saved) {
        WNVR_LOGW("端末 ID を保存できません。今回の起動だけ使う ID: %s", id.c_str());
    }
    return id;
}

uint32_t MakeSessionId(uint32_t previous) {
    std::random_device source;
    std::uniform_int_distribution<uint32_t> distribution(1, std::numeric_limits<uint32_t>::max());
    uint32_t sessionId;
    do {
        sessionId = distribution(source);
    } while (sessionId == previous);
    return sessionId;
}

}
