#include <wnvr/host_ipc/host_ipc.h>

#ifdef _WIN32
#include <wnvr/host_ipc/pipe_io.h>
#endif

#include <doctest/doctest.h>

#include <limits>
#include <string>
#include <thread>

namespace {

using namespace wnvr::ipc;

void Reject(std::string_view text) {
    std::string error;
    CHECK(!Decode(text, &error));
    CHECK(!error.empty());
}

TEST_CASE("壊れた UTF-8 の端末名でも例外を出さない") {
    StatusMessage status;
    status.stats.clientName = "Quest " + std::string(1, static_cast<char>(0xFF)) + " 3";
    try {
        const auto decoded = Decode(Encode(status));
        CHECK(decoded);
        if (decoded) {
            const auto* result = std::get_if<StatusMessage>(&*decoded);
            CHECK(result);
            if (result) {
                CHECK(result->stats.clientName == "Quest \xEF\xBF\xBD 3");
            }
        }
    } catch (...) {
        CHECK(false);
    }
}

#ifdef _WIN32
TEST_CASE("名前付きパイプで大きなメッセージと遅れて届くメッセージを読める") {
    // 実際のドライバのパイプと衝突せず、即時完了する複数断片を検証する。
    const auto name = L"\\\\.\\pipe\\WN-VRBridge.IpcTest." + std::to_wstring(GetCurrentProcessId());
    Handle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
        static_cast<DWORD>(kMaxMessageBytes), static_cast<DWORD>(kMaxMessageBytes), 0, nullptr));
    CHECK(server.m_value != INVALID_HANDLE_VALUE);
    if (server.m_value == INVALID_HANDLE_VALUE) {
        return;
    }
    Handle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    CHECK(client.m_value != INVALID_HANDLE_VALUE);
    if (client.m_value == INVALID_HANDLE_VALUE) {
        return;
    }
    Operation connect(server.m_value);
    const bool connected = ConnectNamedPipe(server.m_value, &connect.m_overlapped) != FALSE;
    CHECK((connected || GetLastError() == ERROR_PIPE_CONNECTED));
    DWORD mode = PIPE_READMODE_MESSAGE;
    CHECK(SetNamedPipeHandleState(client.m_value, &mode, nullptr, nullptr));
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    CHECK(stop.m_value);
    if (!stop.m_value) {
        return;
    }
    const HelloMessage message{kProtocolVersion, std::string(12000, 'x')};
    const HANDLE senders[] = {client.m_value, server.m_value};
    const HANDLE receivers[] = {server.m_value, client.m_value};
    for (size_t index = 0; index < 2; ++index) {
        CHECK(WriteMessage(senders[index], stop.m_value, message));
        bool received = false;
        const auto result = ReadMessages(receivers[index], stop.m_value, nullptr, [] {
            return true;
        }, [&](const Message& value) {
            received = true;
            const auto* hello = std::get_if<HelloMessage>(&value);
            CHECK(hello);
            if (hello) {
                CHECK(hello->version == message.version);
            }
            return false;
        });
        CHECK(received);
        CHECK(result.end == ReadEnd::ReceiveStopped);
    }
    Handle started(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    CHECK(started.m_value);
    if (!started.m_value) {
        return;
    }
    for (size_t index = 0; index < 2; ++index) {
        for (const DWORD delay : {0UL, 20UL, 150UL}) {
            ResetEvent(started.m_value);
            const HelloMessage delayedMessage{kProtocolVersion, "delay-" + std::to_string(delay)};
            bool sent = false;
            std::thread sender([&] {
                // 読み取り開始を待ち、同じ周回での完了と待機時間切れ後の完了を通す。
                if (WaitForSingleObject(started.m_value, 1000) == WAIT_OBJECT_0
                    && WaitForSingleObject(stop.m_value, delay) == WAIT_TIMEOUT) {
                    sent = WriteMessage(senders[index], stop.m_value, delayedMessage);
                }
            });
            bool received = false;
            const auto deadline = GetTickCount64() + 1000;
            const auto result = ReadMessages(receivers[index], stop.m_value, nullptr, [&] {
                SetEvent(started.m_value);
                // 回帰で受信が止まっても、テスト全体を待たせ続けない。
                return GetTickCount64() < deadline;
            }, [&](const Message& value) {
                received = true;
                const auto* hello = std::get_if<HelloMessage>(&value);
                CHECK(hello);
                if (hello) {
                    CHECK(hello->version == delayedMessage.version);
                }
                return false;
            });
            sender.join();
            CHECK(sent);
            CHECK(received);
            CHECK(result.end != ReadEnd::IoError);
            CHECK(result.end == ReadEnd::ReceiveStopped);
        }
    }
}
#endif

TEST_CASE("ペアリングの要求と端末の一覧") {
    const SettingsMessage settingsWithIds{"192.168.1.10", 80, true, true, {"AZaz09-_", "second"}, {"blocked"}};
    const auto decodedSettings = Decode(Encode(settingsWithIds));
    CHECK(decodedSettings);
    if (decodedSettings) {
        const auto* settings = std::get_if<SettingsMessage>(&*decodedSettings);
        CHECK(settings);
        if (settings) {
            CHECK(settings->approvedDeviceIds == settingsWithIds.approvedDeviceIds);
            CHECK(settings->rejectedDeviceIds == settingsWithIds.rejectedDeviceIds);
        }
    }
    const PairingRequestMessage request{"AZaz09-_", "Quest 日本語", "192.168.1.20"};
    const auto decodedRequest = Decode(Encode(request));
    CHECK(decodedRequest);
    if (decodedRequest) {
        const auto* pairing = std::get_if<PairingRequestMessage>(&*decodedRequest);
        CHECK(pairing);
        if (pairing) {
            CHECK(pairing->deviceId == request.deviceId);
            CHECK(pairing->deviceName == request.deviceName);
            CHECK(pairing->address == request.address);
        }
    }
    CHECK(IsValidDeviceId("AZaz09-_"));
    CHECK(IsValidDeviceId(std::string(64, 'a')));
    CHECK(!IsValidDeviceId(std::string(65, 'a')));
    for (bool approved : {false, true}) {
        SettingsMessage settings;
        auto& ids = approved ? settings.approvedDeviceIds : settings.rejectedDeviceIds;
        ids.assign(64, "valid");
        CHECK(Decode(Encode(settings)));
        ids.push_back("overflow");
        Reject(Encode(settings));
        for (const auto& id : {std::string{}, std::string(65, 'a'), std::string("a b"),
                std::string("a.b"), std::string("日本語"), std::string("a\0b", 3)}) {
            CHECK(!IsValidDeviceId(id));
            ids = {id};
            Reject(Encode(settings));
            Reject(Encode(PairingRequestMessage{id, "Quest", "192.168.1.20"}));
        }
    }
    CHECK(Decode(Encode(PairingRequestMessage{"valid", std::string(64, 'x'), "192.168.1.20"})));
    Reject(Encode(PairingRequestMessage{"valid", std::string(65, 'x'), "192.168.1.20"}));
    Reject(R"({"type":"settings","bindAddress":"","bitrateMbps":80,"streamAudioToQuest":true,"muteSpeakerWhileConnected":true,"approvedDeviceIds":"id","rejectedDeviceIds":[]})");
    Reject(R"({"type":"settings","bindAddress":"","bitrateMbps":80,"streamAudioToQuest":true,"muteSpeakerWhileConnected":true,"approvedDeviceIds":[],"rejectedDeviceIds":[42]})");
}

TEST_CASE("各メッセージの往復と不正な JSON") {
    HelloMessage hello{1, "0.1.0"};
    SettingsMessage settings{"192.168.1.10", 120, false, false, {"Quest-3_0123"}, {"rejected"}};
    settings.questResolutionPercent = 125;
    settings.foveatedEncoding = false;
    settings.foveationEdgeRatio = 2.5f;
    StatusMessage status;
    status.stats.clientConnected = true;
    status.stats.clientName = "Quest 3 日本語";
    status.stats.clientSessionId = 0xfedcba98;
    status.stats.renderFps = 1.25;
    status.stats.encodeFps = 2.25;
    status.stats.encodeAverageMs = 3.25;
    status.stats.encodeMaxMs = 4.25;
    status.stats.sentMbps = 5.25;
    status.stats.sentFragments = 6;
    status.stats.sendDroppedFrames = 7;
    status.stats.virtualVsyncHz = 8.25;
    status.stats.receivedFps = 9.25;
    status.stats.lostFrames = 10;
    status.stats.fragmentLossPercent = 11.25;
    status.stats.receivedMbps = 12.25;
    status.stats.decodeAverageMs = 13.25;
    status.stats.decodeMaxMs = 14.25;
    status.stats.displayFps = 15.25;
    status.stats.repeatedFrames = 16;
    status.stats.latencyAverageMs = 17.25;
    status.stats.latencyMaxMs = 18.25;
    status.stats.roundTripMs = 19.25;
    status.stats.audioUnderruns = 20;
    status.stats.audioMissingFrames = 21;
    status.stats.audioBufferAverageMs = 22.25;
    status.stats.pcMicState = 3;
    status.stats.questMicState = 2;
    for (const Message& message : {Message{hello}, Message{settings}, Message{status}, Message{PairingRequestMessage{"Quest-3_0123", "Quest 3", "192.168.1.20"}}}) {
        const auto encoded = Encode(message);
        const auto decoded = Decode(encoded);
        CHECK(decoded);
        if (decoded) {
            CHECK(decoded->index() == message.index());
            CHECK(Encode(*decoded) == encoded);
        }
    }
    CHECK(SettingsMessage{}.streamAudioToQuest);
    for (const uint32_t sessionId : {uint32_t{0}, uint32_t{0xfedcba98}, std::numeric_limits<uint32_t>::max()}) {
        StatusMessage sessionStatus;
        sessionStatus.stats.clientSessionId = sessionId;
        const auto decoded = Decode(Encode(sessionStatus));
        CHECK(decoded);
        if (decoded) {
            const auto* result = std::get_if<StatusMessage>(&*decoded);
            CHECK(result);
            if (result) {
                CHECK(result->stats.clientSessionId == sessionId);
            }
        }
    }
    for (const auto value : {"-1", "4294967296", "1.5", "true", "null", "\"1\""}) {
        auto invalid = Encode(StatusMessage{});
        const auto start = invalid.find("\"clientSessionId\":0");
        invalid.replace(start, std::string("\"clientSessionId\":0").size(),
            std::string("\"clientSessionId\":") + value);
        Reject(invalid);
    }
    auto missingSession = Encode(StatusMessage{});
    const auto sessionStart = missingSession.find("\"clientSessionId\":0,");
    missingSession.erase(sessionStart, std::string("\"clientSessionId\":0,").size());
    Reject(missingSession);
    for (const bool stream : {false, true}) {
        for (const bool mute : {false, true}) {
            settings.streamAudioToQuest = stream;
            settings.muteSpeakerWhileConnected = mute;
            const auto decoded = Decode(Encode(settings));
            CHECK(decoded);
            if (decoded) {
                const auto* result = std::get_if<SettingsMessage>(&*decoded);
                CHECK(result);
                if (result) {
                    CHECK(result->streamAudioToQuest == stream);
                    CHECK(result->muteSpeakerWhileConnected == mute);
                }
            }
        }
    }
    Reject("{");
    Reject(R"({"type":"unknown"})");
    Reject(R"({"type":"hello","version":"0.1.0"})");
    Reject(R"({"type":"hello","protocolVersion":1.5,"version":"0.1.0"})");
    Reject(R"({"type":"hello","protocolVersion":18446744073709551615,"version":"0.1.0"})");
    Reject(R"({"type":"settings","bindAddress":"","bitrateMbps":80})");
    Reject(R"({"type":"settings","bindAddress":"","bitrateMbps":80,"streamAudioToQuest":true,"muteSpeakerWhileConnected":1,"approvedDeviceIds":[],"rejectedDeviceIds":[]})");
    Reject(R"({"type":"settings","bindAddress":"","bitrateMbps":80,"muteSpeakerWhileConnected":true,"approvedDeviceIds":[],"rejectedDeviceIds":[]})");
    for (const auto value : {"1", "null", "\"true\""}) {
        auto invalid = Encode(SettingsMessage{});
        const auto start = invalid.find("\"streamAudioToQuest\":true");
        invalid.replace(start, std::string("\"streamAudioToQuest\":true").size(),
            std::string("\"streamAudioToQuest\":") + value);
        Reject(invalid);
    }
    for (const int bitrate : {9, 201}) {
        settings.bitrateMbps = bitrate;
        Reject(Encode(settings));
    }
    settings.bitrateMbps = 80;
    for (const int bitrate : {10, 200}) {
        settings.bitrateMbps = bitrate;
        CHECK(Decode(Encode(settings)));
    }
    settings.bindAddress.assign(65, 'x');
    Reject(Encode(settings));
    status.stats.clientName.assign(257, 'x');
    Reject(Encode(status));
    status.stats.clientName = "Quest";
    status.stats.renderFps = -1;
    Reject(Encode(status));
    status.stats.renderFps = std::numeric_limits<double>::infinity();
    Reject(Encode(status));
    auto invalidCounter = Encode(StatusMessage{});
    const auto counterStart = invalidCounter.find("\"lostFrames\":0");
    invalidCounter.replace(counterStart, std::string("\"lostFrames\":0").size(), "\"lostFrames\":-1");
    Reject(invalidCounter);
    invalidCounter.replace(counterStart, std::string("\"lostFrames\":-1").size(), "\"lostFrames\":1.5");
    Reject(invalidCounter);
    Reject(R"({"type":"status","stats":[]})");
    Reject(R"({"type":"hello","protocolVersion":1,"version":false})");
    auto encoded = Encode(hello);
    encoded.insert(encoded.size() - 1, R"(,"future":42)");
    CHECK(Decode(encoded));
    Reject(std::string(kMaxMessageBytes + 1, ' '));
    auto missing = Encode(StatusMessage{});
    const auto start = missing.find("\"renderFps\":");
    const auto end = missing.find(',', start);
    missing.erase(start, end - start + 1);
    Reject(missing);
}

TEST_CASE("Quest の解像度の割合を丸めて片目の画素数にする") {
    CHECK(QuestEyeSizeForPercent(70) == EyeSize{1445, 1546});
    CHECK(QuestEyeSizeForPercent(100) == EyeSize{2064, 2208});
    CHECK(QuestEyeSizeForPercent(150) == EyeSize{3096, 3312});
    CHECK(QuestEyeSizeForPercent(50) == QuestEyeSizeForPercent(70));
    CHECK(QuestEyeSizeForPercent(200) == QuestEyeSizeForPercent(150));
}

TEST_CASE("描画の設定は範囲外を拒否する") {
    for (const int percent : {69, 151}) {
        SettingsMessage settings;
        settings.questResolutionPercent = percent;
        std::string error;
        CHECK_FALSE(Decode(Encode(settings), &error));
        CHECK(error == "設定が範囲外です");
    }
    for (const float ratio : {1.4f, 5.1f, std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity()}) {
        SettingsMessage settings;
        settings.foveationEdgeRatio = ratio;
        std::string error;
        CHECK_FALSE(Decode(Encode(settings), &error));
        CHECK(error == "設定が範囲外です");
    }
    for (const int percent : {70, 150}) {
        for (const float ratio : {1.5f, 5.0f}) {
            SettingsMessage settings;
            settings.questResolutionPercent = percent;
            settings.foveationEdgeRatio = ratio;
            settings.foveatedEncoding = false;
            const auto decoded = Decode(Encode(settings));
            REQUIRE(decoded);
            const auto& actual = std::get<SettingsMessage>(*decoded);
            CHECK(actual.questResolutionPercent == percent);
            CHECK(actual.foveationEdgeRatio == ratio);
            CHECK_FALSE(actual.foveatedEncoding);
        }
    }
}

}

TEST_CASE("マイクの設定と現在の状態をパイプで往復できる") {
    CHECK(SettingsMessage{}.micToPc);
    for (const bool enabled : {false, true}) {
        SettingsMessage settings;
        settings.micToPc = enabled;
        const auto decoded = Decode(Encode(settings));
        REQUIRE(decoded);
        CHECK(std::get<SettingsMessage>(*decoded).micToPc == enabled);
    }
    for (int pc = 0; pc <= 3; ++pc) {
        for (int quest = 0; quest <= 3; ++quest) {
            StatusMessage status;
            status.stats.pcMicState = pc;
            status.stats.questMicState = quest;
            const auto decoded = Decode(Encode(status));
            REQUIRE(decoded);
            const auto& result = std::get<StatusMessage>(*decoded).stats;
            CHECK(result.pcMicState == pc);
            CHECK(result.questMicState == quest);
        }
    }
    for (const int invalid : {-1, 4}) {
        StatusMessage status;
        status.stats.pcMicState = invalid;
        Reject(Encode(status));
        status.stats.pcMicState = 0;
        status.stats.questMicState = invalid;
        Reject(Encode(status));
    }
}

TEST_CASE("SteamVR の自動終了設定を往復する") {
    CHECK_FALSE(SettingsMessage{}.autoStopSteamVr);
    for (const bool enabled : {false, true}) {
        SettingsMessage settings;
        settings.autoStopSteamVr = enabled;
        const auto decoded = Decode(Encode(settings));
        REQUIRE(decoded);
        REQUIRE(std::holds_alternative<SettingsMessage>(*decoded));
        CHECK(std::get<SettingsMessage>(*decoded).autoStopSteamVr == enabled);
    }
}
