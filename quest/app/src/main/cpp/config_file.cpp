#include "config_file.h"

#include <wnvr/protocol/messages.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace wnvr::quest {
namespace {

constexpr std::string_view kWhitespace = " \t\r\n\v\f";

std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

// 文字列の全体が 10 進の整数のときだけ成功する（「12abc」や空文字列は失敗）。
template<typename T>
bool ParseWhole(std::string_view text, T& value) {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

}

std::vector<std::string> ReadIntSettings(std::istream& input, std::string_view fileName,
    std::span<const IntSetting> settings) {
    std::vector<std::string> warnings;
    std::string line;
    size_t lineNumber = 0;
    const auto warn = [&](const std::string& reason) {
        warnings.push_back(std::string(fileName) + " の " + std::to_string(lineNumber) + " 行目を無視: " + reason);
    };
    while (std::getline(input, line)) {
        ++lineNumber;
        const auto text = Trim(line);
        if (text.empty() || text.front() == '#') {
            continue;
        }
        const auto separator = text.find('=');
        if (separator == std::string_view::npos) {
            warn("キー=値が必要です");
            continue;
        }
        const auto key = Trim(text.substr(0, separator));
        const auto found = std::find_if(settings.begin(), settings.end(),
            [key](const IntSetting& setting) { return setting.key == key; });
        if (found == settings.end()) {
            warn("不明なキーです");
            continue;
        }
        const auto valueText = Trim(text.substr(separator + 1));
        if (valueText.empty()) {
            warn("整数が必要です");
            continue;
        }
        int value = 0;
        if (!ParseWhole(valueText, value) || value < found->minimum || value > found->maximum) {
            warn(std::to_string(found->minimum) + "〜" + std::to_string(found->maximum) + " の整数が必要です");
            continue;
        }
        found->apply(value);
    }
    if (input.bad()) {
        warnings.push_back(std::string(fileName) + " の読み込み中にエラーが発生しました");
    }
    return warnings;
}

std::optional<EyeSize> ReadEyeSize(std::istream& input, std::vector<std::string>* warnings) {
    EyeSize size;
    const IntSetting settings[]{
        {"width", protocol::kMinQuestEyeSize, protocol::kMaxQuestEyeSize,
            [&](int value) { size.width = static_cast<uint32_t>(value); }},
        {"height", protocol::kMinQuestEyeSize, protocol::kMaxQuestEyeSize,
            [&](int value) { size.height = static_cast<uint32_t>(value); }},
    };
    auto reasons = ReadIntSettings(input, "eye_size.txt", settings);
    const bool valid = size.width != 0 && size.height != 0 && reasons.empty();
    if (size.width == 0 || size.height == 0) {
        reasons.emplace_back("eye_size.txt の width と height の両方が必要です");
    }
    if (warnings) {
        *warnings = std::move(reasons);
    }
    return valid ? std::optional<EyeSize>{size} : std::nullopt;
}

std::string FormatEyeSize(EyeSize size) {
    return "width=" + std::to_string(size.width) + "\nheight=" + std::to_string(size.height) + "\n";
}

EyeSize ChooseEyeSize(EyeSize recommended, EyeSize maximum, std::optional<EyeSize> saved) {
    const auto size = saved.value_or(recommended);
    if (size.width <= maximum.width && size.height <= maximum.height) {
        return size;
    }
    if (size.width == 0 || size.height == 0) {
        return {};
    }
    const double scale = std::min(static_cast<double>(maximum.width) / size.width,
        static_cast<double>(maximum.height) / size.height);
    return {std::min(maximum.width, static_cast<uint32_t>(std::lround(size.width * scale))),
        std::min(maximum.height, static_cast<uint32_t>(std::lround(size.height * scale)))};
}

std::optional<HostAddress> ParseHostLine(std::string_view line, uint16_t defaultPort) {
    const auto text = Trim(line);
    if (text.empty() || text.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto separator = text.find(':');
    HostAddress address{std::string(text.substr(0, separator)), defaultPort};
    if (separator != std::string_view::npos) {
        unsigned port = 0;
        if (!ParseWhole(text.substr(separator + 1), port) || port == 0 || port > 65535) {
            return std::nullopt;
        }
        address.port = static_cast<uint16_t>(port);
    }
    if (address.host.empty() || address.port == 0) {
        return std::nullopt;
    }
    return address;
}

}
