// メッシュ / マテリアル / テクスチャ / UV ビューア
#include "Editor.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include "io/ImageIO.h"
#include "util/Dialogs.h"
#include "util/FileUtil.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>

static const char* kImageFilter = "Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif\0All Files\0*.*\0";

// ---------------------------------------------------------------------------
// メッシュ
// ---------------------------------------------------------------------------
void Editor::DrawMeshes() {
    if (!ImGui::Begin(PanelTitle(ICON_MESH, "メッシュ", "Meshes").c_str(), &m_panels.meshes)) { ImGui::End(); return; }
    PanelHint("形状データ (頂点と三角形) の一覧です。法線の再計算、頂点の結合、UV の確認などができます");
    DrawModelFilterBar();
    std::vector<int> rows;
    {
        const auto& M = Membership();
        for (int i = 0; i < (int)m_scene.meshes.size(); ++i) if (InMask(M.meshes, i)) rows.push_back(i);
    }
    float listH = std::min(220.0f, ImGui::GetContentRegionAvail().y * 0.4f);
    if (ImGui::BeginTable("meshlist", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("名前", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("頂点", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("三角形", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("使用", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)rows.size());
        while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                const int i = rows[row];
                const Mesh& m = m_scene.meshes[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                std::string label = (m.HasSkin() ? "★ " : "■ ") + m.name;
                if (ImGui::Selectable(label.c_str(), m_selMesh == i, ImGuiSelectableFlags_SpanAllColumns)) { m_selMesh = i; m_selPrim = 0; }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    auto users = m_scene.NodesUsingMesh(i);
                    if (!users.empty()) { Select(users[0]); FocusNode(users[0]); }
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::Text("%zu", m.VertexCount());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", m.TriangleCount());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", m_scene.NodesUsingMesh(i).size());
            }
        ImGui::EndTable();
    }
    if (m_selMesh < 0 || m_selMesh >= (int)m_scene.meshes.size()) {
        ImGui::TextDisabled("メッシュを選択してください");
        ImGui::End();
        return;
    }
    Mesh& mesh = m_scene.meshes[m_selMesh];
    ImGui::Separator();
    ImGui::SetNextItemWidth(-60);
    EditValue("メッシュ名変更", mesh.name, [&] { return ImGui::InputText("名前##mesh", &mesh.name); });
    auto users = m_scene.NodesUsingMesh(m_selMesh);
    ImGui::Text("使用ノード:");
    for (int u : users) {
        ImGui::SameLine();
        ImGui::PushID(u);
        if (ImGui::SmallButton(m_scene.nodes[u].name.c_str())) Select(u);
        ImGui::PopID();
    }
    if (users.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(未使用)");
    }
    if (ImGui::Button("メッシュを削除")) {
        Checkpoint("メッシュ削除");
        m_scene.RemoveMesh(m_selMesh);
        ValidateSelection();
        MarkChanged();
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("UV ビューア")) m_panels.uvViewer = true;

    // プリミティブ
    if (mesh.primitives.empty()) { ImGui::End(); return; }
    m_selPrim = std::clamp(m_selPrim, 0, (int)mesh.primitives.size() - 1);
    if (mesh.primitives.size() > 1) {
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt("プリミティブ", &m_selPrim, 0, (int)mesh.primitives.size() - 1);
    }
    Primitive& p = mesh.primitives[m_selPrim];
    if (ImGui::BeginTable("attrs", 2, ImGuiTableFlags_BordersInnerH)) {
        auto row = [](const char* k, const char* fmt, auto... a) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            ImGui::Text(fmt, a...);
        };
        row("頂点 / 三角形", "%zu / %zu", p.VertexCount(), p.TriangleCount());
        row("マテリアル", "%s", p.material >= 0 && p.material < (int)m_scene.materials.size() ? m_scene.materials[p.material].name.c_str() : "(なし)");
        row("法線", "%s", p.HasNormals() ? "あり" : "なし");
        row("接線", "%s", p.HasTangents() ? "あり" : "なし");
        row("UV0 / UV1", "%s / %s", p.HasUV0() ? "あり" : "なし", p.HasUV1() ? "あり" : "なし");
        row("頂点カラー", "%s", p.HasColors() ? "あり" : "なし");
        row("スキンウェイト", "%s", p.HasSkin() ? "あり (4 影響)" : "なし");
        row("モーフターゲット", "%zu", p.targets.size());
        glm::vec3 mn, mx;
        Ops::ComputePrimitiveBounds(p, mn, mx);
        row("AABB 最小", "%.4f %.4f %.4f", mn.x, mn.y, mn.z);
        row("AABB 最大", "%.4f %.4f %.4f", mx.x, mx.y, mx.z);
        row("サイズ", "%.4f %.4f %.4f", mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
        size_t bytes = p.positions.size() * 12 + p.normals.size() * 12 + p.tangents.size() * 16 + p.uv0.size() * 8 + p.uv1.size() * 8 +
                       p.colors.size() * 16 + p.joints.size() * 8 + p.weights.size() * 16 + p.indices.size() * 4;
        row("メモリ", "%.1f KB", bytes / 1024.0);
        ImGui::EndTable();
    }
    DrawPrimitiveTools(m_selMesh, m_selPrim);
    ImGui::End();
}

void Editor::DrawPrimitiveTools(int meshIdx, int primIdx) {
    Mesh& mesh = m_scene.meshes[meshIdx];
    ImGui::SeparatorText("メッシュツール");
    ImGui::Checkbox("全プリミティブに適用", &m_applyToAllPrims);
    auto apply = [&](const char* label, auto&& fn) {
        Checkpoint(label);
        if (m_applyToAllPrims) for (auto& p : mesh.primitives) fn(p);
        else fn(mesh.primitives[primIdx]);
        mesh.Touch();
        MarkChanged();
    };
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("スムーズ角度", &m_normalAngle, 0.0f, 180.0f, "%.0f°");
    if (ImGui::Button("法線を再計算 (スムーズ)")) apply("法線再計算", [&](Primitive& p) { Ops::RecalculateNormals(p, true, m_normalAngle); if (p.HasTangents()) Ops::GenerateTangents(p); });
    ImGui::SameLine();
    if (ImGui::Button("フラット")) apply("フラット法線", [&](Primitive& p) { Ops::RecalculateNormals(p, false); if (p.HasTangents()) Ops::GenerateTangents(p); });
    if (ImGui::Button("法線を反転")) apply("法線反転", [](Primitive& p) { Ops::FlipNormals(p); });
    ImGui::SameLine();
    if (ImGui::Button("面の向きを反転")) apply("巻き順反転", [](Primitive& p) { Ops::ReverseWinding(p); });
    if (ImGui::Button("UV: V 反転")) apply("UV V反転", [](Primitive& p) { Ops::FlipUV(p, false, true); });
    ImGui::SameLine();
    if (ImGui::Button("UV: U 反転")) apply("UV U反転", [](Primitive& p) { Ops::FlipUV(p, true, false); });
    ImGui::SameLine();
    if (ImGui::Button("UV1 → UV0 にコピー")) apply("UVコピー", [](Primitive& p) { if (p.HasUV1()) p.uv0 = p.uv1; });
    if (ImGui::Button("接線を生成")) apply("接線生成", [](Primitive& p) { if (!Ops::GenerateTangents(p)) LOG_WARN("接線の生成には UV0 と法線が必要です"); });
    ImGui::SameLine();
    if (ImGui::Button("接線を削除")) apply("接線削除", [](Primitive& p) { p.tangents.clear(); });
    ImGui::SameLine();
    if (ImGui::Button("頂点カラー削除")) apply("頂点カラー削除", [](Primitive& p) { p.colors.clear(); });
    ImGui::SetNextItemWidth(120);
    ImGui::InputFloat("結合距離", &m_weldEps, 0, 0, "%.6g");
    ImGui::SameLine();
    if (ImGui::Button("頂点を結合")) apply("頂点結合", [&](Primitive& p) { int n = Ops::WeldVertices(p, m_weldEps); LOG_INFO("%d 頂点を結合しました", n); });
    if (ImGui::Button("縮退三角形を削除")) apply("縮退削除", [](Primitive& p) { int n = Ops::RemoveDegenerateTriangles(p); LOG_INFO("%d 個の縮退三角形を削除しました", n); });
    if (mesh.HasSkin()) {
        ImGui::SeparatorText("スキンウェイト");
        if (ImGui::Button("ウェイトを正規化")) apply("ウェイト正規化", [](Primitive& p) { LOG_INFO("%d 頂点を正規化", Ops::NormalizeWeights(p)); });
        ImGui::SetNextItemWidth(100);
        ImGui::InputFloat("閾値", &m_pruneThreshold, 0, 0, "%.4f");
        ImGui::SameLine();
        if (ImGui::Button("小さいウェイトを除去")) apply("ウェイト除去", [&](Primitive& p) { LOG_INFO("%d 個のウェイトを除去", Ops::PruneWeights(p, m_pruneThreshold)); });
    }
    if (mesh.MorphTargetCount() > 0) {
        ImGui::SeparatorText("モーフ");
        if (ImGui::Button("モーフターゲットを削除")) apply("モーフ削除", [&](Primitive& p) { p.targets.clear(); mesh.morphWeights.clear(); });
    }
}

// ---------------------------------------------------------------------------
// マテリアル
// ---------------------------------------------------------------------------
void Editor::DrawMaterials() {
    if (!ImGui::Begin(PanelTitle(ICON_MATERIAL, "マテリアル", "Materials").c_str(), &m_panels.materials)) { ImGui::End(); return; }
    PanelHint("表面の色と質感の設定です。テクスチャパネルの画像をスロットへドラッグすると割り当てられます");
    DrawModelFilterBar();
    if (ImGui::Button("新規")) {
        Checkpoint("マテリアル作成");
        m_selMaterial = Ops::AddMaterial(m_scene, "Material_" + std::to_string(m_scene.materials.size()));
        MarkChanged();
    }
    ImGui::SameLine();
    if (ImGui::Button("複製") && m_selMaterial >= 0) {
        Checkpoint("マテリアル複製");
        Material m = m_scene.materials[m_selMaterial];
        m.name += "_copy";
        m_scene.materials.push_back(m);
        m_selMaterial = (int)m_scene.materials.size() - 1;
        MarkChanged();
    }
    ImGui::SameLine();
    if (ImGui::Button("削除") && m_selMaterial >= 0) {
        Checkpoint("マテリアル削除");
        m_scene.RemoveMaterial(m_selMaterial);
        m_selMaterial = std::min(m_selMaterial, (int)m_scene.materials.size() - 1);
        MarkChanged();
    }
    ImGui::SameLine();
    if (ImGui::Button("選択ノードに割り当て") && m_selMaterial >= 0) {
        Checkpoint("マテリアル割り当て");
        int count = 0;
        for (int s : m_selection) {
            int me = m_scene.nodes[s].mesh;
            if (me < 0) continue;
            for (auto& p : m_scene.meshes[me].primitives) { p.material = m_selMaterial; ++count; }
            m_scene.meshes[me].Touch();
        }
        LOG_INFO("%d プリミティブに割り当てました", count);
        MarkChanged();
    }

    float listH = std::min(180.0f, ImGui::GetContentRegionAvail().y * 0.35f);
    ImGui::BeginChild("matlist", ImVec2(0, listH), ImGuiChildFlags_Borders);
    const auto matMask = Membership().materials;
    for (int i = 0; i < (int)m_scene.materials.size(); ++i) {
        if (!InMask(matMask, i)) continue;
        const Material& m = m_scene.materials[i];
        ImGui::PushID(i);
        ImVec4 c(m.baseColor.r, m.baseColor.g, m.baseColor.b, 1);
        ImGui::ColorButton("##c", c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(14, 14));
        ImGui::SameLine();
        int texCount = 0;
        for (auto& s : m.slots) if (s.Valid()) ++texCount;
        char label[256];
        snprintf(label, sizeof(label), "%s%s", m.name.c_str(), texCount ? "" : "");
        if (ImGui::Selectable(label, m_selMaterial == i)) m_selMaterial = i;
        if (texCount) {
            ImGui::SameLine();
            ImGui::TextDisabled("(tex %d)", texCount);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (m_selMaterial >= 0 && m_selMaterial < (int)m_scene.materials.size()) DrawMaterialEditor(m_selMaterial);
    ImGui::End();
}

bool Editor::DrawTextureSlot(Material& m, int slot) {
    TextureSlot& s = m.slots[slot];
    bool changed = false;
    ImGui::PushID(slot);
    ImTextureID id = s.texture >= 0 ? TexId(s.texture) : (ImTextureID)0;
    const float th = 48.0f;
    if (id) {
        ImGui::Image(id, ImVec2(th, th));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Image(id, ImVec2(256, 256));
            ImGui::EndTooltip();
        }
        if (ImGui::IsItemClicked()) { m_selTexture = s.texture; m_panels.textures = true; }
    } else {
        ImGui::Button(s.texture >= 0 ? "欠落" : "なし", ImVec2(th, th));
    }
    // テクスチャパネルからドラッグ&ドロップで割り当て
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ME_TEXTURE")) {
            Checkpoint("テクスチャ割り当て");
            s.texture = *(const int*)pl->Data;
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s", TexSlotName(slot));
    const char* cur = s.texture >= 0 && s.texture < (int)m_scene.textures.size() ? m_scene.textures[s.texture].name.c_str() : "(なし)";
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##tex", cur)) {
        if (ImGui::Selectable("(なし)", s.texture < 0)) { Checkpoint("テクスチャ解除"); s.texture = -1; changed = true; }
        for (int t = 0; t < (int)m_scene.textures.size(); ++t) {
            ImGui::PushID(t);
            ImTextureID tid = TexId(t);
            if (tid) { ImGui::Image(tid, ImVec2(16, 16)); ImGui::SameLine(); }
            if (ImGui::Selectable(m_scene.textures[t].name.c_str(), s.texture == t)) { Checkpoint("テクスチャ割り当て"); s.texture = t; changed = true; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("ファイル...")) {
        std::string p = Dialogs::OpenFile(kImageFilter, TexSlotName(slot));
        if (!p.empty()) {
            int t = LoadTextureFile(p);
            if (t >= 0) { s.texture = t; changed = true; }
        }
    }
    if (s.texture >= 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("×")) { Checkpoint("テクスチャ解除"); s.texture = -1; changed = true; }
        ImGui::SetNextItemWidth(60);
        int uv = s.uvSet;
        if (ImGui::Combo("UV", &uv, "UV0\0UV1\0")) { Checkpoint("UVセット"); s.uvSet = uv; changed = true; }
    }
    ImGui::EndGroup();
    ImGui::PopID();
    if (changed) MarkChanged();
    return changed;
}

void Editor::DrawMaterialEditor(int idx) {
    Material& m = m_scene.materials[idx];
    ImGui::SetNextItemWidth(-60);
    EditValue("マテリアル名", m.name, [&] { return ImGui::InputText("名前##mat", &m.name); });
    ImGui::SeparatorText("PBR (Metallic-Roughness)");
    EditValue("ベースカラー", m.baseColor, [&] { return ImGui::ColorEdit4("ベースカラー", &m.baseColor.x, ImGuiColorEditFlags_AlphaBar); });
    EditValue("メタリック", m.metallic, [&] { return ImGui::SliderFloat("メタリック", &m.metallic, 0.0f, 1.0f); });
    EditValue("ラフネス", m.roughness, [&] { return ImGui::SliderFloat("ラフネス", &m.roughness, 0.0f, 1.0f); });
    EditValue("発光色", m.emissive, [&] { return ImGui::ColorEdit3("発光色", &m.emissive.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float); });
    EditValue("発光強度", m.emissiveStrength, [&] { return ImGui::DragFloat("発光強度", &m.emissiveStrength, 0.05f, 0.0f, 100.0f); });
    int alpha = (int)m.alphaMode;
    if (EditValue("アルファモード", alpha, [&] { return ImGui::Combo("アルファモード", &alpha, "OPAQUE (不透明)\0MASK (切り抜き)\0BLEND (半透明)\0"); }))
        m.alphaMode = (AlphaMode)alpha;
    if (m.alphaMode == AlphaMode::Mask) EditValue("カットオフ", m.alphaCutoff, [&] { return ImGui::SliderFloat("カットオフ", &m.alphaCutoff, 0.0f, 1.0f); });
    EditValue("両面", m.doubleSided, [&] { return ImGui::Checkbox("両面 (Double Sided)", &m.doubleSided); });
    ImGui::SameLine();
    EditValue("Unlit", m.unlit, [&] { return ImGui::Checkbox("Unlit (KHR_materials_unlit)", &m.unlit); });
    EditValue("法線スケール", m.normalScale, [&] { return ImGui::DragFloat("法線スケール", &m.normalScale, 0.01f, -4.0f, 4.0f); });
    EditValue("AO 強度", m.occlusionStrength, [&] { return ImGui::SliderFloat("AO 強度", &m.occlusionStrength, 0.0f, 1.0f); });

    ImGui::SeparatorText("テクスチャ (サムネイルはテクスチャパネルからドロップ可)");
    for (int s = 0; s < Slot_Count; ++s) DrawTextureSlot(m, s);
    if (m.slots[Slot_MetallicRoughness].Valid())
        ImGui::TextDisabled("glTF: G チャンネル = ラフネス, B チャンネル = メタリック");

    ImGui::SeparatorText("使用箇所");
    int uses = 0;
    for (size_t mi = 0; mi < m_scene.meshes.size(); ++mi)
        for (size_t p = 0; p < m_scene.meshes[mi].primitives.size(); ++p)
            if (m_scene.meshes[mi].primitives[p].material == idx) {
                ImGui::BulletText("%s [%zu]", m_scene.meshes[mi].name.c_str(), p);
                ++uses;
            }
    if (!uses) ImGui::TextDisabled("未使用");
}

// ---------------------------------------------------------------------------
// テクスチャ
// ---------------------------------------------------------------------------
void Editor::DrawTextures() {
    if (!ImGui::Begin(PanelTitle(ICON_TEXTURE, "テクスチャ", "Textures").c_str(), &m_panels.textures)) { ImGui::End(); return; }
    PanelHint("モデルが使う画像の一覧です。見つからない画像は、フォルダを指定して探し直せます");
    DrawModelFilterBar();
    if (ImGui::Button("読み込み...")) {
        std::string p = Dialogs::OpenFile(kImageFilter, "テクスチャを読み込む");
        if (!p.empty()) LoadTextureFile(p);
    }
    ImGui::SameLine();
    int missing = 0;
    for (auto& t : m_scene.textures) if (t.missing || !t.image) ++missing;
    ImGui::BeginDisabled(missing == 0);
    if (ImGui::Button("不足テクスチャを再リンク...")) {
        std::string dir = Dialogs::PickFolder("テクスチャのあるフォルダを選択");
        if (!dir.empty()) {
            RelinkMissingTextures(dir);
            m_importOpts.textureSearchDirs.push_back(dir);
        }
    }
    ImGui::EndDisabled();
    if (missing) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "欠落 %d", missing);
    }
    ImGui::SameLine();
    if (ImGui::Button("未使用を削除")) {
        Checkpoint("未使用テクスチャ削除");
        int n = 0;
        for (int t = (int)m_scene.textures.size() - 1; t >= 0; --t) {
            bool used = false;
            for (auto& m : m_scene.materials) for (auto& s : m.slots) if (s.texture == t) used = true;
            if (!used) { m_scene.RemoveTexture(t); ++n; }
        }
        LOG_INFO("%d 個のテクスチャを削除しました", n);
        ValidateSelection();
        MarkChanged();
    }

    // サムネイル一覧
    float listH = std::min(200.0f, ImGui::GetContentRegionAvail().y * 0.4f);
    ImGui::BeginChild("texgrid", ImVec2(0, listH), ImGuiChildFlags_Borders);
    float cell = 72.0f;
    int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (cell + 8)));
    const auto texMask = Membership().textures;
    int shown = 0;
    for (int i = 0; i < (int)m_scene.textures.size(); ++i) {
        if (!InMask(texMask, i)) continue;
        const Texture& t = m_scene.textures[i];
        ImGui::PushID(i);
        if (shown++ % cols) ImGui::SameLine();
        ImGui::BeginGroup();
        ImTextureID id = TexId(i);
        bool sel = m_selTexture == i;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.6f, 0.1f, 1));
        if (id) {
            if (ImGui::ImageButton("##t", id, ImVec2(cell, cell))) m_selTexture = i;
        } else {
            if (ImGui::Button(t.missing ? "欠落" : "?", ImVec2(cell + 8, cell + 8))) m_selTexture = i;
        }
        if (sel) ImGui::PopStyleColor();
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("ME_TEXTURE", &i, sizeof(int));
            if (id) ImGui::Image(id, ImVec2(48, 48));
            ImGui::Text("%s", t.name.c_str());
            ImGui::EndDragDropSource();
        }
        std::string nm = t.name.size() > 10 ? t.name.substr(0, 9) + "…" : t.name;
        ImGui::TextColored(t.missing ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.8f, 0.8f, 0.8f, 1), "%s", nm.c_str());
        ImGui::EndGroup();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%dx%d %s", t.name.c_str(), t.Width(), t.Height(), t.mimeType.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (m_selTexture < 0 || m_selTexture >= (int)m_scene.textures.size()) { ImGui::End(); return; }
    Texture& t = m_scene.textures[m_selTexture];
    ImGui::SetNextItemWidth(-60);
    EditValue("テクスチャ名", t.name, [&] { return ImGui::InputText("名前##tex", &t.name); });
    ImGui::Text("%d x %d  %s  %s", t.Width(), t.Height(), t.mimeType.c_str(), t.embedded ? "(埋め込み)" : "");
    if (!t.sourcePath.empty()) ImGui::TextWrapped("パス: %s", t.sourcePath.c_str());
    if (!t.originalUri.empty()) ImGui::TextDisabled("元の参照: %s", t.originalUri.c_str());
    if (t.missing) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "ファイルが見つかりません。「置き換え」または「再リンク」してください");

    if (ImGui::Button("置き換え...")) {
        std::string p = Dialogs::OpenFile(kImageFilter, "置き換える画像");
        if (!p.empty()) {
            Texture nt;
            std::string err;
            if (ImageIO::LoadFile(p, nt, &err)) {
                Checkpoint("テクスチャ置き換え");
                nt.name = t.name;
                nt.originalUri = FileUtil::Filename(p);
                nt.wrapS = t.wrapS;
                nt.wrapT = t.wrapT;
                t = nt;
                MarkChanged();
            } else LOG_ERROR("%s", err.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("PNG で保存...") && t.image) {
        std::string p = Dialogs::SaveFile("PNG\0*.png\0", FileUtil::SanitizeFilename(t.name) + ".png", ".png");
        if (!p.empty()) {
            if (ImageIO::SavePNG(p, *t.image)) LOG_INFO("保存しました: %s", p.c_str());
            else LOG_ERROR("保存に失敗しました: %s", p.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("元データを保存...") && t.encoded) {
        std::string ext = ImageIO::ExtensionFromMime(t.mimeType);
        std::string p = Dialogs::SaveFile("All\0*.*\0", FileUtil::SanitizeFilename(t.name) + ext, ext.c_str());
        if (!p.empty()) FileUtil::WriteBinary(p, t.encoded->data(), t.encoded->size());
    }
    ImGui::SameLine();
    if (ImGui::Button("削除##tex")) {
        Checkpoint("テクスチャ削除");
        m_scene.RemoveTexture(m_selTexture);
        ValidateSelection();
        MarkChanged();
        ImGui::End();
        return;
    }
    int wrapS = t.wrapS == WrapMode::Repeat ? 0 : t.wrapS == WrapMode::ClampToEdge ? 1 : 2;
    int wrapT = t.wrapT == WrapMode::Repeat ? 0 : t.wrapT == WrapMode::ClampToEdge ? 1 : 2;
    static const WrapMode modes[] = { WrapMode::Repeat, WrapMode::ClampToEdge, WrapMode::MirroredRepeat };
    ImGui::SetNextItemWidth(120);
    if (EditValue("ラップ S", wrapS, [&] { return ImGui::Combo("ラップ S", &wrapS, "Repeat\0Clamp\0Mirror\0"); })) { t.wrapS = modes[wrapS]; t.uid = NewUid(); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (EditValue("ラップ T", wrapT, [&] { return ImGui::Combo("ラップ T", &wrapT, "Repeat\0Clamp\0Mirror\0"); })) { t.wrapT = modes[wrapT]; t.uid = NewUid(); }
    if (EditValue("フィルタ", t.linearFilter, [&] { return ImGui::Checkbox("リニアフィルタ", &t.linearFilter); })) t.uid = NewUid();
    ImGui::SameLine();
    if (EditValue("ミップマップ", t.generateMips, [&] { return ImGui::Checkbox("ミップマップ", &t.generateMips); })) t.uid = NewUid();

    // 使用箇所
    std::string usedBy;
    for (auto& m : m_scene.materials)
        for (int s = 0; s < Slot_Count; ++s)
            if (m.slots[s].texture == m_selTexture) usedBy += m.name + " (" + TexSlotName(s) + ")  ";
    ImGui::TextWrapped("使用: %s", usedBy.empty() ? "なし" : usedBy.c_str());

    // プレビュー (チャンネル別)
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("チャンネル", &m_texChannel, "RGBA\0R\0G\0B\0A (グレー表示は非対応・アルファ値として表示)\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("ズーム", &m_texZoom, 0.1f, 4.0f);
    ImTextureID id = TexId(m_selTexture);
    if (id && t.Width() > 0) {
        float avail = ImGui::GetContentRegionAvail().x;
        float w = std::min(avail, (float)t.Width()) * m_texZoom;
        float h = w * t.Height() / (float)t.Width();
        ImVec4 tint(1, 1, 1, 1);
        if (m_texChannel == 1) tint = ImVec4(1, 0, 0, 1);
        if (m_texChannel == 2) tint = ImVec4(0, 1, 0, 1);
        if (m_texChannel == 3) tint = ImVec4(0, 0, 1, 1);
        ImGui::BeginChild("preview", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        // 透明部分が分かるようにチェッカー背景
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)m_renderer.CheckerTexture(), p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(w / 32, h / 32));
        if (m_texChannel == 4) {
            ImGui::ImageWithBg(id, ImVec2(w, h), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), ImVec4(0, 0, 0, 1));
        } else {
            ImGui::ImageWithBg(id, ImVec2(w, h), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
        }
        if (ImGui::IsItemHovered() && t.image) {
            ImVec2 mp = ImGui::GetMousePos();
            int px = (int)((mp.x - p0.x) / w * t.Width()), py = (int)((mp.y - p0.y) / h * t.Height());
            if (px >= 0 && py >= 0 && px < t.Width() && py < t.Height()) {
                const uint8_t* c = &t.image->rgba[((size_t)py * t.Width() + px) * 4];
                ImGui::SetTooltip("(%d, %d)  RGBA %d %d %d %d\nUV %.4f %.4f", px, py, c[0], c[1], c[2], c[3], (px + 0.5f) / t.Width(), (py + 0.5f) / t.Height());
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// UV ビューア
// ---------------------------------------------------------------------------
void Editor::DrawUVViewer() {
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_GRID, "UV ビューア", "UVViewer").c_str(), &m_panels.uvViewer)) { ImGui::End(); return; }
    if (m_selMesh < 0 || m_selMesh >= (int)m_scene.meshes.size()) {
        ImGui::TextDisabled("メッシュを選択してください");
        ImGui::End();
        return;
    }
    Mesh& mesh = m_scene.meshes[m_selMesh];
    static int uvSet = 0;
    static bool showTex = true;
    static bool showOutOfRange = true;
    ImGui::Text("%s", mesh.name.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::Combo("##uvset", &uvSet, "UV0\0UV1\0");
    ImGui::SameLine();
    ImGui::Checkbox("テクスチャ", &showTex);
    ImGui::SameLine();
    ImGui::Checkbox("0-1 外を強調", &showOutOfRange);
    if (mesh.primitives.empty()) { ImGui::End(); return; }
    m_selPrim = std::clamp(m_selPrim, 0, (int)mesh.primitives.size() - 1);
    const Primitive& p = mesh.primitives[m_selPrim];
    const auto& uv = uvSet == 0 ? p.uv0 : p.uv1;
    if (uv.size() != p.positions.size()) {
        ImGui::TextDisabled("この UV セットはありません");
        ImGui::End();
        return;
    }
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float sz = std::max(64.0f, std::min(avail.x, avail.y));
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + sz, o.y + sz), IM_COL32(40, 40, 44, 255));
    int mat = p.material;
    if (showTex && mat >= 0 && mat < (int)m_scene.materials.size()) {
        int tex = m_scene.materials[mat].slots[Slot_BaseColor].texture;
        ImTextureID id = TexId(tex);
        if (id) dl->AddImage(id, o, ImVec2(o.x + sz, o.y + sz), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 160));
    }
    for (int g = 0; g <= 4; ++g) {
        float f = g / 4.0f * sz;
        dl->AddLine(ImVec2(o.x + f, o.y), ImVec2(o.x + f, o.y + sz), IM_COL32(90, 90, 90, 255));
        dl->AddLine(ImVec2(o.x, o.y + f), ImVec2(o.x + sz, o.y + f), IM_COL32(90, 90, 90, 255));
    }
    // glTF の UV は左上原点なのでそのまま描画
    size_t triCount = p.indices.size() / 3;
    size_t step = std::max<size_t>(1, triCount / 60000);
    int outside = 0;
    for (size_t t = 0; t < triCount; t += step) {
        ImVec2 pts[3];
        bool out = false;
        for (int k = 0; k < 3; ++k) {
            uint32_t vi = p.indices[t * 3 + k];
            if (vi >= uv.size()) continue;
            glm::vec2 u = uv[vi];
            if (u.x < 0 || u.x > 1 || u.y < 0 || u.y > 1) out = true;
            pts[k] = ImVec2(o.x + u.x * sz, o.y + u.y * sz);
        }
        if (out) ++outside;
        ImU32 col = (out && showOutOfRange) ? IM_COL32(255, 120, 80, 200) : IM_COL32(120, 220, 255, 180);
        dl->AddPolyline(pts, 3, col, ImDrawFlags_Closed, 1.0f);
    }
    ImGui::Dummy(ImVec2(sz, sz));
    ImGui::Text("三角形 %zu  (0-1 外: %d)", triCount, outside);
    ImGui::End();
}
