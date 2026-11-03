// 複数モデル: 選んでいるモデルの判定、モデル一覧、結合 (アニメーションのコピー / ボーンに付ける / まとめる)、抜き取り。
// Maya と同じく、ファイルを読み込むたびにファイル名のグループ (= 1 モデル) がシーンに追加される。
#include "Editor.h"
#include "core/Log.h"
#include "core/ModelOps.h"
#include "core/SceneOps.h"
#include "util/FileUtil.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <functional>

int Editor::ModelOf(int node) const { return Ops::ModelRootOf(m_scene, node); }

int Editor::ActiveModel() const {
    if (m_primary >= 0 && m_primary < (int)m_scene.nodes.size()) return ModelOf(m_primary);
    if (m_scene.roots.size() == 1) return m_scene.roots[0];
    return -1;
}

const SceneInfo& Editor::ModelInfo(int model) const {
    if (model >= 0 && model < (int)m_scene.nodes.size() && m_scene.nodes[model].modelInfo) return *m_scene.nodes[model].modelInfo;
    return m_scene.info;
}

SceneInfo& Editor::ModelInfoMut(int model) {
    if (model >= 0 && model < (int)m_scene.nodes.size() && m_scene.nodes[model].modelInfo) {
        // 元に戻す用のスナップショットと共有しているので、書き換える前に複製する
        auto& p = m_scene.nodes[model].modelInfo;
        p = std::make_shared<SceneInfo>(*p);
        return *p;
    }
    return m_scene.info;
}

std::string Editor::FitTargetLabel(bool allSelected) {
    auto t = FitTargetsForSelection(allSelected);
    if (t.empty()) return "なし";
    std::string s = m_scene.nodes[t[0]].name;
    if (t.size() > 1) s += " ほか " + std::to_string(t.size() - 1) + " 個";
    if (!allSelected && m_selection.size() > 1) s += " (複数選択中は、最後に選んだものだけ)";
    return s;
}

// 選んでいるモデルに属するノード・メッシュ・マテリアル・テクスチャ・アニメーション (パネルの一覧の絞り込みに使う)
const Editor::ModelMembership& Editor::Membership() {
    int model = (m_filterToModel && m_scene.roots.size() > 1) ? ActiveModel() : -1;
    if (m_member.rev == m_sceneRevision && m_member.model == model && m_member.nodes.size() == m_scene.nodes.size()) return m_member;
    const bool modelChanged = m_member.model != model;
    ModelMembership& M = m_member;
    M.rev = m_sceneRevision;
    M.model = model;
    const char all = model < 0 ? 1 : 0;
    M.nodes.assign(m_scene.nodes.size(), all);
    M.meshes.assign(m_scene.meshes.size(), all);
    M.materials.assign(m_scene.materials.size(), all);
    M.textures.assign(m_scene.textures.size(), all);
    M.animations.assign(m_scene.animations.size(), all);
    if (model >= 0) {
        auto sub = Ops::SubtreeMask(m_scene, model);
        for (size_t i = 0; i < sub.size(); ++i) {
            if (!sub[i]) continue;
            M.nodes[i] = 1;
            int me = m_scene.nodes[i].mesh;
            if (me >= 0 && me < (int)M.meshes.size()) M.meshes[me] = 1;
        }
        for (size_t m = 0; m < m_scene.meshes.size(); ++m) {
            if (!M.meshes[m]) continue;
            for (auto& p : m_scene.meshes[m].primitives)
                if (p.material >= 0 && p.material < (int)M.materials.size()) M.materials[p.material] = 1;
        }
        for (size_t m = 0; m < m_scene.materials.size(); ++m) {
            if (!M.materials[m]) continue;
            for (auto& s : m_scene.materials[m].slots)
                if (s.texture >= 0 && s.texture < (int)M.textures.size()) M.textures[s.texture] = 1;
        }
        for (size_t a = 0; a < m_scene.animations.size(); ++a)
            for (auto& c : m_scene.animations[a].channels)
                if (c.node >= 0 && c.node < (int)sub.size() && sub[c.node]) { M.animations[a] = 1; break; }
    }
    // 別のモデルを選んだら、そのモデルのアニメーションに切り替える
    if (modelChanged && model >= 0 && m_anim >= 0 && m_anim < (int)M.animations.size() && !M.animations[m_anim]) {
        m_anim = -1;
        for (size_t a = 0; a < M.animations.size(); ++a) if (M.animations[a]) { m_anim = (int)a; break; }
        m_animTime = 0;
        m_playing = false;
    }
    return M;
}

bool Editor::NodeInActiveModel(int node) {
    const auto& M = Membership();
    return node < 0 || node >= (int)M.nodes.size() || M.nodes[node];
}

bool Editor::IssueInActiveModel(const Issue& is) {
    const auto& M = Membership();
    if (M.model < 0) return true;
    if (is.node >= 0) return InMask(M.nodes, is.node);
    if (is.mesh >= 0) return InMask(M.meshes, is.mesh);
    if (is.material >= 0) return InMask(M.materials, is.material);
    if (is.texture >= 0) return InMask(M.textures, is.texture);
    if (is.animation >= 0) return InMask(M.animations, is.animation);
    return true; // シーン全体の問題
}

// 検証の行をダブルクリックしたとき: 対象を選んでビューに映し、内容に合ったパネルと確認用の表示を開く
void Editor::RevealIssue(const Issue& is) {
    auto has = [&](const char* s) { return is.message.find(s) != std::string::npos || is.category.find(s) != std::string::npos; };
    auto focus = [&](const char* id) { m_focusWindows.push_back(std::string("###") + id); };
    int node = is.node;
    if (node < 0 && is.mesh >= 0) {
        auto users = m_scene.NodesUsingMesh(is.mesh);
        if (!users.empty()) node = users[0];
    }
    if (node >= 0 && node < (int)m_scene.nodes.size()) {
        ClearSelection();
        Select(node);
        FocusNode(node);
        m_revealNode = node;
        m_panels.inspector = true;
        focus("Inspector");
    }
    if (is.mesh >= 0 && is.mesh < (int)m_scene.meshes.size()) {
        m_selMesh = is.mesh;
        m_selPrim = is.prim >= 0 ? is.prim : 0;
        m_panels.meshes = true;
        focus("Meshes");
    }
    if (is.material >= 0 && is.material < (int)m_scene.materials.size()) {
        m_selMaterial = is.material;
        m_panels.materials = true;
        focus("Materials");
    }
    if (is.texture >= 0 && is.texture < (int)m_scene.textures.size()) {
        m_selTexture = is.texture;
        m_panels.textures = true;
        focus("Textures");
    }
    if (is.animation >= 0 && is.animation < (int)m_scene.animations.size()) {
        m_anim = is.animation;
        m_animTime = 0;
        m_panels.animEditor = true;
        m_panels.diagnostics = true;
        m_diagForceTab = 1;
        focus("Animation");
        focus("AnimEditor");
    }
    // 内容に合った確認用の表示
    if (has("開いた辺") || has("穴")) { m_showOpenEdges = true; m_panels.diagnostics = true; m_diagForceTab = 0; focus("Diagnostics"); }
    if (has("非多様体")) { m_showNonManifold = true; m_panels.diagnostics = true; m_diagForceTab = 0; focus("Diagnostics"); }
    if (has("裏返") || has("内側") || has("巻き")) m_rs.mode = RM_Backface;
    else if (has("法線")) m_rs.mode = RM_NormalCheck;
    else if (has("UV")) { m_rs.mode = RM_Checker; m_panels.uvViewer = true; focus("UVViewer"); }
    else if (has("影響") || has("ウェイト")) m_rs.mode = RM_Influences;
    else if (has("接線")) m_rs.mode = RM_Tangents;
    else if (has("頂点カラー")) m_rs.mode = RM_VertexColor;
    if (is.category == "スキン" || has("ボーン") || has("ジョイント")) { m_rs.showBones = true; m_panels.skeleton = true; focus("Skeleton"); }
    if (has("ルートモーション")) { m_showRootPath = true; m_panels.diagnostics = true; m_diagForceTab = 1; }
    LOG_INFO("検証: %s", is.message.c_str());
}

// パネル上部の「どのモデルの一覧か」の表示と、全部表示への切り替え
void Editor::DrawModelFilterBar() {
    if (m_scene.roots.size() <= 1) return;
    int model = ActiveModel();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.45f, 1.0f));
    if (m_filterToModel && model >= 0) ImGui::TextUnformatted(IconText(ICON_MESH, ("モデル: " + m_scene.nodes[model].name).c_str()).c_str());
    else ImGui::TextUnformatted(IconText(ICON_LAYERS, "すべてのモデル").c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Checkbox("選んだモデルだけ", &m_filterToModel);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("オン: 選んでいるモデルのものだけを一覧に出します (何も選んでいなければ全部)");
}

// ---------------------------------------------------------------------------
// モデルパネル
// ---------------------------------------------------------------------------
void Editor::DrawModels() {
    ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_LAYERS, "モデル", "Models").c_str(), &m_panels.models)) { ImGui::End(); return; }
    PanelHint("読み込んだモデル (ファイル) の一覧です。モデル同士の結合や、アニメーションだけ / モデルだけの抜き出しもここで行います");
    if (m_scene.roots.empty()) {
        ImGui::TextDisabled("モデルがありません。上のツールバーの「開く」で何個でも読み込めます");
        ImGui::End();
        return;
    }
    const int active = ActiveModel();
    // ---- 一覧 ----
    if (ImGui::BeginTable("##models", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("名前", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("三角形", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("アニメ", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##vis", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int r : std::vector<int>(m_scene.roots)) {
            if (r < 0 || r >= (int)m_scene.nodes.size()) continue;
            ImGui::PushID(r);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            auto sub = Ops::SubtreeMask(m_scene, r);
            size_t tris = 0;
            for (size_t i = 0; i < sub.size(); ++i)
                if (sub[i] && m_scene.nodes[i].mesh >= 0)
                    for (auto& p : m_scene.meshes[m_scene.nodes[i].mesh].primitives) tris += p.indices.size() / 3;
            int anims = 0;
            for (auto& a : m_scene.animations)
                for (auto& c : a.channels) if (c.node >= 0 && c.node < (int)sub.size() && sub[c.node]) { ++anims; break; }
            if (ImGui::Selectable(m_scene.nodes[r].name.c_str(), r == active, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                ClearSelection();
                Select(r);
                if (ImGui::IsMouseDoubleClicked(0)) FocusNode(r);
            }
            if (ImGui::IsItemHovered()) {
                const SceneInfo& info = ModelInfo(r);
                ImGui::SetTooltip("%s\nダブルクリックでビューポートに映す", info.sourcePath.empty() ? "(作成したオブジェクト)" : info.sourcePath.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%zu", tris);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%d", anims);
            ImGui::TableNextColumn();
            bool vis = m_scene.nodes[r].visible;
            if (ImGui::SmallButton(IconOr(ICON_EYE, vis ? "表示" : "非表示"))) SetVisibility({ r }, !vis, vis ? "非表示" : "表示");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(vis ? "表示中 (クリックで隠す)" : "非表示 (クリックで表示)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(active < 0);
    if (ImGui::Button(IconText(ICON_EYE, "このモデルだけ表示").c_str())) {
        for (int r : m_scene.roots) if (r != active) SetVisibility({ r }, false, "他のモデルを隠す");
        SetVisibility({ active }, true, "表示");
    }
    ImGui::SameLine();
    if (ImGui::Button(IconText(ICON_EYE, "すべて表示").c_str())) ShowAll();
    ImGui::SameLine();
    if (ImGui::Button(IconText(ICON_DELETE, "このモデルを閉じる").c_str())) {
        Checkpoint("モデルを閉じる");
        m_scene.RemoveNodes({ active });
        Ops::RemoveUnusedResources(m_scene);
        m_scene.animations.erase(std::remove_if(m_scene.animations.begin(), m_scene.animations.end(), [](const Animation& a) { return a.channels.empty(); }),
                                 m_scene.animations.end());
        ClearSelection();
        ValidateSelection();
        m_anim = m_scene.animations.empty() ? -1 : std::min(m_anim, (int)m_scene.animations.size() - 1);
        m_renderer.ClearCaches();
        m_topoCache.clear();
        MarkChanged();
    }
    ImGui::EndDisabled();

    // ---- 結合 ----
    ImGui::SeparatorText(IconText(ICON_SKELETON, "結合 (別のモデルと組み合わせる)").c_str());
    if (m_scene.roots.size() < 2) {
        ImGui::TextDisabled("2 つ以上のモデルを読み込むと使えます (例: キャラクターと、モーションだけのファイル)");
    } else {
        auto modelCombo = [&](const char* label, int& sel) {
            bool ok = false;
            for (int r : m_scene.roots) if (r == sel) ok = true;
            if (!ok) sel = -1;
            if (ImGui::BeginCombo(label, sel >= 0 ? m_scene.nodes[sel].name.c_str() : "(選ぶ)")) {
                for (int r : m_scene.roots)
                    if (ImGui::Selectable(m_scene.nodes[r].name.c_str(), r == sel)) sel = r;
                ImGui::EndCombo();
            }
        };
        if (m_combineDst < 0 && active >= 0) m_combineDst = active;
        ImGui::SetNextItemWidth(-140);
        modelCombo("元 (使う側)", m_combineSrc);
        ImGui::SetNextItemWidth(-140);
        modelCombo("先 (受け取る側)", m_combineDst);
        const bool ready = m_combineSrc >= 0 && m_combineDst >= 0 && m_combineSrc != m_combineDst;

        ImGui::Spacing();
        ImGui::TextUnformatted("アニメーションを移す");
        ImGui::TextDisabled("元のアニメーションを、先のボーンへ名前で対応付けてコピーします (mixamorig: などの違いは無視)");
        ImGui::Combo("方法", &m_retarget.mode, "回転をそのままコピー (同じリグ・同じ配布元)\0レストポーズの差を補正 (骨の向きが違うリグ)\0");
        ImGui::Checkbox("腰の移動 (身長の比で調整)", &m_retarget.rootTranslation);
        ImGui::SameLine();
        ImGui::Checkbox("他の骨の移動も", &m_retarget.otherTranslations);
        ImGui::SameLine();
        ImGui::Checkbox("スケール", &m_retarget.scaleKeys);
        ImGui::Checkbox("コピー後に元のモデルを閉じる (モーションだけのファイル向け)", &m_combineCloseSrc);
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button(IconText(ICON_ANIMATION, "アニメーションを移す").c_str())) {
            Checkpoint("アニメーションを移す");
            Ops::RetargetReport rep;
            auto added = Ops::CopyAnimationsBetweenModels(m_scene, m_combineSrc, m_combineDst, {}, m_retarget, &rep);
            m_combineReport = "アニメーション " + std::to_string(rep.animations) + " 個、対応したボーン " + std::to_string(rep.matchedBones) + " 本";
            if (std::abs(rep.heightRatio - 1.0f) > 1e-3f) m_combineReport += "、身長の比 x" + std::to_string(rep.heightRatio).substr(0, 5);
            if (!rep.unmatched.empty()) {
                m_combineReport += "\n対応が見つからないボーン (" + std::to_string(rep.unmatched.size()) + "): ";
                for (size_t i = 0; i < rep.unmatched.size() && i < 12; ++i) m_combineReport += (i ? ", " : "") + rep.unmatched[i];
                if (rep.unmatched.size() > 12) m_combineReport += " ...";
            }
            LOG_INFO("%s", m_combineReport.c_str());
            int dst = m_combineDst;
            if (!added.empty()) {
                if (m_combineCloseSrc) {
                    int src = m_combineSrc;
                    auto srcSub = Ops::SubtreeMask(m_scene, src);
                    int shift = 0;
                    for (int i = 0; i < dst && i < (int)srcSub.size(); ++i) shift += srcSub[i];
                    m_scene.RemoveNodes({ src });
                    Ops::RemoveUnusedResources(m_scene);
                    m_scene.animations.erase(std::remove_if(m_scene.animations.begin(), m_scene.animations.end(), [](const Animation& a) { return a.channels.empty(); }),
                                             m_scene.animations.end());
                    dst -= shift; // 前にあったノードが消えた分だけ番号がずれる
                    m_combineSrc = -1;
                }
                ClearSelection();
                Select(dst);
                m_member.rev = 0;
                m_anim = -1;
                for (int a = 0; a < (int)m_scene.animations.size(); ++a) {
                    bool mine = false;
                    for (auto& c : m_scene.animations[a].channels) if (Ops::ModelRootOf(m_scene, c.node) == dst) mine = true;
                    if (mine) { m_anim = a; break; }
                }
                m_animTime = 0;
                m_playing = m_anim >= 0;
            }
            ValidateSelection();
            m_renderer.ClearCaches();
            MarkChanged();
        }
        ImGui::EndDisabled();
        if (!m_combineReport.empty()) ImGui::TextWrapped("%s", m_combineReport.c_str());

        ImGui::Spacing();
        ImGui::TextUnformatted("モデルを付ける / まとめる");
        ImGui::TextDisabled("武器を手のボーンに付ける、小物を体に付けるなど。先のモデルの中のノードを選びます");
        if (ready) {
            auto sub = Ops::SubtreeMask(m_scene, m_combineDst);
            if (m_attachNode >= 0 && (m_attachNode >= (int)sub.size() || !sub[m_attachNode])) m_attachNode = -1;
            ImGui::SetNextItemWidth(-140);
            if (ImGui::BeginCombo("付け先のノード", m_attachNode >= 0 ? m_scene.nodes[m_attachNode].name.c_str() : "(選ぶ)", ImGuiComboFlags_HeightLarge)) {
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##attachFilter", "名前で絞り込み (hand など)", &m_attachFilter);
                std::string f = m_attachFilter;
                for (auto& c : f) c = (char)std::tolower((unsigned char)c);
                for (int i = 0; i < (int)sub.size(); ++i) {
                    if (!sub[i]) continue;
                    std::string n = m_scene.nodes[i].name, ln = n;
                    for (auto& c : ln) c = (char)std::tolower((unsigned char)c);
                    if (!f.empty() && ln.find(f) == std::string::npos) continue;
                    ImGui::PushID(i);
                    if (ImGui::Selectable(n.c_str(), i == m_attachNode)) m_attachNode = i;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::BeginDisabled(!ready || m_attachNode < 0);
        if (ImGui::Button(IconText(ICON_SKELETON, "ノードに付ける (見た目の位置はそのまま)").c_str())) {
            Checkpoint("モデルを付ける");
            std::string e;
            if (Ops::AttachModel(m_scene, m_combineSrc, m_attachNode, true, &e)) {
                LOG_INFO("'%s' を '%s' に付けました", m_scene.nodes[m_combineSrc].name.c_str(), m_scene.nodes[m_attachNode].name.c_str());
                int s = m_combineSrc;
                ClearSelection();
                Select(s);
                m_combineSrc = -1;
            } else {
                LOG_ERROR("付けられません: %s", e.c_str());
            }
            MarkChanged();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button(IconText(ICON_LAYERS, "1 つのモデルにまとめる").c_str())) {
            Checkpoint("モデルをまとめる");
            int r = Ops::MergeModels(m_scene, m_combineSrc, m_combineDst);
            if (r >= 0) {
                LOG_INFO("モデルを 1 つにまとめました");
                ClearSelection();
                Select(r);
                m_combineDst = r;
                m_combineSrc = -1;
            }
            MarkChanged();
        }
        ImGui::EndDisabled();
    }

    // ---- 抜き取り ----
    ImGui::SeparatorText(IconText(ICON_EXPORT, "抜き取り (選んでいるモデルから)").c_str());
    ImGui::BeginDisabled(active < 0);
    ImGui::Checkbox("元のモデルからは取り除く", &m_extractRemove);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("オン: アニメーションを抜き出したら元のモデルのアニメーションを消す / モデルを抜き出したら元はアニメーションだけにする");
    if (ImGui::Button(IconText(ICON_ANIMATION, "アニメーションだけを抜き出す").c_str())) ExtractFromActiveModel(true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("骨格とアニメーションだけの新しいモデルを作ります (メッシュ・マテリアルなし)。\nモーションだけを別ファイルに書き出したいときに使います");
    ImGui::SameLine();
    if (ImGui::Button(IconText(ICON_MESH, "モデルだけを抜き出す").c_str())) ExtractFromActiveModel(false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("アニメーションを持たない新しいモデルを作ります");
    ImGui::EndDisabled();
    ImGui::TextDisabled("書き出すときに中身を選ぶこともできます (書き出し設定の「書き出す内容」)");
    ImGui::End();
}

void Editor::ExtractFromActiveModel(bool animationOnly) {
    int model = ActiveModel();
    if (model < 0) return;
    Checkpoint(animationOnly ? "アニメーションだけを抜き出す" : "モデルだけを抜き出す");
    Scene part = Ops::ExtractModel(m_scene, model);
    if (animationOnly) {
        if (part.animations.empty()) { LOG_WARN("このモデルにはアニメーションがありません"); return; }
        Ops::StripToAnimationOnly(part);
    } else {
        Ops::StripAnimations(part);
    }
    std::string name = m_scene.nodes[model].name + (animationOnly ? "_Anim" : "_Mesh");
    auto info = m_scene.nodes[model].modelInfo;
    // part の最上位 (元のモデルのグループ) をそのまま新しいモデルとして追加する
    size_t rootsBefore = m_scene.roots.size();
    Ops::AppendScene(m_scene, part);
    int newModel = rootsBefore < m_scene.roots.size() ? m_scene.roots[rootsBefore] : -1;
    if (newModel < 0) return;
    std::string unique = name;
    for (int k = 2;; ++k) {
        bool taken = false;
        for (int r : m_scene.roots) if (r != newModel && m_scene.nodes[r].name == unique) taken = true;
        if (!taken) break;
        unique = name + "_" + std::to_string(k);
    }
    m_scene.nodes[newModel].name = unique;
    m_scene.nodes[newModel].objectRoot = true;
    if (info) m_scene.nodes[newModel].modelInfo = info;
    if (m_extractRemove) {
        if (animationOnly) {
            // 元のモデルを動かすアニメーションを消す
            auto sub = Ops::SubtreeMask(m_scene, model);
            for (auto& a : m_scene.animations)
                a.channels.erase(std::remove_if(a.channels.begin(), a.channels.end(), [&](const AnimChannel& c) { return c.node >= 0 && c.node < (int)sub.size() && sub[c.node]; }),
                                 a.channels.end());
            m_scene.animations.erase(std::remove_if(m_scene.animations.begin(), m_scene.animations.end(), [](const Animation& a) { return a.channels.empty(); }),
                                     m_scene.animations.end());
        } else {
            // 元のモデルはアニメーションだけにする
            Scene rest = Ops::ExtractModel(m_scene, model);
            Ops::StripToAnimationOnly(rest);
            auto sub = Ops::SubtreeMask(m_scene, model);
            int shift = 0;
            for (int i = 0; i < newModel && i < (int)sub.size(); ++i) shift += sub[i];
            m_scene.RemoveNodes({ model });
            newModel -= shift;
            Ops::AppendScene(m_scene, rest);
        }
        Ops::RemoveUnusedResources(m_scene);
        m_scene.animations.erase(std::remove_if(m_scene.animations.begin(), m_scene.animations.end(), [](const Animation& a) { return a.channels.empty(); }),
                                 m_scene.animations.end());
    }
    ClearSelection();
    Select(newModel);
    m_member.rev = 0;
    if (m_anim >= (int)m_scene.animations.size()) m_anim = m_scene.animations.empty() ? -1 : 0;
    ValidateSelection();
    m_renderer.ClearCaches();
    m_topoCache.clear();
    MarkChanged();
    LOG_INFO("'%s' を作成しました", m_scene.nodes[newModel].name.c_str());
}
