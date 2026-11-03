// 単位 / 実寸サイズ / 定規
#include "Editor.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include "core/Units.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>

static uint32_t PackColor(const glm::vec3& c, float a) {
    auto b = [](float f) { return (uint32_t)std::clamp(f * 255.0f + 0.5f, 0.0f, 255.0f); };
    return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | (b(a) << 24);
}

// 1, 2, 5 x 10^n の「きりの良い」間隔
static double NiceStep(double approx) {
    if (approx <= 0) return 1;
    double p = std::pow(10.0, std::floor(std::log10(approx)));
    double m = approx / p;
    return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
}

int Editor::AddRuler() {
    Ruler r;
    r.name = "定規 " + std::to_string(m_rulers.size() + 1);
    if (!m_scene.Empty()) {
        // モデルの横、足元の高さに置き、高さはモデルの高さ
        glm::vec3 d = m_sceneMax - m_sceneMin;
        r.width = std::max(std::max(d.x, d.z) * 0.25f, 0.01f);
        r.origin = glm::vec3(m_sceneMax.x + r.width, m_sceneMin.y, (m_sceneMin.z + m_sceneMax.z) * 0.5f);
        r.height = std::max(d.y, 1e-4f);
    }
    static const glm::vec3 colors[] = { { 1.0f, 0.82f, 0.2f }, { 0.3f, 0.9f, 1.0f }, { 1.0f, 0.4f, 0.7f }, { 0.5f, 1.0f, 0.4f } };
    r.color = colors[m_rulers.size() % 4];
    m_rulers.push_back(r);
    m_activeRuler = (int)m_rulers.size() - 1;
    return m_activeRuler;
}

void Editor::ChangeModelUnit(int newUnit) {
    const int model = ActiveModel();
    int old = ModelInfo(model).modelUnit;
    if (newUnit == old || newUnit < 0 || newUnit >= Unit_Count) return;
    double f = UnitToMeters(newUnit) / UnitToMeters(old >= 0 ? old : Unit_M);
    Checkpoint(std::string("単位を ") + UnitSuffix(newUnit) + " に変更");
    if (m_scene.roots.size() > 1 && model >= 0) {
        // 複数モデル: 選んでいるモデルの中身だけを拡大縮小する (他のモデルはそのまま)
        std::string e;
        if (!Ops::ScaleModelContents(m_scene, model, (float)f, &e)) { LOG_ERROR("単位を変更できません: %s", e.c_str()); return; }
    } else {
        Ops::ScaleScene(m_scene, (float)f);
        m_scene.info.modelUnit = newUnit;
    }
    ModelInfoMut(model).modelUnit = newUnit;
    MarkChanged();
    UpdateWorld();
    FrameAll();
    LOG_INFO("モデルの単位を %s → %s として解釈し直しました (x%g)", UnitSuffix(old), UnitSuffix(newUnit), f);
}

void Editor::SubmitRulerLines() {
    for (size_t i = 0; i < m_rulers.size(); ++i) {
        const Ruler& r = m_rulers[i];
        if (!r.visible) continue;
        bool active = (int)i == m_activeRuler;
        uint32_t c = PackColor(r.color, 1.0f);
        uint32_t cDim = PackColor(r.color, 0.45f);
        const float hw = r.width * 0.5f;
        const glm::vec3 base = r.origin, top = r.origin + glm::vec3(0, r.height, 0);
        auto square = [&](const glm::vec3& c0, uint32_t col) {
            glm::vec3 p[4] = { c0 + glm::vec3(-hw, 0, -hw), c0 + glm::vec3(hw, 0, -hw), c0 + glm::vec3(hw, 0, hw), c0 + glm::vec3(-hw, 0, hw) };
            for (int k = 0; k < 4; ++k) m_renderer.AddLine(p[k], p[(k + 1) % 4], col, r.onTop);
            m_renderer.AddLine(p[0], p[2], cDim, r.onTop); // 対角線で面として見やすく
            m_renderer.AddLine(p[1], p[3], cDim, r.onTop);
        };
        // ワイヤーフレーム 2 枚 (原点の面と高さの面)
        square(base, c);
        square(top, active ? PackColor(glm::vec3(1), 1.0f) : c);
        // 四隅の縦線と中心線
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sz = -1; sz <= 1; sz += 2)
                m_renderer.AddLine(base + glm::vec3(sx * hw, 0, sz * hw), top + glm::vec3(sx * hw, 0, sz * hw), cDim, r.onTop);
        m_renderer.AddLine(base, top, c, r.onTop);
        // 目盛り (表示単位のきりの良い間隔)
        if (r.ticks && std::abs(r.height) > 1e-6f) {
            double unitM = UnitToMeters(m_displayUnit);
            double hDisp = std::abs(r.height) / unitM;
            double step = NiceStep(hDisp / 10.0);
            int count = (int)std::floor(hDisp / step + 1e-6);
            float dir = r.height >= 0 ? 1.0f : -1.0f;
            for (int t = 1; t <= count && t <= 200; ++t) {
                float y = (float)(t * step * unitM) * dir;
                bool major = t % 5 == 0;
                float len = hw * (major ? 0.6f : 0.3f);
                glm::vec3 p = base + glm::vec3(0, y, 0);
                m_renderer.AddLine(p, p + glm::vec3(len, 0, 0), major ? c : cDim, r.onTop);
            }
        }
    }
}

void Editor::DrawRulerLabels(const ImVec2& pos, const ImVec2& size, const glm::mat4& viewProj) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto project = [&](const glm::vec3& p, ImVec2& out) {
        glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
        if (c.w <= 1e-6f) return false;
        glm::vec2 ndc = glm::vec2(c) / c.w;
        out = ImVec2(pos.x + (ndc.x * 0.5f + 0.5f) * size.x, pos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * size.y);
        return true;
    };
    auto label = [&](const ImVec2& at, const std::string& text, ImU32 col) {
        ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddRectFilled(ImVec2(at.x - 3, at.y - 2), ImVec2(at.x + ts.x + 3, at.y + ts.y + 2), IM_COL32(15, 15, 18, 200), 3.0f);
        dl->AddText(at, col, text.c_str());
    };
    for (size_t i = 0; i < m_rulers.size(); ++i) {
        const Ruler& r = m_rulers[i];
        if (!r.visible) continue;
        ImU32 col = IM_COL32((int)(r.color.r * 255), (int)(r.color.g * 255), (int)(r.color.b * 255), 255);
        glm::vec3 top = r.origin + glm::vec3(0, r.height, 0);
        ImVec2 sp;
        if (project(top + glm::vec3(r.width * 0.5f, 0, 0), sp)) {
            std::string t = r.name + ": " + FormatLength(r.height, m_displayUnit);
            if (!m_scene.Empty()) {
                // モデルの上端 (定規が下向きなら下端) との差
                float modelEdge = r.height >= 0 ? m_sceneMax.y : m_sceneMin.y;
                float diff = (r.origin.y + r.height) - modelEdge;
                t += "  (モデル" + std::string(r.height >= 0 ? "上端" : "下端") + "との差 " + (diff >= 0 ? "+" : "") + FormatLength(diff, m_displayUnit) + ")";
            }
            label(ImVec2(sp.x + 6, sp.y - 8), t, col);
        }
        if ((int)i == m_activeRuler && project(r.origin + glm::vec3(r.width * 0.5f, 0, 0), sp)) {
            char b[128];
            snprintf(b, sizeof(b), "原点 (%s, %s, %s)", FormatLength(r.origin.x, m_displayUnit, false).c_str(),
                     FormatLength(r.origin.y, m_displayUnit, false).c_str(), FormatLength(r.origin.z, m_displayUnit, false).c_str());
            label(ImVec2(sp.x + 6, sp.y + 2), b, IM_COL32(220, 220, 220, 230));
        }
    }
}

void Editor::DrawMeasure() {
    if (!ImGui::Begin(PanelTitle(ICON_RULER, "単位 / 定規", "Measure").c_str(), &m_panels.measure)) { ImGui::End(); return; }
    PanelHint("モデルの実寸の確認、単位の修正、定規の配置、指定サイズや倍率での拡大縮小を行います");
    const double unitM = UnitToMeters(m_displayUnit);

    // ---- 単位 ---------------------------------------------------------------
    ImGui::SeparatorText("モデルの長さの単位");
    const SceneInfo& info = ModelInfo(ActiveModel()); // 複数モデルなら選んでいるモデルの情報
    if (m_scene.Empty()) {
        ImGui::TextDisabled("モデルが読み込まれていません");
    } else {
        if (info.sourceUnit >= 0)
            ImGui::Text("ファイルに記録された単位: %s", UnitName(info.sourceUnit));
        else
            ImGui::Text("ファイルに記録された単位: 不明 (%s には単位情報がありません)", info.sourceFormat.c_str());
        if (info.sourceFormat == "FBX") ImGui::TextDisabled("FBX UnitScaleFactor = %g (1 単位 = %g cm)", info.unitScaleFactor, info.unitScaleFactor);
        int mu = std::clamp(info.modelUnit, 0, Unit_Count - 1);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("このモデルの単位", &mu, UnitComboItems())) ChangeModelUnit(mu);
        ImGui::TextWrapped("ファイルの 1 単位を「%s」として扱っています。サイズが合わない場合はここで正しい単位を選ぶと、実寸が再計算されます (Undo 可)。",
                           UnitName(info.modelUnit));
        if (info.sourceUnit >= 0 && info.modelUnit != info.sourceUnit)
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "※ ファイルの記録 (%s) と異なる単位で解釈しています", UnitSuffix(info.sourceUnit));
    }

    ImGui::SetNextItemWidth(200);
    ImGui::Combo("表示単位", &m_displayUnit, UnitComboItems());
    ImGui::SameLine();
    ImGui::Checkbox("ビューに実寸を表示", &m_showSizeOverlay);

    // ---- 実寸 ---------------------------------------------------------------
    ImGui::SeparatorText("実際のサイズ (レストポーズ)");
    if (!m_scene.Empty()) {
        glm::vec3 d = m_sceneMax - m_sceneMin;
        if (ImGui::BeginTable("size", 2, ImGuiTableFlags_BordersInnerH)) {
            auto row = [](const char* k, const std::string& v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", k);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(v.c_str());
            };
            row("幅 (X)", FormatLength(d.x, m_displayUnit));
            row("高さ (Y)", FormatLength(d.y, m_displayUnit));
            row("奥行 (Z)", FormatLength(d.z, m_displayUnit));
            row("対角", FormatLength(glm::length(d), m_displayUnit));
            row("最低点 Y / 最高点 Y", FormatLength(m_sceneMin.y, m_displayUnit) + " / " + FormatLength(m_sceneMax.y, m_displayUnit));
            row("中心", FormatLength((m_sceneMin.x + m_sceneMax.x) * 0.5f, m_displayUnit, false) + ", " +
                          FormatLength((m_sceneMin.y + m_sceneMax.y) * 0.5f, m_displayUnit, false) + ", " +
                          FormatLength((m_sceneMin.z + m_sceneMax.z) * 0.5f, m_displayUnit, false));
            ImGui::EndTable();
        }
        // スケール感の目安
        float h = d.y;
        if (h > 0) {
            const char* ref = h < 0.05f ? "コインくらい" : h < 0.3f ? "手のひらくらい" : h < 1.0f ? "子ども〜机くらい" : h < 2.5f ? "人の身長くらい" : h < 10.0f ? "家くらい" : h < 100.0f ? "ビルくらい" : "とても大きい (単位の確認を推奨)";
            ImGui::TextDisabled("目安: %s (身長 1.7 m の人の %.0f%%)", ref, h / 1.7f * 100.0f);
        }

        ImGui::SeparatorText("指定サイズにリサイズ");
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("##axis", &m_targetAxis, "幅 (X)\0高さ (Y)\0奥行 (Z)\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputFloat("##target", &m_targetSize, 0, 0, "%.4g");
        ImGui::SameLine();
        ImGui::TextUnformatted(UnitSuffix(m_displayUnit));
        ImGui::SameLine();
        if (ImGui::Button("この大きさにする")) {
            // トランスフォームのスケールで変更 (選択があれば選択、無ければ全オブジェクト)
            ScaleTargetsToSize(FitTargetsForSelection(false), m_targetAxis, (float)(m_targetSize * unitM), nullptr, false, false, "サイズ指定でリサイズ");
        }
        ImGui::TextDisabled("対象: %s (トランスフォームのスケールを変更します)", FitTargetLabel(false).c_str());

        ImGui::SeparatorText("倍率で拡大縮小");
        ImGui::SetNextItemWidth(120);
        ImGui::InputFloat("倍##factor", &m_scaleFactor, 0, 0, "%.6g");
        m_scaleFactor = std::max(m_scaleFactor, 1e-6f);
        ImGui::SameLine();
        if (ImGui::Button("この倍率にする"))
            ScaleTargetsToSize(FitTargetsForSelection(), 1, 0.0f, nullptr, false, false, "倍率で拡大縮小", m_scaleFactor);
        for (float q : { 0.01f, 0.1f, 0.5f, 2.0f, 10.0f, 100.0f }) {
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "x%g", q);
            if (q != 0.01f) ImGui::SameLine();
            if (ImGui::SmallButton(lbl)) ScaleTargetsToSize(FitTargetsForSelection(), 1, 0.0f, nullptr, false, false, "倍率で拡大縮小", q);
        }
        ImGui::TextDisabled("対象: %s", FitTargetLabel(true).c_str());
        ImGui::TextDisabled("オブジェクトの原点 (ピボット) を中心に拡大縮小します (原点は動きません)。アニメーション付きのモデルは親 (オブジェクト) で拡大するので、\nルートモーションも同じ倍率になります");
    }

    // ---- 定規 ---------------------------------------------------------------
    ImGui::SeparatorText("定規");
    if (ImGui::Button("定規を追加")) AddRuler();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_activeRuler < 0);
    if (ImGui::Button("削除") && m_activeRuler >= 0 && m_activeRuler < (int)m_rulers.size()) {
        m_rulers.erase(m_rulers.begin() + m_activeRuler);
        m_activeRuler = std::min(m_activeRuler, (int)m_rulers.size() - 1);
        if (m_rulers.empty()) m_rulerGizmo = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("すべて削除")) { m_rulers.clear(); m_activeRuler = -1; m_rulerGizmo = false; }

    for (int i = 0; i < (int)m_rulers.size(); ++i) {
        ImGui::PushID(i);
        ImGui::Checkbox("##vis", &m_rulers[i].visible);
        ImGui::SameLine();
        std::string l = m_rulers[i].name + "  " + FormatLength(m_rulers[i].height, m_displayUnit);
        if (ImGui::Selectable(l.c_str(), m_activeRuler == i)) m_activeRuler = i;
        ImGui::PopID();
    }

    if (m_activeRuler >= 0 && m_activeRuler < (int)m_rulers.size()) {
        Ruler& r = m_rulers[m_activeRuler];
        ImGui::Separator();
        ImGui::SetNextItemWidth(-80);
        ImGui::InputText("名前", &r.name);
        // 値は表示単位で編集する
        glm::vec3 o = r.origin / (float)unitM;
        float speed = (float)std::max(m_sceneRadius / unitM * 0.005, 1e-4);
        ImGui::SetNextItemWidth(-80);
        if (ImGui::DragFloat3("原点", &o.x, speed, 0, 0, "%.4f")) r.origin = o * (float)unitM;
        float h = (float)(r.height / unitM);
        ImGui::SetNextItemWidth(-80);
        if (ImGui::DragFloat("高さ", &h, speed, 0, 0, "%.4f")) r.height = (float)(h * unitM);
        ImGui::SameLine();
        ImGui::TextUnformatted(UnitSuffix(m_displayUnit));
        ImGui::TextDisabled("高さはマイナスも指定できます (原点より下に伸びます)");
        float w = (float)(r.width / unitM);
        ImGui::SetNextItemWidth(-80);
        if (ImGui::DragFloat("枠の大きさ", &w, speed, 1e-6f, 1e9f, "%.4f")) r.width = std::max((float)(w * unitM), 1e-6f);
        ImGui::ColorEdit3("色", &r.color.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::Checkbox("最前面", &r.onTop);
        ImGui::SameLine();
        ImGui::Checkbox("目盛り", &r.ticks);

        ImGui::Checkbox("ビューポートのギズモで原点を動かす", &m_rulerGizmo);
        if (m_rulerGizmo) ImGui::TextDisabled("(オフにするとノードのギズモに戻ります。Ctrl でスナップ)");

        ImGui::TextDisabled("原点を合わせる");
        if (ImGui::SmallButton("ワールド原点")) r.origin = glm::vec3(0);
        ImGui::SameLine();
        if (ImGui::SmallButton("モデルの足元 (中心)") && !m_scene.Empty())
            r.origin = glm::vec3((m_sceneMin.x + m_sceneMax.x) * 0.5f, m_sceneMin.y, (m_sceneMin.z + m_sceneMax.z) * 0.5f);
        ImGui::SameLine();
        if (ImGui::SmallButton("モデルの横") && !m_scene.Empty())
            r.origin = glm::vec3(m_sceneMax.x + r.width, m_sceneMin.y, (m_sceneMin.z + m_sceneMax.z) * 0.5f);
        if (ImGui::SmallButton("選択ノードの位置") && m_primary >= 0 && m_primary < (int)m_world.size()) r.origin = glm::vec3(m_world[m_primary][3]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Y = 0 (地面)")) r.origin.y = 0;

        ImGui::TextDisabled("高さを合わせる");
        if (ImGui::SmallButton("モデル上端まで") && !m_scene.Empty()) r.height = m_sceneMax.y - r.origin.y;
        ImGui::SameLine();
        if (ImGui::SmallButton("モデル下端まで") && !m_scene.Empty()) r.height = m_sceneMin.y - r.origin.y;
        ImGui::SameLine();
        if (ImGui::SmallButton("反転 (±)")) r.height = -r.height;
        ImGui::SameLine();
        if (ImGui::SmallButton("1 m")) r.height = 1.0f;
        ImGui::SameLine();
        if (ImGui::SmallButton("1.7 m (人)")) r.height = 1.7f;

        ImGui::Spacing();
        ImGui::TextWrapped("定規の上端: Y = %s", FormatLength(r.origin.y + r.height, m_displayUnit).c_str());

        // 選択モデルを定規の高さに合わせる
        ImGui::SeparatorText("選択モデルを定規の高さに合わせる");
        auto targets = FitTargetsForSelection(false);
        glm::vec3 tmn, tmx;
        bool hasBounds = !m_scene.Empty() && ComputeTargetsBounds(targets, tmn, tmx);
        if (hasBounds) {
            float curH = tmx.y - tmn.y;
            ImGui::Text("対象: %s  現在の高さ %s → %s (x%.4g)", m_selection.empty() ? "全オブジェクト" : (std::to_string(targets.size()) + " 個").c_str(),
                        FormatLength(curH, m_displayUnit).c_str(), FormatLength(std::abs(r.height), m_displayUnit).c_str(),
                        curH > 1e-9f ? std::abs(r.height) / curH : 0.0f);
        }
        ImGui::Checkbox("足元を定規の原点に合わせる", &m_fitAlignBottom);
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_fitAlignBottom);
        ImGui::Checkbox("XZ も合わせる", &m_fitAlignXZ);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!hasBounds || std::abs(r.height) < 1e-9f);
        if (ImGui::Button("定規の高さに合わせる (スケール)", ImVec2(-1, 0))) {
            glm::vec3 anchor = r.origin;
            ScaleTargetsToSize(targets, 1, std::abs(r.height), m_fitAlignBottom ? &anchor : nullptr, r.height < 0, m_fitAlignXZ, "定規の高さに合わせる");
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("頂点には焼き込まず、トランスフォームのスケール値 (例: 1.8, 1.8, 1.8) を変更します。\n高さがマイナスの定規では、モデルの上端を原点に合わせて下向きに配置します。\n"
                            "大きさはレストポーズで測るので、アニメーションのどのフレームで実行しても同じ位置になります。");
        if (m_primary >= 0 && m_primary < (int)m_scene.nodes.size()) {
            const glm::vec3& s = m_scene.nodes[m_primary].scale;
            ImGui::TextDisabled("選択「%s」のスケール: %.4g, %.4g, %.4g", m_scene.nodes[m_primary].name.c_str(), s.x, s.y, s.z);
        }
    } else {
        ImGui::TextDisabled("「定規を追加」で、高さを指定できるワイヤーフレームの定規を作成します");
    }
    ImGui::End();
}
