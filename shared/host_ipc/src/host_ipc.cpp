#include <wnvr/host_ipc/host_ipc.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace wnvr::ipc {
namespace {

using Json = nlohmann::json;

// 統計の項目とキーの対応は、書く側と読む側でこの 1 か所を使う。
// 項目を足すときはここに 1 行足せば、どちらにも反映される。
template<typename Stats, typename Visitor>
void ForEachStatsField(Stats& stats, Visitor&& visit) {
    visit("clientConnected", stats.clientConnected);
    visit("clientName", stats.clientName);
    visit("clientSessionId", stats.clientSessionId);
    visit("renderFps", stats.renderFps);
    visit("encodeFps", stats.encodeFps);
    visit("encodeAverageMs", stats.encodeAverageMs);
    visit("encodeMaxMs", stats.encodeMaxMs);
    visit("sentMbps", stats.sentMbps);
    visit("sentFragments", stats.sentFragments);
    visit("sendDroppedFrames", stats.sendDroppedFrames);
    visit("virtualVsyncHz", stats.virtualVsyncHz);
    visit("receivedFps", stats.receivedFps);
    visit("lostFrames", stats.lostFrames);
    visit("fragmentLossPercent", stats.fragmentLossPercent);
    visit("receivedMbps", stats.receivedMbps);
    visit("decodeAverageMs", stats.decodeAverageMs);
    visit("decodeMaxMs", stats.decodeMaxMs);
    visit("displayFps", stats.displayFps);
    visit("repeatedFrames", stats.repeatedFrames);
    visit("latencyAverageMs", stats.latencyAverageMs);
    visit("latencyMaxMs", stats.latencyMaxMs);
    visit("roundTripMs", stats.roundTripMs);
    visit("audioUnderruns", stats.audioUnderruns);
    visit("audioMissingFrames", stats.audioMissingFrames);
    visit("audioBufferAverageMs", stats.audioBufferAverageMs);
    visit("pcMicState", stats.pcMicState);
    visit("questMicState", stats.questMicState);
}

// 型が合わない値や範囲外の値は、既定値で埋めずに失敗にする（相手の不具合を隠さない）。
template<class T>
void Read(const Json& object, const char* key, T& result) {
    const auto& value = object.at(key);
    bool valid = false;
    if constexpr (std::is_same_v<T, bool>) {
        valid = value.is_boolean();
    } else if constexpr (std::is_same_v<T, std::string>) {
        valid = value.is_string();
    } else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, float>) {
        valid = value.is_number() && std::isfinite(value.get<T>()) && value.get<double>() >= 0;
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        valid = value.is_number_integer() && (value.is_number_unsigned()
            ? value.get<uint64_t>() <= std::numeric_limits<uint32_t>::max()
            : value.get<int64_t>() >= 0 && value.get<int64_t>() <= std::numeric_limits<uint32_t>::max());
    } else if constexpr (std::is_same_v<T, uint64_t>) {
        valid = value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0);
    } else {
        static_assert(std::is_same_v<T, int>);
        valid = value.is_number_integer() && (value.is_number_unsigned()
            ? value.get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int>::max())
            : value.get<int64_t>() >= std::numeric_limits<int>::min() && value.get<int64_t>() <= std::numeric_limits<int>::max());
    }
    if (!valid) {
        throw std::runtime_error(std::string("不正なキー: ") + key);
    }
    result = value.get<T>();
}

std::vector<std::string> ReadDeviceIds(const Json& object, const char* key) {
    const auto& entries = object.at(key);
    if (!entries.is_array() || entries.size() > kMaxApprovedDevices) {
        throw std::runtime_error("端末一覧が不正です");
    }
    std::vector<std::string> ids;
    for (const auto& entry : entries) {
        if (!entry.is_string() || !IsValidDeviceId(entry.get_ref<const std::string&>())) {
            throw std::runtime_error("端末 ID が不正です");
        }
        ids.push_back(entry.get<std::string>());
    }
    return ids;
}

Json ToJson(const HelloMessage& value) {
    return {{"type", "hello"}, {"protocolVersion", value.protocolVersion}, {"version", value.version}};
}

Json ToJson(const SettingsMessage& value) {
    return {{"type", "settings"}, {"bindAddress", value.bindAddress}, {"bitrateMbps", value.bitrateMbps},
        {"questResolutionPercent", value.questResolutionPercent},
        {"foveatedEncoding", value.foveatedEncoding}, {"foveationEdgeRatio", value.foveationEdgeRatio},
        {"autoStopSteamVr", value.autoStopSteamVr},
        {"streamAudioToQuest", value.streamAudioToQuest}, {"micToPc", value.micToPc},
        {"muteSpeakerWhileConnected", value.muteSpeakerWhileConnected},
        {"approvedDeviceIds", value.approvedDeviceIds}, {"rejectedDeviceIds", value.rejectedDeviceIds}};
}

Json ToJson(const PairingRequestMessage& value) {
    return {{"type", "pairingRequest"}, {"deviceId", value.deviceId},
        {"deviceName", value.deviceName}, {"address", value.address}};
}

Json ToJson(const StatusMessage& value) {
    Json json;
    json["type"] = "status";
    auto& stats = json["stats"];
    ForEachStatsField(value.stats, [&stats](const char* key, const auto& field) { stats[key] = field; });
    return json;
}

HelloMessage ReadHello(const Json& json) {
    HelloMessage value;
    Read(json, "protocolVersion", value.protocolVersion);
    Read(json, "version", value.version);
    return value;
}

SettingsMessage ReadSettings(const Json& json) {
    SettingsMessage value;
    Read(json, "bindAddress", value.bindAddress);
    Read(json, "bitrateMbps", value.bitrateMbps);
    Read(json, "questResolutionPercent", value.questResolutionPercent);
    Read(json, "foveatedEncoding", value.foveatedEncoding);
    // float への変換で境界の外の値が範囲内へ丸まらないよう、元の数値を検査する。
    const auto& ratio = json.at("foveationEdgeRatio");
    if (!ratio.is_number() || !std::isfinite(ratio.get<double>())
        || ratio < kMinFoveationEdgeRatio || ratio > kMaxFoveationEdgeRatio) {
        throw std::runtime_error("設定が範囲外です");
    }
    Read(json, "foveationEdgeRatio", value.foveationEdgeRatio);
    Read(json, "streamAudioToQuest", value.streamAudioToQuest);
    Read(json, "micToPc", value.micToPc);
    Read(json, "autoStopSteamVr", value.autoStopSteamVr);
    Read(json, "muteSpeakerWhileConnected", value.muteSpeakerWhileConnected);
    if (value.bindAddress.size() > 64 || value.bitrateMbps < kMinBitrateMbps || value.bitrateMbps > kMaxBitrateMbps
        || value.questResolutionPercent < kMinQuestResolutionPercent || value.questResolutionPercent > kMaxQuestResolutionPercent) {
        throw std::runtime_error("設定が範囲外です");
    }
    value.approvedDeviceIds = ReadDeviceIds(json, "approvedDeviceIds");
    value.rejectedDeviceIds = ReadDeviceIds(json, "rejectedDeviceIds");
    return value;
}

PairingRequestMessage ReadPairingRequest(const Json& json) {
    PairingRequestMessage value;
    Read(json, "deviceId", value.deviceId);
    Read(json, "deviceName", value.deviceName);
    Read(json, "address", value.address);
    // アドレスは確認のダイアログに出すので、IPv4 の表記に使う文字だけを通す。
    if (!IsValidDeviceId(value.deviceId) || value.deviceName.size() > kMaxDeviceNameBytes
        || value.address.empty() || value.address.size() > 15
        || value.address.find_first_not_of("0123456789.") != std::string::npos) {
        throw std::runtime_error("ペアリング要求が不正です");
    }
    return value;
}

StatusMessage ReadStatus(const Json& json) {
    StatusMessage value;
    const auto& stats = json.at("stats");
    ForEachStatsField(value.stats, [&stats](const char* key, auto& field) { Read(stats, key, field); });
    constexpr int kMaxMicState = 3;
    if (value.stats.pcMicState < 0 || value.stats.pcMicState > kMaxMicState
        || value.stats.questMicState < 0 || value.stats.questMicState > kMaxMicState) {
        throw std::runtime_error("マイクの状態が範囲外です");
    }
    if (value.stats.clientName.size() > 256) {
        throw std::runtime_error("端末名が長すぎます");
    }
    return value;
}

Message ReadMessage(std::string_view text) {
    if (text.size() > kMaxMessageBytes) {
        throw std::runtime_error("メッセージが上限を超えています");
    }
    const auto json = Json::parse(text);
    std::string type;
    Read(json, "type", type);
    if (type == "hello") {
        return ReadHello(json);
    }
    if (type == "settings") {
        return ReadSettings(json);
    }
    if (type == "pairingRequest") {
        return ReadPairingRequest(json);
    }
    if (type == "status") {
        return ReadStatus(json);
    }
    throw std::runtime_error("未知のメッセージ型です");
}

}

EyeSize QuestEyeSizeForPercent(int percent) {
    constexpr double kPercentBase = 100.0;
    const double scale = std::clamp(percent, kMinQuestResolutionPercent, kMaxQuestResolutionPercent) / kPercentBase;
    return {static_cast<int>(std::lround(kQuestPanelEyeWidth * scale)),
        static_cast<int>(std::lround(kQuestPanelEyeHeight * scale))};
}

std::string Encode(const Message& message) {
    const Json json = std::visit([](const auto& value) { return ToJson(value); }, message);
    // 端末名に壊れた UTF-8 が入っていても例外にせず、置き換えて送る。
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<Message> Decode(std::string_view text, std::string* error) {
    // エラー文字列の確保の失敗も含めて、例外を DLL の境界へ漏らさない。
    const auto report = [error](const char* reason) noexcept {
        try {
            if (error) {
                *error = reason;
            }
        } catch (...) {}
    };
    try {
        if (error) {
            error->clear();
        }
        return ReadMessage(text);
    } catch (const std::exception& exception) {
        report(exception.what());
    } catch (...) {
        report("予期しないデコードエラー");
    }
    return std::nullopt;
}

}
