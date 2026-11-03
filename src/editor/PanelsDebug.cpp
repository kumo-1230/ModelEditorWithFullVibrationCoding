// コンソール / 検証 / 統計 / シーン情報 / 履歴 / 入出力設定 / 一括リネーム / 設定 / ヘルプ / ダンプ
#include "Editor.h"
#include "app/SceneDump.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include "util/Dialogs.h"
#include "util/FileUtil.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// コンソール
// ---------------------------------------------------------------------------
void Editor::DrawConsole() {
    if (!ImGui::Begin(PanelTitle(ICON_CONSOLE, "コンソール", "Console").c_str(), &m_panels.console)) { ImGui::End(); return; }
    PanelHint("処理の記録 (ログ) です。読み込み・書き出しの結果や、エラーの詳しい理由が出ます");
    static const char* names[] = { "Debug", "Info", "Warning", "Error" };
    static const ImVec4 colors[] = { ImVec4(0.6f, 0.6f, 0.6f, 1), ImVec4(0.85f, 0.85f, 0.85f, 1), ImVec4(1.0f, 0.8f, 0.3f, 1), ImVec4(1.0f, 0.4f, 0.4f, 1) };
    for (int i = 0; i < 4; ++i) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors[i]);
        char l[64];
        snprintf(l, sizeof(l), "%s (%d)", names[i], Log::Get().CountOf((LogLevel)i));
        ImGui::Checkbox(l, &m_consoleShow[i]);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(Px(160));
    ImGui::InputTextWithHint("##cf", "フィルタ", &m_consoleFilter);
    ImGui::SameLine();
    ImGui::Checkbox("自動スクロール", &m_consoleAutoScroll);
    ImGui::SameLine();
    if (ImGui::Button("クリア")) Log::Get().Clear();
    ImGui::SameLine();
    if (ImGui::Button("コピー")) {
        std::string s;
        Log::Get().Visit([&](const std::vector<LogEntry>& es) { for (auto& e : es) s += e.text + "\n"; });
        ImGui::SetClipboardText(s.c_str());
    }
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    Log::Get().Visit([&](const std::vector<LogEntry>& entries) {
        std::vector<int> visible;
        visible.reserve(entries.size());
        for (int i = 0; i < (int)entries.size(); ++i) {
            const auto& e = entries[i];
            if (!m_consoleShow[(int)e.level]) continue;
            if (!m_consoleFilter.empty() && e.text.find(m_consoleFilter) == std::string::npos) continue;
            visible.push_back(i);
        }
        ImGuiListClipper clip;
        clip.Begin((int)visible.size());
        while (clip.Step())
            for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
                const auto& e = entries[visible[k]];
                ImGui::TextDisabled("%8.2f", e.time);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, colors[(int)e.level]);
                ImGui::TextUnformatted(e.text.c_str());
                ImGui::PopStyleColor();
            }
    });
    size_t rev = Log::Get().Revision();
    if (m_consoleAutoScroll && rev != m_consoleLastRev) ImGui::SetScrollHereY(1.0f);
    m_consoleLastRev = rev;
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// 検証
// ---------------------------------------------------------------------------
void Editor::DrawValidation() {
    if (!ImGui::Begin(PanelTitle(ICON_CHECK, "検証", "Validation").c_str(), &m_panels.validation)) { ImGui::End(); return; }
    PanelHint("書き出す前に直しておきたい問題の一覧です。「修正」ボタンで自動修正できるものもあります");
    DrawModelFilterBar();
    if (ImGui::Button("今すぐ検証")) { m_issues = ValidateScene(m_scene); m_issuesRevision = m_sceneRevision; }
    ImGui::SameLine();
    ImGui::Checkbox("自動", &m_autoValidate);
    ImGui::SameLine();
    int fixable = 0, errors = 0, warnings = 0, infos = 0;
    for (auto& is : m_issues) {
        if (is.fix != FixAction::None) ++fixable;
        if (is.severity == Severity::Error) ++errors;
        else if (is.severity == Severity::Warning) ++warnings;
        else ++infos;
    }
    ImGui::BeginDisabled(fixable == 0);
    if (ImGui::Button("すべて自動修正")) {
        Checkpoint("自動修正");
        int n = 0;
        for (auto& is : ValidateScene(m_scene)) if (is.fix != FixAction::None && ApplyFix(m_scene, is)) ++n;
        LOG_INFO("%d 件の問題を修正しました", n);
        ValidateSelection();
        MarkChanged();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "エラー %d", errors);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "警告 %d", warnings);
    ImGui::SameLine();
    ImGui::TextDisabled("情報 %d", infos);
    static bool showInfo = true;
    ImGui::SameLine();
    ImGui::Checkbox("情報を表示", &showInfo);

    if (ImGui::BeginTable("issues", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("重要度", ImGuiTableColumnFlags_WidthFixed, Px(56));
        ImGui::TableSetupColumn("分類", ImGuiTableColumnFlags_WidthFixed, Px(90));
        ImGui::TableSetupColumn("内容", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("修正", ImGuiTableColumnFlags_WidthFixed, Px(60));
        ImGui::TableHeadersRow();
        int toFix = -1, revealIssue = -1;
        // 表示する行だけを先に集め、見えている行だけを描く (カリング)
        std::vector<int> rows;
        for (int i = 0; i < (int)m_issues.size(); ++i) {
            const Issue& is = m_issues[i];
            if (!showInfo && is.severity == Severity::Info) continue;
            if (!IssueInActiveModel(is)) continue;
            rows.push_back(i);
        }
        ImGuiListClipper clip;
        clip.Begin((int)rows.size());
        while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            const int i = rows[row];
            const Issue& is = m_issues[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            if (is.severity == Severity::Error) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "エラー");
            else if (is.severity == Severity::Warning) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "警告");
            else ImGui::TextDisabled("情報");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(is.category.c_str());
            ImGui::TableNextColumn();
            if (ImGui::Selectable(is.message.c_str(), false, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    revealIssue = i; // 関連するパネル・表示を開く (ループの後で実行)
                } else {
                    if (is.node >= 0 && is.node < (int)m_scene.nodes.size()) { Select(is.node); FocusNode(is.node); }
                    if (is.mesh >= 0) {
                        m_selMesh = is.mesh;
                        auto users = m_scene.NodesUsingMesh(is.mesh);
                        if (!users.empty()) Select(users[0]);
                    }
                    if (is.material >= 0) m_selMaterial = is.material;
                    if (is.texture >= 0) m_selTexture = is.texture;
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("クリック: 対象を選ぶ\nダブルクリック: 関連するパネルと確認用の表示を開く");
            ImGui::TableNextColumn();
            if (is.fix != FixAction::None && ImGui::SmallButton("修正")) toFix = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (revealIssue >= 0 && revealIssue < (int)m_issues.size()) RevealIssue(Issue(m_issues[revealIssue]));
        if (toFix >= 0) {
            Checkpoint("修正: " + m_issues[toFix].message);
            if (ApplyFix(m_scene, m_issues[toFix])) LOG_INFO("修正しました: %s", m_issues[toFix].message.c_str());
            ValidateSelection();
            MarkChanged();
            m_issues = ValidateScene(m_scene);
            m_issuesRevision = m_sceneRevision;
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// 統計 / デバッグ
// ---------------------------------------------------------------------------
void Editor::DrawStats() {
    if (!ImGui::Begin(PanelTitle(ICON_STATS, "統計 / デバッグ", "Stats").c_str(), &m_panels.stats)) { ImGui::End(); return; }
    PanelHint("描画の負荷・メモリ・フレーム時間の統計です");
    ImGuiIO& io = ImGui::GetIO();
    std::vector<float> ft(m_frameTimes.begin(), m_frameTimes.end());
    float maxFt = 0, avg = 0;
    for (float f : ft) { maxFt = std::max(maxFt, f); avg += f; }
    if (!ft.empty()) avg /= ft.size();
    char overlay[64];
    snprintf(overlay, sizeof(overlay), "%.1f FPS  平均 %.2fms  最大 %.2fms", io.Framerate, avg, maxFt);
    ImGui::PlotLines("##ft", ft.data(), (int)ft.size(), 0, overlay, 0.0f, std::max(33.3f, maxFt), ImVec2(-1, 60));
    ImGui::Text("UI 構築時間: %.2f ms", m_lastFrameMs);

    const RenderStats& st = m_renderer.Stats();
    if (ImGui::BeginTable("st", 2, ImGuiTableFlags_BordersInnerH)) {
        auto row = [](const char* k, const char* fmt, auto... a) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            ImGui::Text(fmt, a...);
        };
        row("描画コール", "%d", st.drawCalls);
        row("描画三角形 / 頂点", "%zu / %zu", st.triangles, st.vertices);
        row("CPU 変形メッシュ", "%d (%.2f ms)", st.deformedMeshes, st.deformMs);
        row("GPU バッファ", "%.2f MB (%d)", st.gpuBufferBytes / 1048576.0, st.cachedMeshes);
        row("GPU テクスチャ", "%.2f MB (%d)", st.gpuTextureBytes / 1048576.0, st.cachedTextures);
        row("ノード", "%zu", m_scene.nodes.size());
        row("メッシュ / 頂点 / 三角形", "%zu / %zu / %zu", m_scene.meshes.size(), m_scene.TotalVertices(), m_scene.TotalTriangles());
        row("マテリアル / テクスチャ", "%zu / %zu", m_scene.materials.size(), m_scene.textures.size());
        row("スキン / アニメーション", "%zu / %zu", m_scene.skins.size(), m_scene.animations.size());
        row("シーン AABB", "(%.3f %.3f %.3f) - (%.3f %.3f %.3f)", m_sceneMin.x, m_sceneMin.y, m_sceneMin.z, m_sceneMax.x, m_sceneMax.y, m_sceneMax.z);
        row("シーンサイズ", "%.4f x %.4f x %.4f", m_sceneMax.x - m_sceneMin.x, m_sceneMax.y - m_sceneMin.y, m_sceneMax.z - m_sceneMin.z);
        row("Undo / Redo", "%zu / %zu (上限 %zu)", m_undo.UndoEntries().size(), m_undo.RedoEntries().size(), m_undo.MaxEntries());
        row("シーンリビジョン", "%llu", (unsigned long long)m_sceneRevision);
        row("カメラ", "距離 %.3f  near %.4g  far %.4g", m_camera.distance, m_camera.nearClip, m_camera.farClip);
        row("カメラ注視点", "%.3f %.3f %.3f", m_camera.target.x, m_camera.target.y, m_camera.target.z);
        ImGui::EndTable();
    }
    ImGui::SeparatorText("GPU (Direct3D 11)");
    ImGui::TextWrapped("%s", m_renderer.GPUInfo().c_str());
    ImGui::SeparatorText("デバッグ表示");
    ImGui::Checkbox("法線", &m_rs.showNormals);
    ImGui::SameLine();
    ImGui::Checkbox("接線", &m_rs.showTangents);
    ImGui::SameLine();
    ImGui::Checkbox("AABB", &m_rs.showBounds);
    ImGui::SameLine();
    ImGui::Checkbox("ノード軸", &m_rs.showNodeAxes);
    ImGui::Checkbox("選択のみ", &m_rs.debugSelectedOnly);
    ImGui::SameLine();
    ImGui::Checkbox("スキニング有効", &m_rs.skinningEnabled);
    ImGui::SetNextItemWidth(Px(200));
    if (ImGui::BeginCombo("表示モード", RenderModeName(m_rs.mode))) {
        for (int i = 0; i < RM_Count; ++i) if (ImGui::Selectable(RenderModeName(i), m_rs.mode == i)) m_rs.mode = i;
        ImGui::EndCombo();
    }
    if (ImGui::Button("GPU キャッシュをクリア")) m_renderer.ClearCaches();
    ImGui::SameLine();
    if (ImGui::Button("シーンダンプ")) m_panels.dump = true;
    ImGui::SameLine();
    ImGui::Checkbox("ImGui メトリクス", &m_panels.imguiMetrics);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// シーン情報
// ---------------------------------------------------------------------------
void Editor::DrawSceneInfo() {
    if (!ImGui::Begin(PanelTitle(ICON_INFO, "シーン情報", "SceneInfo").c_str(), &m_panels.sceneInfo)) { ImGui::End(); return; }
    PanelHint("読み込んだファイルの情報 (形式・単位・座標軸・数) です");
    EllipsisText("ファイル: ", ModelInfo(ActiveModel()).sourcePath.empty() ? std::string("(なし)") : ModelInfo(ActiveModel()).sourcePath);
    ImGui::Text("形式: %s", m_scene.info.sourceFormat.c_str());
    static const char* ax[] = { "X", "Y", "Z" };
    ImGui::Text("元の UpAxis: %s   UnitScaleFactor: %g (%s)", ax[std::clamp(m_scene.info.upAxis, 0, 2)], m_scene.info.unitScaleFactor,
                std::abs(m_scene.info.unitScaleFactor - 1.0) < 1e-6 ? "cm" : std::abs(m_scene.info.unitScaleFactor - 100.0) < 1e-6 ? "m" : "?");
    ImGui::TextDisabled("※ インポート設定により読み込み時に m / Y-up へ変換されている場合があります");
    if (ImGui::BeginTable("meta", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("キー");
        ImGui::TableSetupColumn("値");
        ImGui::TableHeadersRow();
        for (auto& [k, v] : m_scene.info.metadata) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// 履歴
// ---------------------------------------------------------------------------
void Editor::DrawHistory() {
    if (!ImGui::Begin(PanelTitle(ICON_HISTORY, "履歴", "History").c_str(), &m_panels.history)) { ImGui::End(); return; }
    PanelHint("これまでの操作の一覧です。行をクリックすると、その時点まで戻れます");
    ImGui::TextDisabled("クリックでその時点まで戻る / 進む");
    const auto& undo = m_undo.UndoEntries();
    const auto& redo = m_undo.RedoEntries();
    int target = 0; // 正: undo 回数, 負: redo 回数
    for (int i = 0; i < (int)undo.size(); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(undo[i].label.c_str())) target = (int)undo.size() - i;
        ImGui::PopID();
    }
    ImGui::Selectable("● 現在", true);
    for (int i = (int)redo.size() - 1; i >= 0; --i) {
        ImGui::PushID(1000 + i);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1));
        if (ImGui::Selectable(redo[i].label.c_str())) target = -((int)redo.size() - i);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (target > 0) { for (int k = 0; k < target; ++k) m_undo.Undo(m_scene); ValidateSelection(); MarkChanged(); }
    if (target < 0) { for (int k = 0; k < -target; ++k) m_undo.Redo(m_scene); ValidateSelection(); MarkChanged(); }
    ImGui::Separator();
    if (ImGui::Button("履歴をクリア")) m_undo.Clear();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// インポート / エクスポート設定
// ---------------------------------------------------------------------------
void Editor::DrawImportWindow() {
    ImGui::SetNextWindowSize(ImVec2(Px(460), Px(520)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_IMPORT, "インポート設定", "ImportSettings").c_str(), &m_panels.importWin)) { ImGui::End(); return; }
    ImportOptions& o = m_importOpts;
    ImGui::Combo("読み込みモード", &m_importMode, "シーンを置き換え\0現在のシーンに追加\0アニメーションのみ (名前で対応付け)\0");
    ImGui::Combo("ドラッグ&ドロップ時", &m_dropMode, "シーンを置き換え\0現在のシーンに追加\0アニメーションのみ\0自動 (空なら開く / あれば追加)\0");
    ImGui::SeparatorText("単位 / 軸");
    ImGui::Checkbox("FBX の単位をメートルに変換 (UnitScaleFactor)", &o.convertUnitsToMeters);
    ImGui::Checkbox("FBX の Z-up を Y-up に変換", &o.convertUpAxisToY);
    ImGui::DragFloat("追加スケール", &o.extraScale, 0.001f, 1e-6f, 1e6f, "%.6g", ImGuiSliderFlags_Logarithmic);
    ImGui::Checkbox("単位行列のダミールートを除去", &o.removeDummyRoot);
    ImGui::SeparatorText("ジオメトリ");
    ImGui::Checkbox("三角形化", &o.triangulate);
    ImGui::Checkbox("法線が無ければ生成", &o.generateNormals);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(80));
    ImGui::SliderFloat("角度", &o.smoothingAngle, 0, 180, "%.0f°");
    ImGui::Checkbox("接線を計算 (glTF 規約)", &o.calcTangents);
    ImGui::Checkbox("同一頂点を結合", &o.joinVertices);
    ImGui::Checkbox("UV の V を反転 (追加)", &o.flipUV);
    ImGui::Checkbox("内向き法線を修正", &o.fixInfacingNormals);
    ImGui::Checkbox("メッシュを最適化 (結合)", &o.optimizeMeshes);
    ImGui::Checkbox("階層を最適化 (ノード削減)", &o.optimizeGraph);
    ImGui::Checkbox("重複マテリアルを削除", &o.removeRedundantMaterials);
    ImGui::SeparatorText("スキン / アニメーション");
    ImGui::Checkbox("ボーンウェイトを 4 本に制限", &o.limitBoneWeights);
    ImGui::Checkbox("アニメーションを読み込む", &o.importAnimations);
    ImGui::Checkbox("モーフターゲットを読み込む", &o.importMorphTargets);
    ImGui::Checkbox("FBX ピボットノードを保持 ($AssimpFbx$)", &o.preservePivots);
    ImGui::Checkbox("データ構造を検証 (Assimp)", &o.validateData);
    ImGui::SeparatorText("テクスチャ検索フォルダ");
    for (size_t i = 0; i < o.textureSearchDirs.size(); ++i) {
        ImGui::PushID((int)i);
        ImGui::TextUnformatted(o.textureSearchDirs[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("×")) { o.textureSearchDirs.erase(o.textureSearchDirs.begin() + i); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    if (ImGui::Button("フォルダを追加...")) {
        std::string d = Dialogs::PickFolder("テクスチャ検索フォルダ");
        if (!d.empty()) o.textureSearchDirs.push_back(d);
    }
    ImGui::Separator();
    if (ImGui::Button("ファイルを選んで読み込み...")) OpenDialog();
    ImGui::SameLine();
    if (ImGui::Button("初期値に戻す")) o = ImportOptions();
    ImGui::SameLine();
    if (ImGui::Button("設定を保存")) SaveSettings();
    ImGui::End();
}

void Editor::DrawExportWindow() {
    ImGui::SetNextWindowSize(ImVec2(Px(480), Px(560)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_EXPORT, "エクスポート", "ExportSettings").c_str(), &m_panels.exportWin)) { ImGui::End(); return; }
    ExportOptions& o = m_exportOpts;
    int fmt = (int)o.format;
    if (ImGui::BeginCombo("形式", ExportFormatName(o.format))) {
        for (int i = 0; i < (int)ExportFormat::Count; ++i)
            if (ImGui::Selectable(ExportFormatName((ExportFormat)i), fmt == i)) o.format = (ExportFormat)i;
        ImGui::EndCombo();
    }
    ImGui::Combo("書き出す内容", &o.content, "全部 (モデル + アニメーション)\0モデルだけ (アニメーションなし)\0アニメーションだけ (骨格 + アニメーション)\0");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("アニメーションだけ: メッシュ・マテリアル・テクスチャを除き、骨格とモーションだけを書き出します");
    if (m_scene.roots.size() > 1) {
        ImGui::Combo("書き出す範囲", &m_exportScope, "選んでいるモデルだけ\0シーン全体 (全モデル)\0");
        int model = ActiveModel();
        if (m_exportScope == 0) {
            if (model >= 0) ImGui::TextDisabled("対象: %s", m_scene.nodes[model].name.c_str());
            else ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "モデルが選ばれていないので、シーン全体を書き出します");
        }
    }
    bool gltf = o.format == ExportFormat::GLB || o.format == ExportFormat::GLTF || o.format == ExportFormat::GLTF_Embedded;
    bool fbx = o.format == ExportFormat::FBX || o.format == ExportFormat::FBX_ASCII;
    ImGui::SeparatorText("変換");
    ImGui::DragFloat("スケール", &o.scale, 0.001f, 1e-6f, 1e6f, "%.6g", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    if (ImGui::SmallButton("x0.01")) o.scale = 0.01f;
    ImGui::SameLine();
    if (ImGui::SmallButton("x1")) o.scale = 1.0f;
    ImGui::SameLine();
    if (ImGui::SmallButton("x100")) o.scale = 100.0f;
    ImGui::Combo("軸変換", &o.upAxisConversion, "なし\0Y-up → Z-up\0Z-up → Y-up\0");
    ImGui::Combo("オブジェクトのスケール", &o.objectScaleMode, "中身に焼き込む (親のスケール = 1)\0スケール付きの親ノードとして残す\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("焼き込む: 親の倍率をボーン位置・全アニメーションの移動キー (ルートモーション含む)・頂点・\n逆バインド行列に変換します。どのフレームでも親で拡大した見た目と一致します (均等スケールのみ)。\n"
                          "残す: エンジンで親 (GameObject) を拡大したのと同じ状態で書き出します。");
    if (o.objectScaleMode == 0) ImGui::Checkbox("自動作成した親ノード (*_Object) を書き出さない", &o.dissolveObjectWrappers);
    ImGui::SeparatorText("内容");
    ImGui::Checkbox("アニメーション", &o.exportAnimations);
    ImGui::SameLine();
    ImGui::Checkbox("スキン", &o.exportSkins);
    ImGui::SameLine();
    ImGui::Checkbox("モーフ", &o.exportMorphTargets);
    ImGui::Checkbox("法線", &o.exportNormals);
    ImGui::SameLine();
    ImGui::Checkbox("接線", &o.exportTangents);
    ImGui::SameLine();
    ImGui::Checkbox("UV1", &o.exportUV1);
    ImGui::SameLine();
    ImGui::Checkbox("頂点カラー", &o.exportColors);
    ImGui::Checkbox("表示中のノードのみ", &o.onlyVisible);
    ImGui::Checkbox("ウェイトを正規化", &o.normalizeWeights);
    ImGui::Checkbox("ノード名を一意にする (推奨)", &o.uniquifyNames);
    if (!gltf) {
        ImGui::SetNextItemWidth(Px(110));
        ImGui::DragFloat("曲線のキーの刻み (fps)", &o.curveBakeFps, 1.0f, 10.0f, 240.0f, "%.0f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("カーブエディタで作った曲線は、この形式では直線のキーに置き換えて書き出します。\n数を大きくするほど曲線に近くなります (glTF は曲線のまま書き出します)");
    }
    ImGui::SeparatorText("テクスチャ");
    if (o.format == ExportFormat::GLB) ImGui::TextDisabled("GLB ではテクスチャはファイル内に埋め込まれます");
    if (o.format == ExportFormat::GLTF_Embedded) ImGui::TextDisabled("base64 で .gltf に埋め込まれます");
    if (fbx) {
        ImGui::Checkbox("FBX にテクスチャを埋め込む", &o.embedTexturesFBX);
        int unit = o.fbxUnitCentimeters ? 1 : 0;
        if (ImGui::Combo("FBX 単位", &unit, "メートル (Unity/Blender 向け)\0センチメートル (Maya 既定)\0")) o.fbxUnitCentimeters = unit == 1;
    }
    if (o.format == ExportFormat::GLTF || (fbx && !o.embedTexturesFBX) || o.format == ExportFormat::OBJ || o.format == ExportFormat::DAE) {
        ImGui::Checkbox("テクスチャを出力先にコピー", &o.copyTextures);
        ImGui::Combo("テクスチャのフォルダ", &o.textureFolderMode, "モデル名 + Texture (おすすめ)\0自分で指定\0");
        if (o.textureFolderMode == 0) {
            std::string stem = !m_lastExportPath.empty() ? FileUtil::Stem(m_lastExportPath) : !m_currentPath.empty() ? FileUtil::Stem(m_currentPath) : "untitled";
            ImGui::TextDisabled("例: %s → %s/", stem.c_str(), TextureFolderName(stem).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("書き出すファイル名に合わせて付けます。\nmy_model → my_model_texture / MyModel → MyModelTexture / myModel → myModelTexture\n"
                                  "空白入りなどの名前はキャメルケース (my model → myModelTexture) に統一します");
        } else {
            ImGui::InputTextWithHint("サブフォルダ", "例: textures (空なら同じ場所)", &o.textureSubdir);
        }
    }
    if (gltf) {
        ImGui::SeparatorText("glTF");
        ImGui::Checkbox("JSON を整形", &o.prettyPrintJson);
        ImGui::Checkbox("書き出し後に再読み込みして検証", &o.verifyAfterExport);
    }
    if (o.format == ExportFormat::STL || o.format == ExportFormat::PLY || o.format == ExportFormat::ThreeMF)
        ImGui::TextDisabled("STL/PLY/3MF はジオメトリのみ (ワールド座標に焼き込み)");
    if (o.format == ExportFormat::ThreeDS) ImGui::TextDisabled("3DS はスキン・アニメーションを持てません。1 メッシュ 65535 頂点までです");

    // 書き出し前のチェック
    int errors = 0;
    for (auto& is : m_issues) if (is.severity == Severity::Error) ++errors;
    if (errors) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "検証エラーが %d 件あります (検証パネルを確認)", errors);

    ImGui::Separator();
    if (!m_lastExportPath.empty()) EllipsisText("前回: ", m_lastExportPath);
    ImGui::BeginDisabled(m_scene.Empty());
    if (ImGui::Button("書き出し...", ImVec2(140, 0))) ExportDialog();
    ImGui::SameLine();
    if (ImGui::Button("前回のパスに上書き", ImVec2(160, 0)) && !m_lastExportPath.empty()) DoExport(m_lastExportPath);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("設定を保存")) SaveSettings();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// 一括リネーム
// ---------------------------------------------------------------------------
void Editor::DrawBatchRename() {
    ImGui::SetNextWindowSize(ImVec2(Px(420), Px(380)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_WRENCH, "一括リネーム", "BatchRename").c_str(), &m_panels.batchRename)) { ImGui::End(); return; }
    ImGui::InputText("検索", &m_batchFind);
    ImGui::InputText("置換", &m_batchReplace);
    ImGui::InputText("接頭辞", &m_batchPrefix);
    ImGui::InputText("接尾辞", &m_batchSuffix);
    ImGui::Checkbox("選択ノードのみ", &m_batchSelectedOnly);
    // プレビュー
    ImGui::SeparatorText("プレビュー");
    ImGui::BeginChild("prev", ImVec2(0, -30), ImGuiChildFlags_Borders);
    int shown = 0;
    for (int i = 0; i < (int)m_scene.nodes.size() && shown < 200; ++i) {
        if (m_batchSelectedOnly && !IsSelected(i)) continue;
        std::string name = m_scene.nodes[i].name;
        std::string out = name;
        if (!m_batchFind.empty()) {
            size_t pos = 0;
            while ((pos = out.find(m_batchFind, pos)) != std::string::npos) { out.replace(pos, m_batchFind.size(), m_batchReplace); pos += m_batchReplace.size(); }
        }
        out = m_batchPrefix + out + m_batchSuffix;
        if (out == name) continue;
        ImGui::Text("%s  →  %s", name.c_str(), out.c_str());
        ++shown;
    }
    if (!shown) ImGui::TextDisabled("変更なし");
    ImGui::EndChild();
    if (ImGui::Button("適用")) {
        Checkpoint("一括リネーム");
        int n = Ops::BatchRename(m_scene, m_batchFind, m_batchReplace, m_batchPrefix, m_batchSuffix, m_batchSelectedOnly ? &m_selection : nullptr);
        LOG_INFO("%d 個のノード名を変更しました", n);
        MarkChanged();
    }
    ImGui::SameLine();
    if (ImGui::Button("mixamorig: を除去")) { m_batchFind = "mixamorig:"; m_batchReplace.clear(); }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// 環境設定 / ヘルプ / ダンプ
// ---------------------------------------------------------------------------
void Editor::DrawSettings() {
    ImGui::SetNextWindowSize(ImVec2(Px(420), Px(300)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_SETTINGS, "環境設定", "Settings").c_str(), &m_panels.settings)) { ImGui::End(); return; }
    ImGui::SliderFloat("UI の倍率", &m_uiScale, 0.75f, 2.0f, "x%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("文字とボタンの大きさ。ウィンドウの大きさと画面の DPI にも自動で合わせます (マウスを離すと反映)");
    int undoMax = (int)m_undo.MaxEntries();
    if (ImGui::SliderInt("Undo の最大数", &undoMax, 1, 200)) m_undo.SetMaxEntries(undoMax);
    ImGui::Checkbox("自動検証", &m_autoValidate);
    ImGui::Checkbox("ビューポートの操作ヘルプを表示", &m_showGizmoHelp);
    ImGui::Checkbox("パネルの説明を表示 (各パネルの上の青い文字)", &m_showPanelHints);
    ImGui::Checkbox("左ドラッグでカメラ回転 (オフ: Maya と同じく矩形選択)", &m_lmbOrbit);
    ImGui::ColorEdit3("背景色", &m_rs.bgColor.x);
    ImGui::SliderInt("MSAA", &m_rs.msaa, 0, 8);
    EllipsisText("設定ファイル: ", m_settingsPath, true);
    if (ImGui::Button("設定フォルダを開く")) Dialogs::OpenInExplorer(m_settingsPath);
    if (ImGui::Button("保存")) SaveSettings();
    ImGui::End();
}

void Editor::DrawHelp() {
    ImGui::SetNextWindowSize(ImVec2(Px(560), Px(520)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_HELP, "画面の見かた・操作方法", "Help").c_str(), &m_panels.help)) { ImGui::End(); return; }
    ImGui::SeparatorText("画面の見かた (Unity / Unreal Engine と同じ配置)");
    if (ImGui::BeginTable("##guide", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("場所", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("下の右 (アニメーションほか)").x + 8);
        ImGui::TableSetupColumn("内容");
        auto row = [](const std::string& where, const char* what) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(where.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", what);
        };
        row(IconText(ICON_OPEN, "上のツールバー"), "開く・書き出し、元に戻す、編集ツール (選択 / 移動 / 回転 / スケール)、アニメーションの再生、全体表示・定規・ヘルプ");
        row(IconText(ICON_HIERARCHY, "左: 階層"), "モデルの部品 (ノード) の親子関係。Unity の Hierarchy、Unreal の Outliner にあたります");
        row(IconText(ICON_VIEWPORT, "中央: ビューポート"), "3D 表示。左上で表示モード・ワイヤー・テクスチャ・ボーンなどの見せ方を切り替えます");
        row(IconText(ICON_INSPECTOR, "右: インスペクター"), "選んだものの詳細 (位置・回転・スケール、メッシュ、スキン)。Unreal の Details にあたります。同じ場所に「単位 / 定規」「シーン情報」のタブがあります");
        row(IconText(ICON_MESH, "下の左: アセット"), "メッシュ・マテリアル・テクスチャ・スケルトン。Unity の Project にあたります");
        row(IconText(ICON_ANIMATION, "下の右"), "アニメーション、検証 (問題の一覧)、モデル診断、コンソール (ログ)、履歴");
        row(IconText(ICON_INFO, "一番下: ステータスバー"), "開いているファイル、選択中のもの、三角形と頂点の数、エラー / 警告の数 (クリックで検証パネル)");
        ImGui::EndTable();
    }
    ImGui::TextDisabled("ボタンやパネルの名前にマウスを乗せると説明が出ます。配置が崩れたら上のツールバーの「配置」ボタンで元に戻せます");
    ImGui::SeparatorText("ビューポート");
    ImGui::BulletText("左クリック: 選択 / 左ドラッグ: 矩形選択 (Ctrl: 切替 / Shift: 追加)");
    ImGui::BulletText("Alt+左ドラッグ / 右ドラッグ: 回転 (環境設定で左ドラッグ=回転にも変更可)");
    ImGui::BulletText("H: 選択の表示切替  Ctrl+H: 非表示  Shift+H: すべて表示  I: 選択のみ表示");
    ImGui::BulletText("中ドラッグ / Shift+右ドラッグ: パン");
    ImGui::BulletText("ホイール / Alt+右ドラッグ: ズーム");
    ImGui::BulletText("F: 選択を表示  A: 全体表示  Num1/3/7: 正面/右/上  Num5: 投影切替");
    ImGui::BulletText("Q/W/E/R: 選択/移動/回転/スケール  X: ローカル/ワールド  Ctrl 押下でスナップ");
    ImGui::BulletText("4: ワイヤーフレーム  5: シェード  6: テクスチャ切替  Space: アニメーション再生");
    ImGui::SeparatorText("編集");
    ImGui::BulletText("Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z): 元に戻す / やり直し");
    ImGui::BulletText("Ctrl+D: 複製  Delete: 削除  F2: 名前変更");
    ImGui::BulletText("階層パネルでドラッグ&ドロップ: 親子付け (Alt: ローカル値を保持)");
    ImGui::BulletText("テクスチャパネルのサムネイルをマテリアルのスロットへドラッグして割り当て");
    ImGui::SeparatorText("ファイル");
    ImGui::BulletText("Ctrl+O: 開く  Ctrl+S: 書き出し  Ctrl+Shift+S: 名前を付けて書き出し  Ctrl+E: 書き出し設定");
    ImGui::BulletText("ウィンドウへのドラッグ&ドロップ: モデル読み込み / 画像はテクスチャとして追加");
    ImGui::SeparatorText("フリーズについて");
    ImGui::TextWrapped("「フリーズ」は Maya の Freeze Transformations と同じく、現在の移動/回転/スケールを頂点と子に焼き込み、"
                       "値を 0/0/1 に戻します。ジョイント (ボーン) には使えないため、リグのルートに付いた 0.01 や 100 のような"
                       "均等スケールは「スケールを階層に適用」を使ってください。ボーン位置・逆バインド行列・アニメーションまで正しく補正されます。");
    ImGui::SeparatorText("コマンドライン");
    ImGui::TextWrapped("ModelEditor.exe --convert in.fbx out.glb [--scale 0.01] [--freeze-all] [--apply-root-scale] [--fix]\n"
                       "ModelEditor.exe --validate in.fbx    /    ModelEditor.exe --dump in.glb    /    --help");
    ImGui::End();
}

void Editor::DrawDump() {
    ImGui::SetNextWindowSize(ImVec2(Px(640), Px(560)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(PanelTitle(ICON_CONSOLE, "シーンダンプ", "Dump").c_str(), &m_panels.dump)) { ImGui::End(); return; }
    if (ImGui::Button("テキストで更新")) m_dumpText = DumpSceneText(m_scene);
    ImGui::SameLine();
    if (ImGui::Button("JSON で更新")) m_dumpText = DumpSceneJson(m_scene);
    ImGui::SameLine();
    if (ImGui::Button("コピー")) ImGui::SetClipboardText(m_dumpText.c_str());
    ImGui::SameLine();
    if (ImGui::Button("保存...")) {
        std::string p = Dialogs::SaveFile("Text\0*.txt;*.json\0", "scene_dump.txt", ".txt");
        if (!p.empty()) FileUtil::WriteText(p, m_dumpText);
    }
    if (m_dumpText.empty()) m_dumpText = DumpSceneText(m_scene);
    ImGui::InputTextMultiline("##dump", &m_dumpText, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
    ImGui::End();
}
