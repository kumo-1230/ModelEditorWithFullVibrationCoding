#pragma once
#include "core/Diagnostics.h"
#include "core/ModelOps.h"
#include "core/Scene.h"
#include "core/Undo.h"
#include "core/Validate.h"
#include "io/Exporter.h"
#include "io/Importer.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "Icons.h"
#include <imgui.h>
#include <deque>
#include <functional>
#include <future>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

// エディター本体。ImGui の各パネルとシーン編集を管理する。
class Editor {
public:
    bool Init(void* hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx, const std::string& gpuInfo);
    // デバイスロスト時などにレンダラーだけ作り直す
    bool ResetRenderer(ID3D11Device* device, ID3D11DeviceContext* ctx, const std::string& gpuInfo);
    bool RendererLost() const { return m_renderer.DeviceLost(); }
    void Shutdown();
    void Frame();                 // 毎フレーム呼ぶ (ImGui::NewFrame の後)
    void OnDropFiles(const std::vector<std::string>& paths);
    bool WantsQuit() const { return m_quitRequested; }
    void RequestQuit();
    void OpenFile(const std::string& path, bool add = false); // add=true: 今のシーンに追加 (Maya の読み込み)
    void SetSelectAfterLoad(const std::string& nodeName) { m_selectAfterLoad = nodeName; }
    void FocusWindowLater(const std::string& windowName) { m_focusWindows.push_back(windowName); }
    void PlayAnimationOnLoad(bool play) { m_playOnLoad = play; }
    void SetRenderMode(int mode) { m_rs.mode = mode; }
    void EnableSmokeTest() { m_smokeTest = true; }
    void AddRulerAfterLoad(float heightMeters) { m_rulerAfterLoad = heightMeters; m_hasRulerAfterLoad = true; }
    int SmokeTestErrors() const { return m_smokeErrors; }
    float UiScaleSetting() const { return m_uiScale; } // 環境設定の UI 倍率 (ウィンドウの大きさに合わせた係数とは別)

private:
    // ---- 状態 -------------------------------------------------------------
    void* m_window = nullptr; // HWND
    Scene m_scene;
    UndoStack m_undo;
    Renderer m_renderer;
    RenderSettings m_rs;
    OrbitCamera m_camera;
    std::string m_currentPath;
    std::string m_lastExportPath;
    bool m_dirty = false;
    uint64_t m_sceneRevision = 1;  // 何か変わったら ++ (変形の再計算、検証の再実行)
    bool m_quitRequested = false;
    bool m_confirmQuitOpen = false;

    // 選択
    std::vector<int> m_selection;  // ノード
    int m_primary = -1;
    int m_selMesh = -1;
    int m_selPrim = 0;
    int m_selMaterial = -1;
    int m_selTexture = -1;
    int m_selBone = -1;
    int m_selSkin = 0;

    // アニメーション
    int m_anim = -1;
    float m_animTime = 0.0f;
    bool m_playing = false;
    bool m_loop = true;
    float m_animSpeed = 1.0f;
    std::vector<NodePose> m_pose;
    std::vector<glm::mat4> m_world;

    // ビューポート / ギズモ
    int m_gizmoOp = 0;  // 0=移動 1=回転 2=スケール 3=なし
    bool m_gizmoLocal = true;
    bool m_snap = false;
    float m_snapT = 0.1f, m_snapR = 15.0f, m_snapS = 0.1f;
    bool m_gizmoWasUsing = false;
    bool m_viewportHovered = false;
    bool m_viewportFocused = false;
    ImVec2 m_viewportPos{ 0, 0 };
    ImVec2 m_viewportSize{ 1, 1 };
    float m_vpToolbarH = 0.0f;      // ビューポート左上の表示ツールバーの高さ (折り返し込み)
    bool m_pickRequested = false;
    ImVec2 m_pickPos;
    bool m_showBoneNames = false;
    float m_sceneRadius = 1.0f;
    glm::vec3 m_sceneMin{ -1 }, m_sceneMax{ 1 };

    // オプション
    ImportOptions m_importOpts;
    ExportOptions m_exportOpts;

    // 非同期インポート
    struct ImportResult {
        bool ok = false;
        Scene scene;
        std::string error;
        std::string path;
        double ms = 0;
    };
    std::future<ImportResult> m_importFuture;
    bool m_importing = false;
    std::string m_importingPath;
    int m_importMode = 1; // 0=置き換え 1=追加 (既定、Maya の読み込みと同じ) 2=アニメーションのみ
    int m_importModeRunning = 0;
    std::deque<std::pair<std::string, int>> m_importQueue; // 複数ファイルを順番に読み込む
    int m_dropMode = 3;            // ドロップ時: 0=置き換え 1=追加 2=アニメのみ 3=自動 (空なら置き換え、あれば追加)

    // 複数オブジェクト / 表示
    void CreatePrimitiveObject(int type);
    void SetVisibility(const std::vector<int>& nodes, bool visible, const char* label);
    void ToggleSelectionVisibility();
    void IsolateSelection();
    void ShowAll();

    // 矩形選択
    bool m_marqueeActive = false;
    ImVec2 m_marqueeStart{ 0, 0 };
    bool m_lmbOrbit = false;       // true: 左ドラッグで回転 (false: Maya と同じく矩形選択)
    void SelectInRect(const ImVec2& a, const ImVec2& b, const ImVec2& vpPos, const ImVec2& vpSize, bool additive, bool toggle);

    // 複数選択の同時編集
    bool m_multiRelative = false;  // true: 差分を加算 / false: 同じ値を設定
    std::vector<int> EditTargets() const; // 編集対象 (選択ノード。子孫の重複を除かない)
    std::vector<int> TopLevelTargets() const; // 選択のうち、他の選択ノードの子孫でないもの
    // スケールを変更するノード。最上位のアニメーション付きノード (ルートモーションを持つルートボーン等) は
    // 自身の移動キーが拡大されないので、アニメーションを持たないオブジェクトノード (親) を返す。
    // create=false で親ノードの作成が必要なら -1
    int ScaleTargetOf(int node, bool create);

    // 複数モデル (Maya の読み込みと同じく、ファイルごとに最上位のグループ = 1 モデル)
    int ModelOf(int node) const;      // ノードが属するモデル (最上位のノード)
    int ActiveModel() const;          // 選んでいるモデル。モデルが 1 つならそれ。決められなければ -1
    const SceneInfo& ModelInfo(int model) const; // モデルごとの元ファイルの情報 (単位・パス)
    SceneInfo& ModelInfoMut(int model);
    std::string FitTargetLabel(bool allSelected);
    struct ModelMembership {
        uint64_t rev = 0;
        int model = -2;
        std::vector<char> nodes, meshes, materials, textures, animations;
    };
    ModelMembership m_member;
    const ModelMembership& Membership(); // 選んでいるモデルに属するもの (一覧の絞り込み用。モデルが決まらなければ全部 1)
    bool NodeInActiveModel(int node);
    bool IssueInActiveModel(const Issue& is); // 検証結果が選んでいるモデルのものか
    void RevealIssue(const Issue& is);        // 検証の行をダブルクリック: 関連するパネルと確認用の表示を開く
    void DrawAnimEditor();                    // アニメーション編集 (Maya のタイムライン / ドープシート相当)
    // カーブエディタ (Maya のグラフエディタ相当)
    void DrawCurveEditor();
    struct CurveView;
    struct CurveKey { int channel = -1, comp = 0, key = -1; };
    struct CurveOrig { CurveKey key; float time = 0; glm::vec4 value{ 0 }; glm::vec3 euler{ 0 }; };
    float CurveKeyValue(const Animation& a, const CurveView& c, int k) const;
    std::vector<CurveKey> m_ceSel;          // 選んだキー
    std::vector<CurveOrig> m_ceOrig;        // ドラッグ開始時の値
    std::set<int64_t> m_ceCurves;           // 表示するカーブ (チャンネル番号 * 4 + 成分)
    int m_ceAnim = -1;
    float m_ceT0 = 0, m_ceT1 = 1, m_ceV0 = -1, m_ceV1 = 1; // 表示範囲 (秒 / 値)
    bool m_ceNormalize = false, m_ceSnap = true, m_ceFrameAll = true, m_ceFrameSel = false;
    int m_ceDrag = 0;                       // 1=キー移動 2=矩形選択 3=パン 4=再生位置 5=接線 6=ズーム
    int m_ceAxisLock = 0, m_ceHandleCurve = -1, m_ceHandleKey = -1;
    ImVec2 m_ceMoveStart{ 0, 0 }, m_ceBoxStart{ 0, 0 };
    std::string m_ceFilter;
    // アニメーション編集の状態
    float m_rangeIn = 0.0f, m_rangeOut = 0.0f; // 範囲 (秒)
    int m_rangeAnim = -1;
    std::vector<int> m_dopeSel;               // ドープシートで選んだノード (チャンネルの行)
    float m_dopeZoom = 1.0f;
    float m_reduceTol = 0.001f;
    std::string m_dopeFilter;
    static bool InMask(const std::vector<char>& m, int i) { return i < 0 || i >= (int)m.size() || m[i]; }
    bool m_filterToModel = true;      // パネルの一覧を選んでいるモデルのものだけにする
    int m_boundsModel = -2;
    void DrawModelFilterBar();
    void DrawModels();
    void DrawBoneMap();             // ボーンの対応付け (元のどのボーンの動きを先のどのボーンに入れるか)
    std::string m_boneMapFilter;
    void ExtractFromActiveModel(bool animationOnly);
    int m_combineSrc = -1, m_combineDst = -1, m_attachNode = -1;
    std::string m_attachFilter, m_combineReport;
    bool m_combineCloseSrc = true;
    bool m_extractRemove = false;
    Ops::RetargetOptions m_retarget;
    int m_exportScope = 0;            // 0=選んでいるモデルだけ (モデルが複数のとき) 1=シーン全体

    // 定規 / サイズ合わせ (トランスフォームのスケールで変更)
    bool m_fitAlignBottom = true;
    bool m_fitAlignXZ = false;
    // 対象の AABB (全頂点で正確に測る)。既定はレストポーズ (実寸表示・定規のボタンと同じ基準)。posed=true で表示中の姿勢
    bool ComputeTargetsBounds(const std::vector<int>& targets, glm::vec3& mn, glm::vec3& mx, bool posed = false);
    // factor > 0 なら targetSize の代わりにその倍率で拡大縮小する
    bool ScaleTargetsToSize(std::vector<int> targets, int axis, float targetSize, const glm::vec3* anchor, bool anchorTop, bool alignXZ, const char* label,
                            float factor = 0.0f);
    float m_scaleFactor = 2.0f;     // 倍率で拡大縮小
    std::vector<int> FitTargetsForSelection(bool allSelected = true); // false: 複数選択でも主選択だけ
    // 表示中の姿勢 (アニメーション適用後) のワールド行列
    std::vector<glm::mat4> PosedWorld() const;

    // 検証
    std::vector<Issue> m_issues;
    uint64_t m_issuesRevision = 0;
    bool m_autoValidate = true;
    double m_lastChangeTime = 0.0; // 編集が止まってから自動検証する (ドラッグ中に毎フレーム重い検証をしない)

    // モデル診断
    bool m_showOpenEdges = false;     // 開いた辺 (穴) を黄色で表示
    bool m_showNonManifold = false;   // 非多様体の辺を桃色で表示
    bool m_showRootPath = false;      // ルートモーションの軌跡
    bool m_showPivots = false;        // オブジェクトのピボット (原点) と中心
    bool m_vertexInspect = false;     // クリックで頂点を調べる
    bool m_vertexHitValid = false;
    MeshHit m_vertexHit;
    struct TopoCache {
        uint64_t uid = 0;
        std::vector<TopologyInfo> prims;
        std::vector<UVInfo> uvs;
    };
    std::unordered_map<int, TopoCache> m_topoCache; // メッシュ番号 → トポロジー (メッシュの uid が変われば作り直す)
    const TopoCache& Topology(int mesh);
    std::vector<AnimationReport> m_animReports;
    uint64_t m_animReportsRev = 0;
    std::vector<glm::vec3> m_rootPath;
    uint64_t m_rootPathRev = 0;
    int m_rootPathAnim = -2;
    int m_diagSkin = 0;
    int m_diagForceTab = -1;          // 自己テスト用: 次のフレームで開くタブ
    void DrawDiagnostics();
    void SubmitDiagnosticLines();
    bool InspectVertexAt(const ImVec2& mouseInViewport, const ImVec2& viewportSize);

    // パネル表示
    struct Panels {
        bool hierarchy = true, inspector = true, viewport = true, meshes = true, materials = true, textures = true;
        bool skeleton = true, animation = true, console = true, validation = true, stats = false, sceneInfo = false;
        bool history = false, uvViewer = false, exportWin = false, importWin = false, batchRename = false;
        bool settings = false, help = false, imguiDemo = false, imguiMetrics = false, dump = false;
        bool measure = true;
        bool diagnostics = true;
        bool models = true;
        bool animEditor = true;
        bool curveEditor = true;
        bool boneMap = false;
    } m_panels;
public:
    // 表示プリセット (表示の設定 + 開くパネル + 確認用の線)
    struct ViewPreset {
        std::string name, description;
        bool builtin = false;
        RenderSettings rs;
        Panels panels;
        bool boneNames = false, sizeOverlay = true, openEdges = false, nonManifold = false, rootPath = false, pivots = false, gizmoHelp = true;
        std::vector<std::string> focus; // 前に出すパネル (### の後ろの ID)
    };
private:
    ViewPreset CaptureViewPreset(const std::string& name) const;
    void ApplyViewPreset(const ViewPreset& p);
    static const std::vector<ViewPreset>& BuiltinPresets();
    void LoadUserPresets();
    void SaveUserPresets();
    void DrawPresetMenu();
    std::vector<ViewPreset> m_userPresets;
    std::string m_activePreset = "標準 (既定に戻す)";
    std::string m_presetName;
    bool m_resetLayout = false;
    bool m_firstFrame = true;

    // その他 UI 状態
    std::vector<std::string> m_recent;
    std::string m_hierarchyFilter;
    float m_hierRowH = 0.0f;        // 階層の 1 行の高さ (画面外の行を描かずに高さだけ確保するカリング用)
    std::string m_consoleFilter;
    bool m_consoleShow[4] = { true, true, true, true };
    bool m_consoleAutoScroll = true;
    size_t m_consoleLastRev = 0;
    int m_renameNode = -1;
    std::string m_renameBuf;
    std::string m_batchFind, m_batchReplace, m_batchPrefix, m_batchSuffix;
    bool m_batchSelectedOnly = false;
    float m_normalAngle = 60.0f;
    float m_weldEps = 1e-5f;
    float m_pruneThreshold = 0.01f;
    float m_unitScale = 0.01f;
    int m_axisFrom = 2, m_axisTo = 1;
    bool m_freezeT = true, m_freezeR = true, m_freezeS = true;
    int m_texChannel = 0; // 0=RGBA 1=R 2=G 3=B 4=A
    float m_texZoom = 1.0f;
    std::string m_dumpText;
    std::deque<float> m_frameTimes;
    double m_lastFrameMs = 0;
    float m_uiScale = 1.0f;
    std::string m_settingsPath;
    bool m_showGizmoHelp = true;
    int m_revealNode = -1;          // 階層パネルで展開・スクロールするノード
    ImGuiID m_editSessionId = 0;    // ドラッグ中の連続編集を 1 つの Undo にまとめる
    Scene m_pendingSnapshot;
    bool m_hasPending = false;
    ImGuiID m_pendingId = 0;
    int m_eulerNode = -1;
    glm::quat m_eulerQuat{ 1, 0, 0, 0 };
    glm::vec3 m_euler{ 0 };
    bool m_uniformScale = false;
    uint64_t m_poseRev = 1;
    uint64_t m_poseSceneRev = 0;
    float m_poseAnimTime = -1.0f;
    int m_poseAnim = -2;
    bool m_applyToAllPrims = false;
    std::string m_selectAfterLoad;

    // 単位 / 実寸 / 定規
    struct Ruler {
        std::string name = "定規";
        glm::vec3 origin{ 0.0f };   // メートル
        float height = 1.0f;        // メートル (マイナス可: 原点より下)
        float width = 0.5f;         // ワイヤーフレーム枠の一辺
        glm::vec3 color{ 1.0f, 0.82f, 0.2f };
        bool visible = true;
        bool onTop = true;
        bool ticks = true;
    };
    std::vector<Ruler> m_rulers;
    int m_activeRuler = -1;
    bool m_rulerGizmo = false;      // ギズモで定規の原点を動かす
    int m_displayUnit = 2;          // 表示単位 (LengthUnit)
    bool m_hasRulerAfterLoad = false;
    float m_rulerAfterLoad = 0.0f;
    bool m_showSizeOverlay = true;
    float m_targetSize = 1.7f;      // 指定サイズにリサイズ (表示単位)
    int m_targetAxis = 1;
    void DrawMeasure();
    void SubmitRulerLines();
    void DrawRulerLabels(const ImVec2& pos, const ImVec2& size, const glm::mat4& viewProj);
    void ChangeModelUnit(int newUnit);
    int AddRuler();
    std::vector<std::string> m_focusWindows;
    int m_frameIndex = 0;
    bool m_playOnLoad = false;
    bool m_smokeTest = false;
    int m_smokeFrame = 0;
    int m_smokeErrors = 0;
    void SmokeTestStep();

    // ---- 内部処理 ---------------------------------------------------------
    void SetupDockLayout(unsigned dockId);
    void DrawMainMenu();
    void DrawMainToolbar();          // メニューの下: ファイル / 元に戻す / 編集ツール / 再生 / 表示
    void DrawStatusBar();            // 一番下: ファイル / 選択 / 規模 / 問題の数
    void DrawWelcome(const ImVec2& pos, const ImVec2& size); // 何も開いていないときの案内
    void PanelHint(const char* text); // パネルの説明 (何を表していて何ができるか)
    static std::string PanelTitle(const char* icon, const char* title, const char* id);
    bool m_showPanelHints = true;
    int m_layoutVersion = 0;         // 配置を変えたら上げる (古い配置の ini を一度だけ作り直す)
    void DrawToolbar();
    void HandleShortcuts();
    void UpdateAnimation(float dt);
    void UpdateWorld();
    void PollImport();

    // ファイル
    void NewScene();
    void OpenDialog();
    void ImportFolderDialog(bool recursive); // フォルダの中のモデルをまとめて読み込む
    void StartImport(const std::string& path, int mode);
    void FinishImport(ImportResult& r);
    void ExportDialog();
    bool DoExport(const std::string& path);
    void QuickExport();
    void AddRecent(const std::string& path);
    void LoadSettings();
    void SaveSettings();

    // 編集補助
    void Checkpoint(const std::string& label);  // 変更直前に呼ぶ
    void MarkChanged();                          // 変更直後に呼ぶ
    template<class T, class F> bool EditValue(const char* label, T& value, F&& widget);
    void Select(int node, bool additive = false, bool toggle = false);
    void ClearSelection();
    bool IsSelected(int node) const;
    void ValidateSelection();
    void DeleteSelected();
    void DuplicateSelected();
    void FrameSelected();
    void FrameAll();
    void FocusNode(int node);
    void ApplyFreeze(int node, bool hierarchy);
    void ApplyRootScale(int node);

    // パネル
    void DrawViewport();
    void DrawViewportOverlay(const ImVec2& pos, const ImVec2& size);
    void DrawGizmo(const ImVec2& pos, const ImVec2& size);
    void HandleViewportInput(const ImVec2& pos, const ImVec2& size);
    void DrawHierarchy();
    void DrawHierarchyNode(int idx, const std::vector<bool>& joints, const std::vector<bool>& filterPass);
    void DrawNodeContextMenu(int idx);
    void DrawInspector();
    void DrawTransformEditor(int idx);
    void DrawMeshes();
    void DrawPrimitiveTools(int mesh, int prim);
    void DrawMaterials();
    void DrawMaterialEditor(int mat);
    bool DrawTextureSlot(Material& m, int slot);
    void DrawTextures();
    void DrawSkeleton();
    void DrawBoneTree(int node, const std::vector<bool>& joints, const Skin& skin);
    void DrawAnimation();
    void DrawConsole();
    void DrawValidation();
    void DrawStats();
    void DrawSceneInfo();
    void DrawHistory();
    void DrawUVViewer();
    void DrawExportWindow();
    void DrawImportWindow();
    void DrawBatchRename();
    void DrawSettings();
    void DrawHelp();
    void DrawDump();
    void DrawImportingPopup();
    void DrawQuitConfirm();

    // テクスチャ
    int LoadTextureFile(const std::string& path);
    void RelinkMissingTextures(const std::string& dir);
    ImTextureID TexId(int texture);
};

// 値の編集ウィジェットを Undo 対応にするヘルパー。
// widget() は値を書き換える ImGui ウィジェット呼び出しで、変更されたら true を返す。
template<class T, class F>
bool Editor::EditValue(const char* label, T& value, F&& widget) {
    T before = value;
    bool changed = widget();
    ImGuiID id = ImGui::GetItemID();
    if (ImGui::IsItemActivated()) {
        // 編集開始時点のシーンを保存 (最初の変更が起きたら Undo に積む)
        T after = value;
        value = before;
        m_pendingSnapshot = m_scene;
        value = after;
        m_hasPending = true;
        m_pendingId = id;
    }
    if (changed) {
        if (m_hasPending && m_pendingId == id) {
            m_undo.Push(m_pendingSnapshot, label);
            m_pendingSnapshot = Scene();
            m_hasPending = false;
            m_editSessionId = id;
        } else if (id != m_editSessionId) {
            T after = value;
            value = before;
            Checkpoint(label);
            value = after;
            m_editSessionId = id;
        }
        MarkChanged();
    }
    return changed;
}
