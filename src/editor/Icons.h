#pragma once
// UI のアイコン (Windows の Segoe Fluent Icons / Segoe MDL2 Assets を本文フォントに合成して使う)。
// アイコンフォントが無い環境では Icons::loaded が false になり、IconText は文字だけを返す。
#include <imgui.h>
#include <cmath>
#include <cstring>
#include <string>

namespace Icons {
inline bool loaded = false;
inline ImFont* large = nullptr; // 見出し用の大きいフォント (無ければ nullptr)
} // namespace Icons

#define ICON_SELECT     ""
#define ICON_MOVE       ""
#define ICON_ROTATE     ""
#define ICON_SCALE      ""
#define ICON_WORLD      ""
#define ICON_LOCAL      ""
#define ICON_SNAP       ""
#define ICON_PLAY       ""
#define ICON_PAUSE      ""
#define ICON_STOP       ""
#define ICON_PREV       ""
#define ICON_NEXT       ""
#define ICON_OPEN       ""
#define ICON_SAVE       ""
#define ICON_EXPORT     ""
#define ICON_FOLDER     ""
#define ICON_UNDO       ""
#define ICON_REDO       ""
#define ICON_HIERARCHY  ""
#define ICON_INSPECTOR  ""
#define ICON_MESH       ""
#define ICON_MATERIAL   ""
#define ICON_TEXTURE    ""
#define ICON_SKELETON   ""
#define ICON_ANIMATION  ""
#define ICON_CONSOLE    ""
#define ICON_CHECK      ""
#define ICON_DIAG       ""
#define ICON_STATS      ""
#define ICON_HISTORY    ""
#define ICON_RULER      ""
#define ICON_INFO       ""
#define ICON_VIEWPORT   ""
#define ICON_FRAME      ""
#define ICON_SETTINGS   ""
#define ICON_HELP       ""
#define ICON_WARNING    ""
#define ICON_ERROR      ""
#define ICON_EYE        ""
#define ICON_LIGHT      ""
#define ICON_GRID       ""
#define ICON_LAYOUT     ""
#define ICON_ADD        ""
#define ICON_DELETE     ""
#define ICON_WRENCH     ""
#define ICON_LAYERS     ""
#define ICON_NORMALS    ""
#define ICON_WIRE       ""
#define ICON_IMPORT     ""

// 固定の大きさ (基準の 15px フォントで決めた値) を、今の UI の大きさに合わせる
inline float Px(float v) { return std::round(v * ImGui::GetFontSize() / 15.0f); }

// 長い文字 (ファイルパスなど) を 1 行に収める。入りきらなければ先頭を「…」で省略して末尾 (ファイル名) を残し、
// マウスを乗せると全文を出す (狭いパネルで 1 文字ずつ縦に折り返されないようにする)
inline void EllipsisText(const char* prefix, const std::string& text, bool disabled = false) {
    std::string full = std::string(prefix ? prefix : "") + text;
    float avail = ImGui::GetContentRegionAvail().x;
    std::string shown = full;
    if (ImGui::CalcTextSize(full.c_str()).x > avail && avail > 0.0f) {
        const std::string head = std::string(prefix ? prefix : "") + "…";
        // 末尾から入るだけ残す (UTF-8 の文字の途中で切らない)
        size_t cut = prefix ? strlen(prefix) : 0;
        while (cut < full.size()) {
            do { ++cut; } while (cut < full.size() && (static_cast<unsigned char>(full[cut]) & 0xC0) == 0x80);
            shown = head + full.substr(cut);
            if (ImGui::CalcTextSize(shown.c_str()).x <= avail) break;
        }
    }
    if (disabled) ImGui::TextDisabled("%s", shown.c_str());
    else ImGui::TextUnformatted(shown.c_str());
    if (shown != full && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", full.c_str());
}

// 次のボタン (label) が今の行の残りに入るなら同じ行に、入らなければ次の行に置く (狭いパネルで枠外にはみ出さない)
inline void SameLineIfFits(const char* nextLabel) {
    const ImGuiStyle& st = ImGui::GetStyle();
    float w = ImGui::CalcTextSize(nextLabel, nullptr, true).x + st.FramePadding.x * 2;
    float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= right) ImGui::SameLine();
}

// "アイコン 文字" (アイコンが無ければ文字だけ)
inline std::string IconText(const char* icon, const char* text) {
    if (!Icons::loaded) return text;
    std::string s = icon;
    if (text && *text) { s += "  "; s += text; }
    return s;
}
// アイコンだけ (無ければ代わりの文字)
inline const char* IconOr(const char* icon, const char* fallback) { return Icons::loaded ? icon : fallback; }

// インスペクターの見出し (Unity のコンポーネント見出しのような灰色の帯。青は選択だけに使う)
inline bool SectionHeader(const char* icon, const char* label, ImGuiTreeNodeFlags flags = 0) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.19f, 0.19f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
    bool open = ImGui::CollapsingHeader((IconText(icon, label) + "###" + label).c_str(), flags);
    ImGui::PopStyleColor(3);
    return open;
}
