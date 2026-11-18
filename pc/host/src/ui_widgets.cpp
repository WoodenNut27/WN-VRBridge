#include "ui_widgets.h"

#include "app_icon.h"

namespace wnvr::host::ui {
namespace {

ImFont* g_headingFont = nullptr;

constexpr float kBaseFontSize = 17.0f;

ImVec4 Rgb(uint32_t rgb, float alpha = 1.0f) {
    return {static_cast<float>((rgb >> 16) & 0xff) / 255.0f, static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
        static_cast<float>(rgb & 0xff) / 255.0f, alpha};
}

// 配色。暗い背景に、状態の色（緑・青・黄・赤）だけが目立つようにする。
constexpr uint32_t kBackground = 0x16181d;
constexpr uint32_t kCard = 0x1f2228;
constexpr uint32_t kBorder = 0x30353d;
constexpr uint32_t kControl = 0x2a2e36;
constexpr uint32_t kControlHovered = 0x343a44;
constexpr uint32_t kControlActive = 0x3e4551;
constexpr uint32_t kText = 0xe6e9ed;
constexpr uint32_t kTextDim = 0x9aa3ad;
constexpr uint32_t kAccent = 0x4c8dff;

void WrappedColored(const ImVec4& color, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

}

void SetHeadingFont(ImFont* font) {
    g_headingFont = font;
}

void ApplyTheme(float dpiScale) {
    // 基準スタイルから適用し、モニター間の移動で倍率が累積しないようにする。
    ImGui::GetStyle() = ImGuiStyle();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {22.0f, 18.0f};
    style.FramePadding = {12.0f, 7.0f};
    style.ItemSpacing = {10.0f, 9.0f};
    style.ItemInnerSpacing = {8.0f, 6.0f};
    style.CellPadding = {8.0f, 5.0f};
    style.IndentSpacing = 22.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 14.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 7.0f;
    style.TabRounding = 7.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DisabledAlpha = 0.45f;

    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = Rgb(kText);
    colors[ImGuiCol_TextDisabled] = Rgb(kTextDim);
    colors[ImGuiCol_WindowBg] = Rgb(kBackground);
    colors[ImGuiCol_ChildBg] = Rgb(kCard);
    colors[ImGuiCol_PopupBg] = Rgb(kCard);
    colors[ImGuiCol_Border] = Rgb(kBorder);
    colors[ImGuiCol_Separator] = Rgb(kBorder);
    colors[ImGuiCol_FrameBg] = Rgb(kControl);
    colors[ImGuiCol_FrameBgHovered] = Rgb(kControlHovered);
    colors[ImGuiCol_FrameBgActive] = Rgb(kControlActive);
    colors[ImGuiCol_Button] = Rgb(kControl);
    colors[ImGuiCol_ButtonHovered] = Rgb(kControlHovered);
    colors[ImGuiCol_ButtonActive] = Rgb(kControlActive);
    colors[ImGuiCol_Header] = Rgb(kControl);
    colors[ImGuiCol_HeaderHovered] = Rgb(kControlHovered);
    colors[ImGuiCol_HeaderActive] = Rgb(kControlActive);
    colors[ImGuiCol_CheckMark] = Rgb(kAccent);
    colors[ImGuiCol_SliderGrab] = Rgb(kAccent);
    colors[ImGuiCol_SliderGrabActive] = Rgb(0x7aaaff);
    colors[ImGuiCol_Tab] = Rgb(kBackground);
    colors[ImGuiCol_TabHovered] = Rgb(kControlHovered);
    colors[ImGuiCol_TabSelected] = Rgb(kControl);
    colors[ImGuiCol_TabSelectedOverline] = Rgb(kAccent);
    colors[ImGuiCol_TabDimmed] = Rgb(kBackground);
    colors[ImGuiCol_TabDimmedSelected] = Rgb(kControl);
    colors[ImGuiCol_TableHeaderBg] = Rgb(kControl);
    colors[ImGuiCol_TableBorderLight] = Rgb(kBorder, 0.6f);
    colors[ImGuiCol_TableBorderStrong] = Rgb(kBorder);
    colors[ImGuiCol_TableRowBg] = Rgb(0x000000, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = Rgb(0xffffff, 0.025f);
    colors[ImGuiCol_ScrollbarBg] = Rgb(0x000000, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = Rgb(kControlHovered);
    colors[ImGuiCol_ScrollbarGrabHovered] = Rgb(kControlActive);
    colors[ImGuiCol_ScrollbarGrabActive] = Rgb(kControlActive);
    colors[ImGuiCol_NavCursor] = Rgb(kAccent);

    style.ScaleAllSizes(dpiScale);
    style.FontSizeBase = kBaseFontSize;
    style.FontScaleDpi = dpiScale;
}

ImVec4 ToneColor(Tone tone) {
    return Rgb(ToneRgb(tone));
}

void PageTitle(const char* text) {
    ImGui::PushFont(g_headingFont, kBaseFontSize * 1.45f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void SectionTitle(const char* text) {
    ImGui::PushFont(g_headingFont, kBaseFontSize * 1.08f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void HelpText(const char* text) {
    WrappedColored(Rgb(kTextDim), text);
}

void BulletHelp(const char* text) {
    // 行頭の点は本文より目立たせず、字下げも控えめにする。
    const auto& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Text, Rgb(kTextDim));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{style.FramePadding.x * 0.3f, style.FramePadding.y});
    ImGui::Bullet();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    HelpText(text);
}

bool Checkbox(const char* label, bool* value) {
    const auto& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{style.FramePadding.y * 0.55f, style.FramePadding.y * 0.55f});
    const bool changed = ImGui::Checkbox(label, value);
    ImGui::PopStyleVar();
    return changed;
}

void ErrorText(const char* text) {
    WrappedColored(ToneColor(Tone::Error), text);
}

void StatusDot(Tone tone) {
    // 文字の高さの中央に丸を置き、続く文字とのあいだを少し空ける。
    const float size = ImGui::GetFontSize();
    const float radius = size * 0.28f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled({origin.x + radius, origin.y + size * 0.52f}, radius,
        ImGui::GetColorU32(ToneColor(tone)), 24);
    ImGui::Dummy({radius * 2.0f, size});
    ImGui::SameLine(0.0f, size * 0.45f);
}

void StatusLine(Tone tone, const char* text) {
    StatusDot(tone);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
}

void Badge(const char* text, Tone tone) {
    const float size = ImGui::GetFontSize();
    const ImVec2 padding{size * 0.55f, size * 0.12f};
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 corner{origin.x + textSize.x + padding.x * 2.0f, origin.y + textSize.y + padding.y * 2.0f};
    auto color = ToneColor(tone);
    auto* drawList = ImGui::GetWindowDrawList();
    // 薄く塗った上に同じ色の文字を載せ、文字の背景が暗くても明るくても読めるようにする。
    drawList->AddRectFilled(origin, corner, ImGui::GetColorU32({color.x, color.y, color.z, 0.18f}), size * 0.5f);
    drawList->AddText({origin.x + padding.x, origin.y + padding.y}, ImGui::GetColorU32(color), text);
    ImGui::Dummy({corner.x - origin.x, corner.y - origin.y});
}

bool BeginCard(const char* id) {
    return ImGui::BeginChild(id, {0.0f, 0.0f},
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}

void EndCard() {
    ImGui::EndChild();
}

bool BeginFields(const char* id, float labelWidthEm) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * labelWidthEm);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void Field(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgb(kTextDim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::TableSetColumnIndex(1);
}

void EndFields() {
    ImGui::EndTable();
}

void Gap(float em) {
    ImGui::Dummy({0.0f, ImGui::GetFontSize() * em});
}

}
