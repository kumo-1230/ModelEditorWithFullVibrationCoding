// 階層パネル / インスペクター
#include "Editor.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cctype>

static bool ContainsI(const std::string& s, const std::string& sub) {
    if (sub.empty()) return true;
    auto it = std::search(s.begin(), s.end(), sub.begin(), sub.end(), [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != s.end();
}

// ---------------------------------------------------------------------------
// 階層
// ---------------------------------------------------------------------------
void Editor::DrawHierarchy() {
    if (!ImGui::Begin(PanelTitle(ICON_HIERARCHY, "階層", "Hierarchy").c_str(), &m_panels.hierarchy)) { ImGui::End(); return; }
    PanelHint("モデルの部品 (ノード) の親子関係です。クリックで選択、ドラッグ&ドロップで親子付け、右クリックで操作メニューが開きます");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "名前で検索...", &m_hierarchyFilter);
    // ボタンはパネルの幅に入るだけ横に並べ、入らなければ次の行へ回す (枠外に出て押せなくならないように)
    ImGui::TextDisabled("ノード %zu  選択 %zu", m_scene.nodes.size(), m_selection.size());
    SameLineIfFits("+ノード");
    if (ImGui::SmallButton("+ノード")) {
        Checkpoint("ノード作成");
        int n = m_scene.AddNode("Node", m_primary);
        Select(n);
        MarkChanged();
    }
    SameLineIfFits("すべて展開");
    if (ImGui::SmallButton("すべて展開")) m_revealNode = -2;
    SameLineIfFits("表示切替 (H)");
    if (ImGui::SmallButton("表示切替 (H)")) ToggleSelectionVisibility();
    SameLineIfFits("選択のみ (I)");
    if (ImGui::SmallButton("選択のみ (I)")) IsolateSelection();
    SameLineIfFits("全表示 (Shift+H)");
    if (ImGui::SmallButton("全表示 (Shift+H)")) ShowAll();
    SameLineIfFits("+形状");
    if (ImGui::SmallButton("+形状")) ImGui::OpenPopup("primpop");
    if (ImGui::BeginPopup("primpop")) {
        for (int t = 0; t < Ops::Prim_Count; ++t)
            if (ImGui::MenuItem(Ops::PrimitiveName(t))) CreatePrimitiveObject(t);
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("オブジェクト %zu", m_scene.roots.size());
    ImGui::Separator();

    ImGui::BeginChild("tree", ImVec2(0, 0), ImGuiChildFlags_None);
    auto joints = m_scene.JointMask();
    std::vector<bool> pass(m_scene.nodes.size(), m_hierarchyFilter.empty());
    if (!m_hierarchyFilter.empty()) {
        // 一致したノードとその祖先を表示
        for (int i = 0; i < (int)m_scene.nodes.size(); ++i) {
            if (!ContainsI(m_scene.nodes[i].name, m_hierarchyFilter)) continue;
            int p = i;
            int guard = 0;
            while (p >= 0 && !pass[p] && guard++ < 4096) { pass[p] = true; p = m_scene.nodes[p].parent; }
        }
    }
    for (int r : std::vector<int>(m_scene.roots))
        if (r < (int)m_scene.nodes.size()) DrawHierarchyNode(r, joints, pass);

    // 空き領域へのドロップ = ルートに移動
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(40.0f, ImGui::GetContentRegionAvail().y)));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ME_NODE")) {
            int src = *(const int*)p->Data;
            Checkpoint("ルートに移動");
            m_scene.Reparent(src, -1, true);
            MarkChanged();
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked()) ClearSelection();
    ImGui::EndChild();
    if (m_revealNode == -2) m_revealNode = -1;
    else if (m_revealNode >= 0) m_revealNode = -1;
    ImGui::End();
}

void Editor::DrawHierarchyNode(int idx, const std::vector<bool>& joints, const std::vector<bool>& pass) {
    if (!pass[idx]) return;
    Node& n = m_scene.nodes[idx];
    ImGui::PushID(idx);
    bool leaf = true;
    for (int c : n.children) if (c < (int)pass.size() && pass[c]) leaf = false;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (IsSelected(idx)) flags |= ImGuiTreeNodeFlags_Selected;
    const bool forceOpen = !m_hierarchyFilter.empty() || m_revealNode == -2 || (m_revealNode >= 0 && m_scene.IsAncestor(idx, m_revealNode));

    // カリング: 画面外の行は描かずに高さだけ確保する (開いている子も同じように処理し、スクロール量は変えない)
    {
        const float rowH = m_hierRowH > 0 ? m_hierRowH : ImGui::GetFrameHeightWithSpacing();
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        const bool onScreen = ImGui::IsRectVisible(cp, ImVec2(cp.x + 1.0f, cp.y + rowH));
        if (!onScreen && m_renameNode != idx && m_revealNode != idx && !forceOpen) {
            const ImGuiID id = ImGui::GetID("##node");
            const bool open = !leaf && ImGui::GetStateStorage()->GetInt(id, 0) != 0;
            ImGui::Dummy(ImVec2(1.0f, rowH - ImGui::GetStyle().ItemSpacing.y));
            if (open) {
                ImGui::TreePushOverrideID(id); // 描いたときと同じ ID / 字下げにする
                for (int c : std::vector<int>(n.children))
                    if (c < (int)m_scene.nodes.size()) DrawHierarchyNode(c, joints, pass);
                ImGui::TreePop();
            }
            ImGui::PopID();
            return;
        }
    }
    const float rowStartY = ImGui::GetCursorPosY();
    if (forceOpen) ImGui::SetNextItemOpen(true);

    // 表示/非表示
    bool vis = n.visible;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 1));
    if (ImGui::Checkbox("##vis", &vis)) {
        // 選択中のノードなら選択全体に適用
        if (IsSelected(idx) && m_selection.size() > 1) SetVisibility(EditTargets(), vis, vis ? "表示" : "非表示");
        else SetVisibility({ idx }, vis, vis ? "表示" : "非表示");
    }
    ImGui::PopStyleVar();
    ImGui::SameLine();

    const char* icon = "○";
    ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (joints[idx]) { icon = "◆"; col = ImVec4(0.45f, 0.8f, 1.0f, 1); }
    if (n.mesh >= 0) { icon = n.skin >= 0 ? "★" : "■"; col = n.skin >= 0 ? ImVec4(1.0f, 0.75f, 0.45f, 1) : ImVec4(0.75f, 1.0f, 0.6f, 1); }
    else if (n.objectRoot) { icon = IconOr(ICON_LAYERS, "□"); col = ImVec4(0.85f, 0.75f, 1.0f, 1); } // モデル (ファイルのグループ) / オブジェクト
    if (!n.visible) col.w = 0.45f;

    bool open;
    if (m_renameNode == idx) {
        ImGui::Text("%s", icon);
        ImGui::SameLine();
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rename", &m_renameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (!m_renameBuf.empty() && m_renameBuf != n.name) {
                Checkpoint("名前変更");
                m_scene.nodes[idx].name = m_renameBuf;
                MarkChanged();
            }
            m_renameNode = -1;
        }
        if (ImGui::IsItemDeactivated() || ImGui::IsKeyPressed(ImGuiKey_Escape)) m_renameNode = -1;
        open = !leaf && ImGui::TreeNodeEx("##renaming", flags | ImGuiTreeNodeFlags_DefaultOpen);
        if (open && !leaf) {
            for (int c : std::vector<int>(m_scene.nodes[idx].children)) DrawHierarchyNode(c, joints, pass);
            ImGui::TreePop();
        }
        ImGui::PopID();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, col);
    std::string label = std::string(icon) + " " + n.name;
    open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
    ImGui::PopStyleColor();
    if (m_revealNode == idx) ImGui::SetScrollHereY(0.5f);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && m_primary >= 0 && m_scene.nodes[m_primary].parent == n.parent) {
            // 兄弟を範囲選択
            int parent = n.parent;
            const auto& sib = parent >= 0 ? m_scene.nodes[parent].children : m_scene.roots;
            auto a = std::find(sib.begin(), sib.end(), m_primary), b = std::find(sib.begin(), sib.end(), idx);
            if (a != sib.end() && b != sib.end()) {
                if (a > b) std::swap(a, b);
                for (auto it = a; it <= b; ++it) if (!IsSelected(*it)) m_selection.push_back(*it);
                m_primary = idx;
            }
        } else {
            Select(idx, false, io.KeyCtrl);
        }
        m_revealNode = -1;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) FocusNode(idx);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("[%d] %s", idx, n.name.c_str());
        ImGui::TextDisabled("%s", m_scene.NodePath(idx).c_str()); // ツールチップは中身に合わせて広がるので省略しない
        if (n.mesh >= 0 && n.mesh < (int)m_scene.meshes.size())
            ImGui::Text("メッシュ: %s (%zu 頂点 / %zu 三角形)", m_scene.meshes[n.mesh].name.c_str(), m_scene.meshes[n.mesh].VertexCount(), m_scene.meshes[n.mesh].TriangleCount());
        if (n.skin >= 0) ImGui::Text("スキン: %d", n.skin);
        if (joints[idx]) ImGui::Text("ジョイント");
        ImGui::EndTooltip();
    }

    // ドラッグ & ドロップで親子付け
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("ME_NODE", &idx, sizeof(int));
        ImGui::Text("%s", n.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ME_NODE")) {
            int src = *(const int*)p->Data;
            if (src != idx && !m_scene.IsAncestor(src, idx)) {
                Checkpoint("親子付け");
                m_scene.Reparent(src, idx, !ImGui::GetIO().KeyAlt); // Alt でローカル値を保持
                MarkChanged();
            } else {
                LOG_WARN("循環する親子付けはできません");
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("ctx")) {
        if (!IsSelected(idx)) Select(idx);
        DrawNodeContextMenu(idx);
        ImGui::EndPopup();
    }
    m_hierRowH = ImGui::GetCursorPosY() - rowStartY; // 描いた行の高さ (画面外の行の高さに使う)

    if (open && !leaf) {
        for (int c : std::vector<int>(m_scene.nodes[idx].children))
            if (c < (int)m_scene.nodes.size()) DrawHierarchyNode(c, joints, pass);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Editor::DrawNodeContextMenu(int idx) {
    if (idx < 0 || idx >= (int)m_scene.nodes.size()) return;
    ImGui::TextDisabled("%s", m_scene.nodes[idx].name.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("名前変更", "F2")) { m_renameNode = idx; m_renameBuf = m_scene.nodes[idx].name; }
    if (ImGui::MenuItem("表示する (フォーカス)", "F")) FocusNode(idx);
    if (ImGui::MenuItem("子ノードを追加")) {
        Checkpoint("ノード作成");
        int n = m_scene.AddNode("Node", idx);
        Select(n);
        MarkChanged();
    }
    if (ImGui::MenuItem("複製", "Ctrl+D")) DuplicateSelected();
    if (ImGui::MenuItem("複製 (メッシュもコピー)")) {
        Checkpoint("複製");
        int n = Ops::DuplicateNode(m_scene, idx, true);
        Select(n);
        MarkChanged();
    }
    if (ImGui::MenuItem("ルートに移動 (親子解除)", nullptr, false, m_scene.nodes[idx].parent >= 0)) {
        Checkpoint("親子解除");
        m_scene.Reparent(idx, -1, true);
        MarkChanged();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("フリーズ (T/R/S)")) ApplyFreeze(idx, false);
    if (ImGui::MenuItem("階層ごとフリーズ")) ApplyFreeze(idx, true);
    if (ImGui::MenuItem("スケールを階層に適用")) ApplyRootScale(idx);
    if (ImGui::MenuItem("オブジェクトとして扱う (親でスケール)", nullptr, m_scene.nodes[idx].objectRoot, !Ops::NodeHasAnimation(m_scene, idx))) {
        Checkpoint("オブジェクト指定");
        m_scene.nodes[idx].objectRoot = !m_scene.nodes[idx].objectRoot;
        MarkChanged();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("このノードのスケールを、書き出し時に中身へ焼き込むか、親ノードとして残すかを選べるようにします");
    if (ImGui::MenuItem("オブジェクト用の親ノードを作成", nullptr, false, Ops::FindObjectNode(m_scene, idx) < 0)) {
        Checkpoint("オブジェクト作成");
        int o = Ops::EnsureObjectNode(m_scene, idx);
        if (o >= 0) Select(o);
        MarkChanged();
    }
    if (ImGui::MenuItem("トランスフォームをリセット")) {
        Checkpoint("リセット");
        Ops::ResetTransform(m_scene.nodes[idx]);
        m_eulerNode = -1;
        MarkChanged();
    }
    if (ImGui::MenuItem("ピボットをメッシュ中心へ", nullptr, false, m_scene.nodes[idx].mesh >= 0)) {
        Checkpoint("ピボット中心");
        std::string e;
        if (!Ops::CenterPivot(m_scene, idx, &e)) LOG_ERROR("%s", e.c_str());
        MarkChanged();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("子孫をすべて選択")) {
        std::function<void(int)> rec = [&](int i) { if (!IsSelected(i)) m_selection.push_back(i); for (int c : m_scene.nodes[i].children) rec(c); };
        rec(idx);
    }
    if (ImGui::MenuItem("子孫を表示")) {
        Checkpoint("表示");
        std::function<void(int)> rec = [&](int i) { m_scene.nodes[i].visible = true; for (int c : m_scene.nodes[i].children) rec(c); };
        rec(idx);
        MarkChanged();
    }
    if (ImGui::MenuItem("これだけ表示 (他を非表示)")) {
        Checkpoint("表示");
        for (auto& n : m_scene.nodes) n.visible = false;
        std::function<void(int)> rec = [&](int i) { m_scene.nodes[i].visible = true; for (int c : m_scene.nodes[i].children) rec(c); };
        rec(idx);
        for (int p = m_scene.nodes[idx].parent; p >= 0; p = m_scene.nodes[p].parent) m_scene.nodes[p].visible = true;
        MarkChanged();
    }
    if (ImGui::MenuItem("すべて表示")) {
        Checkpoint("表示");
        for (auto& n : m_scene.nodes) n.visible = true;
        MarkChanged();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("削除", "Delete")) DeleteSelected();
}

// ---------------------------------------------------------------------------
// インスペクター
// ---------------------------------------------------------------------------
void Editor::DrawTransformEditor(int idx) {
    Node& n = m_scene.nodes[idx];
    std::vector<int> targets = EditTargets();
    const bool multi = targets.size() > 1;
    // 表示・編集するのは「画面に見えている現在値」(アニメーション適用後)。
    // 変更は差分として、レスト値とアニメーションのキーの両方に適用する (Maya のチャンネルボックスと同じ感覚)
    auto base = [&](int tg) {
        if (tg >= 0 && tg < (int)m_pose.size()) return m_pose[tg];
        NodePose p;
        p.t = m_scene.nodes[tg].translation;
        p.r = m_scene.nodes[tg].rotation;
        p.s = m_scene.nodes[tg].scale;
        return p;
    };
    const NodePose pb = base(idx);
    // オイラー角は編集中の値をキャッシュしてジンバルの跳びを防ぐ
    if (m_eulerNode != idx || m_eulerQuat != pb.r) {
        m_euler = glm::degrees(glm::eulerAngles(pb.r));
        m_eulerNode = idx;
        m_eulerQuat = pb.r;
    }
    auto eulerOf = [](const glm::quat& q) { return glm::degrees(glm::eulerAngles(q)); };
    // 複数選択時、値が揃っていない成分を調べる
    auto mixedOf = [&](auto getter) {
        glm::bvec3 mixed(false);
        glm::vec3 ref = getter(pb);
        for (int t : targets)
            for (int k = 0; k < 3; ++k)
                if (std::abs(getter(base(t))[k] - ref[k]) > 1e-4f) mixed[k] = true;
        return mixed;
    };
    glm::bvec3 mixT = mixedOf([](const NodePose& p) { return p.t; });
    glm::bvec3 mixR = mixedOf([&](const NodePose& p) { return eulerOf(p.r); });
    // スケールはオブジェクトノード (親) に向くことがある。親がまだ無ければスケール 1 として表示し、編集時に作る
    auto scaleOf = [&](int tg) {
        int st = ScaleTargetOf(tg, false);
        return st < 0 ? glm::vec3(1.0f) : base(st).s;
    };
    glm::bvec3 mixS(false);
    for (int t : targets)
        for (int k = 0; k < 3; ++k)
            if (std::abs(scaleOf(t)[k] - scaleOf(idx)[k]) > 1e-4f) mixS[k] = true;
    const int scaleTarget = ScaleTargetOf(idx, false);
    auto mixedLabel = [](glm::bvec3 m) {
        std::string s;
        if (m.x) s += "X ";
        if (m.y) s += "Y ";
        if (m.z) s += "Z ";
        return s;
    };
    bool animated = false;
    for (int tg : targets) if (Ops::NodeHasAnimation(m_scene, tg)) animated = true;

    if (multi) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%zu 個に同時適用 (表示値は %s)", targets.size(), n.name.c_str());
        ImGui::Checkbox("相対で適用 (差分を加算 / スケールは倍率)", &m_multiRelative);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("オフ: 変更した成分を全員同じ値にします (Maya のチャンネルボックスと同じ)\nオン: 変更量を全員に加えます");
    }
    if (animated) ImGui::TextColored(ImVec4(0.6f, 0.85f, 1, 1), "アニメーション付き: 現在フレームの値を表示しています。変更するとキー全体も同じだけ補正します");

    const bool rel = m_multiRelative && multi;
    const glm::quat qI(1, 0, 0, 0);
    float speed = std::max(m_sceneRadius * 0.005f, 0.001f);
    ImGui::PushItemWidth(-90);
    // 移動
    glm::vec3 t = pb.t, t0 = t;
    if (EditValue("移動", t, [&] { return ImGui::DragFloat3("移動", &t.x, speed, 0, 0, "%.4f"); })) {
        for (int tg : targets) {
            NodePose b = base(tg);
            glm::vec3 nt = b.t;
            for (int k = 0; k < 3; ++k)
                if (t[k] != t0[k]) nt[k] = rel ? nt[k] + (t[k] - t0[k]) : t[k];
            Ops::OffsetNodeTRS(m_scene, tg, nt - b.t, qI, glm::vec3(1.0f));
        }
    }
    if (multi && glm::any(mixT)) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "混在 %s", mixedLabel(mixT).c_str()); }
    // 回転 (度)
    glm::vec3 e = m_euler, e0 = e;
    if (EditValue("回転", e, [&] { return ImGui::DragFloat3("回転 (度)", &e.x, 0.5f, 0, 0, "%.3f"); })) {
        for (int tg : targets) {
            NodePose b = base(tg);
            glm::vec3 te = tg == idx ? e0 : eulerOf(b.r);
            for (int k = 0; k < 3; ++k)
                if (e[k] != e0[k]) te[k] = rel ? te[k] + (e[k] - e0[k]) : e[k];
            glm::quat nq = glm::normalize(glm::quat(glm::radians(te)));
            Ops::OffsetNodeTRS(m_scene, tg, glm::vec3(0.0f), glm::normalize(nq * glm::inverse(b.r)), glm::vec3(1.0f));
            if (tg == idx) m_eulerQuat = nq;
        }
        m_euler = e;
    }
    if (multi && glm::any(mixR)) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "混在 %s", mixedLabel(mixR).c_str()); }
    // スケール
    glm::vec3 s = scaleOf(idx), s0 = s;
    if (EditValue("スケール", s, [&] { return ImGui::DragFloat3(scaleTarget == idx ? "スケール" : "スケール (親)", &s.x, 0.005f, 0, 0, "%.4f"); })) {
        int changedAxis = s.x != s0.x ? 0 : s.y != s0.y ? 1 : 2;
        float ratio = s0[changedAxis] != 0 ? s[changedAxis] / s0[changedAxis] : 1.0f;
        std::set<int> done;
        for (int tg : targets) {
            glm::vec3 bs = scaleOf(tg);
            int st = ScaleTargetOf(tg, true);
            if (st < 0 || !done.insert(st).second) continue;
            NodePose b;
            b.s = bs;
            glm::vec3 ns = b.s;
            if (m_uniformScale) {
                if (rel || tg == idx) ns *= ratio;
                else ns = glm::vec3(s[changedAxis]);
            } else {
                for (int k = 0; k < 3; ++k)
                    if (s[k] != s0[k]) ns[k] = rel ? (s0[k] != 0 ? ns[k] * (s[k] / s0[k]) : s[k]) : s[k];
            }
            glm::vec3 mul(1.0f);
            for (int k = 0; k < 3; ++k) {
                if (std::abs(b.s[k]) > 1e-12f) mul[k] = ns[k] / b.s[k];
                else LOG_WARN("'%s' のスケール %c が 0 のため倍率で変更できません", m_scene.nodes[st].name.c_str(), "XYZ"[k]);
            }
            Ops::OffsetNodeTRS(m_scene, st, glm::vec3(0.0f), qI, mul); // 原点 (ピボット) を中心に拡大。原点は動かさない
        }
    }
    if (multi && glm::any(mixS)) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "混在 %s", mixedLabel(mixS).c_str()); }
    if (scaleTarget != idx)
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1, 1), "スケールはオブジェクト '%s' (アニメーションの無い親) に適用します。\nルートモーションも同じ倍率になり、ずれません",
                           scaleTarget >= 0 ? m_scene.nodes[scaleTarget].name.c_str() : (m_scene.nodes[idx].name + "_Object (自動作成)").c_str());
    ImGui::PopItemWidth();
    ImGui::Checkbox("均等スケール", &m_uniformScale);
    ImGui::SameLine();
    ImGui::TextDisabled("(四元数 %.3f %.3f %.3f %.3f)", pb.r.x, pb.r.y, pb.r.z, pb.r.w);

    auto resetAll = [&](const char* label, int what) {
        Checkpoint(label);
        for (int tg : targets) {
            NodePose b = base(tg);
            if (what == 0) Ops::OffsetNodeTRS(m_scene, tg, -b.t, qI, glm::vec3(1.0f));
            if (what == 1) Ops::OffsetNodeTRS(m_scene, tg, glm::vec3(0.0f), glm::inverse(b.r), glm::vec3(1.0f));
            if (what == 2) {
                int st = ScaleTargetOf(tg, false);
                if (st < 0) continue; // 親が無い = スケール 1
                glm::vec3 bs = base(st).s, mul(1.0f);
                for (int k = 0; k < 3; ++k) if (std::abs(bs[k]) > 1e-12f) mul[k] = 1.0f / bs[k];
                Ops::OffsetNodeTRS(m_scene, st, glm::vec3(0.0f), qI, mul);
            }
        }
        m_eulerNode = -1;
        MarkChanged();
    };
    if (ImGui::Button("T をリセット")) resetAll("移動リセット", 0);
    ImGui::SameLine();
    if (ImGui::Button("R をリセット")) resetAll("回転リセット", 1);
    ImGui::SameLine();
    if (ImGui::Button("S をリセット")) resetAll("スケールリセット", 2);
    ImGui::SeparatorText("フリーズ (Maya: Freeze Transformations)");
    ImGui::TextWrapped("現在の値をメッシュ頂点と子ノードに焼き込み、移動=0 / 回転=0 / スケール=1 に戻します。見た目は変わりません。");
    ImGui::Checkbox("移動##fz", &m_freezeT);
    ImGui::SameLine();
    ImGui::Checkbox("回転##fz", &m_freezeR);
    ImGui::SameLine();
    ImGui::Checkbox("スケール##fz", &m_freezeS);
    if (ImGui::Button("フリーズ")) for (int tg : targets) ApplyFreeze(tg, false);
    ImGui::SameLine();
    if (ImGui::Button("階層ごとフリーズ")) for (int tg : TopLevelTargets()) ApplyFreeze(tg, true);
    ImGui::SameLine();
    if (ImGui::Button("スケールを階層に適用")) for (int tg : TopLevelTargets()) ApplyRootScale(tg);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("均等スケール (例: FBX の 0.01) を子孫のボーン位置・メッシュ・逆バインド行列・\nアニメーションに伝播させ、このノードのスケールを 1 にします。\nスキン付きリグでも壊れません。");
    if (n.mesh >= 0 && ImGui::Button("ピボットをメッシュ中心へ")) {
        Checkpoint("ピボット中心");
        for (int tg : targets) {
            std::string err;
            if (m_scene.nodes[tg].mesh >= 0 && !Ops::CenterPivot(m_scene, tg, &err)) LOG_ERROR("%s", err.c_str());
        }
        MarkChanged();
    }
    if (ImGui::TreeNode("ワールド行列 / ローカル行列")) {
        auto showMat = [](const char* title, const glm::mat4& m) {
            ImGui::TextDisabled("%s", title);
            for (int r = 0; r < 4; ++r) ImGui::Text("%10.4f %10.4f %10.4f %10.4f", m[0][r], m[1][r], m[2][r], m[3][r]);
        };
        showMat("ローカル", n.LocalMatrix());
        if (idx < (int)m_world.size()) {
            showMat("ワールド (現在のポーズ)", m_world[idx]);
            glm::vec3 wt, ws;
            glm::quat wr;
            DecomposeTRS(m_world[idx], wt, wr, ws);
            ImGui::Text("ワールド位置 %.4f %.4f %.4f", wt.x, wt.y, wt.z);
            ImGui::Text("ワールドスケール %.4f %.4f %.4f", ws.x, ws.y, ws.z);
            if (ImGui::SmallButton("ワールド行列をコピー")) {
                char buf[512];
                const glm::mat4& m = m_world[idx];
                snprintf(buf, sizeof(buf), "%g %g %g %g\n%g %g %g %g\n%g %g %g %g\n%g %g %g %g", m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1], m[3][1],
                         m[0][2], m[1][2], m[2][2], m[3][2], m[0][3], m[1][3], m[2][3], m[3][3]);
                ImGui::SetClipboardText(buf);
            }
        }
        ImGui::TreePop();
    }
}

void Editor::DrawInspector() {
    if (!ImGui::Begin(PanelTitle(ICON_INSPECTOR, "インスペクター", "Inspector").c_str(), &m_panels.inspector)) { ImGui::End(); return; }
    PanelHint("選んだノードの位置・回転・スケールと、付いているメッシュやスキンの詳細を表示・編集します");
    if (m_primary < 0 || m_primary >= (int)m_scene.nodes.size()) {
        ImGui::TextDisabled("何も選択されていません。\n階層かビューポートでクリックすると、\nここに詳細が表示されます");
        ImGui::End();
        return;
    }
    int idx = m_primary;
    Node& n = m_scene.nodes[idx];

    if (m_selection.size() > 1) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%zu 個のノードを選択中: トランスフォームと倍率をまとめて変更できます", m_selection.size());
        if (ImGui::Button("選択をすべてフリーズ")) for (int s : std::vector<int>(m_selection)) ApplyFreeze(s, false);
        ImGui::SameLine();
        if (ImGui::Button("すべて非表示")) { Checkpoint("非表示"); for (int s : m_selection) m_scene.nodes[s].visible = false; MarkChanged(); }
        ImGui::SameLine();
        if (ImGui::Button("すべて表示")) { Checkpoint("表示"); for (int s : m_selection) m_scene.nodes[s].visible = true; MarkChanged(); }
        ImGui::Separator();
    }

    ImGui::Text("ノード [%d]", idx);
    ImGui::SameLine();
    EllipsisText("", m_scene.NodePath(idx), true);
    ImGui::SetNextItemWidth(-60);
    EditValue("名前変更", n.name, [&] { return ImGui::InputText("名前", &n.name); });
    {
        bool vis = n.visible;
        if (ImGui::Checkbox("表示", &vis)) SetVisibility(EditTargets(), vis, vis ? "表示" : "非表示");
    }
    auto joints = m_scene.JointMask();
    if (joints[idx]) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.8f, 1, 1), "◆ ジョイント");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("親: %s", n.parent >= 0 ? m_scene.nodes[n.parent].name.c_str() : "(ルート)");

    if (SectionHeader(ICON_MOVE, "トランスフォーム (位置・回転・スケール)", ImGuiTreeNodeFlags_DefaultOpen)) DrawTransformEditor(idx);

    // 複数選択中は、全部にそろえて変えられるもの (トランスフォームと倍率) だけを出す
    if (m_selection.size() > 1) {
        if (SectionHeader(ICON_SCALE, "倍率で拡大縮小 (選択したすべて)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(Px(120));
            ImGui::InputFloat("倍##multiFactor", &m_scaleFactor, 0, 0, "%.6g");
            m_scaleFactor = std::max(m_scaleFactor, 1e-6f);
            ImGui::SameLine();
            if (ImGui::Button("この倍率にする##multi")) ScaleTargetsToSize(FitTargetsForSelection(), 1, 0.0f, nullptr, false, false, "倍率で拡大縮小", m_scaleFactor);
            for (float q : { 0.5f, 2.0f, 10.0f, 100.0f, 0.01f }) {
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "x%g##multi", q);
                if (q != 0.5f) ImGui::SameLine();
                if (ImGui::SmallButton(lbl)) ScaleTargetsToSize(FitTargetsForSelection(), 1, 0.0f, nullptr, false, false, "倍率で拡大縮小", q);
            }
            ImGui::TextDisabled("それぞれの原点を中心に拡大します");
        }
        ImGui::Spacing();
        ImGui::TextDisabled("メッシュ・スキン・アニメーションなどの詳細は、\n1 つだけ選んだときに表示します");
        ImGui::End();
        return;
    }

    if (SectionHeader(ICON_MESH, "メッシュ (形状)", ImGuiTreeNodeFlags_DefaultOpen)) {
        Node& nd = m_scene.nodes[idx];
        const char* cur = nd.mesh >= 0 && nd.mesh < (int)m_scene.meshes.size() ? m_scene.meshes[nd.mesh].name.c_str() : "(なし)";
        if (ImGui::BeginCombo("メッシュ", cur)) {
            if (ImGui::Selectable("(なし)", nd.mesh < 0)) { Checkpoint("メッシュ変更"); nd.mesh = -1; nd.skin = -1; MarkChanged(); }
            for (int i = 0; i < (int)m_scene.meshes.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(m_scene.meshes[i].name.c_str(), nd.mesh == i)) { Checkpoint("メッシュ変更"); nd.mesh = i; MarkChanged(); }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (nd.mesh >= 0 && nd.mesh < (int)m_scene.meshes.size()) {
            Mesh& mesh = m_scene.meshes[nd.mesh];
            auto users = m_scene.NodesUsingMesh(nd.mesh);
            ImGui::Text("頂点 %zu / 三角形 %zu / プリミティブ %zu", mesh.VertexCount(), mesh.TriangleCount(), mesh.primitives.size());
            if (users.size() > 1) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%zu ノードで共有 (インスタンス)", users.size());
                ImGui::SameLine();
                if (ImGui::SmallButton("専有にする")) { Checkpoint("メッシュ専有化"); Ops::MakeMeshUnique(m_scene, idx); MarkChanged(); }
            }
            if (ImGui::SmallButton("メッシュパネルで開く")) { m_selMesh = nd.mesh; m_panels.meshes = true; ImGui::SetWindowFocus("メッシュ###Meshes"); }
            ImGui::SeparatorText("マテリアル割り当て");
            for (size_t p = 0; p < mesh.primitives.size(); ++p) {
                Primitive& prim = mesh.primitives[p];
                ImGui::PushID((int)p);
                const char* mn = prim.material >= 0 && prim.material < (int)m_scene.materials.size() ? m_scene.materials[prim.material].name.c_str() : "(なし)";
                char label[64];
                snprintf(label, sizeof(label), "プリミティブ %zu", p);
                if (ImGui::BeginCombo(label, mn)) {
                    if (ImGui::Selectable("(なし)", prim.material < 0)) { Checkpoint("マテリアル割り当て"); prim.material = -1; mesh.Touch(); MarkChanged(); }
                    for (int m = 0; m < (int)m_scene.materials.size(); ++m) {
                        ImGui::PushID(m);
                        if (ImGui::Selectable(m_scene.materials[m].name.c_str(), prim.material == m)) { Checkpoint("マテリアル割り当て"); prim.material = m; mesh.Touch(); MarkChanged(); }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("編集") && prim.material >= 0) { m_selMaterial = prim.material; m_panels.materials = true; ImGui::SetWindowFocus("マテリアル###Materials"); }
                ImGui::PopID();
            }
            int morphs = mesh.MorphTargetCount();
            if (morphs > 0) {
                ImGui::SeparatorText("モーフターゲット (ブレンドシェイプ)");
                mesh.morphWeights.resize(morphs, 0.0f);
                for (int t = 0; t < morphs; ++t) {
                    std::string name = "target_" + std::to_string(t);
                    for (auto& p : mesh.primitives) if (t < (int)p.targets.size() && !p.targets[t].name.empty()) { name = p.targets[t].name; break; }
                    ImGui::PushID(t);
                    float& w = mesh.morphWeights[t];
                    EditValue("モーフウェイト", w, [&] { return ImGui::SliderFloat(name.c_str(), &w, 0.0f, 1.0f); });
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("すべて 0")) { Checkpoint("モーフリセット"); for (auto& w : mesh.morphWeights) w = 0; MarkChanged(); }
            }
        }
    }

    if (SectionHeader(ICON_SKELETON, "スキン (ボーンによる変形)")) {
        Node& nd = m_scene.nodes[idx];
        const char* cur = nd.skin >= 0 && nd.skin < (int)m_scene.skins.size() ? m_scene.skins[nd.skin].name.c_str() : "(なし)";
        if (ImGui::BeginCombo("スキン", cur)) {
            if (ImGui::Selectable("(なし)", nd.skin < 0)) { Checkpoint("スキン変更"); nd.skin = -1; MarkChanged(); }
            for (int i = 0; i < (int)m_scene.skins.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(m_scene.skins[i].name.c_str(), nd.skin == i)) { Checkpoint("スキン変更"); nd.skin = i; MarkChanged(); }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (nd.skin >= 0 && nd.skin < (int)m_scene.skins.size()) {
            const Skin& s = m_scene.skins[nd.skin];
            ImGui::Text("ジョイント数: %zu", s.joints.size());
            ImGui::Text("スケルトンルート: %s", s.skeleton >= 0 && s.skeleton < (int)m_scene.nodes.size() ? m_scene.nodes[s.skeleton].name.c_str() : "(なし)");
            ImGui::TextDisabled("※ スキンメッシュはジョイントで変形されるため、このノード自身の変換は無視されます (glTF 仕様)");
            if (ImGui::SmallButton("スケルトンパネルで開く")) { m_selSkin = nd.skin; m_panels.skeleton = true; }
        }
    }

    if (SectionHeader(ICON_ANIMATION, "アニメーション")) {
        int count = 0;
        for (size_t a = 0; a < m_scene.animations.size(); ++a)
            for (auto& c : m_scene.animations[a].channels)
                if (c.node == idx) {
                    static const char* paths[] = { "移動", "回転", "スケール" };
                    ImGui::BulletText("%s: %s (%zu キー)", m_scene.animations[a].name.c_str(), paths[(int)c.path], c.times.size());
                    ++count;
                }
        if (!count) ImGui::TextDisabled("このノードにアニメーションはありません");
    }
    ImGui::End();
}
