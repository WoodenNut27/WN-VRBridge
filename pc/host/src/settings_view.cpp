#include "settings_view.h"

#include <wnvr/win32/audio_devices.h>
#include <objbase.h>

#include "driver_pipe_client.h"
#include "host_log.h"
#include "startup_registration.h"
#include "ui_widgets.h"

#include <wnvr/protocol/messages.h>
#include <wnvr/version.h>
#include <wnvr/win32/platform.h>

#include <shellapi.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <utility>

namespace wnvr::host {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kSavedNoticeDuration = std::chrono::seconds(3);
constexpr const char* kAllAdapters = "自動（すべてのアダプターで待ち受ける）";
// 「項目名 | 値」の表の、項目名の列の幅（文字の高さの何倍か）。
constexpr float kStatLabelWidth = 10.5f;
// 統計のグループを 2 列に並べるのに必要な幅（文字の高さの何倍か）。
constexpr float kTwoColumnWidth = 44.0f;
// パケットの損失がこの割合を超えたら、色を変えて知らせる。
constexpr double kLossWarningPercent = 1.0;
constexpr double kLossErrorPercent = 5.0;
constexpr float kInfoLabelWidth = 8.5f;

std::string PathText(const std::filesystem::path& path) {
    return win32::ToUtf8(path.wstring(), win32::InvalidText::Replace);
}

std::string AdapterLabel(const NetworkAdapter& adapter) {
    char speed[48]{};
    const double gbps = static_cast<double>(adapter.speedBps) / 1'000'000'000.0;
    // 1 Gbps に満たない回線は Mbps で見せる。
    if (gbps >= 1.0) {
        std::snprintf(speed, sizeof(speed), "%.1f Gbps", gbps);
    } else {
        std::snprintf(speed, sizeof(speed), "%.0f Mbps", gbps * 1000.0);
    }
    return adapter.name + " — " + adapter.ipv4 + "（" + speed + "）";
}

bool IsDriverAction(SetupTasks::Action action) {
    return action == SetupTasks::Action::AddDriver || action == SetupTasks::Action::RemoveDriver
        || action == SetupTasks::Action::RemoveOtherDrivers;
}

std::string OutcomeText(const SetupTasks::Outcome& outcome) {
    using Action = SetupTasks::Action;
    if (!outcome.success) {
        const char* what = "";
        switch (outcome.action) {
        case Action::AddDriver: what = "ドライバを登録できませんでした"; break;
        case Action::RemoveDriver: what = "ドライバの登録を解除できませんでした"; break;
        case Action::RemoveOtherDrivers: what = "別の場所の登録を解除できませんでした"; break;
        case Action::AddFirewall: what = "許可ルールを登録できませんでした"; break;
        case Action::RemoveFirewall: what = "許可ルールを削除できませんでした"; break;
        }
        return std::string(what) + "（" + outcome.error + "）";
    }
    std::string text;
    switch (outcome.action) {
    case Action::AddDriver: text = "ドライバを登録しました。"; break;
    case Action::RemoveDriver: text = "ドライバの登録を解除しました。"; break;
    case Action::RemoveOtherDrivers: text = "別の場所のドライバの登録を解除しました。"; break;
    case Action::AddFirewall: text = "許可ルールを登録しました。"; break;
    case Action::RemoveFirewall: text = "許可ルールを削除しました。"; break;
    }
    if (outcome.needsSteamVrRestart) {
        text += " SteamVR を起動し直すと反映されます。";
    }
    return text;
}

// 見出しの右に、状態のラベルを置く。
void TitleWithBadge(const char* title, const char* badge, Tone tone) {
    ui::SectionTitle(title);
    ImGui::SameLine();
    ui::Badge(badge, tone);
}

void LinkField(const char* label, const LinkStatus& link) {
    ui::Field(label);
    ui::StatusLine(link.tone, link.text.c_str());
}

}

SettingsView::SettingsView(Context context) : m_context(std::move(context)) {}

void SettingsView::OnShown() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 既に異なる COM モードの UI スレッドなら、その初期化をそのまま使う。
    if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE) {
        m_vbCableFound = win32::FindRenderEndpoint(win32::kVbCableRenderName).has_value();
    } else {
        m_vbCableFound = false;
    }
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    m_adapters = EnumerateAdapters();
    m_startupRegistered = IsStartupRegistered();
    m_startupError.clear();
    m_bitrateEdit = m_context.settings.bitrateMbps;
    m_questResolutionEdit = m_context.settings.questResolutionPercent;
    m_foveationRatioEdit = m_context.settings.foveationEdgeRatio;
    m_context.setup.Refresh();
}

bool SettingsView::NeedsSetup() const {
    const auto firewall = m_context.setup.Firewall().state;
    return m_context.setup.DriverState() == DriverSetup::Missing
        || firewall == FirewallRuleState::Missing || firewall == FirewallRuleState::Outdated;
}

void SettingsView::Commit() {
    if (SaveSettings(m_context.settings)) {
        m_saveError.clear();
        m_savedNoticeUntil = Clock::now() + kSavedNoticeDuration;
    } else {
        // 保存できなくても、動いている SteamVR には反映する（利用者が変えたかったのは今の動き）。
        m_saveError = "設定ファイルに保存できませんでした。今回の起動中だけ反映されます（詳しくはログを確認してください）。";
    }
    if (m_context.onSettingsChanged) {
        m_context.onSettingsChanged();
    }
}

void SettingsView::OpenFolder(const std::filesystem::path& folder) {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(m_owner, L"open",
        folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        LogError("フォルダを開けませんでした: %lld", static_cast<long long>(result));
    }
}

void SettingsView::Draw(HWND owner) {
    m_owner = owner;
    const auto stats = m_context.driverPipe.LatestStats();
    DrawHeader(Summarize(m_status, stats, m_context.setup.DriverState()));
    ui::Gap(0.2f);
    if (ImGui::BeginTabBar("pages")) {
        const auto tab = [this](const char* label, Page page, auto&& draw) {
            const ImGuiTabItemFlags flags = m_requestedPage == page ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(label, nullptr, flags)) {
                return;
            }
            // タブの中身だけをスクロールさせ、上の要約と下の案内は常に見えるようにする。
            const float footer = ImGui::GetFrameHeightWithSpacing();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
            const bool visible = ImGui::BeginChild("page", {0.0f, -footer});
            ImGui::PopStyleColor();
            if (visible) {
                ui::Gap(0.3f);
                draw();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("状態", Page::Status, [&] { DrawStatusPage(stats); });
        tab("設定", Page::Settings, [&] { DrawSettingsPage(); });
        // 表示する文字が変わってもタブの選択が外れないよう、### 以降を ID にする。
        tab(NeedsSetup() ? "セットアップ（未完了）###setup" : "セットアップ###setup", Page::Setup,
            [&] { DrawSetupPage(); });
        tab("情報", Page::About, [&] { DrawAboutPage(); });
        m_requestedPage.reset();
        ImGui::EndTabBar();
    }
    DrawFooter();
}

// ---- 上の要約 ----

void SettingsView::DrawHeader(const StatusSummary& summary) {
    ui::PageTitle("WN-VRBridge");
    const std::string version = std::string("バージョン ") + kVersion;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
        - ImGui::CalcTextSize(version.c_str()).x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", version.c_str());

    // 枠を状態の色にして、ひと目で良い・悪いが分かるようにする。
    auto border = ui::ToneColor(summary.tone);
    border.w = 0.65f;
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    const bool visible = ui::BeginCard("summary");
    ImGui::PopStyleColor();
    if (visible) {
        ui::StatusDot(summary.tone);
        ui::SectionTitle(summary.title.c_str());
        ui::HelpText(summary.detail.c_str());
        if (summary.tone == Tone::Warning && m_context.setup.DriverState() == DriverSetup::Missing) {
            if (ImGui::Button("セットアップを開く")) {
                m_requestedPage = Page::Setup;
            }
        }
    }
    ui::EndCard();
}

// ---- 状態 ----

void SettingsView::DrawStatusPage(const std::optional<ipc::StatsSnapshot>& stats) {
    if (ui::BeginCard("links")) {
        ui::SectionTitle("接続の状態");
        if (ui::BeginFields("linkFields", kInfoLabelWidth)) {
            LinkField("PC の待ち受け", DescribeHost(m_status));
            LinkField("SteamVR", DescribeSteamVr(m_status));
            LinkField("Quest", DescribeQuest(m_status, stats));
            LinkField("マイク", DescribeMic(m_context.settings.micToPc,
                m_status.state == HostState::DriverConnected ? stats : std::nullopt));
            ui::EndFields();
        }
    }
    ui::EndCard();
    if (m_status.state == HostState::DriverConnected && stats && stats->clientConnected) {
        DrawStatistics(*stats);
        return;
    }
    ui::Gap();
    if (ui::BeginCard("noStats")) {
        ui::SectionTitle("通信の状況");
        ui::HelpText("Quest とつながると、ここに遅延・映像・音声の状況が表示されます。");
    }
    ui::EndCard();
}

void SettingsView::DrawStatistics(const ipc::StatsSnapshot& stats) {
    const auto count = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    // 4 つのグループを 2 列に並べ、スクロールせずに見渡せるようにする。幅が足りなければ 1 列にする。
    const int columns = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * kTwoColumnWidth ? 2 : 1;
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{spacing.x * 0.6f, spacing.y * 0.6f});
    const bool visible = ImGui::BeginTable("statCards", columns,
        ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings);
    // 余白の指定は外側の表だけに効かせ、グループの中の表は元の余白に戻す。
    ImGui::PopStyleVar();
    if (!visible) {
        return;
    }
    const auto group = [](const char* id, const char* title, auto&& rows) {
        ImGui::TableNextColumn();
        if (ui::BeginCard(id)) {
            ui::SectionTitle(title);
            ImGui::PushID(id);
            if (ui::BeginFields("fields", kStatLabelWidth)) {
                rows();
                ui::EndFields();
            }
            ImGui::PopID();
        }
        ui::EndCard();
    };
    group("latency", "遅延", [&] {
        ui::Field("頭の動きから表示まで");
        ImGui::Text("%.1f ms（最大 %.1f）", stats.latencyAverageMs, stats.latencyMaxMs);
        ui::Field("ネットワークの往復");
        ImGui::Text("%.1f ms", stats.roundTripMs);
        ui::Field("エンコード（PC）");
        ImGui::Text("%.1f ms（最大 %.1f）", stats.encodeAverageMs, stats.encodeMaxMs);
        ui::Field("デコード（Quest）");
        ImGui::Text("%.1f ms（最大 %.1f）", stats.decodeAverageMs, stats.decodeMaxMs);
    });
    group("frames", "1 秒あたりのコマ数", [&] {
        ui::Field("PC の描画");
        ImGui::Text("%.1f fps", stats.renderFps);
        ui::Field("PC のエンコード");
        ImGui::Text("%.1f fps", stats.encodeFps);
        ui::Field("Quest の受信");
        ImGui::Text("%.1f fps", stats.receivedFps);
        ui::Field("Quest の表示");
        ImGui::Text("%.1f fps", stats.displayFps);
    });
    group("network", "通信", [&] {
        ui::Field("送信のビットレート");
        ImGui::Text("%.1f Mbps", stats.sentMbps);
        ui::Field("受信のビットレート");
        ImGui::Text("%.1f Mbps", stats.receivedMbps);
        ui::Field("パケットの損失");
        // 損失が目立つ値になったら色を変え、ビットレートを下げる目安にする。
        const Tone lossTone = stats.fragmentLossPercent >= kLossErrorPercent ? Tone::Error
            : stats.fragmentLossPercent >= kLossWarningPercent ? Tone::Warning : Tone::Good;
        ImGui::TextColored(ui::ToneColor(lossTone), "%.2f %%", stats.fragmentLossPercent);
        ui::Field("届かなかったフレーム");
        ImGui::Text("%llu 枚", count(stats.lostFrames));
    });
    group("smoothness", "音声と取りこぼし", [&] {
        ui::Field("音の途切れ");
        if (m_context.settings.streamAudioToQuest) {
            ImGui::Text("%llu 回", count(stats.audioUnderruns));
        } else {
            ImGui::TextDisabled("Quest には送っていません");
        }
        ui::Field("Quest にためている音");
        if (m_context.settings.streamAudioToQuest) {
            ImGui::Text("%.1f ms", stats.audioBufferAverageMs);
        } else {
            ImGui::TextDisabled("—");
        }
        ui::Field("同じ画像の表示し直し");
        ImGui::Text("%llu 回", count(stats.repeatedFrames));
        ui::Field("送れなかったフレーム");
        ImGui::Text("%llu 枚", count(stats.sendDroppedFrames));
    });
    ImGui::EndTable();
    ui::HelpText("数値は直近 5 秒の値です。");
}

// ---- 設定 ----

void SettingsView::DrawSettingsPage() {
    auto& settings = m_context.settings;
    ui::HelpText("変更はすぐに保存されます。反映されるタイミングは各項目の説明を確認してください。");
    if (!m_saveError.empty()) {
        ui::ErrorText(m_saveError.c_str());
    }
    ui::Gap(0.2f);

    if (ui::BeginCard("videoSettings")) {
        ui::SectionTitle("映像");
        ImGui::TextUnformatted("ビットレート（1 秒あたりに送る映像のデータの量）");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##bitrate", &m_bitrateEdit, ipc::kMinBitrateMbps, ipc::kMaxBitrateMbps, "%d Mbps",
            ImGuiSliderFlags_AlwaysClamp);
        // つまみを動かしているあいだは反映せず、離したときに 1 回だけ反映する
        // （反映のたびにエンコーダーを設定し直すため）。
        if (ImGui::IsItemDeactivatedAfterEdit() && m_bitrateEdit != settings.bitrateMbps) {
            settings.bitrateMbps = m_bitrateEdit;
            Commit();
        } else if (!ImGui::IsItemActive()) {
            m_bitrateEdit = settings.bitrateMbps;
        }
        ui::HelpText("大きくすると画質が上がりますが、Wi-Fi が混んでいると映像が乱れたり止まったりします。"
            "迷ったら 80 Mbps のままにしてください。");
        if (settings.bitrateMbps != ipc::kDefaultBitrateMbps && ImGui::Button("80 Mbps に戻す")) {
            settings.bitrateMbps = ipc::kDefaultBitrateMbps;
            m_bitrateEdit = settings.bitrateMbps;
            Commit();
        }
        ImGui::TextUnformatted("Quest の描画の解像度（画面に対する割合）");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##questResolution", &m_questResolutionEdit, ipc::kMinQuestResolutionPercent,
            ipc::kMaxQuestResolutionPercent, "%d%%", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            m_questResolutionEdit = std::clamp(static_cast<int>(std::lround(
                static_cast<double>(m_questResolutionEdit) / ipc::kQuestResolutionPercentStep))
                * ipc::kQuestResolutionPercentStep, ipc::kMinQuestResolutionPercent, ipc::kMaxQuestResolutionPercent);
            if (m_questResolutionEdit != settings.questResolutionPercent) {
                settings.questResolutionPercent = m_questResolutionEdit;
                Commit();
            }
        } else if (!ImGui::IsItemActive()) {
            m_questResolutionEdit = settings.questResolutionPercent;
        }
        const auto eyeSize = ipc::QuestEyeSizeForPercent(m_questResolutionEdit);
        const auto resolutionHelp = "片目 " + std::to_string(eyeSize.width) + "×" + std::to_string(eyeSize.height)
            + " で描きます（画面は " + std::to_string(ipc::kQuestPanelEyeWidth) + "×"
            + std::to_string(ipc::kQuestPanelEyeHeight)
            + "）。Quest のアプリを次に起動したときから使われます。大きくすると文字がくっきりしますが、Quest の負荷が増えます。";
        ui::HelpText(resolutionHelp.c_str());
        if (settings.questResolutionPercent != ipc::kDefaultQuestResolutionPercent
            && ImGui::Button("100% に戻す##questResolutionReset")) {
            settings.questResolutionPercent = ipc::kDefaultQuestResolutionPercent;
            m_questResolutionEdit = settings.questResolutionPercent;
            Commit();
        }
        if (ui::Checkbox("中心を細かく、周りを粗く送る（foveation）##foveation", &settings.foveatedEncoding)) {
            Commit();
        }
        if (settings.foveatedEncoding) {
            ImGui::TextUnformatted("周りを縮める比率");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##foveationRatio", &m_foveationRatioEdit, ipc::kMinFoveationEdgeRatio,
                ipc::kMaxFoveationEdgeRatio, "1/%.1f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                m_foveationRatioEdit = std::clamp(std::round(m_foveationRatioEdit / ipc::kFoveationEdgeRatioStep)
                    * ipc::kFoveationEdgeRatioStep, ipc::kMinFoveationEdgeRatio, ipc::kMaxFoveationEdgeRatio);
                if (m_foveationRatioEdit != settings.foveationEdgeRatio) {
                    settings.foveationEdgeRatio = m_foveationRatioEdit;
                    Commit();
                }
            } else if (!ImGui::IsItemActive()) {
                m_foveationRatioEdit = settings.foveationEdgeRatio;
            }
            ui::HelpText("視野の中心だけをそのまま送り、周りを縦横とも 1/N に縮めて送ります。小さくすると周りがくっきりしますが、送るデータが増えます。");
            if (settings.foveationEdgeRatio != ipc::kDefaultFoveationEdgeRatio
                && ImGui::Button("1/4 に戻す##foveationRatioReset")) {
                settings.foveationEdgeRatio = ipc::kDefaultFoveationEdgeRatio;
                m_foveationRatioEdit = settings.foveationEdgeRatio;
                Commit();
            }
        }
        ui::HelpText("foveation の変更は、次に Quest がつながったときから使われます。");
    }
    ui::EndCard();
    ui::Gap();

    if (ui::BeginCard("steamVrSettings")) {
        ui::SectionTitle("SteamVR");
        if (ui::Checkbox("Quest のアプリを起動したら、SteamVR を自動で起動する", &settings.autoStartSteamVr)) {
            Commit();
        }
        ui::HelpText("オフにすると、Quest でトリガーを引いたときに起動します。");
        if (ui::Checkbox("Quest のアプリを終了したら、SteamVR を自動で終了する", &settings.autoStopSteamVr)) {
            Commit();
        }
        ui::HelpText("Quest のアプリを終了してから 15 秒たってもつながらないときに終了します。ヘッドセットを外しただけでは終了しません。");
    }
    ui::EndCard();
    ui::Gap();

    if (ui::BeginCard("audioSettings")) {
        ui::SectionTitle("音声");
        if (ui::Checkbox("Quest のマイクを PC のマイクとして使う", &settings.micToPc)) {
            Commit();
        }
        ui::HelpText("VRChat などのアプリで、マイクに「CABLE Output (VB-Audio Virtual Cable)」を選んでください。");
        if (settings.micToPc && !m_vbCableFound) {
            ui::ErrorText("VB-CABLE（仮想オーディオケーブル）が見つかりません。VB-Audio のサイトからインストールし、PC を再起動してください。");
        }
        bool changed = ui::Checkbox("PC の音を Quest で鳴らす", &settings.streamAudioToQuest);
        changed |= ui::Checkbox("Quest とつながっているあいだ、PC のスピーカーを消音にする",
            &settings.muteSpeakerWhileConnected);
        if (changed) {
            Commit();
        }
        ui::HelpText(DescribeAudioRoute(settings.streamAudioToQuest, settings.muteSpeakerWhileConnected));
        if (settings.muteSpeakerWhileConnected) {
            ui::HelpText("切断すると、スピーカーは元の状態に戻ります。");
        }
    }
    ui::EndCard();
    ui::Gap();

    if (ui::BeginCard("networkSettings")) {
        ui::SectionTitle("ネットワーク");
        DrawAdapterSelector();
    }
    ui::EndCard();
    ui::Gap();

    if (ui::BeginCard("appSettings")) {
        ui::SectionTitle("このアプリ");
        bool startup = m_startupRegistered;
        if (ui::Checkbox("Windows の起動時に、このアプリを自動で起動する", &startup)) {
            if (SetStartupRegistered(startup)) {
                m_startupRegistered = startup;
                m_startupError.clear();
            } else {
                m_startupError = "自動で起動する設定を変えられませんでした（詳しくはログを確認してください）。";
            }
        }
        ui::HelpText("自動で起動したときは、ウィンドウを出さずにタスクトレイに入ります。");
        if (!m_startupError.empty()) {
            ui::ErrorText(m_startupError.c_str());
        }
    }
    ui::EndCard();
    ui::Gap();

    if (ui::BeginCard("devices")) {
        ui::SectionTitle("接続を許可した Quest");
        DrawApprovedDevices();
    }
    ui::EndCard();
}

void SettingsView::DrawAdapterSelector() {
    auto& settings = m_context.settings;
    std::string selectedLabel = kAllAdapters;
    bool missing = false;
    if (!settings.adapterId.empty()) {
        missing = true;
        selectedLabel = "（見つかりません）" + settings.adapterId;
        for (const auto& adapter : m_adapters) {
            if (adapter.id == settings.adapterId) {
                selectedLabel = AdapterLabel(adapter);
                missing = false;
                break;
            }
        }
    }
    ImGui::TextUnformatted("Quest からの接続を待ち受けるネットワーク");
    const float buttonWidth = ImGui::CalcTextSize("一覧を更新").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(buttonWidth + ImGui::GetStyle().ItemSpacing.x));
    bool changed = false;
    if (ImGui::BeginCombo("##adapter", selectedLabel.c_str())) {
        const bool allSelected = settings.adapterId.empty();
        if (ImGui::Selectable(kAllAdapters, allSelected) && !allSelected) {
            settings.adapterId.clear();
            changed = true;
        }
        if (allSelected) {
            ImGui::SetItemDefaultFocus();
        }
        for (const auto& adapter : m_adapters) {
            ImGui::PushID(adapter.id.c_str());
            const bool selected = settings.adapterId == adapter.id;
            // 同じ名前のアダプターがあっても区別できるよう、ID は PushID の値にする。
            const auto label = AdapterLabel(adapter) + "###choice";
            if (ImGui::Selectable(label.c_str(), selected) && !selected) {
                settings.adapterId = adapter.id;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("一覧を更新")) {
        m_adapters = EnumerateAdapters();
    }
    if (changed) {
        Commit();
    }
    if (missing) {
        ui::ErrorText("選んでいたアダプターが見つかりません。つなぎ直すか、別のものを選んでください。");
    }
    ui::HelpText("Quest と同じネットワークにつながっているものを選びます。ふつうは「自動」のままでかまいません。"
        "Quest とつながっているあいだに変えたときは、切断したあとに切り替わります。");
}

void SettingsView::DrawApprovedDevices() {
    auto& devices = m_context.settings.approvedDevices;
    if (devices.empty()) {
        ui::HelpText("まだありません。Quest から初めて接続すると、この PC に確認の画面が出ます。");
        return;
    }
    ui::HelpText("ここにある Quest は、確認なしでつながります。");
    std::optional<size_t> removeIndex;
    if (ImGui::BeginTable("deviceTable", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("名前", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("許可した日時", ImGuiTableColumnFlags_WidthStretch, 3.5f);
        ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t index = 0; index < devices.size(); ++index) {
            const auto& device = devices[index];
            ImGui::PushID(device.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(device.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            // ID は長いので、確認のダイアログと同じ先頭の 8 文字だけを見せる。
            ImGui::Text("%.8s", device.id.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(device.approvedAt.c_str());
            ImGui::TableSetColumnIndex(3);
            if (ImGui::Button("許可を取り消す")) {
                removeIndex = index;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeIndex) {
        const auto& device = devices[*removeIndex];
        LogInfo("Quest の承認を削除しました: ID %.8s %s", device.id.c_str(), device.name.c_str());
        devices.erase(devices.begin() + static_cast<std::ptrdiff_t>(*removeIndex));
        Commit();
    }
}

// ---- セットアップ ----

void SettingsView::DrawSetupPage() {
    ui::HelpText("初めて使うときに、次の 2 つを済ませてください。このアプリのフォルダを動かしたときは、もう一度行います。");
    ui::Gap(0.2f);
    if (ui::BeginCard("driverSetup")) {
        DrawDriverSetup();
    }
    ui::EndCard();
    ui::Gap();
    if (ui::BeginCard("firewallSetup")) {
        DrawFirewallSetup();
    }
    ui::EndCard();
    ui::Gap();
    if (ui::BeginCard("questSetup")) {
        ui::SectionTitle("Quest 側の準備");
        ui::HelpText("Quest には、配布フォルダの quest フォルダにあるアプリ（APK）を入れておきます。"
            "入れ方は、同じフォルダの README.md の「Quest にアプリを入れる」にあります。");
    }
    ui::EndCard();
}

void SettingsView::DrawDriverSetup() {
    auto& setup = m_context.setup;
    const auto& driver = setup.Driver();
    switch (setup.DriverState()) {
    case DriverSetup::Registered:
        TitleWithBadge("1. SteamVR にドライバを登録する", "登録済み", Tone::Good);
        break;
    case DriverSetup::Missing:
        TitleWithBadge("1. SteamVR にドライバを登録する", "未登録", Tone::Warning);
        break;
    case DriverSetup::Unknown:
        TitleWithBadge("1. SteamVR にドライバを登録する", "確認できません", Tone::Error);
        break;
    }
    ui::HelpText("SteamVR が、Quest を VR ヘッドセットとして使えるようにします。");
    if (!driver.error.empty()) {
        ui::ErrorText(driver.error.c_str());
    }
    if (!driver.driverPath.empty() && ui::BeginFields("driverFields", kInfoLabelWidth)) {
        ui::Field("ドライバの場所");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(PathText(driver.driverPath).c_str());
        ImGui::PopTextWrapPos();
        ui::EndFields();
    }
    const bool steamVrRunning = m_context.driverPipe.IsConnected();
    ImGui::BeginDisabled(setup.IsBusy());
    if (driver.CanChange()) {
        if (!driver.registered) {
            if (ImGui::Button("ドライバを登録する")) {
                setup.Start(SetupTasks::Action::AddDriver, m_owner, steamVrRunning);
            }
        } else if (ImGui::Button("登録を解除する")) {
            setup.Start(SetupTasks::Action::RemoveDriver, m_owner, steamVrRunning);
        }
    }
    if (!driver.otherPaths.empty()) {
        ui::Gap(0.2f);
        ui::StatusLine(Tone::Warning, "別の場所にある WN-VRBridge のドライバも登録されています。"
            "古いフォルダの登録が残っていると、SteamVR がそちらを読み込むことがあります。");
        for (const auto& path : driver.otherPaths) {
            ImGui::Indent();
            ui::HelpText(PathText(path).c_str());
            ImGui::Unindent();
        }
        if (driver.CanChange() && ImGui::Button("別の場所の登録を解除する")) {
            setup.Start(SetupTasks::Action::RemoveOtherDrivers, m_owner, steamVrRunning);
        }
    }
    ImGui::EndDisabled();
    DrawSetupOutcome(true);
}

void SettingsView::DrawFirewallSetup() {
    auto& setup = m_context.setup;
    const auto& firewall = setup.Firewall();
    switch (firewall.state) {
    case FirewallRuleState::Registered:
        TitleWithBadge("2. Windows ファイアウォールで通信を許可する", "許可済み", Tone::Good);
        break;
    case FirewallRuleState::Missing:
        TitleWithBadge("2. Windows ファイアウォールで通信を許可する", "未登録", Tone::Warning);
        break;
    case FirewallRuleState::Outdated:
        TitleWithBadge("2. Windows ファイアウォールで通信を許可する", "登録し直しが必要", Tone::Warning);
        break;
    case FirewallRuleState::Unknown:
        TitleWithBadge("2. Windows ファイアウォールで通信を許可する", "確認できません", Tone::Error);
        break;
    }
    const std::string ports = "UDP " + std::to_string(protocol::kDiscoveryPort) + "・"
        + std::to_string(protocol::kDefaultPort);
    ui::HelpText(("Quest からの通信（" + ports + "）を、このアプリと SteamVR が受け取れるようにします。"
        "許可するのは「プライベート」と「ドメイン」のネットワークだけです。").c_str());
    if (firewall.state == FirewallRuleState::Unknown && !firewall.error.empty()) {
        ui::ErrorText(firewall.error.c_str());
    }
    if (firewall.state == FirewallRuleState::Outdated) {
        ui::HelpText("このアプリか SteamVR の場所が、登録したときと変わっています。もう一度登録してください。");
    }
    ImGui::BeginDisabled(setup.IsBusy());
    if (firewall.state != FirewallRuleState::Registered) {
        ImGui::BeginDisabled(!setup.CanAddFirewall());
        if (ImGui::Button("許可ルールを登録する")) {
            setup.Start(SetupTasks::Action::AddFirewall, m_owner, false);
        }
        ImGui::EndDisabled();
    }
    if (firewall.state != FirewallRuleState::Missing) {
        if (firewall.state != FirewallRuleState::Registered) {
            ImGui::SameLine();
        }
        if (ImGui::Button("許可ルールを削除する")) {
            setup.Start(SetupTasks::Action::RemoveFirewall, m_owner, false);
        }
    }
    ImGui::EndDisabled();
    ui::HelpText("登録と削除には管理者の権限が必要です。ボタンを押すと、Windows の確認の画面が出ます。");
    if (firewall.state != FirewallRuleState::Registered && !setup.CanAddFirewall()) {
        ui::ErrorText("SteamVR の vrserver.exe が見つからないため、登録できません。SteamVR が入っているか確かめてください。");
    }
    DrawSetupOutcome(false);
}

void SettingsView::DrawSetupOutcome(bool driverActions) {
    const auto& setup = m_context.setup;
    const auto& outcome = setup.LastOutcome();
    if (setup.IsBusy()) {
        // どちらの操作か分からないあいだは、両方のグループに出しておく。
        ui::StatusLine(Tone::Busy, "処理しています…");
    } else if (outcome && IsDriverAction(outcome->action) == driverActions) {
        ui::StatusLine(outcome->success ? Tone::Good : Tone::Error, OutcomeText(*outcome).c_str());
    }
}

// ---- 情報 ----

void SettingsView::DrawAboutPage() {
    if (ui::BeginCard("files")) {
        ui::SectionTitle("ファイルの場所");
        const auto logDirectory = GetLogDirectory();
        const auto settingsDirectory = SettingsPath().parent_path();
        if (ui::BeginFields("fileFields", kInfoLabelWidth)) {
            ui::Field("このアプリのログ");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(PathText(logDirectory).c_str());
            ImGui::PopTextWrapPos();
            ui::Field("設定ファイル");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(PathText(SettingsPath()).c_str());
            ImGui::PopTextWrapPos();
            ui::Field("ドライバのログ");
            ui::HelpText("SteamVR のログ（Steam の logs フォルダの vrserver.txt）の、[WN-VRBridge] で始まる行");
            ui::EndFields();
        }
        if (ImGui::Button("ログのフォルダを開く")) {
            OpenFolder(logDirectory);
        }
        ImGui::SameLine();
        if (ImGui::Button("設定のフォルダを開く")) {
            OpenFolder(settingsDirectory);
        }
    }
    ui::EndCard();
    ui::Gap();

    if (ui::BeginCard("help")) {
        ui::SectionTitle("うまくいかないとき");
        ui::BulletHelp("Quest が「PC を探しています」のまま: PC と Quest が同じネットワークにいるか、"
            "「セットアップ」の 2 つが済んでいるかを確かめてください。");
        ui::BulletHelp("映像が乱れる・止まる: 「設定」でビットレートを下げてください。"
            "PC は有線でつなぎ、Quest は Wi-Fi のアクセスポイントの近くで使ってください。");
        ui::BulletHelp("SteamVR を終了したあと、もう一度使う: Quest でトリガーを引くか、Quest のアプリを起動し直します。");
    }
    ui::EndCard();
    ui::Gap();

    if (ui::BeginCard("exit")) {
        ui::SectionTitle("終了");
        ui::HelpText("ウィンドウを閉じても、このアプリはタスクトレイで動き続け、Quest からの接続を待ちます。"
            "完全に終了すると、Quest からつないでも SteamVR は自動で起動しません。");
        if (ImGui::Button("WN-VRBridge を終了する") && m_context.onExit) {
            m_context.onExit();
        }
    }
    ui::EndCard();
}

// ---- 下の案内 ----

void SettingsView::DrawFooter() {
    // どの表示のときも同じ高さに置く（丸と文字の位置がずれないよう、行ごと下げる）。
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y);
    if (!m_saveError.empty()) {
        ui::StatusLine(Tone::Error, "設定を保存できませんでした");
    } else if (Clock::now() < m_savedNoticeUntil) {
        ui::StatusLine(Tone::Good, "設定を保存しました");
    } else {
        ImGui::TextDisabled("ウィンドウを閉じても、タスクトレイで動き続けます。");
    }
}

}
