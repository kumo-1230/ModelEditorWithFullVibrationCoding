#pragma once
// UI のアイコン (Windows の Segoe Fluent Icons / Segoe MDL2 Assets を本文フォントに合成して使う)。
// アイコンフォントが無い環境では Icons::loaded が false になり、IconText は文字だけを返す。
#include <imgui.h>
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
