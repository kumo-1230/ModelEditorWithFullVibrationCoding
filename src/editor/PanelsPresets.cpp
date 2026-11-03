// 表示プリセット: 表示の設定 (表示モード・ワイヤー・ボーン・グリッド・ライト・開くパネル・確認用の線) を名前を付けて保存し、切り替える。
// 用途別の組み込みプリセットと、自分で保存したプリセット (%APPDATA%\ModelEditor\ModelEditor_presets.ini) がある。
#include "Editor.h"
#include "core/Log.h"
#include "util/FileUtil.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <sstream>

namespace {
struct Field {
    const char* key;
    char type; // b=bool i=int f=float v=vec3
    void* ptr;
};
} // namespace

// 保存・読み込みする項目 (キー名は設定ファイルに書く名前)
static std::vector<Field> PresetFields(Editor::ViewPreset& p) {
    RenderSettings& r = p.rs;
    auto& pa = p.panels;
    return {
        { "mode", 'i', &r.mode }, { "wireframe", 'b', &r.wireframe }, { "wire_on_shaded", 'b', &r.wireOnShaded },
        { "highlight", 'b', &r.highlightSelection }, { "grid", 'b', &r.showGrid }, { "grid_spacing", 'f', &r.gridSpacing },
        { "grid_major", 'i', &r.gridMajorEvery }, { "grid_extent", 'f', &r.gridExtent }, { "origin_axes", 'b', &r.showOriginAxes },
        { "bones", 'b', &r.showBones }, { "bones_on_top", 'b', &r.bonesOnTop }, { "normals", 'b', &r.showNormals },
        { "tangents", 'b', &r.showTangents }, { "debug_selected", 'b', &r.debugSelectedOnly }, { "bounds", 'b', &r.showBounds },
        { "node_axes", 'b', &r.showNodeAxes }, { "backface", 'b', &r.backfaceCulling }, { "textures", 'b', &r.useTextures },
        { "normal_maps", 'b', &r.useNormalMaps }, { "vertex_colors", 'b', &r.useVertexColors }, { "skinning", 'b', &r.skinningEnabled },
        { "msaa", 'i', &r.msaa }, { "normal_length", 'f', &r.normalLength }, { "bone_size", 'f', &r.boneSize },
        { "bg", 'v', &r.bgColor }, { "light_dir", 'v', &r.lightDir }, { "light", 'f', &r.lightIntensity }, { "ambient", 'f', &r.ambient },
        { "exposure", 'f', &r.exposure },
        { "bone_names", 'b', &p.boneNames }, { "size_overlay", 'b', &p.sizeOverlay }, { "open_edges", 'b', &p.openEdges },
        { "non_manifold", 'b', &p.nonManifold }, { "root_path", 'b', &p.rootPath }, { "pivots", 'b', &p.pivots }, { "gizmo_help", 'b', &p.gizmoHelp },
        { "p_hierarchy", 'b', &pa.hierarchy }, { "p_inspector", 'b', &pa.inspector }, { "p_meshes", 'b', &pa.meshes },
        { "p_materials", 'b', &pa.materials }, { "p_textures", 'b', &pa.textures }, { "p_skeleton", 'b', &pa.skeleton },
        { "p_animation", 'b', &pa.animation }, { "p_console", 'b', &pa.console }, { "p_validation", 'b', &pa.validation },
        { "p_stats", 'b', &pa.stats }, { "p_sceneinfo", 'b', &pa.sceneInfo }, { "p_history", 'b', &pa.history }, { "p_uv", 'b', &pa.uvViewer },
        { "p_measure", 'b', &pa.measure }, { "p_diagnostics", 'b', &pa.diagnostics }, { "p_models", 'b', &pa.models },
        { "p_animeditor", 'b', &pa.animEditor },
    };
}

Editor::ViewPreset Editor::CaptureViewPreset(const std::string& name) const {
    ViewPreset p;
    p.name = name;
    p.rs = m_rs;
    p.panels = m_panels;
    p.boneNames = m_showBoneNames;
    p.sizeOverlay = m_showSizeOverlay;
    p.openEdges = m_showOpenEdges;
    p.nonManifold = m_showNonManifold;
    p.rootPath = m_showRootPath;
    p.pivots = m_showPivots;
    p.gizmoHelp = m_showGizmoHelp;
    return p;
}

void Editor::ApplyViewPreset(const ViewPreset& p) {
    m_rs = p.rs;
    // 表示するパネルだけを切り替える (設定ウィンドウなどの一時的なウィンドウは今のまま)
    Panels keep = m_panels;
    m_panels = p.panels;
    m_panels.viewport = true;
    m_panels.exportWin = keep.exportWin;
    m_panels.importWin = keep.importWin;
    m_panels.batchRename = keep.batchRename;
    m_panels.settings = keep.settings;
    m_panels.help = keep.help;
    m_panels.imguiDemo = keep.imguiDemo;
    m_panels.imguiMetrics = keep.imguiMetrics;
    m_panels.dump = keep.dump;
    m_showBoneNames = p.boneNames;
    m_showSizeOverlay = p.sizeOverlay;
    m_showOpenEdges = p.openEdges;
    m_showNonManifold = p.nonManifold;
    m_showRootPath = p.rootPath;
    m_showPivots = p.pivots;
    m_showGizmoHelp = p.gizmoHelp;
    for (auto& f : p.focus) m_focusWindows.push_back("###" + f);
    m_activePreset = p.name;
    LOG_INFO("表示プリセット '%s' にしました", p.name.c_str());
}

// 用途別の組み込みプリセット
const std::vector<Editor::ViewPreset>& Editor::BuiltinPresets() {
    static std::vector<ViewPreset> list;
    if (!list.empty()) return list;
    ViewPreset base;
    base.builtin = true;
    {
        ViewPreset p = base;
        p.name = "標準 (既定に戻す)";
        p.description = "最初の状態の表示です";
        list.push_back(p);
    }
    {
        ViewPreset p = base;
        p.name = "形の確認 (モデリング)";
        p.description = "面 + ワイヤー、テクスチャなし。穴 (開いた辺) を黄色で表示します";
        p.rs.wireOnShaded = true;
        p.rs.useTextures = false;
        p.rs.showBones = false;
        p.openEdges = true;
        p.panels.diagnostics = true;
        p.focus = { "Meshes", "Diagnostics" };
        list.push_back(p);
    }
    {
        ViewPreset p = base;
        p.name = "テクスチャ / UV の確認";
        p.description = "UV チェッカーで伸びや向きを確認し、UV ビューアを開きます";
        p.rs.mode = RM_Checker;
        p.rs.showBones = false;
        p.panels.uvViewer = true;
        p.panels.materials = true;
        p.panels.textures = true;
        p.focus = { "Textures", "Materials" };
        list.push_back(p);
    }
    {
        ViewPreset p = base;
        p.name = "アニメーションの確認";
        p.description = "ボーンとルートモーションの軌跡を表示し、アニメーション編集を開きます";
        p.rs.showBones = true;
        p.rs.bonesOnTop = true;
        p.rootPath = true;
        p.panels.animation = true;
        p.panels.animEditor = true;
        p.focus = { "AnimEditor" };
        list.push_back(p);
    }
    {
        ViewPreset p = base;
        p.name = "リグ / ウェイトの確認";
        p.description = "1 頂点に影響するボーンの数を色で表示し、ボーン名も出します";
        p.rs.mode = RM_Influences;
        p.rs.showBones = true;
        p.rs.bonesOnTop = true;
        p.boneNames = true;
        p.panels.skeleton = true;
        p.focus = { "Skeleton" };
        list.push_back(p);
    }
    {
        ViewPreset p = base;
        p.name = "書き出し前のチェック";
        p.description = "裏返った面を赤で表示し、検証・モデル診断・単位を開きます";
        p.rs.mode = RM_Backface;
        p.openEdges = true;
        p.nonManifold = true;
        p.pivots = true;
        p.panels.validation = true;
        p.panels.diagnostics = true;
        p.panels.measure = true;
        p.focus = { "Validation", "Measure" };
        list.push_back(p);
    }
    {
        ViewPreset p = base;
        p.name = "見せる (スクリーンショット)";
        p.description = "グリッド・ボーン・説明の文字を消して、モデルだけをきれいに表示します";
        p.rs.showGrid = false;
        p.rs.showBones = false;
        p.rs.showOriginAxes = false;
        p.rs.highlightSelection = false;
        p.rs.msaa = 8;
        p.sizeOverlay = false;
        p.gizmoHelp = false;
        list.push_back(p);
    }
    return list;
}

static std::string PresetsPath() { return FileUtil::Join(FileUtil::AppDataDir(), "ModelEditor_presets.ini"); }

void Editor::LoadUserPresets() {
    m_userPresets.clear();
    std::vector<uint8_t> data;
    if (!FileUtil::ReadBinary(PresetsPath(), data)) return;
    std::istringstream in(std::string(data.begin(), data.end()));
    std::string line;
    ViewPreset* cur = nullptr;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[preset]") {
            m_userPresets.push_back(ViewPreset());
            cur = &m_userPresets.back();
            continue;
        }
        size_t eq = line.find('=');
        if (!cur || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "name") { cur->name = v; continue; }
        for (auto& f : PresetFields(*cur)) {
            if (k != f.key) continue;
            if (f.type == 'b') *(bool*)f.ptr = v == "1";
            else if (f.type == 'i') *(int*)f.ptr = atoi(v.c_str());
            else if (f.type == 'f') *(float*)f.ptr = (float)atof(v.c_str());
            else if (f.type == 'v') {
                glm::vec3& x = *(glm::vec3*)f.ptr;
                sscanf(v.c_str(), "%f %f %f", &x.x, &x.y, &x.z);
            }
        }
    }
    m_userPresets.erase(std::remove_if(m_userPresets.begin(), m_userPresets.end(), [](const ViewPreset& p) { return p.name.empty(); }), m_userPresets.end());
}

void Editor::SaveUserPresets() {
    std::ostringstream o;
    for (auto& cp : m_userPresets) {
        ViewPreset p = cp;
        o << "[preset]\nname=" << p.name << "\n";
        for (auto& f : PresetFields(p)) {
            o << f.key << "=";
            if (f.type == 'b') o << (*(bool*)f.ptr ? 1 : 0);
            else if (f.type == 'i') o << *(int*)f.ptr;
            else if (f.type == 'f') o << *(float*)f.ptr;
            else if (f.type == 'v') { auto& x = *(glm::vec3*)f.ptr; o << x.x << " " << x.y << " " << x.z; }
            o << "\n";
        }
    }
    FileUtil::WriteText(PresetsPath(), o.str());
}

// ツールバーの「表示プリセット」ボタンから開くメニュー
void Editor::DrawPresetMenu() {
    ImGui::TextDisabled("用途に合わせて表示をまとめて切り替えます");
    ImGui::SeparatorText("組み込み");
    for (auto& p : BuiltinPresets()) {
        if (ImGui::Selectable(p.name.c_str(), m_activePreset == p.name)) ApplyViewPreset(p);
        if (ImGui::IsItemHovered() && !p.description.empty()) ImGui::SetTooltip("%s", p.description.c_str());
    }
    ImGui::SeparatorText("自分で保存したもの");
    if (m_userPresets.empty()) ImGui::TextDisabled("まだありません");
    int remove = -1;
    for (int i = 0; i < (int)m_userPresets.size(); ++i) {
        ImGui::PushID(i);
        if (ImGui::SmallButton(IconOr(ICON_DELETE, "x"))) remove = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("このプリセットを削除");
        ImGui::SameLine();
        if (ImGui::Selectable(m_userPresets[i].name.c_str(), m_activePreset == m_userPresets[i].name, ImGuiSelectableFlags_NoAutoClosePopups))
            ApplyViewPreset(m_userPresets[i]);
        ImGui::PopID();
    }
    if (remove >= 0) {
        m_userPresets.erase(m_userPresets.begin() + remove);
        SaveUserPresets();
    }
    ImGui::SeparatorText("今の表示を保存");
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##presetName", "名前 (例: 自分用の確認)", &m_presetName);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_presetName.empty());
    if (ImGui::Button("保存")) {
        ViewPreset p = CaptureViewPreset(m_presetName);
        bool replaced = false;
        for (auto& e : m_userPresets) if (e.name == p.name) { e = p; replaced = true; }
        if (!replaced) m_userPresets.push_back(p);
        SaveUserPresets();
        m_activePreset = p.name;
        LOG_INFO("表示プリセット '%s' を保存しました", p.name.c_str());
        m_presetName.clear();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("表示モード・ワイヤー・ボーン・グリッド・ライト・開いているパネル・確認用の線を保存します");
}
