// モデル診断パネル / ビューポートの診断表示 (穴・非多様体の辺・ルートモーションの軌跡・ピボット) / 頂点インスペクター
#include "Editor.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include "core/Units.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace {
const ImVec4 kOk(0.55f, 0.9f, 0.55f, 1), kWarn(1.0f, 0.8f, 0.3f, 1), kBad(1.0f, 0.45f, 0.4f, 1);
constexpr size_t kMaxOverlayLines = 200000; // 重いメッシュで止まらないよう、診断の線の数に上限を設ける

// 数が 0 なら薄く、あれば警告色で表示する表のセル
void CountCell(int n, const ImVec4& col = kWarn) {
    ImGui::TableNextColumn();
    if (n) ImGui::TextColored(col, "%d", n);
    else ImGui::TextDisabled("0");
}
} // namespace

const Editor::TopoCache& Editor::Topology(int mesh) {
    TopoCache& c = m_topoCache[mesh];
    const Mesh& m = m_scene.meshes[mesh];
    if (c.uid != m.uid || c.prims.size() != m.primitives.size()) {
        c.uid = m.uid;
        c.prims.clear();
        c.uvs.clear();
        for (auto& p : m.primitives) {
            c.prims.push_back(AnalyzeTopology(p, true));
            c.uvs.push_back(AnalyzeUV(p));
        }
    }
    return c;
}

// ---------------------------------------------------------------------------
// ビューポートに描く診断の線
// ---------------------------------------------------------------------------
void Editor::SubmitDiagnosticLines() {
    if (m_scene.Empty() || m_world.size() != m_scene.nodes.size()) return;
    size_t lines = 0;
    // 開いた辺 / 非多様体の辺 (表示中の姿勢で)
    if (m_showOpenEdges || m_showNonManifold) {
        std::vector<glm::vec3> wp;
        for (int i = 0; i < (int)m_scene.nodes.size() && lines < kMaxOverlayLines; ++i) {
            const Node& n = m_scene.nodes[i];
            if (n.mesh < 0 || n.mesh >= (int)m_scene.meshes.size() || !n.visible) continue;
            if (m_rs.debugSelectedOnly && !m_selection.empty() && !IsSelected(i)) continue;
            const TopoCache& tc = Topology(n.mesh);
            const Mesh& mesh = m_scene.meshes[n.mesh];
            for (size_t pi = 0; pi < mesh.primitives.size() && pi < tc.prims.size(); ++pi) {
                const TopologyInfo& t = tc.prims[pi];
                if (t.openEdgeVerts.empty() && t.nonManifoldEdgeVerts.empty()) continue;
                DeformPositions(m_scene, m_world, i, mesh.primitives[pi], wp);
                auto draw = [&](const std::vector<uint32_t>& ev, uint32_t col) {
                    for (size_t k = 0; k + 1 < ev.size() && lines < kMaxOverlayLines; k += 2, ++lines)
                        if (ev[k] < wp.size() && ev[k + 1] < wp.size()) m_renderer.AddLine(wp[ev[k]], wp[ev[k + 1]], col, true);
                };
                if (m_showOpenEdges) draw(t.openEdgeVerts, IM_COL32(255, 220, 40, 255));
                if (m_showNonManifold) draw(t.nonManifoldEdgeVerts, IM_COL32(255, 60, 200, 255));
            }
        }
    }
    // ルートモーションの軌跡 (1 秒ごとに目盛り)
    if (m_showRootPath && m_anim >= 0 && m_anim < (int)m_scene.animations.size()) {
        if (m_rootPathRev != m_sceneRevision || m_rootPathAnim != m_anim) {
            const Animation& a = m_scene.animations[m_anim];
            int root = FindRootMotionNode(m_scene, a);
            m_rootPath = root >= 0 ? SampleNodeTrajectory(m_scene, a, root, 30.0f) : std::vector<glm::vec3>();
            m_rootPathRev = m_sceneRevision;
            m_rootPathAnim = m_anim;
        }
        const float tick = m_sceneRadius * 0.03f;
        for (size_t k = 1; k < m_rootPath.size(); ++k) {
            m_renderer.AddLine(m_rootPath[k - 1], m_rootPath[k], IM_COL32(60, 220, 255, 255), true);
            // 地面への投影 (どこを歩いているか)
            glm::vec3 a = m_rootPath[k - 1], b = m_rootPath[k];
            a.y = b.y = 0.0f;
            m_renderer.AddLine(a, b, IM_COL32(60, 220, 255, 110), false);
        }
        for (size_t k = 0; k < m_rootPath.size(); k += 30)
            m_renderer.AddLine(m_rootPath[k] - glm::vec3(0, tick, 0), m_rootPath[k] + glm::vec3(0, tick, 0), IM_COL32(255, 255, 255, 255), true);
    }
    // オブジェクトのピボット (原点) と、メッシュの中心までの線
    if (m_showPivots) {
        // 各ノードが属するオブジェクト (一番近い objectRoot、無ければルート) ごとに、表示中の姿勢の範囲をまとめる (頂点は間引いて軽く)
        const int nn = (int)m_scene.nodes.size();
        std::vector<int> owner(nn, -1);
        for (int i = 0; i < nn; ++i) {
            int o = i;
            for (int p = i; p >= 0; p = m_scene.nodes[p].parent) {
                o = p;
                if (m_scene.nodes[p].objectRoot) break;
            }
            owner[i] = o;
        }
        std::vector<glm::vec3> mn(nn, glm::vec3(1e30f)), mx(nn, glm::vec3(-1e30f));
        for (int i = 0; i < nn; ++i) {
            glm::vec3 a, b;
            if (m_scene.nodes[i].mesh >= 0 && m_scene.ComputeNodeBounds(i, m_world, a, b, 500)) {
                mn[owner[i]] = glm::min(mn[owner[i]], a);
                mx[owner[i]] = glm::max(mx[owner[i]], b);
            }
        }
        const float s = m_sceneRadius * 0.04f;
        for (int i = 0; i < nn; ++i) {
            const Node& n = m_scene.nodes[i];
            if (!(n.objectRoot || n.parent < 0)) continue;
            glm::vec3 p(m_world[i][3]);
            m_renderer.AddLine(p - glm::vec3(s, 0, 0), p + glm::vec3(s, 0, 0), IM_COL32(255, 80, 80, 255), true);
            m_renderer.AddLine(p - glm::vec3(0, s, 0), p + glm::vec3(0, s, 0), IM_COL32(80, 255, 80, 255), true);
            m_renderer.AddLine(p - glm::vec3(0, 0, s), p + glm::vec3(0, 0, s), IM_COL32(80, 140, 255, 255), true);
            if (mn[i].x <= mx[i].x) {
                glm::vec3 c = (mn[i] + mx[i]) * 0.5f;
                if (glm::length(c - p) > m_sceneRadius * 0.02f) m_renderer.AddLine(p, c, IM_COL32(255, 255, 255, 150), true);
            }
        }
    }
    // 調べている頂点: 十字・法線・三角形
    if (m_vertexHitValid && m_vertexHit.node >= 0 && m_vertexHit.node < (int)m_scene.nodes.size()) {
        const Node& n = m_scene.nodes[m_vertexHit.node];
        if (n.mesh >= 0 && n.mesh < (int)m_scene.meshes.size() && m_vertexHit.prim < (int)m_scene.meshes[n.mesh].primitives.size()) {
            const Primitive& p = m_scene.meshes[n.mesh].primitives[m_vertexHit.prim];
            if (m_vertexHit.vertex < p.positions.size() && (size_t)m_vertexHit.tri * 3 + 2 < p.indices.size()) {
                glm::vec3 nrm;
                glm::vec3 v = DeformedVertex(m_scene, m_world, m_vertexHit.node, p, m_vertexHit.vertex, &nrm);
                const float s = m_sceneRadius * 0.015f;
                uint32_t c = IM_COL32(255, 255, 0, 255);
                m_renderer.AddLine(v - glm::vec3(s, 0, 0), v + glm::vec3(s, 0, 0), c, true);
                m_renderer.AddLine(v - glm::vec3(0, s, 0), v + glm::vec3(0, s, 0), c, true);
                m_renderer.AddLine(v - glm::vec3(0, 0, s), v + glm::vec3(0, 0, s), c, true);
                if (glm::length(nrm) > 0) m_renderer.AddLine(v, v + nrm * m_sceneRadius * 0.1f, IM_COL32(80, 160, 255, 255), true);
                glm::vec3 t[3];
                for (int k = 0; k < 3; ++k) t[k] = DeformedVertex(m_scene, m_world, m_vertexHit.node, p, p.indices[m_vertexHit.tri * 3 + k]);
                for (int k = 0; k < 3; ++k) m_renderer.AddLine(t[k], t[(k + 1) % 3], IM_COL32(255, 140, 0, 255), true);
            }
        }
    }
}

bool Editor::InspectVertexAt(const ImVec2& mouse, const ImVec2& size) {
    if (m_scene.Empty() || m_world.size() != m_scene.nodes.size() || size.x < 1 || size.y < 1) return false;
    glm::mat4 invVP = glm::inverse(m_camera.Projection(size.x / size.y) * m_camera.View());
    float x = mouse.x / size.x * 2.0f - 1.0f, y = 1.0f - mouse.y / size.y * 2.0f;
    // 深度の範囲 (0〜1 / -1〜1) に依らないよう、どちらでも範囲内の 2 点からレイを作る
    glm::vec4 a = invVP * glm::vec4(x, y, 0.0f, 1.0f), b = invVP * glm::vec4(x, y, 0.5f, 1.0f);
    glm::vec3 o = glm::vec3(a) / a.w, d = glm::vec3(b) / b.w - o;
    if (m_camera.ortho) o -= glm::normalize(d) * m_camera.farClip; // 平行投影はカメラの後ろからも当てる
    m_vertexHitValid = RaycastMeshes(m_scene, m_world, o, d, m_vertexHit);
    if (m_vertexHitValid) LOG_INFO("頂点を調べる: '%s' 頂点 %u (三角形 %u)", m_scene.nodes[m_vertexHit.node].name.c_str(), m_vertexHit.vertex, m_vertexHit.tri);
    return m_vertexHitValid;
}

// ---------------------------------------------------------------------------
// モデル診断パネル
// ---------------------------------------------------------------------------
void Editor::DrawDiagnostics() {
    if (!ImGui::Begin(PanelTitle(ICON_DIAG, "モデル診断", "Diagnostics").c_str(), &m_panels.diagnostics)) { ImGui::End(); return; }
    PanelHint("穴・裏返った面・ウェイト・アニメーションの不具合を詳しく調べます。ビューポートで色分けして確認できます");
    if (m_scene.Empty()) {
        ImGui::TextDisabled("モデルを読み込むと、穴・裏返り・ウェイト・アニメーションなどの問題を調べられます");
        ImGui::End();
        return;
    }
    // ---- 表示 (ビューポート) -----------------------------------------------
    ImGui::TextDisabled("ビューポートで確認:");
    ImGui::SameLine();
    auto modeButton = [&](const char* label, int mode, const char* tip) {
        bool on = m_rs.mode == mode;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(label)) m_rs.mode = on ? RM_Lit : mode;
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
    };
    modeButton("裏面", RM_Backface, "表面は灰色、裏面は赤で表示します。外から赤く見える面は裏返っています");
    modeButton("法線の向き", RM_NormalCheck, "頂点法線が面の向きと逆の場所を桃色で表示します (ライティングが黒く・変になる原因)");
    modeButton("ウェイト数", RM_Influences, "1 頂点に影響するボーンの数を、青 (1 本) 〜 赤 (4 本) で表示します。黒 (0 本) は動かない頂点です");
    modeButton("UV チェッカー", RM_Checker, "UV の伸び・つぶれ・向きを市松模様で確認します");
    ImGui::NewLine();
    ImGui::Checkbox("開いた辺 (黄)", &m_showOpenEdges);
    ImGui::SameLine();
    ImGui::Checkbox("非多様体の辺 (桃)", &m_showNonManifold);
    ImGui::SameLine();
    ImGui::Checkbox("ルートモーションの軌跡 (水色)", &m_showRootPath);
    ImGui::SameLine();
    ImGui::Checkbox("ピボット", &m_showPivots);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("オブジェクトの原点 (ピボット) を十字で、メッシュの中心までを白線で表示します");
    ImGui::SameLine();
    ImGui::Checkbox("選択のみ", &m_rs.debugSelectedOnly);

    auto tabFlags = [&](int tab) { return m_diagForceTab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
    if (ImGui::BeginTabBar("diag")) {
        // ---- メッシュ -----------------------------------------------------
        if (ImGui::BeginTabItem("メッシュ", nullptr, tabFlags(0))) {
            ImGui::TextDisabled("辺は頂点の位置で溶接して数えます (UV の継ぎ目は穴として数えません)。行をクリックすると選択します");
            const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                       ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("meshdiag", 13, tf, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupScrollFreeze(1, 1);
                for (const char* h : { "ノード / メッシュ", "頂点", "三角形", "開いた辺", "非多様体", "向き不一致", "裏返し", "法線が逆", "重なり", "縮退",
                                       "孤立頂点", "UV 範囲", "最大影響" })
                    ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (int i = 0; i < (int)m_scene.nodes.size(); ++i) {
                    const Node& n = m_scene.nodes[i];
                    if (n.mesh < 0 || n.mesh >= (int)m_scene.meshes.size()) continue;
                    const Mesh& mesh = m_scene.meshes[n.mesh];
                    const TopoCache& tc = Topology(n.mesh);
                    for (size_t pi = 0; pi < mesh.primitives.size() && pi < tc.prims.size(); ++pi) {
                        const Primitive& p = mesh.primitives[pi];
                        const TopologyInfo& t = tc.prims[pi];
                        const UVInfo& uv = tc.uvs[pi];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(i * 1000 + (int)pi);
                        char label[256];
                        snprintf(label, sizeof(label), "%s / %s[%zu]", n.name.c_str(), mesh.name.c_str(), pi);
                        if (ImGui::Selectable(label, IsSelected(i), ImGuiSelectableFlags_SpanAllColumns)) {
                            Select(i);
                            m_selMesh = n.mesh;
                            m_selPrim = (int)pi;
                        }
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        ImGui::Text("%zu", p.positions.size());
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", t.triangles);
                        CountCell(t.openEdges, kWarn);
                        CountCell(t.nonManifoldEdges, kBad);
                        CountCell(t.windingMismatch, kBad);
                        ImGui::TableNextColumn();
                        if (!t.closed) ImGui::TextDisabled("-");
                        else if (t.signedVolume < 0) ImGui::TextColored(kBad, "裏返し");
                        else ImGui::TextColored(kOk, "OK");
                        CountCell(p.HasNormals() ? t.normalsAgainstFaces : 0, kBad);
                        CountCell(t.duplicateTriangles, kBad);
                        CountCell(t.degenerateTriangles, kWarn);
                        CountCell(t.isolatedVertices, kWarn);
                        ImGui::TableNextColumn();
                        if (!p.HasUV0()) ImGui::TextColored(kWarn, "UV なし");
                        else ImGui::TextColored(uv.outside01 || uv.nan ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%.2f〜%.2f / %.2f〜%.2f", uv.min.x, uv.max.x,
                                                uv.min.y, uv.max.y);
                        ImGui::TableNextColumn();
                        int maxInf = 0;
                        if (p.HasSkin())
                            for (auto& w : p.weights) {
                                int c = 0;
                                for (int k = 0; k < 4; ++k) if (w[k] > 1e-4f) ++c;
                                maxInf = std::max(maxInf, c);
                            }
                        if (p.HasSkin()) ImGui::Text("%d", maxInf);
                        else ImGui::TextDisabled("-");
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // ---- アニメーション -------------------------------------------------
        if (ImGui::BeginTabItem("アニメーション", nullptr, tabFlags(1))) {
            if (m_scene.animations.empty()) ImGui::TextDisabled("アニメーションがありません");
            else {
                bool settled = ImGui::GetTime() - m_lastChangeTime > 0.3;
                if ((m_animReportsRev != m_sceneRevision || m_animReports.size() != m_scene.animations.size()) && settled) {
                    m_animReports.clear();
                    for (int a = 0; a < (int)m_scene.animations.size(); ++a) m_animReports.push_back(AnalyzeAnimation(m_scene, a));
                    m_animReportsRev = m_sceneRevision;
                }
                ImGui::TextDisabled("ルートモーション = 移動キーが変化する最上位のノード。ループ誤差 = 最初と最後の姿勢の差 (ルートから見た位置)");
                const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
                if (ImGui::BeginTable("animdiag", 11, tf)) {
                    for (const char* h : { "名前", "長さ", "キー", "一定のch", "回転の反転", "NaN", "ルート", "移動距離", "最高速度", "ループ誤差", "範囲の最大倍率" })
                        ImGui::TableSetupColumn(h);
                    ImGui::TableHeadersRow();
                    for (int a = 0; a < (int)m_animReports.size() && a < (int)m_scene.animations.size(); ++a) {
                        const AnimationReport& r = m_animReports[a];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(a);
                        if (ImGui::Selectable(m_scene.animations[a].name.c_str(), m_anim == a, ImGuiSelectableFlags_SpanAllColumns)) { m_anim = a; m_animTime = 0; }
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f 秒", r.duration);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r.keys);
                        CountCell(r.constantChannels, ImGui::GetStyleColorVec4(ImGuiCol_Text));
                        CountCell(r.quatFlips, kWarn);
                        CountCell(r.nanKeys, kBad);
                        ImGui::TableNextColumn();
                        if (r.rootNode >= 0) ImGui::TextUnformatted(m_scene.nodes[r.rootNode].name.c_str());
                        else ImGui::TextDisabled("なし");
                        ImGui::TableNextColumn();
                        if (r.rootNode >= 0) ImGui::Text("%s (直線 %s)", FormatLength(r.rootPathLength, m_displayUnit).c_str(), FormatLength(r.rootDisplacement, m_displayUnit).c_str());
                        else ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        if (r.rootNode >= 0) ImGui::Text("%.2f m/s", r.rootMaxSpeed);
                        else ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextColored(r.loopError > m_sceneRadius * 0.04f ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s",
                                           FormatLength(r.loopError, m_displayUnit).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextColored(r.maxBoundsRatio > 3.0f ? kBad : ImGui::GetStyleColorVec4(ImGuiCol_Text), "x%.2f (%.2f 秒)", r.maxBoundsRatio, r.maxBoundsTime);
                    }
                    ImGui::EndTable();
                }
                if (m_anim >= 0 && m_anim < (int)m_scene.animations.size()) {
                    Animation& a = m_scene.animations[m_anim];
                    ImGui::Text("選択中: %s", a.name.c_str());
                    if (ImGui::Button("回転キーの反転・長さを修正")) {
                        Checkpoint("回転キーを修正");
                        LOG_INFO("'%s' の回転キーを %d 個修正しました", a.name.c_str(), FixQuaternionKeys(a));
                        MarkChanged();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("隣のキーと符号が逆の回転キーを揃え、キーの間で一回転して見える問題を直します");
                    ImGui::SameLine();
                    if (ImGui::Button("一定のチャンネルを削除")) {
                        Checkpoint("一定のチャンネルを削除");
                        size_t before = a.channels.size();
                        a.channels.erase(std::remove_if(a.channels.begin(), a.channels.end(), [&](const AnimChannel& c) {
                            if (c.values.size() < 2 || c.node < 0 || c.node >= (int)m_scene.nodes.size()) return false;
                            for (auto& v : c.values) if (glm::length(v - c.values[0]) > 1e-6f) return false;
                            // レスト値と同じなら、チャンネルが無くても同じ姿勢になる
                            const Node& nd = m_scene.nodes[c.node];
                            glm::vec4 rest = c.path == AnimPath::Translation ? glm::vec4(nd.translation, 0)
                                           : c.path == AnimPath::Scale       ? glm::vec4(nd.scale, 0)
                                                                             : glm::vec4(nd.rotation.x, nd.rotation.y, nd.rotation.z, nd.rotation.w);
                            return glm::length(c.values[0] - rest) < 1e-5f || (c.path == AnimPath::Rotation && glm::length(c.values[0] + rest) < 1e-5f);
                        }), a.channels.end());
                        LOG_INFO("'%s' のチャンネルを %zu 本削除しました (レスト値と同じ一定のもの)", a.name.c_str(), before - a.channels.size());
                        MarkChanged();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("最後のフレームへ (ループ確認)")) { m_animTime = a.duration; m_playing = false; }
                }
            }
            ImGui::EndTabItem();
        }

        // ---- スキン ---------------------------------------------------------
        if (ImGui::BeginTabItem("スキン", nullptr, tabFlags(2))) {
            if (m_scene.skins.empty()) ImGui::TextDisabled("スキン (ボーン付きメッシュ) がありません");
            else {
                m_diagSkin = std::clamp(m_diagSkin, 0, (int)m_scene.skins.size() - 1);
                if (ImGui::BeginCombo("スキン", m_scene.skins[m_diagSkin].name.c_str())) {
                    for (int s = 0; s < (int)m_scene.skins.size(); ++s) {
                        ImGui::PushID(s);
                        if (ImGui::Selectable(m_scene.skins[s].name.c_str(), s == m_diagSkin)) m_diagSkin = s;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                const Skin& sk = m_scene.skins[m_diagSkin];
                auto counts = JointVertexCounts(m_scene, m_diagSkin);
                // 影響ボーン数の分布
                int hist[5] = {};
                for (auto& n : m_scene.nodes) {
                    if (n.skin != m_diagSkin || n.mesh < 0 || n.mesh >= (int)m_scene.meshes.size()) continue;
                    for (auto& p : m_scene.meshes[n.mesh].primitives)
                        if (p.HasSkin())
                            for (auto& w : p.weights) {
                                int c = 0;
                                for (int k = 0; k < 4; ++k) if (w[k] > 1e-4f) ++c;
                                ++hist[c];
                            }
                }
                ImGui::Text("ジョイント %zu 本 / 影響ボーン数の分布: 0 本 %d, 1 本 %d, 2 本 %d, 3 本 %d, 4 本 %d", sk.joints.size(), hist[0], hist[1], hist[2], hist[3], hist[4]);
                if (hist[0]) ImGui::TextColored(kBad, "ウェイトが無い (どのボーンにも付いていない) 頂点が %d 個あります。アニメーションでその場に残ります", hist[0]);
                for (int i = 0; i < (int)m_scene.nodes.size(); ++i) {
                    const Node& n = m_scene.nodes[i];
                    if (n.skin != m_diagSkin || n.mesh < 0) continue;
                    float dev = BindPoseDeviation(m_scene, i);
                    ImGui::BulletText("%s: レストポーズでの変形 %s%s", n.name.c_str(), FormatLength(dev, m_displayUnit).c_str(),
                                      dev > m_sceneRadius * 0.02f ? " (バインドポーズと違う)" : " (バインドポーズと一致)");
                }
                ImGui::TextDisabled("ジョイントをクリックすると、そのボーンのウェイトを表示します");
                const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("joints", 4, tf, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, Px(40));
                    ImGui::TableSetupColumn("ジョイント");
                    ImGui::TableSetupColumn("頂点数", ImGuiTableColumnFlags_WidthFixed, Px(80));
                    ImGui::TableSetupColumn("スケール", ImGuiTableColumnFlags_WidthFixed, Px(160));
                    ImGui::TableHeadersRow();
                    for (size_t j = 0; j < sk.joints.size(); ++j) {
                        int jn = sk.joints[j];
                        if (jn < 0 || jn >= (int)m_scene.nodes.size()) continue;
                        const Node& n = m_scene.nodes[jn];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%zu", j);
                        ImGui::TableNextColumn();
                        ImGui::PushID((int)j);
                        if (ImGui::Selectable(n.name.c_str(), m_selBone == jn, ImGuiSelectableFlags_SpanAllColumns)) {
                            m_selBone = jn;
                            m_rs.mode = RM_Weights;
                        }
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        int c = j < counts.size() ? counts[j] : 0;
                        if (c) ImGui::Text("%d", c);
                        else ImGui::TextDisabled("0");
                        ImGui::TableNextColumn();
                        bool nonUniform = std::abs(n.scale.x - n.scale.y) > 1e-4f || std::abs(n.scale.y - n.scale.z) > 1e-4f;
                        ImGui::TextColored(nonUniform ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%.3g, %.3g, %.3g", n.scale.x, n.scale.y, n.scale.z);
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }

        // ---- 頂点を調べる -----------------------------------------------------
        if (ImGui::BeginTabItem("頂点を調べる", nullptr, tabFlags(3))) {
            ImGui::Checkbox("ビューポートのクリックで頂点を調べる", &m_vertexInspect);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("オンの間は、クリックしても選択は変わらず、クリックした場所の頂点の情報を表示します");
            ImGui::SameLine();
            if (ImGui::SmallButton("クリア")) m_vertexHitValid = false;
            if (!m_vertexHitValid) ImGui::TextDisabled("ビューポートでメッシュをクリックしてください (表示中のポーズで判定します)");
            else if (m_vertexHit.node >= 0 && m_vertexHit.node < (int)m_scene.nodes.size()) {
                const Node& n = m_scene.nodes[m_vertexHit.node];
                if (n.mesh >= 0 && n.mesh < (int)m_scene.meshes.size() && m_vertexHit.prim < (int)m_scene.meshes[n.mesh].primitives.size()) {
                    const Mesh& mesh = m_scene.meshes[n.mesh];
                    const Primitive& p = mesh.primitives[m_vertexHit.prim];
                    uint32_t v = m_vertexHit.vertex;
                    if (v < p.positions.size()) {
                        glm::vec3 dn;
                        glm::vec3 dp = DeformedVertex(m_scene, m_world, m_vertexHit.node, p, v, &dn);
                        ImGui::Text("ノード: %s   メッシュ: %s [%d]", n.name.c_str(), mesh.name.c_str(), m_vertexHit.prim);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("選択")) Select(m_vertexHit.node);
                        size_t ti = (size_t)m_vertexHit.tri * 3;
                        if (ti + 2 < p.indices.size())
                            ImGui::Text("三角形 %u (頂点 %u, %u, %u)   当たった位置の重心座標 %.2f, %.2f, %.2f", m_vertexHit.tri, p.indices[ti], p.indices[ti + 1], p.indices[ti + 2],
                                        m_vertexHit.bary.x, m_vertexHit.bary.y, m_vertexHit.bary.z);
                        ImGui::SeparatorText((std::string("頂点 ") + std::to_string(v)).c_str());
                        ImGui::Text("位置 (メッシュ)    %.5f, %.5f, %.5f", p.positions[v].x, p.positions[v].y, p.positions[v].z);
                        ImGui::Text("位置 (ワールド)    %.5f, %.5f, %.5f", dp.x, dp.y, dp.z);
                        if (v < p.normals.size()) {
                            const glm::vec3& nv = p.normals[v];
                            ImGui::Text("法線              %.4f, %.4f, %.4f (長さ %.4f)", nv.x, nv.y, nv.z, glm::length(nv));
                            ImGui::Text("法線 (変形後)     %.4f, %.4f, %.4f", dn.x, dn.y, dn.z);
                        } else ImGui::TextColored(kWarn, "法線なし");
                        if (v < p.tangents.size())
                            ImGui::Text("接線              %.4f, %.4f, %.4f  w=%.0f", p.tangents[v].x, p.tangents[v].y, p.tangents[v].z, p.tangents[v].w);
                        if (v < p.uv0.size()) ImGui::Text("UV0               %.5f, %.5f", p.uv0[v].x, p.uv0[v].y);
                        if (v < p.uv1.size()) ImGui::Text("UV1               %.5f, %.5f", p.uv1[v].x, p.uv1[v].y);
                        if (v < p.colors.size()) ImGui::Text("頂点カラー        %.3f, %.3f, %.3f, %.3f", p.colors[v].x, p.colors[v].y, p.colors[v].z, p.colors[v].w);
                        if (p.material >= 0 && p.material < (int)m_scene.materials.size()) ImGui::Text("マテリアル        %s", m_scene.materials[p.material].name.c_str());
                        if (p.HasSkin()) {
                            ImGui::SeparatorText("ウェイト");
                            const Skin* sk = n.skin >= 0 && n.skin < (int)m_scene.skins.size() ? &m_scene.skins[n.skin] : nullptr;
                            float sum = 0;
                            for (int k = 0; k < 4; ++k) {
                                float w = p.weights[v][k];
                                sum += w;
                                if (w <= 0) continue;
                                int j = p.joints[v][k];
                                int jn = sk && j < (int)sk->joints.size() ? sk->joints[j] : -1;
                                ImGui::PushID(k);
                                char lbl[256];
                                snprintf(lbl, sizeof(lbl), "%5.1f%%  [%d] %s", w * 100.0f, j, jn >= 0 ? m_scene.nodes[jn].name.c_str() : "(範囲外)");
                                if (ImGui::Selectable(lbl, m_selBone == jn) && jn >= 0) { m_selBone = jn; m_rs.mode = RM_Weights; }
                                ImGui::PopID();
                            }
                            ImGui::TextColored(std::abs(sum - 1.0f) > 1e-3f ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text), "合計 %.4f", sum);
                        }
                        if (!p.targets.empty()) {
                            ImGui::SeparatorText("モーフ (この頂点の移動量)");
                            for (size_t t = 0; t < p.targets.size(); ++t)
                                if (v < p.targets[t].dPositions.size() && glm::length(p.targets[t].dPositions[v]) > 1e-7f)
                                    ImGui::BulletText("%s: %.4f", p.targets[t].name.c_str(), glm::length(p.targets[t].dPositions[v]));
                        }
                    }
                }
            }
            ImGui::EndTabItem();
        }

        // ---- エンジン向けチェック ---------------------------------------------
        if (ImGui::BeginTabItem("エンジン向けチェック", nullptr, tabFlags(4))) {
            ImGui::TextDisabled("Unity / Unreal / Web (three.js など) に持っていく前の確認です");
            auto row = [&](bool ok, const char* item, const std::string& detail) {
                ImGui::TextColored(ok ? kOk : kWarn, ok ? "OK" : "要確認");
                ImGui::SameLine(80);
                ImGui::TextUnformatted(item);
                if (!detail.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", detail.c_str()); }
            };
            glm::vec3 d = m_sceneMax - m_sceneMin;
            row(d.y > 0.05f && d.y < 50.0f, "大きさが現実的 (高さ 5 cm〜50 m)", "高さ " + FormatLength(d.y, m_displayUnit) + "。100 倍や 0.01 倍なら単位の間違い");
            glm::vec3 c = (m_sceneMin + m_sceneMax) * 0.5f;
            row(glm::length(glm::vec2(c.x, c.z)) < std::max(glm::length(d), 1.0f) && std::abs(m_sceneMin.y) < std::max(d.y * 0.1f, 0.01f), "原点に立っている (足元が Y=0、XZ が原点付近)",
                "中心 " + FormatLength(c.x, m_displayUnit, false) + ", " + FormatLength(c.z, m_displayUnit, false) + " / 最低点 " + FormatLength(m_sceneMin.y, m_displayUnit));
            bool rootScaleOk = true;
            for (int r : m_scene.roots) {
                const glm::vec3& s = m_scene.nodes[r].scale;
                if (glm::length(s - glm::vec3(1.0f)) > 1e-4f) rootScaleOk = false;
            }
            row(rootScaleOk, "ルートのスケールが 1", "FBX の 0.01 / 100 は「スケールを階層に適用」で 1 にできます");
            size_t maxVerts = 0;
            for (auto& m : m_scene.meshes) for (auto& p : m.primitives) maxVerts = std::max(maxVerts, p.positions.size());
            row(maxVerts <= 65535, "1 メッシュ 65535 頂点以下 (16bit インデックス)", "最大 " + std::to_string(maxVerts) + " 頂点。超えると古い環境やモバイルで分割が必要");
            size_t maxJoints = 0;
            for (auto& s : m_scene.skins) maxJoints = std::max(maxJoints, s.joints.size());
            row(maxJoints <= 255, "ボーン数 255 本以下", "最大 " + std::to_string(maxJoints) + " 本 (モバイルや WebGL では少ないほど軽い)");
            int dupNames = 0, nonAscii = 0;
            {
                std::set<std::string> names;
                for (auto& n : m_scene.nodes) {
                    if (!names.insert(n.name).second) ++dupNames;
                    if (HasNonAscii(n.name)) ++nonAscii;
                }
            }
            row(dupNames == 0, "ノード名の重複なし", dupNames ? std::to_string(dupNames) + " 個 (アニメーションの対応付けが壊れます)" : "");
            row(nonAscii == 0, "ノード名が英数字のみ", nonAscii ? std::to_string(nonAscii) + " 個が日本語などを含みます" : "");
            int roots = 0;
            for (size_t s = 0; s < m_scene.skins.size(); ++s) {
                std::set<int> js(m_scene.skins[s].joints.begin(), m_scene.skins[s].joints.end()), rs;
                for (int j : m_scene.skins[s].joints) {
                    if (j < 0 || j >= (int)m_scene.nodes.size()) continue;
                    int r = j;
                    while (m_scene.nodes[r].parent >= 0 && js.count(m_scene.nodes[r].parent)) r = m_scene.nodes[r].parent;
                    rs.insert(r);
                }
                roots = std::max(roots, (int)rs.size());
            }
            row(roots <= 1, "ルートボーンが 1 本 (Unity Humanoid / Unreal)", roots > 1 ? std::to_string(roots) + " 本に分かれています" : "");
            int badTex = 0;
            for (auto& t : m_scene.textures) {
                int w = t.Width(), h = t.Height();
                if (w > 4096 || h > 4096 || (w > 0 && (w & (w - 1))) || (h > 0 && (h & (h - 1)))) ++badTex;
            }
            row(badTex == 0, "テクスチャが 2 のべき乗・4096 以下", badTex ? std::to_string(badTex) + " 枚 (ミップマップや圧縮で不利)" : "");
            int errs = 0;
            for (auto& is : m_issues) if (is.severity == Severity::Error) ++errs;
            row(errs == 0, "検証エラーなし", errs ? std::to_string(errs) + " 件 (検証パネルを確認)" : "");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    m_diagForceTab = -1;
    ImGui::End();
}
