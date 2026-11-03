// スケルトン (ボーン一覧) / アニメーション
#include "Editor.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>

// ---------------------------------------------------------------------------
// スケルトン
// ---------------------------------------------------------------------------
void Editor::DrawBoneTree(int node, const std::vector<bool>& joints, const Skin& skin) {
    const Node& n = m_scene.nodes[node];
    // ジョイントでない子孫もたどり、ジョイントだけを表示する
    bool isJoint = joints[node];
    if (!isJoint) {
        for (int c : n.children) DrawBoneTree(c, joints, skin);
        return;
    }
    bool hasJointChild = false;
    std::function<bool(int)> anyJoint = [&](int i) {
        for (int c : m_scene.nodes[i].children) if (joints[c] || anyJoint(c)) return true;
        return false;
    };
    hasJointChild = anyJoint(node);
    ImGui::PushID(node);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (!hasJointChild) flags |= ImGuiTreeNodeFlags_Leaf;
    if (m_selBone == node) flags |= ImGuiTreeNodeFlags_Selected;
    int jointIndex = -1;
    for (size_t j = 0; j < skin.joints.size(); ++j) if (skin.joints[j] == node) { jointIndex = (int)j; break; }
    char label[256];
    if (jointIndex >= 0) snprintf(label, sizeof(label), "◆ %s  [%d]", n.name.c_str(), jointIndex);
    else snprintf(label, sizeof(label), "◇ %s  (別スキン)", n.name.c_str());
    bool open = ImGui::TreeNodeEx("##bone", flags, "%s", label);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_selBone = node;
        Select(node);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) FocusNode(node);
    if (open) {
        for (int c : n.children) DrawBoneTree(c, joints, skin);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Editor::DrawSkeleton() {
    if (!ImGui::Begin(PanelTitle(ICON_SKELETON, "スケルトン", "Skeleton").c_str(), &m_panels.skeleton)) { ImGui::End(); return; }
    PanelHint("ボーン (骨格) とスキンの一覧です。ボーンを選ぶとビューポートで強調表示されます");
    if (m_scene.skins.empty()) {
        ImGui::TextDisabled("スキン (ボーン) はありません");
        auto joints = m_scene.JointMask();
        (void)joints;
        ImGui::End();
        return;
    }
    m_selSkin = std::clamp(m_selSkin, 0, (int)m_scene.skins.size() - 1);
    if (ImGui::BeginCombo("スキン", m_scene.skins[m_selSkin].name.c_str())) {
        for (int i = 0; i < (int)m_scene.skins.size(); ++i) {
            ImGui::PushID(i);
            char l[256];
            snprintf(l, sizeof(l), "%s (%zu ジョイント)", m_scene.skins[i].name.c_str(), m_scene.skins[i].joints.size());
            if (ImGui::Selectable(l, i == m_selSkin)) m_selSkin = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    Skin& skin = m_scene.skins[m_selSkin];
    EditValue("スキン名", skin.name, [&] { return ImGui::InputText("名前##skin", &skin.name); });

    ImGui::Checkbox("ボーン表示", &m_rs.showBones);
    ImGui::SameLine();
    ImGui::Checkbox("最前面", &m_rs.bonesOnTop);
    ImGui::SameLine();
    ImGui::Checkbox("名前", &m_showBoneNames);
    ImGui::SetNextItemWidth(Px(120));
    ImGui::SliderFloat("サイズ", &m_rs.boneSize, 0.1f, 5.0f);
    ImGui::SameLine();
    bool weightMode = m_rs.mode == RM_Weights;
    if (ImGui::Checkbox("ウェイト表示", &weightMode)) m_rs.mode = weightMode ? RM_Weights : RM_Lit;
    if (ImGui::Button("名前空間を除去")) { Checkpoint("名前空間除去"); LOG_INFO("%d 個修正", Ops::StripNamespaces(m_scene)); MarkChanged(); }
    ImGui::SameLine();
    if (ImGui::Button("一括リネーム...")) m_panels.batchRename = true;
    ImGui::SameLine();
    if (ImGui::Button("バインドポーズ")) { m_anim = -1; m_playing = false; m_animTime = 0; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("アニメーションを止めてレストポーズ (バインドポーズ) を表示");

    // ジョイントツリー
    float treeH = ImGui::GetContentRegionAvail().y * 0.55f;
    ImGui::BeginChild("bones", ImVec2(0, treeH), ImGuiChildFlags_Borders);
    auto joints = m_scene.JointMask();
    std::vector<bool> inSkin(m_scene.nodes.size(), false);
    for (int j : skin.joints) if (j >= 0 && j < (int)inSkin.size()) inSkin[j] = true;
    // スキンのジョイントのうち、親がスキンに含まれないものをルートとする
    for (int j : skin.joints) {
        if (j < 0 || j >= (int)m_scene.nodes.size()) continue;
        int p = m_scene.nodes[j].parent;
        bool parentJoint = false;
        while (p >= 0) {
            if (inSkin[p]) { parentJoint = true; break; }
            p = m_scene.nodes[p].parent;
        }
        if (!parentJoint) DrawBoneTree(j, inSkin, skin);
    }
    ImGui::EndChild();

    // 選択ボーンの詳細
    if (m_selBone >= 0 && m_selBone < (int)m_scene.nodes.size()) {
        int jointIndex = -1;
        for (size_t j = 0; j < skin.joints.size(); ++j) if (skin.joints[j] == m_selBone) { jointIndex = (int)j; break; }
        ImGui::Text("ボーン: %s", m_scene.nodes[m_selBone].name.c_str());
        if (jointIndex >= 0) {
            // 影響する頂点数
            size_t influenced = 0;
            float maxW = 0;
            for (int ni = 0; ni < (int)m_scene.nodes.size(); ++ni) {
                const Node& nd = m_scene.nodes[ni];
                if (nd.skin != m_selSkin || nd.mesh < 0) continue;
                for (auto& p : m_scene.meshes[nd.mesh].primitives) {
                    if (!p.HasSkin()) continue;
                    for (size_t v = 0; v < p.joints.size(); ++v)
                        for (int k = 0; k < 4; ++k)
                            if (p.joints[v][k] == jointIndex && p.weights[v][k] > 0) { ++influenced; maxW = std::max(maxW, p.weights[v][k]); }
                }
            }
            ImGui::Text("ジョイント番号 %d / 影響頂点 %zu / 最大ウェイト %.3f", jointIndex, influenced, maxW);
            if (jointIndex < (int)skin.inverseBind.size() && ImGui::TreeNode("逆バインド行列")) {
                const glm::mat4& m = skin.inverseBind[jointIndex];
                for (int r = 0; r < 4; ++r) ImGui::Text("%10.4f %10.4f %10.4f %10.4f", m[0][r], m[1][r], m[2][r], m[3][r]);
                // バインド時のワールド位置 = inverse(IBM) の平行移動
                glm::mat4 bind = glm::inverse(m);
                ImGui::TextDisabled("バインド位置 %.4f %.4f %.4f", bind[3][0], bind[3][1], bind[3][2]);
                if (m_selBone < (int)m_world.size()) {
                    glm::vec3 cur(m_world[m_selBone][3]);
                    ImGui::TextDisabled("現在位置   %.4f %.4f %.4f", cur.x, cur.y, cur.z);
                }
                ImGui::TreePop();
            }
        } else {
            ImGui::TextDisabled("このスキンのジョイントではありません");
        }
    }
    ImGui::Separator();
    if (ImGui::Button("このスキンのウェイトを正規化")) {
        Checkpoint("ウェイト正規化");
        int n = 0;
        for (auto& nd : m_scene.nodes)
            if (nd.skin == m_selSkin && nd.mesh >= 0) {
                for (auto& p : m_scene.meshes[nd.mesh].primitives) if (p.HasSkin()) n += Ops::NormalizeWeights(p);
                m_scene.meshes[nd.mesh].Touch();
            }
        LOG_INFO("%d 頂点を正規化しました", n);
        MarkChanged();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// アニメーション
// ---------------------------------------------------------------------------
void Editor::DrawAnimation() {
    if (!ImGui::Begin(PanelTitle(ICON_ANIMATION, "アニメーション", "Animation").c_str(), &m_panels.animation)) { ImGui::End(); return; }
    PanelHint("モーションの一覧と再生です。書き出しに反映される速度の変更もここで行います");
    DrawModelFilterBar();
    if (m_scene.animations.empty()) {
        ImGui::TextDisabled("アニメーションはありません");
        ImGui::TextDisabled("ファイル > アニメーションのみ取り込み で別ファイルのモーションを追加できます");
        ImGui::End();
        return;
    }
    // 左: クリップ一覧, 右: 再生コントロール
    ImGui::BeginChild("clips", ImVec2(Px(220), 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    if (ImGui::Selectable("(レストポーズ)", m_anim < 0)) { m_anim = -1; m_playing = false; }
    const auto animMask = Membership().animations;
    for (int i = 0; i < (int)m_scene.animations.size(); ++i) {
        if (!InMask(animMask, i)) continue;
        ImGui::PushID(i);
        char l[256];
        snprintf(l, sizeof(l), "%s  (%.2fs)", m_scene.animations[i].name.c_str(), m_scene.animations[i].duration);
        if (ImGui::Selectable(l, m_anim == i)) { m_anim = i; m_animTime = 0; }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { m_anim = i; m_playing = true; }
        if (ImGui::BeginPopupContextItem("actx")) {
            if (ImGui::MenuItem("複製")) { Checkpoint("アニメーション複製"); Animation a = m_scene.animations[i]; a.name += "_copy"; m_scene.animations.push_back(a); MarkChanged(); }
            if (ImGui::MenuItem("削除")) {
                Checkpoint("アニメーション削除");
                m_scene.RemoveAnimation(i);
                ValidateSelection();
                MarkChanged();
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("controls");
    if (m_anim >= 0 && m_anim < (int)m_scene.animations.size()) {
        Animation& a = m_scene.animations[m_anim];
        ImGui::SetNextItemWidth(Px(240));
        EditValue("アニメーション名", a.name, [&] { return ImGui::InputText("名前##anim", &a.name); });
        ImGui::SameLine();
        ImGui::TextDisabled("チャンネル %zu / 長さ %.3f 秒", a.channels.size(), a.duration);

        if (ImGui::Button(m_playing ? "一時停止" : " 再生 ")) m_playing = !m_playing;
        ImGui::SameLine();
        if (ImGui::Button("停止")) { m_playing = false; m_animTime = 0; }
        ImGui::SameLine();
        if (ImGui::Button("|<")) m_animTime = 0;
        ImGui::SameLine();
        if (ImGui::Button("<")) m_animTime = std::max(0.0f, m_animTime - 1.0f / 30.0f);
        ImGui::SameLine();
        if (ImGui::Button(">")) m_animTime = std::min(a.duration, m_animTime + 1.0f / 30.0f);
        ImGui::SameLine();
        if (ImGui::Button(">|")) m_animTime = a.duration;
        ImGui::SameLine();
        ImGui::Checkbox("ループ", &m_loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(120));
        ImGui::SliderFloat("プレビュー速度", &m_animSpeed, -2.0f, 4.0f, "%.2fx");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("再生の確認用です (データは変わらず、書き出しにも影響しません)。\n書き出す速度を変えるには下の「アニメーション速度」を使います");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##time", &m_animTime, 0.0f, std::max(a.duration, 1e-3f), "%.3f 秒");
        ImGui::TextDisabled("フレーム (30fps): %d / %d", (int)(m_animTime * 30.0f + 0.5f), (int)(a.duration * 30.0f + 0.5f));

        // アニメーションの速度 (キーの時刻を変える。書き出しにも反映)
        ImGui::SeparatorText("アニメーション速度 (書き出しに反映)");
        static float speed = 1.5f;
        static float targetLen = 1.0f;
        static bool allAnims = false;
        auto applySpeed = [&](float s, const char* label) {
            if (!(s > 0.0f) || std::abs(s - 1.0f) < 1e-6f) return;
            Checkpoint(label);
            if (allAnims) for (auto& an : m_scene.animations) Ops::ScaleAnimationSpeed(an, s);
            else Ops::ScaleAnimationSpeed(a, s);
            m_animTime /= s; // 同じ動きの位置を表示し続ける
            LOG_INFO("%s のアニメーション速度を x%.4g にしました (長さ %.3f 秒)", allAnims ? "全" : ("'" + a.name + "'").c_str(), s, a.duration);
            MarkChanged();
        };
        ImGui::SetNextItemWidth(Px(100));
        ImGui::InputFloat("倍速##speed", &speed, 0, 0, "%.4g");
        speed = std::max(speed, 1e-3f);
        ImGui::SameLine();
        if (ImGui::Button("この速さにする")) applySpeed(speed, "アニメーション速度");
        ImGui::SameLine();
        ImGui::TextDisabled("→ 長さ %.3f 秒", a.duration / speed);
        for (float q : { 0.25f, 0.5f, 0.75f, 1.25f, 1.5f, 2.0f }) {
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "x%g", q);
            if (q != 0.25f) ImGui::SameLine();
            if (ImGui::SmallButton(lbl)) applySpeed(q, "アニメーション速度");
        }
        ImGui::SetNextItemWidth(Px(100));
        ImGui::InputFloat("秒##len", &targetLen, 0, 0, "%.4g");
        targetLen = std::max(targetLen, 1e-3f);
        ImGui::SameLine();
        if (ImGui::Button("この長さにする") && a.duration > 1e-6f) applySpeed(a.duration / targetLen, "アニメーションの長さ");
        ImGui::SameLine();
        if (ImGui::SmallButton("今の長さ")) targetLen = a.duration;
        ImGui::Checkbox("全アニメーションに適用", &allAnims);
        ImGui::TextDisabled("x2 で 2 倍速 (長さは半分)。ルートモーションの移動距離は変わらず、速さだけが変わります");

        if (ImGui::TreeNode("ツール")) {
            if (ImGui::Button("ルートの移動キーを削除 (インプレース化)")) {
                Checkpoint("インプレース化");
                int removed = 0;
                for (auto& c : a.channels) {
                    if (c.path != AnimPath::Translation || c.node < 0) continue;
                    // スキンのスケルトンルート、またはジョイントの最上位を対象
                    bool isRootJoint = false;
                    for (auto& s : m_scene.skins) {
                        for (int j : s.joints) if (j == c.node) {
                            int p = m_scene.nodes[j].parent;
                            bool parentIsJoint = false;
                            for (int j2 : s.joints) if (j2 == p) parentIsJoint = true;
                            if (!parentIsJoint) isRootJoint = true;
                        }
                    }
                    if (!isRootJoint) continue;
                    glm::vec4 first = c.values.empty() ? glm::vec4(0) : c.values.front();
                    for (auto& v : c.values) { v.x = first.x; v.z = first.z; }
                    for (auto* tans : { &c.inTangents, &c.outTangents }) for (auto& d : *tans) { d.x = 0; d.z = 0; } // 曲線の水平方向の傾きも 0
                    ++removed;
                }
                LOG_INFO("%d チャンネルの水平移動を固定しました", removed);
                MarkChanged();
            }
            if (ImGui::Button("キーの重複を削除 (最適化)")) {
                Checkpoint("キー最適化");
                size_t before = 0, after = 0;
                for (auto& c : a.channels) {
                    before += c.times.size();
                    if (c.times.size() < 3 || c.interp == Interp::CubicSpline) { after += c.times.size(); continue; } // 曲線は接線があるのでそのまま
                    std::vector<float> t2{ c.times[0] };
                    std::vector<glm::vec4> v2{ c.values[0] };
                    for (size_t k = 1; k + 1 < c.times.size(); ++k) {
                        // 前後と同じ値なら省略
                        if (glm::length(c.values[k] - v2.back()) < 1e-6f && glm::length(c.values[k + 1] - c.values[k]) < 1e-6f) continue;
                        t2.push_back(c.times[k]);
                        v2.push_back(c.values[k]);
                    }
                    t2.push_back(c.times.back());
                    v2.push_back(c.values.back());
                    c.times = std::move(t2);
                    c.values = std::move(v2);
                    after += c.times.size();
                }
                LOG_INFO("キー数 %zu → %zu", before, after);
                MarkChanged();
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("チャンネル一覧")) {
            if (ImGui::BeginTable("chs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, 200))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("ノード");
                ImGui::TableSetupColumn("種類", ImGuiTableColumnFlags_WidthFixed, Px(70));
                ImGui::TableSetupColumn("キー", ImGuiTableColumnFlags_WidthFixed, Px(50));
                ImGui::TableSetupColumn("範囲", ImGuiTableColumnFlags_WidthFixed, Px(110));
                ImGui::TableHeadersRow();
                static const char* paths[] = { "移動", "回転", "スケール" };
                ImGuiListClipper clip;
                clip.Begin((int)a.channels.size());
                while (clip.Step())
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                        const AnimChannel& c = a.channels[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(i);
                        const char* nm = c.node >= 0 && c.node < (int)m_scene.nodes.size() ? m_scene.nodes[c.node].name.c_str() : "?";
                        if (ImGui::Selectable(nm, IsSelected(c.node), ImGuiSelectableFlags_SpanAllColumns)) Select(c.node);
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s%s", paths[(int)c.path], c.interp == Interp::Step ? " (Step)" : "");
                        ImGui::TableNextColumn();
                        ImGui::Text("%zu", c.times.size());
                        ImGui::TableNextColumn();
                        if (!c.times.empty()) ImGui::Text("%.2f-%.2f", c.times.front(), c.times.back());
                    }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::TextDisabled("クリップを選択してください (レストポーズ表示中)");
    }
    ImGui::EndChild();
    ImGui::End();
}
