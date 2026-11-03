// カーブエディタ (Maya のグラフエディタ相当)。
// 左: アニメーションするノードとチャンネル (移動 / 回転 / スケールの X Y Z) の一覧。選んだもののカーブを右に出す
// 右: 横 = 時間 (フレーム)、縦 = 値。回転はオイラー角 (度) で表示・編集し、内部の四元数に戻す
// 操作 (Maya と同じ): ホイール / Alt+右ドラッグ = ズーム、Alt+中ドラッグ = パン、左クリック / ドラッグ = キーの選択、
//   中ドラッグ (または選んだキーを左ドラッグ) = キーを動かす、接線のハンドルをドラッグ = 傾き、F = 選択を表示、A = 全体を表示、
//   S = 今の時刻にキーを打つ、Delete = キーを削除。Shift で縦横どちらかに固定
#include "Editor.h"
#include "core/Log.h"
#include "core/ModelOps.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace {
constexpr float kFps = 30.0f;
const ImU32 kAxisCol[3] = { IM_COL32(235, 85, 85, 255), IM_COL32(95, 205, 95, 255), IM_COL32(95, 145, 245, 255) };
const char* kPathName[3] = { "移動", "回転", "スケール" };
const char* kAxisName[3] = { "X", "Y", "Z" };

glm::vec3 QuatToEulerDeg(const glm::vec4& v) {
    glm::quat q(v.w, v.x, v.y, v.z);
    return glm::degrees(glm::eulerAngles(glm::normalize(q)));
}
glm::vec4 EulerDegToQuat(const glm::vec3& e) {
    glm::quat q(glm::radians(e));
    return glm::vec4(q.x, q.y, q.z, q.w);
}
// 前の値に近い角度にそろえる (-180 ↔ 180 の飛びをなくす)
glm::vec3 Unwrap(glm::vec3 e, const glm::vec3& prev) {
    for (int c = 0; c < 3; ++c) e[c] += 360.0f * std::round((prev[c] - e[c]) / 360.0f);
    return e;
}
float NiceStep(float range, float pixels, float minPx) {
    float raw = range / std::max(1.0f, pixels / minPx);
    float p = std::pow(10.0f, std::floor(std::log10(std::max(raw, 1e-9f))));
    for (float m : { 1.0f, 2.0f, 5.0f, 10.0f })
        if (p * m >= raw) return p * m;
    return p * 10.0f;
}
} // namespace

// 1 本のカーブ = (チャンネル, 成分)。回転はキーごとのオイラー角を連続になるようにそろえて持つ
struct Editor::CurveView {
    int channel = -1, comp = 0;
    AnimPath path = AnimPath::Translation;
    std::vector<glm::vec3> euler; // 回転のときのキーごとのオイラー角 (度)
    float normMin = 0, normMax = 1; // 正規化表示用
};

float Editor::CurveKeyValue(const Animation& a, const CurveView& c, int k) const {
    const AnimChannel& ch = a.channels[c.channel];
    if (c.path == AnimPath::Rotation) return k < (int)c.euler.size() ? c.euler[k][c.comp] : 0.0f;
    return ch.values[k][c.comp];
}

void Editor::DrawCurveEditor() {
    ImGui::SetNextWindowSize(ImVec2(Px(900), Px(460)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_STATS, "カーブエディタ", "CurveEditor").c_str(), &m_panels.curveEditor)) { ImGui::End(); return; }
    PanelHint("Maya のグラフエディタと同じ操作でキーと曲線を編集します。左でノードやチャンネルを選び、右でキーをドラッグします (ホイール: ズーム / Alt+中ドラッグ: 移動 / F・A: 表示を合わせる)");
    DrawModelFilterBar();
    const auto animMask = Membership().animations;
    if (m_anim < 0 || m_anim >= (int)m_scene.animations.size() || !InMask(animMask, m_anim)) {
        ImGui::TextDisabled("アニメーションを選んでください (上のツールバーかアニメーションパネル)");
        ImGui::End();
        return;
    }
    Animation& anim = m_scene.animations[m_anim];
    if (m_ceAnim != m_anim) { m_ceAnim = m_anim; m_ceSel.clear(); m_ceFrameAll = true; }
    m_ceSel.erase(std::remove_if(m_ceSel.begin(), m_ceSel.end(),
                                 [&](const CurveKey& k) { return k.channel >= (int)anim.channels.size() || k.key >= (int)anim.channels[k.channel].times.size(); }),
                  m_ceSel.end());

    // ---- ツールバー ----
    auto isSel = [&](int ch, int comp, int key) {
        for (auto& s : m_ceSel) if (s.channel == ch && s.comp == comp && s.key == key) return true;
        return false;
    };
    auto selectedKeysOf = [&](int ch) {
        std::set<int> ks;
        for (auto& s : m_ceSel) if (s.channel == ch) ks.insert(s.key);
        return std::vector<int>(ks.begin(), ks.end());
    };
    auto applyTangent = [&](Ops::TangentMode mode, const char* label) {
        if (m_ceSel.empty()) return;
        Checkpoint(label);
        std::set<int> chs;
        for (auto& s : m_ceSel) chs.insert(s.channel);
        for (int c : chs) Ops::SetKeyTangents(anim.channels[c], selectedKeysOf(c), mode);
        MarkChanged();
    };
    ImGui::BeginDisabled(m_ceSel.empty());
    if (ImGui::Button("スプライン")) applyTangent(Ops::TangentMode::Spline, "接線: スプライン");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("選んだキーを滑らかにつなぐ (Maya のスプライン)");
    SameLineIfFits("クランプ");
    if (ImGui::Button("クランプ")) applyTangent(Ops::TangentMode::Clamped, "接線: クランプ");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("滑らかだが、同じ値が続くところでは行き過ぎない");
    SameLineIfFits("リニア");
    if (ImGui::Button("リニア")) applyTangent(Ops::TangentMode::Linear, "接線: リニア");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("キーの間を直線でつなぐ");
    SameLineIfFits("フラット");
    if (ImGui::Button("フラット")) applyTangent(Ops::TangentMode::Flat, "接線: フラット");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("キーの位置で傾きを 0 にする (止まる・ため)");
    SameLineIfFits("ステップ");
    if (ImGui::Button("ステップ")) applyTangent(Ops::TangentMode::Step, "接線: ステップ");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("次のキーまで値を変えない (チャンネル全体に効きます)");
    ImGui::EndDisabled();
    SameLineIfFits("キーを打つ (S)");
    bool insertReq = ImGui::Button("キーを打つ (S)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("表示中のカーブに、今の時刻でキーを打ちます (形は変わりません)");
    SameLineIfFits("削除 (Del)");
    ImGui::BeginDisabled(m_ceSel.empty());
    bool deleteReq = ImGui::Button("削除 (Del)");
    ImGui::EndDisabled();
    SameLineIfFits("全体 (A)");
    if (ImGui::Button("全体 (A)")) m_ceFrameAll = true;
    SameLineIfFits("選択 (F)");
    if (ImGui::Button("選択 (F)")) m_ceFrameSel = true;
    SameLineIfFits("正規化");
    ImGui::Checkbox("正規化", &m_ceNormalize);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("値の大きさが違うカーブ (回転の度と移動の m など) を同じ高さに並べて表示します");
    SameLineIfFits("フレームにスナップ");
    ImGui::Checkbox("フレームにスナップ", &m_ceSnap);

    // ---- 左: ノードとチャンネルの一覧 ----
    const float listW = Px(210);
    ImGui::BeginChild("##ceList", ImVec2(listW, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ceFilter", "ノード名で絞り込み", &m_ceFilter);
    std::map<int, std::vector<int>> chansByNode; // ノード → チャンネル番号
    for (int i = 0; i < (int)anim.channels.size(); ++i)
        if (anim.channels[i].node >= 0 && anim.channels[i].node < (int)m_scene.nodes.size()) chansByNode[anim.channels[i].node].push_back(i);
    std::string filter = m_ceFilter;
    for (auto& c : filter) c = (char)std::tolower((unsigned char)c);
    auto curveId = [](int ch, int comp) { return (int64_t)ch * 4 + comp; };
    const bool ctrl = ImGui::GetIO().KeyCtrl;
    for (auto& [node, chans] : chansByNode) {
        std::string nm = m_scene.nodes[node].name, lnm = nm;
        for (auto& c : lnm) c = (char)std::tolower((unsigned char)c);
        if (!filter.empty() && lnm.find(filter) == std::string::npos) continue;
        ImGui::PushID(node);
        bool anyShown = false;
        for (int c : chans) for (int k = 0; k < 3; ++k) if (m_ceCurves.count(curveId(c, k))) anyShown = true;
        bool open = ImGui::TreeNodeEx("##n", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | (anyShown ? ImGuiTreeNodeFlags_Selected : 0), "%s", nm.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            // ノードを選ぶと、そのノードの全チャンネルのカーブを出す (Ctrl で追加)
            if (!ctrl) m_ceCurves.clear();
            for (int c : chans) for (int k = 0; k < 3; ++k) m_ceCurves.insert(curveId(c, k));
            Select(node);
            m_ceFrameAll = true;
        }
        if (open) {
            for (int c : chans) {
                const AnimChannel& ch = anim.channels[c];
                for (int k = 0; k < 3; ++k) {
                    ImGui::PushID(c * 4 + k);
                    bool shown = m_ceCurves.count(curveId(c, k)) > 0;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kAxisCol[k]));
                    char lbl[64];
                    snprintf(lbl, sizeof(lbl), "%s %s%s", kPathName[(int)ch.path], kAxisName[k], ch.interp == Interp::CubicSpline ? "  (曲線)" : ch.interp == Interp::Step ? "  (ステップ)" : "");
                    if (ImGui::Selectable(lbl, shown)) {
                        if (!ctrl) m_ceCurves.clear();
                        if (shown && ctrl) m_ceCurves.erase(curveId(c, k));
                        else m_ceCurves.insert(curveId(c, k));
                        m_ceFrameAll = true;
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // 表示するカーブ (一覧で何も選んでいなければ、シーンで選んでいるノードのもの)
    std::vector<CurveView> curves;
    {
        std::set<int64_t> ids = m_ceCurves;
        if (ids.empty())
            for (int s : m_selection) {
                auto it = chansByNode.find(s);
                if (it != chansByNode.end()) for (int c : it->second) for (int k = 0; k < 3; ++k) ids.insert(curveId(c, k));
            }
        for (int64_t id : ids) {
            int c = (int)(id / 4), k = (int)(id % 4);
            if (c < 0 || c >= (int)anim.channels.size() || k > 2) continue;
            const AnimChannel& ch = anim.channels[c];
            if (ch.times.empty() || ch.values.size() < ch.times.size()) continue;
            CurveView cv;
            cv.channel = c;
            cv.comp = k;
            cv.path = ch.path;
            if (ch.path == AnimPath::Rotation) {
                glm::vec3 prev(0.0f);
                for (size_t i = 0; i < ch.times.size(); ++i) {
                    glm::vec3 e = QuatToEulerDeg(ch.values[i]);
                    if (i > 0) e = Unwrap(e, prev);
                    cv.euler.push_back(e);
                    prev = e;
                }
            }
            float mn = 1e30f, mx = -1e30f;
            for (int i = 0; i < (int)ch.times.size(); ++i) { float v = CurveKeyValue(anim, cv, i); mn = std::min(mn, v); mx = std::max(mx, v); }
            if (mx - mn < 1e-6f) { mn -= 1.0f; mx += 1.0f; }
            cv.normMin = mn;
            cv.normMax = mx;
            curves.push_back(std::move(cv));
        }
    }
    // 正規化表示: カーブごとに -1〜1 に縮める
    auto toDisp = [&](const CurveView& c, float v) { return m_ceNormalize ? (v - c.normMin) / (c.normMax - c.normMin) * 2.0f - 1.0f : v; };
    auto fromDispDelta = [&](const CurveView& c, float dv) { return m_ceNormalize ? dv * (c.normMax - c.normMin) * 0.5f : dv; };

    // ---- 右: グラフ ----
    ImGui::BeginChild("##ceGraph", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // 選んだキー 1 つの数値
    {
        if (m_ceSel.size() == 1) {
            const CurveKey s = m_ceSel[0];
            CurveView* cv = nullptr;
            for (auto& c : curves) if (c.channel == s.channel && c.comp == s.comp) cv = &c;
            if (cv) {
                AnimChannel& ch = anim.channels[s.channel];
                float frame = ch.times[s.key] * kFps;
                float val = CurveKeyValue(anim, *cv, s.key);
                ImGui::SetNextItemWidth(Px(90));
                if (ImGui::InputFloat("フレーム##ceT", &frame, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                    float lo = s.key > 0 ? ch.times[s.key - 1] + 1e-4f : 0.0f;
                    float hi = s.key + 1 < (int)ch.times.size() ? ch.times[s.key + 1] - 1e-4f : 1e9f;
                    Checkpoint("キーの時刻");
                    ch.times[s.key] = std::clamp(frame / kFps, lo, hi);
                    anim.RecomputeDuration();
                    MarkChanged();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Px(110));
                if (ImGui::InputFloat(cv->path == AnimPath::Rotation ? "値 (度)##ceV" : "値##ceV", &val, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                    Checkpoint("キーの値");
                    if (cv->path == AnimPath::Rotation) {
                        glm::vec3 e = cv->euler[s.key];
                        e[s.comp] = val;
                        glm::vec4 q = EulerDegToQuat(e);
                        if (glm::dot(q, ch.values[s.key]) < 0) q = -q;
                        ch.values[s.key] = q;
                    } else {
                        ch.values[s.key][s.comp] = val;
                    }
                    MarkChanged();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s %s %s", m_scene.nodes[ch.node].name.c_str(), kPathName[(int)ch.path], kAxisName[s.comp]);
            }
        } else {
            ImGui::TextDisabled(m_ceSel.empty() ? "キーを選ぶと、ここで時刻と値を数値で入力できます" : "%zu 個のキーを選択中", m_ceSel.size());
        }
    }
    const float rulerH = ImGui::GetTextLineHeight() + 4;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    avail = ImGui::GetContentRegionAvail();
    ImVec2 size(std::max(avail.x, 50.0f), std::max(avail.y, 50.0f));
    ImRect graph(ImVec2(p0.x, p0.y + rulerH), ImVec2(p0.x + size.x, p0.y + size.y));
    ImGui::InvisibleButton("##ceCanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 表示範囲を合わせる
    auto frameKeys = [&](bool selectedOnly) {
        float t0 = 1e30f, t1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
        for (auto& c : curves) {
            const AnimChannel& ch = anim.channels[c.channel];
            for (int k = 0; k < (int)ch.times.size(); ++k) {
                if (selectedOnly && !isSel(c.channel, c.comp, k)) continue;
                float v = toDisp(c, CurveKeyValue(anim, c, k));
                t0 = std::min(t0, ch.times[k]); t1 = std::max(t1, ch.times[k]);
                v0 = std::min(v0, v); v1 = std::max(v1, v);
            }
        }
        if (t0 > t1) { t0 = 0; t1 = std::max(anim.duration, 1.0f / kFps); v0 = -1; v1 = 1; }
        if (t1 - t0 < 1.0f / kFps) { t0 -= 0.5f / kFps * 10; t1 += 0.5f / kFps * 10; }
        if (v1 - v0 < 1e-4f) { v0 -= 1.0f; v1 += 1.0f; }
        float pt = (t1 - t0) * 0.06f, pv = (v1 - v0) * 0.12f;
        m_ceT0 = t0 - pt; m_ceT1 = t1 + pt; m_ceV0 = v0 - pv; m_ceV1 = v1 + pv;
    };
    if (focused && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) m_ceFrameAll = true;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) m_ceFrameSel = true;
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) insertReq = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) deleteReq = true;
    }
    if (m_ceFrameAll) { frameKeys(false); m_ceFrameAll = false; }
    if (m_ceFrameSel) { frameKeys(!m_ceSel.empty()); m_ceFrameSel = false; }
    if (m_ceT1 - m_ceT0 < 1e-5f) { m_ceT0 = 0; m_ceT1 = std::max(anim.duration, 0.1f); }
    if (m_ceV1 - m_ceV0 < 1e-7f) { m_ceV0 = -1; m_ceV1 = 1; }

    const float gw = graph.GetWidth(), gh = graph.GetHeight();
    auto X = [&](float t) { return graph.Min.x + (t - m_ceT0) / (m_ceT1 - m_ceT0) * gw; };
    auto Y = [&](float v) { return graph.Max.y - (v - m_ceV0) / (m_ceV1 - m_ceV0) * gh; };
    auto T = [&](float x) { return m_ceT0 + (x - graph.Min.x) / gw * (m_ceT1 - m_ceT0); };
    auto V = [&](float y) { return m_ceV0 + (graph.Max.y - y) / gh * (m_ceV1 - m_ceV0); };

    // ---- 背景・目盛り ----
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(30, 30, 30, 255));
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, graph.Min.y), IM_COL32(22, 22, 22, 255));
    {
        float stepF = std::max(1.0f, NiceStep((m_ceT1 - m_ceT0) * kFps, gw, Px(60)));
        for (float f = std::floor(m_ceT0 * kFps / stepF) * stepF; f <= m_ceT1 * kFps; f += stepF) {
            float x = X(f / kFps);
            dl->AddLine(ImVec2(x, graph.Min.y), ImVec2(x, graph.Max.y), IM_COL32(48, 48, 48, 255));
            char l[16];
            snprintf(l, sizeof(l), "%g", f);
            dl->AddText(ImVec2(x + 2, p0.y + 1), IM_COL32(150, 150, 150, 255), l);
        }
        float stepV = NiceStep(m_ceV1 - m_ceV0, gh, Px(40));
        for (float v = std::floor(m_ceV0 / stepV) * stepV; v <= m_ceV1; v += stepV) {
            float y = Y(v);
            dl->AddLine(ImVec2(graph.Min.x, y), ImVec2(graph.Max.x, y), std::abs(v) < stepV * 0.01f ? IM_COL32(80, 80, 80, 255) : IM_COL32(44, 44, 44, 255));
            char l[24];
            snprintf(l, sizeof(l), "%g", std::abs(v) < stepV * 0.01f ? 0.0f : v);
            dl->AddText(ImVec2(graph.Min.x + 3, y - ImGui::GetTextLineHeight()), IM_COL32(120, 120, 120, 255), l);
        }
        // アニメーションの範囲の外を暗く
        dl->AddRectFilled(ImVec2(graph.Min.x, graph.Min.y), ImVec2(std::max(graph.Min.x, X(0)), graph.Max.y), IM_COL32(0, 0, 0, 60));
        dl->AddRectFilled(ImVec2(std::min(graph.Max.x, X(anim.duration)), graph.Min.y), ImVec2(graph.Max.x, graph.Max.y), IM_COL32(0, 0, 0, 60));
    }

    // ---- カーブ ----
    const float keyR = std::max(3.0f, Px(4));
    struct Handle { int curve, key; ImVec2 pos; };
    std::vector<Handle> handles;
    for (int ci = 0; ci < (int)curves.size(); ++ci) {
        const CurveView& c = curves[ci];
        const AnimChannel& ch = anim.channels[c.channel];
        ImU32 col = kAxisCol[c.comp];
        // 線: 画面の 2px ごとに曲線を評価する (キーの外は平らに延長)
        std::vector<ImVec2> pts;
        glm::vec3 prevE = c.path == AnimPath::Rotation && !c.euler.empty() ? c.euler.front() : glm::vec3(0.0f);
        float tFirst = ch.times.front(), tLast = ch.times.back();
        const int steps = std::max(2, (int)(gw / 2));
        // 回転は前の時刻の角度から連続になるように、キーの前から順に評価する
        float tStart = std::min(m_ceT0, tFirst);
        if (c.path == AnimPath::Rotation) {
            prevE = c.euler.front();
            if (tStart < tFirst) tStart = tFirst;
        }
        for (int i = 0; i <= steps; ++i) {
            float t = m_ceT0 + (m_ceT1 - m_ceT0) * i / steps;
            float v;
            if (c.path == AnimPath::Rotation) {
                float tt = std::clamp(t, tFirst, tLast);
                // 表示範囲の始まりがキーの途中なら、最初のキーから順に角度をつないでおく
                if (i == 0 && tt > tFirst) {
                    for (int k = 0; k < (int)ch.times.size() && ch.times[k] <= tt; ++k) prevE = c.euler[k];
                }
                glm::vec3 e = Unwrap(QuatToEulerDeg(EvalChannel(ch, tt)), prevE);
                prevE = e;
                v = e[c.comp];
            } else {
                v = EvalChannel(ch, t)[c.comp];
            }
            pts.push_back(ImVec2(X(t), Y(toDisp(c, v))));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.6f);
        // キー
        for (int k = 0; k < (int)ch.times.size(); ++k) {
            ImVec2 kp(X(ch.times[k]), Y(toDisp(c, CurveKeyValue(anim, c, k))));
            if (kp.x < graph.Min.x - keyR || kp.x > graph.Max.x + keyR) continue;
            bool sel = isSel(c.channel, c.comp, k);
            dl->AddRectFilled(ImVec2(kp.x - keyR, kp.y - keyR), ImVec2(kp.x + keyR, kp.y + keyR), sel ? IM_COL32(255, 220, 60, 255) : IM_COL32(20, 20, 20, 255));
            dl->AddRect(ImVec2(kp.x - keyR, kp.y - keyR), ImVec2(kp.x + keyR, kp.y + keyR), sel ? IM_COL32(255, 255, 255, 255) : col);
            // 選んだキーの接線ハンドル (曲線の移動・スケールのみ。回転は接線の種類で指定)
            if (sel && ch.IsCubic() && c.path != AnimPath::Rotation) {
                float slope = fromDispDelta(c, 1.0f) != 0 ? ch.outTangents[k][c.comp] / fromDispDelta(c, 1.0f) : 0.0f; // 表示上の傾き
                ImVec2 dir(gw / (m_ceT1 - m_ceT0), -slope * gh / (m_ceV1 - m_ceV0));
                float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (len > 1e-6f) {
                    dir.x /= len; dir.y /= len;
                    float hl = Px(45);
                    ImVec2 hOut(kp.x + dir.x * hl, kp.y + dir.y * hl), hIn(kp.x - dir.x * hl, kp.y - dir.y * hl);
                    dl->AddLine(hIn, hOut, IM_COL32(200, 200, 200, 200), 1.0f);
                    dl->AddCircleFilled(hOut, keyR * 0.8f, IM_COL32(230, 230, 230, 255));
                    dl->AddCircleFilled(hIn, keyR * 0.8f, IM_COL32(230, 230, 230, 255));
                    handles.push_back({ ci, k, hOut });
                    handles.push_back({ ci, k, hIn });
                }
            }
        }
    }
    // 再生位置
    {
        float x = X(m_animTime);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, graph.Max.y), IM_COL32(255, 196, 64, 255), 1.5f);
    }
    if (curves.empty()) {
        const char* msg = "左の一覧か、階層・ビューポートでノードを選ぶと、そのカーブが出ます";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(graph.GetCenter().x - ts.x * 0.5f, graph.GetCenter().y), IM_COL32(160, 160, 160, 255), msg);
    }

    // ---- 操作 ----
    const ImVec2 mp = io.MousePos;
    auto hitKey = [&](ImVec2 at) -> CurveKey {
        float best = keyR * 2.2f;
        CurveKey hit{ -1, 0, -1 };
        for (auto& c : curves) {
            const AnimChannel& ch = anim.channels[c.channel];
            for (int k = 0; k < (int)ch.times.size(); ++k) {
                ImVec2 kp(X(ch.times[k]), Y(toDisp(c, CurveKeyValue(anim, c, k))));
                float d = std::max(std::abs(kp.x - at.x), std::abs(kp.y - at.y));
                if (d < best) { best = d; hit = { c.channel, c.comp, k }; }
            }
        }
        return hit;
    };
    // 動かす前の値 (ドラッグ中は開始時からの差で動かすので、誤差がたまらない)
    auto beginMove = [&]() {
        Checkpoint("キーを動かす");
        m_ceOrig.clear();
        for (auto& s : m_ceSel) {
            const AnimChannel& ch = anim.channels[s.channel];
            CurveOrig o;
            o.key = s;
            o.time = ch.times[s.key];
            o.value = ch.values[s.key];
            for (auto& c : curves)
                if (c.channel == s.channel && c.path == AnimPath::Rotation) o.euler = c.euler[s.key];
            m_ceOrig.push_back(o);
        }
        m_ceMoveStart = mp;
        m_ceAxisLock = 0;
    };
    auto applyMove = [&]() {
        float dt = T(mp.x) - T(m_ceMoveStart.x), dv = V(mp.y) - V(m_ceMoveStart.y);
        if (io.KeyShift) {
            if (m_ceAxisLock == 0 && (std::abs(mp.x - m_ceMoveStart.x) > 4 || std::abs(mp.y - m_ceMoveStart.y) > 4))
                m_ceAxisLock = std::abs(mp.x - m_ceMoveStart.x) > std::abs(mp.y - m_ceMoveStart.y) ? 1 : 2;
            if (m_ceAxisLock == 1) dv = 0;
            if (m_ceAxisLock == 2) dt = 0;
        } else {
            m_ceAxisLock = 0;
        }
        if (m_ceSnap) dt = std::round(dt * kFps) / kFps;
        // 時刻: 選んでいない隣のキーを追い越さない範囲に収める
        std::set<std::pair<int, int>> movedKeys;
        for (auto& o : m_ceOrig) movedKeys.insert({ o.key.channel, o.key.key });
        for (auto& o : m_ceOrig) {
            const AnimChannel& ch = anim.channels[o.key.channel];
            int k = o.key.key;
            int prev = k - 1, next = k + 1;
            while (prev >= 0 && movedKeys.count({ o.key.channel, prev })) --prev;
            while (next < (int)ch.times.size() && movedKeys.count({ o.key.channel, next })) ++next;
            float lo = prev >= 0 ? ch.times[prev] + 1e-3f : 0.0f;
            float hi = next < (int)ch.times.size() ? ch.times[next] - 1e-3f : 1e9f;
            // 同じチャンネルの選んだキーは一緒に動くので、元の時刻を基準に範囲を出す
            float origPrev = prev >= 0 ? ch.times[prev] : -1e9f;
            (void)origPrev;
            dt = std::clamp(dt, lo - o.time, hi - o.time);
        }
        std::set<std::pair<int, int>> timeDone;
        // 回転は同じキーの複数の成分をまとめてオイラー角から作り直す
        std::map<std::pair<int, int>, glm::vec3> eulerNew;
        for (auto& o : m_ceOrig) {
            AnimChannel& ch = anim.channels[o.key.channel];
            if (timeDone.insert({ o.key.channel, o.key.key }).second) ch.times[o.key.key] = o.time + dt;
            CurveView* cv = nullptr;
            for (auto& c : curves) if (c.channel == o.key.channel && c.comp == o.key.comp) cv = &c;
            float dval = cv ? fromDispDelta(*cv, dv) : dv;
            if (ch.path == AnimPath::Rotation) {
                auto key = std::make_pair(o.key.channel, o.key.key);
                if (!eulerNew.count(key)) eulerNew[key] = o.euler;
                eulerNew[key][o.key.comp] = o.euler[o.key.comp] + dval;
            } else {
                ch.values[o.key.key][o.key.comp] = o.value[o.key.comp] + dval;
            }
        }
        for (auto& [key, e] : eulerNew) {
            AnimChannel& ch = anim.channels[key.first];
            glm::vec4 q = EulerDegToQuat(e);
            const glm::vec4& ref = key.second > 0 ? ch.values[key.second - 1] : ch.values[key.second];
            if (glm::dot(q, ref) < 0) q = -q;
            ch.values[key.second] = q;
        }
        anim.RecomputeDuration();
        MarkChanged();
    };

    if (hovered) {
        // ホイール: ズーム (Ctrl = 時間だけ / Shift = 値だけ)
        if (io.MouseWheel != 0.0f) {
            float z = std::pow(0.85f, io.MouseWheel);
            float tm = T(mp.x), vm = V(mp.y);
            if (!io.KeyShift) { m_ceT0 = tm + (m_ceT0 - tm) * z; m_ceT1 = tm + (m_ceT1 - tm) * z; }
            if (!io.KeyCtrl) { m_ceV0 = vm + (m_ceV0 - vm) * z; m_ceV1 = vm + (m_ceV1 - vm) * z; }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (mp.y < graph.Min.y) {
                m_ceDrag = 4; // 目盛り: 再生位置
            } else {
                // 接線ハンドル
                int hHit = -1;
                for (int i = 0; i < (int)handles.size(); ++i)
                    if (std::abs(handles[i].pos.x - mp.x) < keyR * 1.8f && std::abs(handles[i].pos.y - mp.y) < keyR * 1.8f) hHit = i;
                if (hHit >= 0) {
                    m_ceDrag = 5;
                    m_ceHandleCurve = curves[handles[hHit].curve].channel * 4 + curves[handles[hHit].curve].comp;
                    m_ceHandleKey = handles[hHit].key;
                    Checkpoint("接線を動かす");
                } else {
                    CurveKey hit = hitKey(mp);
                    if (hit.channel >= 0) {
                        bool already = isSel(hit.channel, hit.comp, hit.key);
                        if (io.KeyCtrl) {
                            m_ceSel.erase(std::remove_if(m_ceSel.begin(), m_ceSel.end(), [&](const CurveKey& s) { return s.channel == hit.channel && s.comp == hit.comp && s.key == hit.key; }), m_ceSel.end());
                        } else if (io.KeyShift) {
                            if (already) m_ceSel.erase(std::remove_if(m_ceSel.begin(), m_ceSel.end(), [&](const CurveKey& s) { return s.channel == hit.channel && s.comp == hit.comp && s.key == hit.key; }), m_ceSel.end());
                            else m_ceSel.push_back(hit);
                        } else if (!already) {
                            m_ceSel = { hit };
                        }
                        if (!io.KeyCtrl && isSel(hit.channel, hit.comp, hit.key)) { m_ceDrag = 1; beginMove(); }
                    } else {
                        m_ceDrag = 2; // 矩形選択
                        m_ceBoxStart = mp;
                    }
                }
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            if (io.KeyAlt || m_ceSel.empty()) { m_ceDrag = 3; } // パン
            else { m_ceDrag = 1; beginMove(); }                 // Maya と同じく中ドラッグで選んだキーを動かす
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && io.KeyAlt) m_ceDrag = 6; // ズーム
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !io.KeyAlt) ImGui::OpenPopup("##ceCtx");
    }
    if (m_ceDrag != 0) {
        bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
        switch (m_ceDrag) {
        case 1: applyMove(); break;
        case 2: {
            ImVec2 a(std::min(m_ceBoxStart.x, mp.x), std::min(m_ceBoxStart.y, mp.y)), b(std::max(m_ceBoxStart.x, mp.x), std::max(m_ceBoxStart.y, mp.y));
            dl->AddRectFilled(a, b, IM_COL32(0, 112, 224, 40));
            dl->AddRect(a, b, IM_COL32(0, 140, 255, 200));
            if (!down) {
                if (!io.KeyShift && !io.KeyCtrl) m_ceSel.clear();
                for (auto& c : curves) {
                    const AnimChannel& ch = anim.channels[c.channel];
                    for (int k = 0; k < (int)ch.times.size(); ++k) {
                        ImVec2 kp(X(ch.times[k]), Y(toDisp(c, CurveKeyValue(anim, c, k))));
                        if (kp.x < a.x || kp.x > b.x || kp.y < a.y || kp.y > b.y) continue;
                        bool s = isSel(c.channel, c.comp, k);
                        if (io.KeyCtrl && s) m_ceSel.erase(std::remove_if(m_ceSel.begin(), m_ceSel.end(), [&](const CurveKey& x) { return x.channel == c.channel && x.comp == c.comp && x.key == k; }), m_ceSel.end());
                        else if (!s) m_ceSel.push_back({ c.channel, c.comp, k });
                    }
                }
            }
            break;
        }
        case 3: {
            float dt = io.MouseDelta.x / gw * (m_ceT1 - m_ceT0), dv = io.MouseDelta.y / gh * (m_ceV1 - m_ceV0);
            m_ceT0 -= dt; m_ceT1 -= dt; m_ceV0 += dv; m_ceV1 += dv;
            break;
        }
        case 4:
            m_animTime = std::clamp(T(mp.x), 0.0f, anim.duration);
            if (m_ceSnap) m_animTime = std::round(m_animTime * kFps) / kFps;
            m_playing = false;
            break;
        case 5: {
            // 接線ハンドル: マウスの位置への傾きにする (入る側と出る側は同じ傾き = Maya の「接線を統一」)
            int chIdx = m_ceHandleCurve / 4, comp = m_ceHandleCurve % 4;
            if (chIdx < (int)anim.channels.size() && m_ceHandleKey < (int)anim.channels[chIdx].times.size()) {
                AnimChannel& ch = anim.channels[chIdx];
                CurveView* cv = nullptr;
                for (auto& c : curves) if (c.channel == chIdx && c.comp == comp) cv = &c;
                if (cv && ch.IsCubic()) {
                    float kt = ch.times[m_ceHandleKey];
                    float kv = toDisp(*cv, ch.values[m_ceHandleKey][comp]);
                    float dtm = T(mp.x) - kt, dvm = V(mp.y) - kv;
                    if (std::abs(dtm) > 1e-5f) {
                        float slope = fromDispDelta(*cv, dvm / dtm);
                        ch.inTangents[m_ceHandleKey][comp] = slope;
                        ch.outTangents[m_ceHandleKey][comp] = slope;
                        MarkChanged();
                    }
                }
            }
            break;
        }
        case 6: {
            float zt = std::pow(1.01f, -io.MouseDelta.x), zv = std::pow(1.01f, io.MouseDelta.y);
            float tm = T(m_ceBoxStart.x == 0 ? mp.x : mp.x), vm = V(mp.y);
            m_ceT0 = tm + (m_ceT0 - tm) * zt; m_ceT1 = tm + (m_ceT1 - tm) * zt;
            m_ceV0 = vm + (m_ceV0 - vm) * zv; m_ceV1 = vm + (m_ceV1 - vm) * zv;
            break;
        }
        default: break;
        }
        if (!down) m_ceDrag = 0;
    }
    // 右クリックメニュー
    if (ImGui::BeginPopup("##ceCtx")) {
        ImGui::BeginDisabled(m_ceSel.empty());
        if (ImGui::MenuItem("スプライン")) applyTangent(Ops::TangentMode::Spline, "接線: スプライン");
        if (ImGui::MenuItem("クランプ")) applyTangent(Ops::TangentMode::Clamped, "接線: クランプ");
        if (ImGui::MenuItem("リニア")) applyTangent(Ops::TangentMode::Linear, "接線: リニア");
        if (ImGui::MenuItem("フラット")) applyTangent(Ops::TangentMode::Flat, "接線: フラット");
        if (ImGui::MenuItem("ステップ")) applyTangent(Ops::TangentMode::Step, "接線: ステップ");
        ImGui::Separator();
        if (ImGui::MenuItem("キーを削除", "Del")) deleteReq = true;
        ImGui::EndDisabled();
        if (ImGui::MenuItem("今の時刻にキーを打つ", "S")) insertReq = true;
        if (ImGui::MenuItem("全体を表示", "A")) m_ceFrameAll = true;
        if (ImGui::MenuItem("選択を表示", "F")) m_ceFrameSel = true;
        ImGui::EndPopup();
    }
    dl->PopClipRect();
    ImGui::EndChild();

    // キーを打つ / 消す
    if (insertReq && !curves.empty()) {
        Checkpoint("キーを打つ");
        std::set<int> chs;
        for (auto& c : curves) chs.insert(c.channel);
        for (int c : chs) Ops::InsertKey(anim.channels[c], m_animTime);
        m_ceSel.clear();
        anim.RecomputeDuration();
        anim.duration = std::max(anim.duration, m_animTime);
        MarkChanged();
    }
    if (deleteReq && !m_ceSel.empty()) {
        Checkpoint("キーを削除");
        // 同じチャンネルの X Y Z は時刻を共有しているので、キーはチャンネルごと消える
        std::map<int, std::set<int>> del;
        for (auto& s : m_ceSel) del[s.channel].insert(s.key);
        for (auto& [c, ks] : del) {
            AnimChannel& ch = anim.channels[c];
            for (auto it = ks.rbegin(); it != ks.rend(); ++it)
                if (ch.times.size() > 1) Ops::EraseKey(ch, *it); // 最後の 1 つは残す
        }
        m_ceSel.clear();
        anim.RecomputeDuration();
        MarkChanged();
    }
    ImGui::End();
}
