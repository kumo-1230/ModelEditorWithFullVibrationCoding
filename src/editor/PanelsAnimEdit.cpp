// アニメーション編集 (Maya のタイムスライダー + ドープシート相当)。
// 範囲 (イン / アウト) を決めて「切り抜き / カット / 新しいクリップへ」、ノード (行) を選んで「キーやチャンネルを消す」、全体の速度を変える。
#include "Editor.h"
#include "core/Log.h"
#include "core/ModelOps.h"
#include "core/SceneOps.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <map>

namespace {
constexpr float kFps = 30.0f;
const ImU32 kColT = IM_COL32(235, 90, 80, 255);    // 移動キー
const ImU32 kColR = IM_COL32(90, 200, 110, 255);   // 回転キー
const ImU32 kColS = IM_COL32(90, 150, 240, 255);   // スケールキー
const ImU32 kRange = IM_COL32(0, 112, 224, 45);
const ImU32 kPlayhead = IM_COL32(255, 196, 64, 255);

void Diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
}
} // namespace

void Editor::DrawAnimEditor() {
    ImGui::SetNextWindowSize(ImVec2(900, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_ANIMATION, "アニメーション編集", "AnimEditor").c_str(), &m_panels.animEditor)) { ImGui::End(); return; }
    PanelHint("Maya のドープシートのように編集します。上の目盛りで再生位置、青い範囲で切り抜き / カット、行 (ノード) を選んでキーの削除ができます");
    DrawModelFilterBar();
    const auto animMask = Membership().animations;
    if (m_anim < 0 || m_anim >= (int)m_scene.animations.size()) {
        // 編集するアニメーションを選ぶ
        int shown = 0;
        for (int i = 0; i < (int)m_scene.animations.size(); ++i) {
            if (!InMask(animMask, i)) continue;
            if (shown++ == 0) ImGui::TextDisabled("編集するアニメーションを選んでください");
            ImGui::PushID(i);
            if (ImGui::Selectable(m_scene.animations[i].name.c_str())) { m_anim = i; m_animTime = 0; }
            ImGui::PopID();
        }
        if (shown == 0)
            ImGui::TextDisabled(m_scene.animations.empty() ? "アニメーションがありません。モデルパネルの「結合」で別のモデルから移せます"
                                                           : "このモデルにはアニメーションがありません。モデルパネルの「結合」で別のモデルから移せます");
        ImGui::End();
        return;
    }
    Animation& anim = m_scene.animations[m_anim];
    if (m_rangeAnim != m_anim || m_rangeOut > anim.duration + 1e-4f || m_rangeOut <= m_rangeIn) {
        m_rangeAnim = m_anim;
        m_rangeIn = 0.0f;
        m_rangeOut = anim.duration;
    }
    auto finish = [&](const char* msg) {
        anim.duration = std::max(anim.duration, 0.0f);
        m_animTime = std::clamp(m_animTime, 0.0f, m_scene.animations[m_anim].duration);
        m_rangeAnim = -1; // 範囲を新しい長さに合わせ直す
        MarkChanged();
        LOG_INFO("%s", msg);
    };

    // ---- クリップと長さ ----
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("クリップ", anim.name.c_str())) {
        for (int i = 0; i < (int)m_scene.animations.size(); ++i) {
            if (!InMask(animMask, i)) continue;
            ImGui::PushID(i);
            if (ImGui::Selectable(m_scene.animations[i].name.c_str(), i == m_anim)) { m_anim = i; m_animTime = 0; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("長さ %.3f 秒 (%d フレーム @%g fps)   チャンネル %zu", anim.duration, (int)std::round(anim.duration * kFps), kFps, anim.channels.size());

    // ---- 範囲 ----
    ImGui::SeparatorText("範囲 (イン / アウト)");
    float inF = m_rangeIn * kFps, outF = m_rangeOut * kFps;
    ImGui::SetNextItemWidth(110);
    if (ImGui::DragFloat("開始 (フレーム)", &inF, 0.25f, 0.0f, anim.duration * kFps, "%.1f")) m_rangeIn = std::clamp(inF / kFps, 0.0f, m_rangeOut);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::DragFloat("終了 (フレーム)", &outF, 0.25f, 0.0f, anim.duration * kFps, "%.1f")) m_rangeOut = std::clamp(outF / kFps, m_rangeIn, anim.duration);
    ImGui::SameLine();
    if (ImGui::SmallButton("再生位置を開始に")) m_rangeIn = std::min(m_animTime, m_rangeOut);
    ImGui::SameLine();
    if (ImGui::SmallButton("再生位置を終了に")) m_rangeOut = std::max(m_animTime, m_rangeIn);
    ImGui::SameLine();
    if (ImGui::SmallButton("全体")) { m_rangeIn = 0; m_rangeOut = anim.duration; }
    const bool rangeOk = m_rangeOut - m_rangeIn > 1e-4f;
    const bool rangeIsAll = m_rangeIn <= 1e-4f && m_rangeOut >= anim.duration - 1e-4f;
    ImGui::BeginDisabled(!rangeOk);
    if (ImGui::Button(IconText(ICON_FRAME, "範囲だけ残す (切り抜き)").c_str())) {
        Checkpoint("アニメーションの切り抜き");
        Ops::TrimAnimation(anim, m_rangeIn, m_rangeOut);
        finish("範囲だけを残しました");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("範囲の外を捨てて、範囲の開始を 0 秒にします");
    ImGui::SameLine();
    ImGui::BeginDisabled(rangeIsAll);
    if (ImGui::Button(IconText(ICON_DELETE, "範囲を削除して詰める (カット)").c_str())) {
        Checkpoint("アニメーションのカット");
        Ops::CutAnimationRange(anim, m_rangeIn, m_rangeOut);
        finish("範囲を削除しました");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("範囲の中を消し、後ろを前に詰めます");
    ImGui::SameLine();
    if (ImGui::Button(IconText(ICON_ADD, "範囲を新しいクリップへ").c_str())) {
        Checkpoint("クリップの分割");
        char name[64];
        snprintf(name, sizeof(name), "_%d-%d", (int)std::round(m_rangeIn * kFps), (int)std::round(m_rangeOut * kFps));
        Animation part = Ops::ExtractAnimationRange(anim, m_rangeIn, m_rangeOut, anim.name + name);
        m_scene.animations.push_back(std::move(part)); // ここで anim の参照は無効になる
        m_anim = (int)m_scene.animations.size() - 1;
        m_animTime = 0;
        m_rangeAnim = -1;
        m_member.rev = 0;
        MarkChanged();
        LOG_INFO("範囲を新しいクリップ '%s' にしました", m_scene.animations[m_anim].name.c_str());
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("元のクリップはそのままで、範囲をコピーした新しいクリップを作ります (歩きの一部をループ用に切り出すなど)");
    ImGui::EndDisabled();

    // ---- 速度 ----
    ImGui::SeparatorText("速度");
    static float speed = 1.5f;
    ImGui::SetNextItemWidth(90);
    ImGui::InputFloat("倍速", &speed, 0, 0, "%.3g");
    speed = std::clamp(speed, 0.01f, 100.0f);
    ImGui::SameLine();
    if (ImGui::Button("この速さにする")) {
        Checkpoint("アニメーションの速度");
        Ops::ScaleAnimationSpeed(anim, speed);
        finish("速度を変えました");
    }
    for (float q : { 0.5f, 0.75f, 1.25f, 1.5f, 2.0f }) {
        ImGui::SameLine();
        char l[16];
        snprintf(l, sizeof(l), "x%g", q);
        if (ImGui::SmallButton(l)) {
            Checkpoint("アニメーションの速度");
            Ops::ScaleAnimationSpeed(anim, q);
            finish("速度を変えました");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(書き出しに反映。プレビューだけの速さはアニメーションパネル)");

    // ---- ドープシート ----
    ImGui::SeparatorText("ドープシート (行 = ノード、◆ = キー: 赤 移動 / 緑 回転 / 青 スケール)");
    // ノードごとのキー
    struct Row { int node; int mask = 0; std::vector<std::pair<float, int>> keys; };
    std::map<int, Row> rowsByNode;
    for (const auto& ch : anim.channels) {
        if (ch.node < 0 || ch.node >= (int)m_scene.nodes.size()) continue;
        Row& r = rowsByNode[ch.node];
        r.node = ch.node;
        r.mask |= 1 << (int)ch.path;
        for (float t : ch.times) r.keys.push_back({ t, (int)ch.path });
    }
    std::vector<Row> rows;
    std::string filter = m_dopeFilter;
    for (auto& c : filter) c = (char)std::tolower((unsigned char)c);
    for (auto& [n, r] : rowsByNode) {
        if (!filter.empty()) {
            std::string nm = m_scene.nodes[n].name;
            for (auto& c : nm) c = (char)std::tolower((unsigned char)c);
            if (nm.find(filter) == std::string::npos) continue;
        }
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        int da = 0, db = 0;
        for (int p = m_scene.nodes[a.node].parent; p >= 0; p = m_scene.nodes[p].parent) ++da;
        for (int p = m_scene.nodes[b.node].parent; p >= 0; p = m_scene.nodes[p].parent) ++db;
        return da != db ? da < db : a.node < b.node;
    });
    m_dopeSel.erase(std::remove_if(m_dopeSel.begin(), m_dopeSel.end(), [&](int n) { return !rowsByNode.count(n); }), m_dopeSel.end());
    auto isSel = [&](int n) { return std::find(m_dopeSel.begin(), m_dopeSel.end(), n) != m_dopeSel.end(); };

    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##dopeFilter", "ノード名で絞り込み", &m_dopeFilter);
    ImGui::SameLine();
    if (ImGui::SmallButton("全部選ぶ")) { m_dopeSel.clear(); for (auto& r : rows) m_dopeSel.push_back(r.node); }
    ImGui::SameLine();
    if (ImGui::SmallButton("選択を外す")) m_dopeSel.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("シーンの選択から")) {
        m_dopeSel.clear();
        for (int s : m_selection) if (rowsByNode.count(s)) m_dopeSel.push_back(s);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("選択中 %zu / %zu ノード", m_dopeSel.size(), rows.size());

    static bool delT = true, delR = true, delS = true;
    ImGui::BeginDisabled(m_dopeSel.empty());
    if (ImGui::Button(IconText(ICON_DELETE, "選んだノードのアニメーションを消す").c_str())) {
        Checkpoint("ノードのアニメーションを削除");
        int mask = (delT ? 1 : 0) | (delR ? 2 : 0) | (delS ? 4 : 0);
        int n = Ops::RemoveChannelsForNodes(anim, m_dopeSel, mask);
        m_dopeSel.clear();
        char msg[128];
        snprintf(msg, sizeof(msg), "%d 個のチャンネルを削除しました", n);
        finish(msg);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("選んだノードの動きをこのクリップから無くします (例: 不要なルートモーション、指や小物の動き)");
    ImGui::SameLine();
    ImGui::Checkbox("移動", &delT);
    ImGui::SameLine();
    ImGui::Checkbox("回転", &delR);
    ImGui::SameLine();
    ImGui::Checkbox("スケール", &delS);
    ImGui::SameLine();
    ImGui::BeginDisabled(!rangeOk);
    if (ImGui::Button("範囲のキーを消す")) {
        Checkpoint("範囲のキーを削除");
        int n = Ops::DeleteKeysInRange(anim, m_dopeSel, m_rangeIn, m_rangeOut);
        char msg[128];
        snprintf(msg, sizeof(msg), "%d 個のキーを削除しました (範囲の両端はつながります)", n);
        finish(msg);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::DragFloat("##tol", &m_reduceTol, 0.0001f, 0.0f, 0.1f, "%.4f");
    ImGui::SameLine();
    if (ImGui::Button("キーを間引く")) {
        Checkpoint("キーの間引き");
        int n = Ops::ReduceKeys(anim, m_reduceTol);
        char msg[128];
        snprintf(msg, sizeof(msg), "変化の無いキーを %d 個間引きました (許容 %.4f)", n, m_reduceTol);
        finish(msg);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("前後のキーの補間で再現できるキーを消して軽くします (数値は許容する誤差: m / ラジアン)");

    // 目盛り + 行
    const float dur = std::max(anim.duration, 1e-4f);
    const float nameW = 200.0f;
    if (ImGui::BeginTable("##dope", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ノード", ImGuiTableColumnFlags_WidthFixed, nameW);
        ImGui::TableSetupColumn("キー", ImGuiTableColumnFlags_WidthStretch);
        // 目盛り (クリック / ドラッグで再生位置、Shift+ドラッグで範囲)
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("フレーム %d", (int)std::round(m_animTime * kFps));
        ImGui::TableNextColumn();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = std::max(ImGui::GetContentRegionAvail().x, 10.0f);
        float h = ImGui::GetFrameHeight();
        auto toX = [&](float t) { return p0.x + (t / dur) * (w - 8) + 4; };
        auto toT = [&](float x) { return std::clamp((x - p0.x - 4) / (w - 8) * dur, 0.0f, dur); };
        ImGui::InvisibleButton("##ruler", ImVec2(w, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(28, 28, 28, 255));
        dl->AddRectFilled(ImVec2(toX(m_rangeIn), p0.y), ImVec2(toX(m_rangeOut), p0.y + h), IM_COL32(0, 112, 224, 90));
        int frames = (int)std::round(dur * kFps);
        int step = std::max(1, (int)std::ceil(frames / std::max(1.0f, w / 50.0f)));
        for (int f = 0; f <= frames; f += step) {
            float x = toX(f / kFps);
            dl->AddLine(ImVec2(x, p0.y + h * 0.55f), ImVec2(x, p0.y + h), IM_COL32(120, 120, 120, 255));
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%d", f);
            dl->AddText(ImVec2(x + 2, p0.y), IM_COL32(170, 170, 170, 255), lbl);
        }
        static bool rangeDrag = false;
        static float rangeAnchor = 0.0f;
        if (ImGui::IsItemActivated()) {
            rangeDrag = ImGui::GetIO().KeyShift;
            rangeAnchor = toT(ImGui::GetIO().MousePos.x);
        }
        if (ImGui::IsItemActive()) {
            float t = toT(ImGui::GetIO().MousePos.x);
            if (rangeDrag) { m_rangeIn = std::min(rangeAnchor, t); m_rangeOut = std::max(rangeAnchor, t); }
            else { m_animTime = t; m_playing = false; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("クリック / ドラッグ: 再生位置\nShift + ドラッグ: 範囲を選ぶ");
        float px = toX(m_animTime);
        dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p0.y + h), kPlayhead, 2.0f);

        ImGuiListClipper clip;
        clip.Begin((int)rows.size());
        while (clip.Step()) {
            for (int ri = clip.DisplayStart; ri < clip.DisplayEnd; ++ri) {
                const Row& r = rows[ri];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(r.node);
                int depth = 0;
                for (int p = m_scene.nodes[r.node].parent; p >= 0; p = m_scene.nodes[p].parent) ++depth;
                std::string label = std::string(std::min(depth, 12), ' ') + m_scene.nodes[r.node].name;
                bool sel = isSel(r.node);
                if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    if (ImGui::GetIO().KeyCtrl) {
                        if (sel) m_dopeSel.erase(std::find(m_dopeSel.begin(), m_dopeSel.end(), r.node));
                        else m_dopeSel.push_back(r.node);
                    } else if (ImGui::GetIO().KeyShift && !m_dopeSel.empty()) {
                        // 最後に選んだ行からこの行までをまとめて選ぶ
                        int last = m_dopeSel.back(), a = -1, b = ri;
                        for (int k = 0; k < (int)rows.size(); ++k) if (rows[k].node == last) a = k;
                        if (a >= 0) for (int k = std::min(a, b); k <= std::max(a, b); ++k) if (!isSel(rows[k].node)) m_dopeSel.push_back(rows[k].node);
                    } else {
                        m_dopeSel = { r.node };
                    }
                    Select(r.node); // シーンでも選ぶ (ビューポートで確認できる)
                }
                if (ImGui::IsItemHovered()) {
                    std::string kinds = std::string((r.mask & 1) ? "移動 " : "") + ((r.mask & 2) ? "回転 " : "") + ((r.mask & 4) ? "スケール" : "");
                    ImGui::SetTooltip("%s\nキー %zu 個 (%s)\nクリック: 選ぶ / Ctrl: 追加・解除 / Shift: 範囲で選ぶ", m_scene.nodes[r.node].name.c_str(), r.keys.size(),
                                      kinds.c_str());
                }
                ImGui::TableNextColumn();
                ImVec2 q0 = ImGui::GetCursorScreenPos();
                float rh = ImGui::GetTextLineHeight();
                ImGui::Dummy(ImVec2(w, rh));
                dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(ImVec2(toX(m_rangeIn), q0.y), ImVec2(toX(m_rangeOut), q0.y + rh), kRange);
                const float cy = q0.y + rh * 0.5f, rad = std::max(2.5f, rh * 0.22f);
                for (auto& [t, path] : r.keys) {
                    float x = toX(t);
                    float off = (path - 1) * rad * 0.9f; // 同じ時刻の T/R/S を少しずらす
                    Diamond(dl, ImVec2(x + off, cy), rad, path == 0 ? kColT : path == 1 ? kColR : kColS);
                }
                dl->AddLine(ImVec2(px, q0.y), ImVec2(px, q0.y + rh), kPlayhead, 1.0f);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
