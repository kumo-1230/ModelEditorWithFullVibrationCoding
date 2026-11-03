// ビューポート: 3D 描画 / カメラ操作 / ギズモ / ピッキング
#include "Editor.h"
#include "core/Log.h"
#include "core/Units.h"
#include "core/SceneOps.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

void Editor::DrawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin(PanelTitle(ICON_VIEWPORT, "ビューポート", "Viewport").c_str(), &m_panels.viewport, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        m_viewportHovered = false;
        ImGui::End();
        return;
    }
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 8.0f);
    size.y = std::max(size.y, 8.0f);
    m_viewportPos = pos;
    m_viewportSize = size;
    m_viewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // カメラのクリップ面を距離に合わせて調整
    m_camera.nearClip = std::max(m_camera.distance * 0.002f, 1e-5f);
    m_camera.farClip = std::max(m_camera.distance * 50.0f + m_sceneRadius * 10.0f, m_camera.distance + m_rs.gridExtent * 3.0f);

    FrameInput fi;
    fi.scene = &m_scene;
    fi.world = &m_world;
    fi.settings = &m_rs;
    fi.camera = &m_camera;
    fi.selection = m_selection;
    fi.selectedBone = m_selBone;
    fi.width = (int)size.x;
    fi.height = (int)size.y;
    fi.poseRevision = m_poseRev;
    fi.sceneRadius = m_sceneRadius;

    SubmitRulerLines();
    SubmitDiagnosticLines();
    m_renderer.Render(fi);
    if (m_pickRequested && m_vertexInspect) {
        // 頂点を調べるモード: 選択は変えず、クリックした頂点の情報を診断パネルに出す
        InspectVertexAt(ImVec2(m_pickPos.x - pos.x, m_pickPos.y - pos.y), size);
        m_pickRequested = false;
    }
    if (m_pickRequested) {
        int hit = m_renderer.Pick(fi, (int)(m_pickPos.x - pos.x), (int)(m_pickPos.y - pos.y));
        bool ctrl = ImGui::GetIO().KeyCtrl, shift = ImGui::GetIO().KeyShift;
        if (hit >= 0) {
            Select(hit, shift, ctrl);
            LOG_DEBUG("ピック: [%d] %s", hit, m_scene.nodes[hit].name.c_str());
        } else if (!ctrl && !shift) {
            ClearSelection();
        }
        m_pickRequested = false;
    }

    ImGui::Image((ImTextureID)(intptr_t)m_renderer.ColorTexture(), size);
    m_viewportHovered = ImGui::IsItemHovered();

    DrawRulerLabels(pos, size, m_camera.Projection(size.x / size.y) * m_camera.View());
    DrawGizmo(pos, size);
    HandleViewportInput(pos, size);
    DrawViewportOverlay(pos, size);
    // 何も開いていなければ、何をすればよいかを案内する (ファイルのドロップ先でもある)
    if (m_scene.Empty() && !m_importing) DrawWelcome(pos, size);
    ImGui::End();
}

void Editor::DrawGizmo(const ImVec2& pos, const ImVec2& size) {
    ImGuizmo::SetOrthographic(m_camera.ortho);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
    glm::mat4 view = m_camera.View();
    glm::mat4 proj = m_camera.Projection(size.x / size.y);

    // ボーン名
    if (m_showBoneNames && !m_scene.skins.empty()) {
        auto joints = m_scene.JointMask();
        glm::mat4 vp = proj * view;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < (int)m_scene.nodes.size(); ++i) {
            if (!joints[i]) continue;
            glm::vec4 c = vp * glm::vec4(glm::vec3(m_world[i][3]), 1.0f);
            if (c.w <= 0) continue;
            glm::vec2 ndc = glm::vec2(c) / c.w;
            ImVec2 sp(pos.x + (ndc.x * 0.5f + 0.5f) * size.x, pos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * size.y);
            bool sel = i == m_selBone;
            dl->AddText(ImVec2(sp.x + 4, sp.y - 6), sel ? IM_COL32(255, 230, 60, 255) : IM_COL32(170, 220, 255, 200), m_scene.nodes[i].name.c_str());
        }
    }

    // ビューキューブ
    {
        glm::mat4 v = view;
        float cubeSize = 96.0f;
        ImGuizmo::ViewManipulate(glm::value_ptr(v), m_camera.distance, ImVec2(pos.x + size.x - cubeSize - 8, pos.y + std::max(m_vpToolbarH, ImGui::GetFrameHeight()) + 12), ImVec2(cubeSize, cubeSize), 0x10101010);
        if (v != view) {
            glm::mat4 inv = glm::inverse(v);
            glm::vec3 camPos(inv[3]);
            glm::vec3 fwd = -glm::normalize(glm::vec3(inv[2]));
            m_camera.target = camPos + fwd * m_camera.distance;
            m_camera.pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
            m_camera.yaw = std::atan2(-fwd.x, -fwd.z);
        }
    }

    // 定規の原点をギズモで移動
    if (m_rulerGizmo && m_activeRuler >= 0 && m_activeRuler < (int)m_rulers.size()) {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), m_rulers[m_activeRuler].origin);
        float snap[3] = { m_snapT, m_snapT, m_snapT };
        bool snapOn = m_snap || ImGui::GetIO().KeyCtrl;
        if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(m), nullptr, snapOn ? snap : nullptr))
            m_rulers[m_activeRuler].origin = glm::vec3(m[3]);
        m_gizmoWasUsing = false;
        return;
    }
    if (m_primary < 0 || m_primary >= (int)m_scene.nodes.size() || m_gizmoOp == 3) {
        m_gizmoWasUsing = false;
        return;
    }
    ImGuizmo::OPERATION op = m_gizmoOp == 0 ? ImGuizmo::TRANSLATE : m_gizmoOp == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
    ImGuizmo::MODE mode = (m_gizmoLocal || m_gizmoOp == 2) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    glm::mat4 world = m_world[m_primary];
    glm::mat4 before = world;
    float snap[3];
    float s = m_gizmoOp == 0 ? m_snapT : m_gizmoOp == 1 ? m_snapR : m_snapS;
    snap[0] = snap[1] = snap[2] = s;
    bool snapOn = m_snap || ImGui::GetIO().KeyCtrl;

    Node savedNode = m_scene.nodes[m_primary];
    bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, mode, glm::value_ptr(world), nullptr, snapOn ? snap : nullptr);
    bool using_ = ImGuizmo::IsUsing();
    if (using_ && !m_gizmoWasUsing) {
        // ドラッグ開始: 変更前の状態を Undo に積む
        Checkpoint(m_gizmoOp == 0 ? "移動" : m_gizmoOp == 1 ? "回転" : "スケール");
    }
    m_gizmoWasUsing = using_;
    if (changed) {
        glm::mat4 delta = world * glm::inverse(before);
        std::set<int> scaled; // 同じオブジェクトノードを二重に拡大しない
        // 表示中の姿勢 (アニメーション適用後) からの差分を、レスト値とアニメーションのキーに適用する
        auto applyWorld = [&](int idx, const glm::mat4& newWorld) {
            const Node& n = m_scene.nodes[idx];
            // 親ノードを途中で自動作成した場合、その親のワールド行列は単位行列
            glm::mat4 parentWorld = n.parent >= 0 && n.parent < (int)m_world.size() ? m_world[n.parent] : glm::mat4(1.0f);
            glm::mat4 local = glm::inverse(parentWorld) * newWorld;
            glm::vec3 t, sc;
            glm::quat r;
            DecomposeTRS(local, t, r, sc);
            NodePose cur;
            if (idx < (int)m_pose.size()) cur = m_pose[idx];
            else { cur.t = n.translation; cur.r = n.rotation; cur.s = n.scale; }
            glm::vec3 dT(0.0f), mul(1.0f);
            glm::quat dR(1, 0, 0, 0);
            if (m_gizmoOp == 0 || m_gizmoOp == 1) dT = t - cur.t;
            if (m_gizmoOp == 1) dR = glm::normalize(r * glm::inverse(cur.r));
            if (m_gizmoOp == 2)
                for (int k = 0; k < 3; ++k) mul[k] = std::abs(cur.s[k]) > 1e-12f ? sc[k] / cur.s[k] : 1.0f;
            if (m_gizmoOp == 2) {
                // ルートモーションを持つノード等はオブジェクトノード (親) を同じ倍率で拡大する
                int st = ScaleTargetOf(idx, true);
                if (st >= 0 && scaled.insert(st).second) Ops::OffsetNodeTRS(m_scene, st, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), mul);
                return;
            }
            Ops::OffsetNodeTRS(m_scene, idx, dT, dR, mul);
        };
        applyWorld(m_primary, world);
        for (int sidx : m_selection) {
            if (sidx == m_primary || sidx < 0 || sidx >= (int)m_world.size()) continue;
            // 親が選択済みなら二重適用しない
            bool parentSelected = false;
            for (int o : m_selection) if (o != sidx && m_scene.IsAncestor(o, sidx)) parentSelected = true;
            if (!parentSelected) applyWorld(sidx, delta * m_world[sidx]);
        }
        m_eulerNode = -1;
        MarkChanged();
    }
    (void)savedNode;
}

void Editor::HandleViewportInput(const ImVec2& pos, const ImVec2& size) {
    ImGuiIO& io = ImGui::GetIO();
    static int dragMode = 0; // 0=なし 1=回転 2=パン 3=ズーム 4=クリック待ち
    static ImVec2 pressPos;
    bool gizmoActive = ImGuizmo::IsUsingAny() || ImGuizmo::IsViewManipulateHovered() ||
                       (ImGuizmo::IsOver() && m_primary >= 0 && m_gizmoOp != 3) ||
                       ImGui::IsAnyItemHovered(); // ツールバー上のクリックは無視

    if (m_viewportHovered && !gizmoActive) {
        bool alt = io.KeyAlt;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dragMode = alt ? 1 : 4;
            pressPos = io.MousePos;
            ImGui::SetWindowFocus();
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            dragMode = alt ? 3 : (io.KeyShift ? 2 : 1);
            ImGui::SetWindowFocus();
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            dragMode = 2;
            ImGui::SetWindowFocus();
        }
        if (io.MouseWheel != 0.0f) m_camera.Dolly(io.MouseWheel * (io.KeyShift ? 0.25f : 1.0f));
    }
    ImVec2 d = io.MouseDelta;
    switch (dragMode) {
    case 1:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_camera.Orbit(d.x * 0.008f, d.y * 0.008f);
        else dragMode = 0;
        break;
    case 2:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_camera.Pan(d.x, d.y, size.y);
        else dragMode = 0;
        break;
    case 3:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_camera.Dolly((d.x - d.y) * 0.02f);
        else dragMode = 0;
        break;
    case 4:
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImVec2 diff(io.MousePos.x - pressPos.x, io.MousePos.y - pressPos.y);
            if (m_marqueeActive) {
                SelectInRect(pressPos, io.MousePos, pos, size, io.KeyShift, io.KeyCtrl);
            } else if (diff.x * diff.x + diff.y * diff.y < 16.0f && m_viewportHovered) {
                m_pickRequested = true;
                m_pickPos = io.MousePos;
            }
            m_marqueeActive = false;
            dragMode = 0;
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) {
            if (m_lmbOrbit) {
                m_camera.Orbit(d.x * 0.008f, d.y * 0.008f);
            } else {
                // Maya と同じく左ドラッグで矩形選択
                m_marqueeActive = true;
                m_marqueeStart = pressPos;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(pressPos, io.MousePos, IM_COL32(80, 150, 255, 40));
                dl->AddRect(pressPos, io.MousePos, IM_COL32(120, 180, 255, 220));
            }
        }
        break;
    }
    (void)pos;
}

void Editor::SelectInRect(const ImVec2& a, const ImVec2& b, const ImVec2& vpPos, const ImVec2& vpSize, bool additive, bool toggle) {
    ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
    glm::mat4 vp = m_camera.Projection(vpSize.x / vpSize.y) * m_camera.View();
    auto inside = [&](const glm::vec3& p) {
        glm::vec4 c = vp * glm::vec4(p, 1.0f);
        if (c.w <= 1e-6f) return false;
        float sx = vpPos.x + (c.x / c.w * 0.5f + 0.5f) * vpSize.x;
        float sy = vpPos.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * vpSize.y;
        return sx >= lo.x && sx <= hi.x && sy >= lo.y && sy <= hi.y;
    };
    // 親が非表示のものは対象外
    auto visible = [&](int i) {
        for (int p = i; p >= 0; p = m_scene.nodes[p].parent) if (!m_scene.nodes[p].visible) return false;
        return true;
    };
    std::vector<int> hits;
    for (int i = 0; i < (int)m_scene.nodes.size(); ++i) {
        if (m_scene.nodes[i].mesh < 0 || !visible(i)) continue;
        glm::vec3 mn, mx;
        if (!m_scene.ComputeNodeBounds(i, m_world, mn, mx, 500)) continue;
        bool hit = inside((mn + mx) * 0.5f);
        for (int k = 0; k < 8 && !hit; ++k) hit = inside(glm::vec3((k & 1) ? mx.x : mn.x, (k & 2) ? mx.y : mn.y, (k & 4) ? mx.z : mn.z));
        if (hit) hits.push_back(i);
    }
    if (!additive && !toggle) ClearSelection();
    for (int h : hits) Select(h, true, toggle);
    LOG_DEBUG("矩形選択: %zu 個", hits.size());
}

// ビューポート左上の表示ツールバー (Unreal Engine のビューポート上部と同じく「どう見せるか」だけを置く)。
// 編集ツール (移動・回転など) は画面上のメインツールバーにある
void Editor::DrawToolbar() {
    auto tip = [](const char* text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
    };
    auto toggle = [&](const char* icon, const char* label, bool active, const char* tipText) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.44f, 0.88f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.53f, 0.96f, 1.0f));
        }
        bool pressed = ImGui::Button(IconText(icon, label).c_str());
        if (active) ImGui::PopStyleColor(2);
        tip(tipText);
        return pressed;
    };
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("ウェイト (選択ボーン)").x + ImGui::GetFrameHeight() * 2);
    if (ImGui::BeginCombo("##mode", IconText(ICON_LIGHT, RenderModeName(m_rs.mode)).c_str())) {
        for (int i = 0; i < RM_Count; ++i)
            if (ImGui::Selectable(RenderModeName(i), m_rs.mode == i)) m_rs.mode = i;
        ImGui::EndCombo();
    }
    tip("表示モード\nライティング (完成イメージ) のほか、法線・UV・ウェイト・裏面などを色で確認できます");
    SameLineIfFits(IconText(ICON_WIRE, "ワイヤーのみ").c_str()); // 入らなければ次の行へ (狭いビューポートで枠外に出さない)
    const char* wireLabel = m_rs.wireframe ? "ワイヤーのみ" : "ワイヤー";
    if (toggle(ICON_WIRE, wireLabel, m_rs.wireframe || m_rs.wireOnShaded, "ワイヤーフレーム (4)\n押すたびに  なし → 面 + 線 → 線のみ  と切り替わります")) {
        if (!m_rs.wireframe && !m_rs.wireOnShaded) m_rs.wireOnShaded = true;
        else if (m_rs.wireOnShaded) { m_rs.wireOnShaded = false; m_rs.wireframe = true; }
        else m_rs.wireframe = false;
    }
    SameLineIfFits(IconText(ICON_TEXTURE, "テクスチャ").c_str());
    if (toggle(ICON_TEXTURE, "テクスチャ", m_rs.useTextures, "テクスチャ (6)\nオフにすると画像を貼らずに形だけを確認できます")) m_rs.useTextures = !m_rs.useTextures;
    SameLineIfFits(IconText(ICON_SKELETON, "ボーン").c_str());
    if (toggle(ICON_SKELETON, "ボーン", m_rs.showBones, "ボーン (骨格) を表示")) m_rs.showBones = !m_rs.showBones;
    SameLineIfFits(IconText(ICON_NORMALS, "法線").c_str());
    if (toggle(ICON_NORMALS, "法線", m_rs.showNormals, "法線 (面の向き) を線で表示\n長さや対象は「表示」で変えられます")) m_rs.showNormals = !m_rs.showNormals;
    SameLineIfFits(IconText(ICON_GRID, "グリッド").c_str());
    if (toggle(ICON_GRID, "グリッド", m_rs.showGrid, "床のグリッド\n1 マスの大きさは「表示」で変えられます")) m_rs.showGrid = !m_rs.showGrid;
    SameLineIfFits(IconText(ICON_EYE, "表示 ▼").c_str());
    if (ImGui::Button(IconText(ICON_EYE, "表示 ▼").c_str())) ImGui::OpenPopup("ViewOptions");
    tip("そのほかの表示設定\nライト・背景色・グリッド・スナップの刻み・視野角など");
    if (ImGui::BeginPopup("ViewOptions")) {
        ImGui::Checkbox("シェード + ワイヤー", &m_rs.wireOnShaded);
        ImGui::Checkbox("ワイヤーフレームのみ", &m_rs.wireframe);
        ImGui::Checkbox("選択を強調", &m_rs.highlightSelection);
        ImGui::Checkbox("ノーマルマップ", &m_rs.useNormalMaps);
        ImGui::Checkbox("頂点カラーを乗算", &m_rs.useVertexColors);
        ImGui::Checkbox("背面カリング", &m_rs.backfaceCulling);
        ImGui::Checkbox("スキニング (アニメーション変形)", &m_rs.skinningEnabled);
        ImGui::Separator();
        ImGui::Checkbox("ボーン", &m_rs.showBones);
        ImGui::SameLine();
        ImGui::Checkbox("最前面", &m_rs.bonesOnTop);
        ImGui::SameLine();
        ImGui::Checkbox("名前", &m_showBoneNames);
        ImGui::SliderFloat("ボーンサイズ", &m_rs.boneSize, 0.1f, 5.0f);
        ImGui::Checkbox("法線", &m_rs.showNormals);
        ImGui::SameLine();
        ImGui::Checkbox("接線/従法線", &m_rs.showTangents);
        ImGui::SameLine();
        ImGui::Checkbox("AABB", &m_rs.showBounds);
        ImGui::Checkbox("選択のみ (法線/接線/AABB)", &m_rs.debugSelectedOnly);
        ImGui::SliderFloat("法線の長さ", &m_rs.normalLength, 0.001f, 0.3f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("ノード軸", &m_rs.showNodeAxes);
        ImGui::SameLine();
        ImGui::Checkbox("原点軸", &m_rs.showOriginAxes);
        ImGui::SameLine();
        ImGui::Checkbox("グリッド", &m_rs.showGrid);
        ImGui::SetNextItemWidth(Px(110));
        ImGui::DragFloat("1 マス (m)", &m_rs.gridSpacing, 0.001f, 0.001f, 100.0f, "%.3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(80));
        ImGui::DragInt("太線の間隔", &m_rs.gridMajorEvery, 0.2f, 1, 100);
        ImGui::SetNextItemWidth(Px(110));
        ImGui::DragFloat("範囲 ± (m)", &m_rs.gridExtent, 0.1f, 0.01f, 10000.0f, "%.2f");
        ImGui::SameLine();
        if (ImGui::SmallButton("既定 (10cm / 1m / ±10m)")) { m_rs.gridSpacing = 0.1f; m_rs.gridMajorEvery = 10; m_rs.gridExtent = 10.0f; }
        ImGui::Separator();
        ImGui::ColorEdit3("背景色", &m_rs.bgColor.x);
        ImGui::SliderFloat3("ライト方向", &m_rs.lightDir.x, -1.0f, 1.0f);
        ImGui::SliderFloat("ライト強度", &m_rs.lightIntensity, 0.0f, 10.0f);
        ImGui::SliderFloat("環境光", &m_rs.ambient, 0.0f, 2.0f);
        ImGui::SliderFloat("露出", &m_rs.exposure, 0.1f, 4.0f);
        ImGui::SliderInt("MSAA", &m_rs.msaa, 0, 8);
        ImGui::Separator();
        ImGui::SliderAngle("視野角", &m_camera.fovY, 10.0f, 120.0f);
        ImGui::Checkbox("平行投影", &m_camera.ortho);
        ImGui::DragFloat("移動スナップ", &m_snapT, 0.01f, 0.001f, 100.0f);
        ImGui::DragFloat("回転スナップ (度)", &m_snapR, 1.0f, 1.0f, 90.0f);
        ImGui::DragFloat("スケールスナップ", &m_snapS, 0.01f, 0.01f, 10.0f);
        ImGui::EndPopup();
    }
}

void Editor::DrawViewportOverlay(const ImVec2& pos, const ImVec2& size) {
    // 表示ツールバー
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 8, pos.y + 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.09f, 0.09f, 0.09f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.09f, 0.09f, 0.09f, 0.85f));
    ImGui::BeginGroup();
    DrawToolbar();
    ImGui::EndGroup();
    m_vpToolbarH = ImGui::GetItemRectMax().y - pos.y; // 折り返して 2 行になることもあるので、下の文字はこの下に置く
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    // 統計
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const RenderStats& st = m_renderer.Stats();
    char buf[512];
    snprintf(buf, sizeof(buf), "三角形 %zu  頂点 %zu  描画 %d  変形 %d (%.2fms)\n%s  %s  グリッド 1マス = %s",
             st.triangles, st.vertices, st.drawCalls, st.deformedMeshes, st.deformMs,
             m_camera.ortho ? "平行投影" : "透視投影", m_gizmoLocal ? "ローカル" : "ワールド", FormatLength(m_rs.gridSpacing, m_displayUnit).c_str());
    dl->AddText(ImVec2(pos.x + 8, pos.y + size.y - 40), IM_COL32(220, 220, 220, 200), buf);
    if (m_primary >= 0 && m_primary < (int)m_scene.nodes.size()) {
        std::string sel = "選択: " + m_scene.nodes[m_primary].name;
        if (m_selection.size() > 1) sel += " (+" + std::to_string(m_selection.size() - 1) + ")";
        dl->AddText(ImVec2(pos.x + 8, pos.y + size.y - 58), IM_COL32(255, 200, 100, 230), sel.c_str());
    }
    if (m_anim >= 0 && m_anim < (int)m_scene.animations.size()) {
        snprintf(buf, sizeof(buf), "%s %s  %.2f / %.2f s", m_playing ? "再生中" : "停止", m_scene.animations[m_anim].name.c_str(), m_animTime, m_scene.animations[m_anim].duration);
        dl->AddText(ImVec2(pos.x + 8, pos.y + std::max(m_vpToolbarH, ImGui::GetFrameHeight()) + 8), IM_COL32(150, 230, 150, 230), buf);
    }
    // 実寸サイズ
    if (m_showSizeOverlay && !m_scene.Empty()) {
        glm::vec3 d = m_sceneMax - m_sceneMin;
        std::string s = "実寸  幅 " + FormatLength(d.x, m_displayUnit) + "  ×  高さ " + FormatLength(d.y, m_displayUnit) + "  ×  奥行 " + FormatLength(d.z, m_displayUnit);
        ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        ImVec2 p0(pos.x + 10, pos.y + std::max(m_vpToolbarH, ImGui::GetFrameHeight()) + 12 + ImGui::GetTextLineHeightWithSpacing());
        dl->AddRectFilled(ImVec2(p0.x - 6, p0.y - 3), ImVec2(p0.x + ts.x + 6, p0.y + ts.y + 3), IM_COL32(20, 22, 26, 190), 4.0f);
        dl->AddText(p0, IM_COL32(255, 225, 120, 255), s.c_str());
    }
    if (m_showGizmoHelp && m_viewportHovered) {
        const char* help = m_lmbOrbit ? "左ドラッグ/Alt+左/右: 回転  中/Shift+右: パン  ホイール: ズーム  F: 選択  A: 全体  H: 表示切替"
                                      : "左ドラッグ: 矩形選択  Alt+左/右: 回転  中/Shift+右: パン  ホイール: ズーム  F: 選択  A: 全体  H: 表示切替";
        ImVec2 ts = ImGui::CalcTextSize(help);
        // 左下の統計と重なるほど狭いときは出さない
        if (ts.x < size.x * 0.55f) dl->AddText(ImVec2(pos.x + size.x - ts.x - 8, pos.y + size.y - ts.y - 6), IM_COL32(200, 200, 200, 140), help);
    }
}
