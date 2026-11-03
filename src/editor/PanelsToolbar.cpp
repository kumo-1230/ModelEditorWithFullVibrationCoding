// メインツールバー / ステータスバー / ようこそ画面 / パネルの説明。
// Unity と Unreal Engine の配置を参考に、よく使う操作は上のツールバー、状態は下のステータスバーにまとめる。
#include "Editor.h"
#include "Icons.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include "core/Units.h"
#include "util/FileUtil.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace {
const ImVec4 kAccent(0.0f, 0.44f, 0.88f, 1.0f);
const ImVec4 kAccentHover(0.12f, 0.53f, 0.96f, 1.0f);
const ImVec4 kBarBg(0.082f, 0.082f, 0.082f, 1.0f);

float UiScale() { return ImGui::GetFontSize() / 15.0f; }

void Tooltip(const char* tip) {
    if (tip && *tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        // 折り返し位置は固定の幅で決める。0 (ウィンドウの右端) にすると、ツールチップは中身に合わせて幅を決めるので
        // 「幅が狭い → 折り返す → さらに狭い」となり、1 文字ずつの縦書きになってしまう
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(Px(420), ImGui::GetMainViewport()->WorkSize.x * 0.5f));
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// アイコンだけのボタン (アイコンが無ければ fallback の文字)。active なら青。tip にボタンの名前と説明を書く
bool IconButton(const char* icon, const char* fallback, const char* tip, bool active = false, bool enabled = true) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    }
    ImGui::BeginDisabled(!enabled);
    std::string label = std::string(IconOr(icon, fallback)) + "##" + fallback;
    float h = ImGui::GetFrameHeight();
    bool pressed = Icons::loaded ? ImGui::Button(label.c_str(), ImVec2(std::round(h * 1.2f), h)) : ImGui::Button(label.c_str());
    ImGui::EndDisabled();
    if (active) ImGui::PopStyleColor(2);
    Tooltip(tip);
    return pressed;
}

// アイコン + 文字のボタン
bool LabelButton(const char* icon, const char* text, const char* tip, bool active = false) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    }
    bool pressed = ImGui::Button(IconText(icon, text).c_str());
    if (active) ImGui::PopStyleColor(2);
    Tooltip(tip);
    return pressed;
}

void BarSeparator() {
    ImGui::SameLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 2, p.y + 3), ImVec2(p.x + 2, p.y + h - 3), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::Dummy(ImVec2(5, h));
    ImGui::SameLine();
}
} // namespace

// パネル上部の説明 (このパネルが何を表していて、何ができるか)。環境設定で消せる
// 1 行に収め、入りきらなければ「…」で省略してマウスを乗せると全文を出す (狭いパネルで場所を取らない)
void Editor::PanelHint(const char* text) {
    if (!m_showPanelHints) return;
    std::string s = IconText(ICON_INFO, text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    float h = ImGui::GetTextLineHeight();
    ImVec2 full = ImGui::CalcTextSize(s.c_str());
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.64f, 0.72f, 1.0f));
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), p, ImVec2(p.x + w, p.y + h), p.x + w, p.x + w, s.c_str(), nullptr, &full);
    ImGui::PopStyleColor();
    if (full.x > w) Tooltip(text);
    ImGui::Separator();
}

std::string Editor::PanelTitle(const char* icon, const char* title, const char* id) {
    return IconText(icon, title) + "###" + id;
}

// ---------------------------------------------------------------------------
// メインツールバー (メニューの下): ファイル / 元に戻す / 編集ツール / アニメーション再生 / 表示
// ---------------------------------------------------------------------------
void Editor::DrawMainToolbar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = std::round(5 * UiScale());
    const float height = ImGui::GetFrameHeight() + pad * 2;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(8 * UiScale()), pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(std::round(4 * UiScale()), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBarBg);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##MainToolbar", vp, ImGuiDir_Up, height, flags)) {
        const bool hasScene = !m_scene.Empty();
        // ファイル
        if (LabelButton(ICON_OPEN, "開く", "モデルを読み込む (Ctrl+O)\nFBX / glTF / GLB / OBJ / VRM などを今のシーンに追加します。\nダイアログで Ctrl / Shift を押すと複数のファイルをまとめて選べます。フォルダごと読み込むときは「ファイル > フォルダからまとめて読み込み」。\nウィンドウに何個でもドラッグ&ドロップできます")) {
            m_importMode = 1;
            OpenDialog();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasScene);
        if (LabelButton(ICON_EXPORT, "書き出し", "名前を付けて書き出し (Ctrl+Shift+S)\n読み込んだファイルと同じフォルダが開きます。形式は保存ダイアログで選べます"))
            ExportDialog();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (IconButton(ICON_SETTINGS, "設定", "書き出し設定 (Ctrl+E)\n形式・スケール・テクスチャのフォルダ・軸の変換などを決めます", m_panels.exportWin))
            m_panels.exportWin = !m_panels.exportWin;
        BarSeparator();
        // 元に戻す
        std::string undoTip = m_undo.CanUndo() ? "元に戻す (Ctrl+Z)\n" + m_undo.UndoEntries().back().label : std::string("元に戻す (Ctrl+Z)");
        if (IconButton(ICON_UNDO, "<-", undoTip.c_str(), false, m_undo.CanUndo())) { m_undo.Undo(m_scene); ValidateSelection(); MarkChanged(); }
        ImGui::SameLine();
        std::string redoTip = m_undo.CanRedo() ? "やり直し (Ctrl+Y)\n" + m_undo.RedoEntries().back().label : std::string("やり直し (Ctrl+Y)");
        if (IconButton(ICON_REDO, "->", redoTip.c_str(), false, m_undo.CanRedo())) { m_undo.Redo(m_scene); ValidateSelection(); MarkChanged(); }
        BarSeparator();
        // 編集ツール
        if (IconButton(ICON_SELECT, "選択", "選択ツール (Q)\nクリックで選ぶだけで、ギズモ (矢印) は出しません", m_gizmoOp == 3)) m_gizmoOp = 3;
        ImGui::SameLine();
        if (IconButton(ICON_MOVE, "移動", "移動ツール (W)\n選んだオブジェクトを矢印でドラッグして動かします", m_gizmoOp == 0)) m_gizmoOp = 0;
        ImGui::SameLine();
        if (IconButton(ICON_ROTATE, "回転", "回転ツール (E)\n選んだオブジェクトを輪をドラッグして回します", m_gizmoOp == 1)) m_gizmoOp = 1;
        ImGui::SameLine();
        if (IconButton(ICON_SCALE, "拡大", "スケールツール (R)\n選んだオブジェクトの大きさを変えます。原点 (ピボット) を中心に拡大し、原点は動きません", m_gizmoOp == 2)) m_gizmoOp = 2;
        ImGui::SameLine();
        if (LabelButton(m_gizmoLocal ? ICON_LOCAL : ICON_WORLD, m_gizmoLocal ? "ローカル" : "ワールド",
                        "ギズモの向き (X で切り替え)\nローカル: オブジェクト自身の向き / ワールド: シーン全体の XYZ"))
            m_gizmoLocal = !m_gizmoLocal;
        ImGui::SameLine();
        if (LabelButton(ICON_SNAP, "スナップ", "スナップ (Ctrl を押している間も有効)\n移動・回転・スケールを決まった刻みで動かします。刻みはビューポートの「表示」で変えられます", m_snap))
            m_snap = !m_snap;
        BarSeparator();

        // アニメーション (中央)
        ImGui::BeginDisabled(m_scene.animations.empty());
        ImGui::SetNextItemWidth(std::round(170 * UiScale()));
        const char* animName = m_anim >= 0 && m_anim < (int)m_scene.animations.size() ? m_scene.animations[m_anim].name.c_str()
                                                                                       : (m_scene.animations.empty() ? "アニメーションなし" : "レストポーズ");
        if (ImGui::BeginCombo("##anim", animName)) {
            if (ImGui::Selectable("レストポーズ (アニメーションなし)", m_anim < 0)) { m_anim = -1; m_playing = false; }
            const auto animMask = Membership().animations;
            for (int i = 0; i < (int)m_scene.animations.size(); ++i) {
                if (!InMask(animMask, i)) continue;
                ImGui::PushID(i);
                if (ImGui::Selectable(m_scene.animations[i].name.c_str(), m_anim == i)) { m_anim = i; m_animTime = 0; }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        Tooltip("再生するアニメーション\n詳しい操作 (速度・長さ・削除など) はアニメーションパネルで行います");
        ImGui::SameLine();
        bool hasAnim = m_anim >= 0 && m_anim < (int)m_scene.animations.size();
        if (IconButton(ICON_PREV, "|<", "先頭へ", false, hasAnim)) m_animTime = 0;
        ImGui::SameLine();
        if (IconButton(m_playing ? ICON_PAUSE : ICON_PLAY, m_playing ? "||" : ">", m_playing ? "一時停止 (Space)" : "再生 (Space)", m_playing,
                       !m_scene.animations.empty())) {
            if (!hasAnim) { m_anim = 0; m_animTime = 0; }
            m_playing = !m_playing;
        }
        ImGui::SameLine();
        if (IconButton(ICON_STOP, "[]", "停止 (先頭に戻す)", false, hasAnim)) { m_playing = false; m_animTime = 0; }
        ImGui::SameLine();
        if (IconButton(ICON_NEXT, ">|", "末尾へ", false, hasAnim)) m_animTime = m_scene.animations[m_anim].duration;
        ImGui::EndDisabled();
        if (hasAnim) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%.2f / %.2f 秒", m_animTime, m_scene.animations[m_anim].duration);
        }

        // 右端: 表示
        const float bw = Icons::loaded ? std::round(ImGui::GetFrameHeight() * 1.2f) : ImGui::CalcTextSize("ヘルプ").x + ImGui::GetStyle().FramePadding.x * 2;
        const std::string presetLabel = IconText(ICON_EYE, ("表示: " + m_activePreset + " ▼").c_str());
        const float presetW = ImGui::CalcTextSize(presetLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        const float right = presetW + bw * 4 + ImGui::GetStyle().ItemSpacing.x * 4 + ImGui::GetStyle().WindowPadding.x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12, ImGui::GetWindowWidth() - right));
        if (ImGui::Button(presetLabel.c_str())) ImGui::OpenPopup("##presets");
        Tooltip("表示プリセット\n形の確認・UV・アニメーション・ウェイト・書き出し前など、用途に合わせて表示をまとめて切り替えます。\n今の表示を名前を付けて保存したり、既定に戻したりできます");
        if (ImGui::BeginPopup("##presets")) {
            DrawPresetMenu();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (IconButton(ICON_FRAME, "全体", "全体を表示 (A)\nモデル全体が見えるようにカメラを合わせます。選んだものだけなら F", false, hasScene)) FrameAll();
        ImGui::SameLine();
        if (IconButton(ICON_RULER, "定規", "単位 / 定規パネル\n実寸の確認、定規の追加、指定サイズや倍率での拡大縮小を行います", m_panels.measure)) m_panels.measure = !m_panels.measure;
        ImGui::SameLine();
        if (IconButton(ICON_LAYOUT, "配置", "パネルの配置を最初の状態に戻す")) m_resetLayout = true;
        ImGui::SameLine();
        if (IconButton(ICON_HELP, "ヘルプ", "画面の見かた・操作方法 (F1)", m_panels.help)) m_panels.help = !m_panels.help;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// ---------------------------------------------------------------------------
// ステータスバー (画面の一番下): ファイル / 選択 / 規模 / 問題の数 / FPS
// ---------------------------------------------------------------------------
void Editor::DrawStatusBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float height = ImGui::GetTextLineHeight() + std::round(8 * UiScale());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(10 * UiScale()), std::round(4 * UiScale())));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBarBg);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##StatusBar", vp, ImGuiDir_Down, height, flags)) {
        const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        // ファイル
        std::string file = m_currentPath.empty() ? "無題 (まだ何も開いていません)" : FileUtil::Filename(m_currentPath);
        if (m_importing) file = "読み込み中... " + FileUtil::Filename(m_importingPath);
        ImGui::TextUnformatted(IconText(ICON_FOLDER, file.c_str()).c_str());
        if (!m_currentPath.empty()) Tooltip(m_currentPath.c_str());
        if (m_dirty) {
            ImGui::SameLine(0, 4);
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "(未書き出しの変更あり)");
        }
        ImGui::SameLine(0, 24);
        // 選択
        if (m_primary >= 0 && m_primary < (int)m_scene.nodes.size()) {
            std::string sel = "選択: " + m_scene.nodes[m_primary].name;
            if (m_selection.size() > 1) sel += " ほか " + std::to_string(m_selection.size() - 1) + " 個";
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.45f, 1.0f), "%s", sel.c_str());
        } else {
            ImGui::TextColored(dim, "選択なし");
        }
        ImGui::SameLine(0, 24);
        if (!m_scene.Empty()) {
            ImGui::TextColored(dim, "三角形 %zu   頂点 %zu   ノード %zu   単位 %s", m_scene.TotalTriangles(), m_scene.TotalVertices(), m_scene.nodes.size(),
                               UnitSuffix(m_displayUnit));
        }

        // 右端: 問題の数 (クリックで検証パネル) と FPS
        int errors = 0, warns = 0;
        for (auto& is : m_issues) { if (is.severity == Severity::Error) ++errors; else if (is.severity == Severity::Warning) ++warns; }
        char issues[128], fps[32];
        snprintf(issues, sizeof(issues), "%s %d   %s %d", IconOr(ICON_ERROR, "エラー"), errors, IconOr(ICON_WARNING, "警告"), warns);
        snprintf(fps, sizeof(fps), "%.0f FPS", ImGui::GetIO().Framerate);
        float w = ImGui::CalcTextSize(issues).x + ImGui::CalcTextSize(fps).x + 40;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x));
        ImVec4 col = errors ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : warns ? ImVec4(1.0f, 0.8f, 0.35f, 1.0f) : ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        if (ImGui::Selectable(issues, false, 0, ImGui::CalcTextSize(issues))) {
            m_panels.validation = true;
            ImGui::SetWindowFocus("###Validation");
        }
        ImGui::PopStyleColor();
        Tooltip(errors || warns ? "モデルの問題 (エラー / 警告) の数です。クリックすると検証パネルで内容と自動修正を確認できます"
                                : "問題は見つかっていません。クリックすると検証パネルを開きます");
        ImGui::SameLine(0, 20);
        ImGui::TextColored(dim, "%s", fps);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// ---------------------------------------------------------------------------
// 何も開いていないときのビューポート: 何をすればよいかを案内する
// ---------------------------------------------------------------------------
void Editor::DrawWelcome(const ImVec2& pos, const ImVec2& size) {
    const float s = UiScale();
    ImVec2 box(std::min(size.x - 40, 560 * s), 0);
    if (box.x < 200) return;
    // 前のフレームの高さで縦の中央に置く (表示ツールバーの下にはみ出さない)
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID hId = ImGui::GetID("##welcomeH");
    float lastH = st->GetFloat(hId, 300 * s);
    float top = ImGui::GetFrameHeight() + 20;
    ImGui::SetCursorScreenPos(ImVec2(pos.x + (size.x - box.x) * 0.5f, pos.y + std::max(top, (size.y - lastH) * 0.45f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.1f, 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 20 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8 * s);
    ImGui::BeginChild("##welcome", box, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    if (Icons::large) ImGui::PushFont(Icons::large);
    ImGui::TextUnformatted(IconText(ICON_IMPORT, "モデルを開いて始めましょう").c_str());
    if (Icons::large) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextUnformatted("FBX / glTF / GLB / OBJ / VRM などを、この画面にドラッグ&ドロップします。");
    ImGui::TextDisabled("何個でも続けて読み込めます (Maya の読み込みと同じく、ファイルごとにグループになります)");
    ImGui::Spacing();
    if (LabelButton(ICON_OPEN, "ファイルを開く... (Ctrl+O)", "モデルファイルを選んで開きます")) { m_importMode = 1; OpenDialog(); }
    ImGui::SameLine();
    if (LabelButton(ICON_MESH, "立方体を置いて試す", "練習用に立方体を作ります (作成メニューから他の形も作れます)")) CreatePrimitiveObject(Ops::Prim_Cube);
    SameLineIfFits(IconText(ICON_FOLDER, "フォルダから読み込む").c_str());
    if (LabelButton(ICON_FOLDER, "フォルダから読み込む", "フォルダの中のモデルをまとめて読み込みます")) ImportFolderDialog(false);
    if (!m_recent.empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText("最近使ったファイル");
        int n = 0;
        for (auto& r : std::vector<std::string>(m_recent)) {
            if (n++ >= 4) break;
            if (ImGui::Selectable(IconText(ICON_FOLDER, FileUtil::Filename(r).c_str()).c_str())) StartImport(r, 0);
            Tooltip(r.c_str());
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("ボタンにマウスを乗せると説明が出ます。画面の見かたは F1 (右上の ? ボタン)");
    ImGui::EndChild();
    st->SetFloat(hId, ImGui::GetItemRectSize().y);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}
