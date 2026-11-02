// エディター本体: 初期化 / メインメニュー / ショートカット / ファイル操作 / 設定
#include "Editor.h"
#include "app/SceneDump.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include "core/Units.h"
#include "io/ImageIO.h"
#include "util/Dialogs.h"
#include "util/FileUtil.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <sstream>

bool Editor::ResetRenderer(ID3D11Device* device, ID3D11DeviceContext* ctx, const std::string& gpuInfo) {
    m_renderer.Shutdown();
    m_renderer = Renderer();
    ++m_poseRev;
    return m_renderer.Init(device, ctx, gpuInfo);
}

bool Editor::Init(void* hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx, const std::string& gpuInfo) {
    m_window = hwnd;
    Dialogs::SetOwner(hwnd);
    if (!m_renderer.Init(device, ctx, gpuInfo)) {
        LOG_ERROR("レンダラーの初期化に失敗しました");
        return false;
    }
    m_settingsPath = FileUtil::Join(FileUtil::AppDataDir(), "ModelEditor_settings.ini");
    LoadSettings();
    UpdateWorld();
    LOG_INFO("エディターを起動しました。ファイルをウィンドウにドラッグ&ドロップするか、ファイル > 開く で読み込めます。");
    return true;
}

void Editor::Shutdown() {
    if (m_importFuture.valid()) m_importFuture.wait();
    SaveSettings();
    m_renderer.Shutdown();
}

void Editor::RequestQuit() {
    if (m_dirty) m_confirmQuitOpen = true;
    else m_quitRequested = true;
}

// ---------------------------------------------------------------------------
// Undo / 変更通知
// ---------------------------------------------------------------------------
void Editor::Checkpoint(const std::string& label) {
    m_undo.Push(m_scene, label);
    m_dirty = true;
}

void Editor::MarkChanged() {
    m_dirty = true;
    ++m_sceneRevision;
}

// ---------------------------------------------------------------------------
// 選択
// ---------------------------------------------------------------------------
bool Editor::IsSelected(int node) const {
    return std::find(m_selection.begin(), m_selection.end(), node) != m_selection.end();
}

void Editor::Select(int node, bool additive, bool toggle) {
    if (node < 0 || node >= (int)m_scene.nodes.size()) {
        if (!additive && !toggle) ClearSelection();
        return;
    }
    if (toggle) {
        auto it = std::find(m_selection.begin(), m_selection.end(), node);
        if (it != m_selection.end()) {
            m_selection.erase(it);
            m_primary = m_selection.empty() ? -1 : m_selection.back();
            return;
        }
        m_selection.push_back(node);
    } else if (additive) {
        if (!IsSelected(node)) m_selection.push_back(node);
    } else {
        m_selection = { node };
    }
    m_primary = node;
    const Node& n = m_scene.nodes[node];
    if (n.mesh >= 0) {
        m_selMesh = n.mesh;
        m_selPrim = 0;
        if (!m_scene.meshes[n.mesh].primitives.empty()) {
            int mat = m_scene.meshes[n.mesh].primitives[0].material;
            if (mat >= 0) m_selMaterial = mat;
        }
    }
    if (n.skin >= 0) m_selSkin = n.skin;
    auto joints = m_scene.JointMask();
    if (joints[node]) m_selBone = node;
    m_revealNode = node;
}

void Editor::ClearSelection() {
    m_selection.clear();
    m_primary = -1;
}

void Editor::ValidateSelection() {
    int n = (int)m_scene.nodes.size();
    m_selection.erase(std::remove_if(m_selection.begin(), m_selection.end(), [n](int i) { return i < 0 || i >= n; }), m_selection.end());
    if (m_primary >= n) m_primary = m_selection.empty() ? -1 : m_selection.back();
    if (m_selMesh >= (int)m_scene.meshes.size()) m_selMesh = -1;
    if (m_selMesh >= 0 && m_selPrim >= (int)m_scene.meshes[m_selMesh].primitives.size()) m_selPrim = 0;
    if (m_selMaterial >= (int)m_scene.materials.size()) m_selMaterial = -1;
    if (m_selTexture >= (int)m_scene.textures.size()) m_selTexture = -1;
    if (m_selBone >= n) m_selBone = -1;
    if (m_selSkin >= (int)m_scene.skins.size()) m_selSkin = 0;
    if (m_anim >= (int)m_scene.animations.size()) { m_anim = m_scene.animations.empty() ? -1 : 0; m_animTime = 0; }
    m_eulerNode = -1;
}

void Editor::DeleteSelected() {
    if (m_selection.empty()) return;
    auto joints = m_scene.JointMask();
    // ジョイントを含む部分木の削除は、そのスキンが使われているならブロック
    std::vector<bool> willRemove(m_scene.nodes.size(), false);
    std::function<void(int)> mark = [&](int i) { willRemove[i] = true; for (int c : m_scene.nodes[i].children) mark(c); };
    for (int s : m_selection) mark(s);
    for (size_t si = 0; si < m_scene.skins.size(); ++si) {
        bool skinUsed = false;
        for (size_t ni = 0; ni < m_scene.nodes.size(); ++ni)
            if (m_scene.nodes[ni].skin == (int)si && !willRemove[ni]) skinUsed = true;
        if (!skinUsed) continue;
        for (int j : m_scene.skins[si].joints)
            if (j >= 0 && willRemove[j]) {
                LOG_ERROR("ジョイント '%s' はスキン '%s' で使用中のため削除できません (先にスキンメッシュを削除してください)",
                          m_scene.nodes[j].name.c_str(), m_scene.skins[si].name.c_str());
                return;
            }
    }
    Checkpoint("ノード削除");
    size_t before = m_scene.nodes.size();
    m_scene.RemoveNodes(m_selection);
    LOG_INFO("%zu 個のノードを削除しました", before - m_scene.nodes.size());
    ClearSelection();
    ValidateSelection();
    MarkChanged();
}

void Editor::DuplicateSelected() {
    if (m_selection.empty()) return;
    Checkpoint("複製");
    std::vector<int> created;
    for (int s : std::vector<int>(m_selection)) {
        int n = Ops::DuplicateNode(m_scene, s, false);
        if (n >= 0) created.push_back(n);
    }
    m_selection = created;
    m_primary = created.empty() ? -1 : created.back();
    MarkChanged();
    LOG_INFO("%zu 個のノードを複製しました (メッシュはインスタンス共有)", created.size());
}

void Editor::FrameAll() {
    UpdateWorld();
    m_scene.ComputeBounds(m_world, m_sceneMin, m_sceneMax);
    if (m_scene.Empty()) { m_sceneMin = glm::vec3(-1); m_sceneMax = glm::vec3(1); }
    m_camera.Frame(m_sceneMin, m_sceneMax);
}

void Editor::FocusNode(int node) {
    if (node < 0 || node >= (int)m_world.size()) return;
    glm::vec3 mn, mx;
    bool any = m_scene.ComputeNodeBounds(node, m_world, mn, mx);
    if (!any) {
        glm::vec3 c(m_world[node][3]);
        float r = m_sceneRadius * 0.1f;
        mn = c - glm::vec3(r);
        mx = c + glm::vec3(r);
    }
    m_camera.Frame(mn, mx);
}

void Editor::FrameSelected() {
    if (m_primary >= 0) FocusNode(m_primary);
    else FrameAll();
}

void Editor::ApplyFreeze(int node, bool hierarchy) {
    if (node < 0) return;
    Scene backup = m_scene;
    std::string err;
    if (hierarchy) {
        int n = Ops::FreezeHierarchy(m_scene, node, m_freezeT, m_freezeR, m_freezeS, &err);
        LOG_INFO("%d 個のノードをフリーズしました", n);
    } else if (!Ops::FreezeTransform(m_scene, node, m_freezeT, m_freezeR, m_freezeS, &err)) {
        LOG_ERROR("フリーズ失敗: %s", err.c_str());
        m_scene = std::move(backup);
        return;
    } else {
        LOG_INFO("'%s' をフリーズしました (T=%d R=%d S=%d)", m_scene.nodes[node].name.c_str(), m_freezeT, m_freezeR, m_freezeS);
    }
    m_undo.Push(backup, hierarchy ? "階層をフリーズ" : "フリーズ");
    m_eulerNode = -1;
    MarkChanged();
}

void Editor::ApplyRootScale(int node) {
    if (node < 0) return;
    Scene backup = m_scene;
    std::string err;
    if (!Ops::ApplyUniformScaleToHierarchy(m_scene, node, &err)) {
        LOG_ERROR("スケール適用失敗: %s", err.c_str());
        m_scene = std::move(backup);
        return;
    }
    m_undo.Push(backup, "スケールを階層に適用");
    MarkChanged();
}

// ---------------------------------------------------------------------------
// アニメーション / ワールド行列
// ---------------------------------------------------------------------------
void Editor::UpdateAnimation(float dt) {
    if (m_anim < 0 || m_anim >= (int)m_scene.animations.size()) { m_playing = false; return; }
    const Animation& a = m_scene.animations[m_anim];
    if (m_playing) {
        m_animTime += dt * m_animSpeed;
        if (a.duration <= 0) m_animTime = 0;
        else if (m_animTime > a.duration) {
            if (m_loop) m_animTime = std::fmod(m_animTime, a.duration);
            else { m_animTime = a.duration; m_playing = false; }
        } else if (m_animTime < 0) {
            m_animTime = m_loop ? a.duration + std::fmod(m_animTime, a.duration) : 0.0f;
        }
    }
}

void Editor::UpdateWorld() {
    m_pose = m_scene.RestPose();
    if (m_anim >= 0 && m_anim < (int)m_scene.animations.size()) SampleAnimation(m_scene, m_scene.animations[m_anim], m_animTime, m_pose);
    m_world = m_scene.ComputeWorldMatrices(m_pose);
    if (m_poseSceneRev != m_sceneRevision) {
        // シーンの大きさ (グリッド間隔・カメラのクリップ・法線の長さに使う) はレストポーズ基準で、
        // シーンが編集された時だけ更新する。アニメーション再生中に変化させるとグリッドが伸縮してしまう
        m_scene.ComputeBounds(m_scene.ComputeWorldMatrices(), m_sceneMin, m_sceneMax);
        m_sceneRadius = std::max(glm::length(m_sceneMax - m_sceneMin) * 0.5f, 1e-3f);
    }
    if (m_poseSceneRev != m_sceneRevision || m_poseAnimTime != m_animTime || m_poseAnim != m_anim) {
        ++m_poseRev;
        m_poseSceneRev = m_sceneRevision;
        m_poseAnimTime = m_animTime;
        m_poseAnim = m_anim;
    }
}

// ---------------------------------------------------------------------------
// ファイル
// ---------------------------------------------------------------------------
void Editor::NewScene() {
    Checkpoint("新規シーン");
    m_scene.Clear();
    m_currentPath.clear();
    ClearSelection();
    ValidateSelection();
    m_anim = -1;
    m_renderer.ClearCaches();
    MarkChanged();
    m_dirty = false;
}

void Editor::OpenDialog() {
    std::string path = Dialogs::OpenFile(ImportFileFilter(), "モデルを開く");
    if (!path.empty()) StartImport(path, m_importMode);
}

void Editor::OpenFile(const std::string& path) { StartImport(path, 0); }

void Editor::StartImport(const std::string& path, int mode) {
    if (m_importing) {
        // 読み込み中なら順番待ちに入れる (複数ファイルのドロップ等)
        m_importQueue.push_back({ path, mode });
        LOG_INFO("読み込み待ち: %s", FileUtil::Filename(path).c_str());
        return;
    }
    m_importing = true;
    m_importingPath = path;
    ImportOptions opts = m_importOpts;
    m_importModeRunning = mode;
    m_importFuture = std::async(std::launch::async, [path, opts]() {
        ImportResult r;
        r.path = path;
        auto t0 = std::chrono::steady_clock::now();
        try {
            r.ok = ImportModel(path, opts, r.scene, r.error);
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = std::string("例外: ") + e.what();
        } catch (...) {
            r.ok = false;
            r.error = "不明な例外";
        }
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return r;
    });
}

void Editor::PollImport() {
    if (!m_importing || !m_importFuture.valid()) return;
    if (m_importFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    ImportResult r = m_importFuture.get();
    m_importing = false;
    FinishImport(r);
    if (!m_importQueue.empty()) {
        auto next = m_importQueue.front();
        m_importQueue.pop_front();
        StartImport(next.first, next.second);
    }
}

void Editor::FinishImport(ImportResult& r) {
    if (!r.ok) {
        LOG_ERROR("'%s' を読み込めませんでした: %s", r.path.c_str(), r.error.c_str());
        return;
    }
    int mode = m_importModeRunning;
    if (mode == 3) mode = m_scene.Empty() ? 0 : 1; // 自動: 空なら置き換え、あれば追加
    if (mode == 2) {
        Checkpoint("アニメーション取り込み");
        int unmatched = 0;
        size_t before = m_scene.animations.size();
        int matched = Ops::ImportAnimationsByName(m_scene, r.scene, &unmatched);
        LOG_INFO("アニメーション %zu 個を取り込みました (対応チャンネル %d, 未対応 %d)", m_scene.animations.size() - before, matched, unmatched);
        if (m_scene.animations.size() > before) { m_anim = (int)before; m_animTime = 0; m_playing = true; }
        MarkChanged();
        return;
    }
    if (mode == 1 && !m_scene.Empty()) {
        Checkpoint("シーンに追加");
        size_t rootsBefore = m_scene.roots.size();
        Ops::AppendScene(m_scene, r.scene);
        // 追加したファイルを 1 つのオブジェクト (ファイル名のグループ) にまとめる
        std::vector<int> newRoots(m_scene.roots.begin() + rootsBefore, m_scene.roots.end());
        int group = m_scene.AddNode(FileUtil::Stem(r.path), -1);
        for (int nr : newRoots) m_scene.Reparent(nr, group, false);
        m_scene.nodes[group].objectRoot = true;
        ClearSelection();
        Select(group);
        LOG_INFO("'%s' をオブジェクト '%s' として追加しました (オブジェクト数 %zu)", FileUtil::Filename(r.path).c_str(),
                 m_scene.nodes[group].name.c_str(), m_scene.roots.size());
        MarkChanged();
        return;
    }
    if (!m_scene.Empty()) Checkpoint("ファイルを開く");
    m_scene = std::move(r.scene);
    m_currentPath = r.path;
    m_renderer.ClearCaches();
    ClearSelection();
    m_selMesh = m_scene.meshes.empty() ? -1 : 0;
    m_selMaterial = m_scene.materials.empty() ? -1 : 0;
    m_selTexture = m_scene.textures.empty() ? -1 : 0;
    m_selBone = -1;
    m_selSkin = 0;
    m_anim = m_scene.animations.empty() ? -1 : 0;
    m_animTime = 0;
    m_playing = false;
    ValidateSelection();
    MarkChanged();
    m_dirty = false;
    AddRecent(r.path);
    FrameAll();
    std::string title = "ModelEditor - " + FileUtil::Filename(r.path);
    {
        std::wstring wt = FileUtil::ToPath(title).wstring();
        SetWindowTextW((HWND)m_window, wt.c_str());
    }
    m_lastExportPath.clear();
    if (m_playOnLoad && m_anim >= 0) m_playing = true;
    if (m_hasRulerAfterLoad) {
        UpdateWorld();
        int r = AddRuler();
        if (m_rulerAfterLoad != 0.0f) m_rulers[r].height = m_rulerAfterLoad;
        m_hasRulerAfterLoad = false;
    }
    if (!m_selectAfterLoad.empty()) {
        int n = m_scene.FindNode(m_selectAfterLoad);
        if (n >= 0) Select(n);
        m_selectAfterLoad.clear();
    }
}

static const char* kExportFilter =
    "glTF Binary (*.glb)\0*.glb\0glTF (*.gltf)\0*.gltf\0FBX (*.fbx)\0*.fbx\0OBJ (*.obj)\0*.obj\0Collada (*.dae)\0*.dae\0STL (*.stl)\0*.stl\0PLY (*.ply)\0*.ply\0"
    "DirectX (*.x)\0*.x\0" "3MF (*.3mf)\0*.3mf\0" "3ds Max (*.3ds)\0*.3ds\0";

void Editor::ExportDialog() {
    std::string base = m_currentPath.empty() ? "untitled" : FileUtil::Stem(m_currentPath);
    std::string ext = ExportFormatExt(m_exportOpts.format);
    std::string path = Dialogs::SaveFile(kExportFilter, base + ext, ext.c_str(), "書き出し");
    if (!path.empty()) DoExport(path);
}

bool Editor::DoExport(const std::string& path) {
    std::string err;
    ExportOptions opts = m_exportOpts;
    opts.format = ExportFormatFromPath(path, m_exportOpts.format);
    bool ok = false;
    try {
        ok = ExportScene(m_scene, path, opts, err);
    } catch (const std::exception& e) {
        err = std::string("例外: ") + e.what();
    } catch (...) {
        err = "不明な例外";
    }
    if (!ok) {
        LOG_ERROR("書き出しに失敗しました: %s", err.c_str());
        return false;
    }
    m_lastExportPath = path;
    m_dirty = false;
    AddRecent(path);
    return true;
}

void Editor::QuickExport() {
    if (m_lastExportPath.empty()) ExportDialog();
    else DoExport(m_lastExportPath);
}

void Editor::AddRecent(const std::string& path) {
    m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), path), m_recent.end());
    m_recent.insert(m_recent.begin(), path);
    if (m_recent.size() > 12) m_recent.resize(12);
    SaveSettings();
}

void Editor::OnDropFiles(const std::vector<std::string>& paths) {
    for (auto& p : paths) {
        std::string ext = FileUtil::LowerExt(p);
        if (IsSupportedModelExtension(ext)) {
            StartImport(p, m_dropMode); // 複数ファイルは順番に読み込む
            continue;
        }
        static const char* imgExts[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr" };
        bool isImage = false;
        for (auto* e : imgExts) if (ext == e) isImage = true;
        if (isImage) {
            int t = LoadTextureFile(p);
            // マテリアルが選択されていてベースカラーが空なら割り当て
            if (t >= 0 && m_selMaterial >= 0 && m_selMaterial < (int)m_scene.materials.size() && !m_scene.materials[m_selMaterial].slots[Slot_BaseColor].Valid()) {
                m_scene.materials[m_selMaterial].slots[Slot_BaseColor].texture = t;
                LOG_INFO("マテリアル '%s' のベースカラーに割り当てました", m_scene.materials[m_selMaterial].name.c_str());
            }
        } else if (!IsSupportedModelExtension(ext)) {
            LOG_WARN("未対応のファイル形式です: %s", p.c_str());
        }
    }
}

int Editor::LoadTextureFile(const std::string& path) {
    Texture tex;
    std::string err;
    if (!ImageIO::LoadFile(path, tex, &err)) {
        LOG_ERROR("テクスチャを読み込めません: %s", err.c_str());
        return -1;
    }
    tex.originalUri = FileUtil::Filename(path);
    Checkpoint("テクスチャ読み込み");
    m_scene.textures.push_back(std::move(tex));
    m_selTexture = (int)m_scene.textures.size() - 1;
    MarkChanged();
    LOG_INFO("テクスチャを読み込みました: %s (%dx%d)", path.c_str(), m_scene.textures.back().Width(), m_scene.textures.back().Height());
    return m_selTexture;
}

void Editor::RelinkMissingTextures(const std::string& dir) {
    Checkpoint("テクスチャ再リンク");
    int fixedCount = 0;
    for (auto& t : m_scene.textures) {
        if (!t.missing && t.image) continue;
        std::string p = FileUtil::ResolveTexturePath(t.originalUri, dir, { dir });
        if (p.empty()) continue;
        Texture nt = t;
        if (ImageIO::LoadFile(p, nt, nullptr)) {
            nt.missing = false;
            nt.uid = NewUid();
            nt.name = t.name;
            t = nt;
            ++fixedCount;
        }
    }
    LOG_INFO("%d 個のテクスチャを再リンクしました", fixedCount);
    MarkChanged();
}

ImTextureID Editor::TexId(int texture) {
    if (texture < 0 || texture >= (int)m_scene.textures.size()) return (ImTextureID)0;
    return (ImTextureID)(intptr_t)m_renderer.GetTextureSRV(m_scene.textures[texture]);
}

// ---------------------------------------------------------------------------
// 設定
// ---------------------------------------------------------------------------
void Editor::LoadSettings() {
    std::vector<uint8_t> data;
    if (!FileUtil::ReadBinary(m_settingsPath, data)) return;
    std::istringstream in(std::string(data.begin(), data.end()));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto b = [&]() { return v == "1"; };
        auto f = [&]() { return (float)atof(v.c_str()); };
        if (k == "recent") m_recent.push_back(v);
        else if (k == "ui_scale") m_uiScale = std::clamp(f(), 0.5f, 3.0f);
        else if (k == "bg") sscanf(v.c_str(), "%f %f %f", &m_rs.bgColor.r, &m_rs.bgColor.g, &m_rs.bgColor.b);
        else if (k == "msaa") m_rs.msaa = atoi(v.c_str());
        else if (k == "show_grid") m_rs.showGrid = b();
        else if (k == "grid_spacing") m_rs.gridSpacing = std::max(f(), 1e-4f);
        else if (k == "grid_major") m_rs.gridMajorEvery = std::max(atoi(v.c_str()), 1);
        else if (k == "grid_extent") m_rs.gridExtent = std::max(f(), 1e-3f);
        else if (k == "show_bones") m_rs.showBones = b();
        else if (k == "bones_on_top") m_rs.bonesOnTop = b();
        else if (k == "backface") m_rs.backfaceCulling = b();
        else if (k == "light_intensity") m_rs.lightIntensity = f();
        else if (k == "ambient") m_rs.ambient = f();
        else if (k == "exposure") m_rs.exposure = f();
        else if (k == "imp_units") m_importOpts.convertUnitsToMeters = b();
        else if (k == "imp_axis") m_importOpts.convertUpAxisToY = b();
        else if (k == "imp_dummy") m_importOpts.removeDummyRoot = b();
        else if (k == "imp_tangents") m_importOpts.calcTangents = b();
        else if (k == "imp_join") m_importOpts.joinVertices = b();
        else if (k == "imp_flipuv") m_importOpts.flipUV = b();
        else if (k == "imp_pivots") m_importOpts.preservePivots = b();
        else if (k == "imp_scale") m_importOpts.extraScale = f();
        else if (k == "imp_search") m_importOpts.textureSearchDirs.push_back(v);
        else if (k == "exp_format") m_exportOpts.format = (ExportFormat)std::clamp(atoi(v.c_str()), 0, (int)ExportFormat::Count - 1);
        else if (k == "exp_texdir") m_exportOpts.textureSubdir = v;
        else if (k == "exp_verify") m_exportOpts.verifyAfterExport = b();
        else if (k == "exp_embed_fbx") m_exportOpts.embedTexturesFBX = b();
        else if (k == "exp_objscale") m_exportOpts.objectScaleMode = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "exp_objdissolve") m_exportOpts.dissolveObjectWrappers = b();
        else if (k == "undo_max") m_undo.SetMaxEntries((size_t)atoi(v.c_str()));
        else if (k == "auto_validate") m_autoValidate = b();
        else if (k == "drop_mode") m_dropMode = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "lmb_orbit") m_lmbOrbit = b();
        else if (k == "display_unit") m_displayUnit = std::clamp(atoi(v.c_str()), 0, 4);
        else if (k == "size_overlay") m_showSizeOverlay = b();
    }
}

void Editor::SaveSettings() {
    std::ostringstream o;
    for (auto& r : m_recent) o << "recent=" << r << "\n";
    o << "ui_scale=" << m_uiScale << "\n";
    o << "bg=" << m_rs.bgColor.r << " " << m_rs.bgColor.g << " " << m_rs.bgColor.b << "\n";
    o << "msaa=" << m_rs.msaa << "\n";
    o << "grid_spacing=" << m_rs.gridSpacing << "\ngrid_major=" << m_rs.gridMajorEvery << "\ngrid_extent=" << m_rs.gridExtent << "\n";
    o << "show_grid=" << m_rs.showGrid << "\nshow_bones=" << m_rs.showBones << "\nbones_on_top=" << m_rs.bonesOnTop << "\n";
    o << "backface=" << m_rs.backfaceCulling << "\nlight_intensity=" << m_rs.lightIntensity << "\nambient=" << m_rs.ambient << "\nexposure=" << m_rs.exposure << "\n";
    o << "imp_units=" << m_importOpts.convertUnitsToMeters << "\nimp_axis=" << m_importOpts.convertUpAxisToY << "\nimp_dummy=" << m_importOpts.removeDummyRoot << "\n";
    o << "imp_tangents=" << m_importOpts.calcTangents << "\nimp_join=" << m_importOpts.joinVertices << "\nimp_flipuv=" << m_importOpts.flipUV << "\n";
    o << "imp_pivots=" << m_importOpts.preservePivots << "\nimp_scale=" << m_importOpts.extraScale << "\n";
    for (auto& d : m_importOpts.textureSearchDirs) o << "imp_search=" << d << "\n";
    o << "exp_format=" << (int)m_exportOpts.format << "\nexp_texdir=" << m_exportOpts.textureSubdir << "\n";
    o << "exp_verify=" << m_exportOpts.verifyAfterExport << "\nexp_embed_fbx=" << m_exportOpts.embedTexturesFBX << "\n";
    o << "exp_objscale=" << m_exportOpts.objectScaleMode << "\nexp_objdissolve=" << m_exportOpts.dissolveObjectWrappers << "\n";
    o << "undo_max=" << m_undo.MaxEntries() << "\nauto_validate=" << m_autoValidate << "\n";
    o << "drop_mode=" << m_dropMode << "\nlmb_orbit=" << m_lmbOrbit << "\n";
    o << "display_unit=" << m_displayUnit << "\nsize_overlay=" << m_showSizeOverlay << "\n";
    FileUtil::WriteText(m_settingsPath, o.str());
}

// ---------------------------------------------------------------------------
// フレーム
// ---------------------------------------------------------------------------
void Editor::Frame() {
    ImGuiIO& io = ImGui::GetIO();
    auto frameStart = std::chrono::steady_clock::now();
    if (!ImGui::IsAnyItemActive()) {
        m_editSessionId = 0;
        if (m_hasPending) { m_hasPending = false; m_pendingSnapshot = Scene(); }
    }
    m_frameTimes.push_back(io.DeltaTime * 1000.0f);
    while (m_frameTimes.size() > 240) m_frameTimes.pop_front();

    m_renderer.BeginFrame();
    PollImport();
    HandleShortcuts();
    UpdateAnimation(io.DeltaTime);
    UpdateWorld();

    if (m_autoValidate && m_issuesRevision != m_sceneRevision) {
        m_issues = ValidateScene(m_scene);
        m_issuesRevision = m_sceneRevision;
    }

    DrawMainMenu();
    ImGuiID dockId = ImGui::DockSpaceOverViewport(ImGui::GetID("MainDock"), ImGui::GetMainViewport());
    if (m_firstFrame) {
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        if (!node || node->IsLeafNode()) m_resetLayout = true; // ini が無い初回
        m_firstFrame = false;
    }
    if (m_resetLayout) {
        SetupDockLayout(dockId);
        m_resetLayout = false;
    }

    if (m_panels.viewport) DrawViewport();
    if (m_panels.hierarchy) DrawHierarchy();
    if (m_panels.inspector) DrawInspector();
    if (m_panels.meshes) DrawMeshes();
    if (m_panels.materials) DrawMaterials();
    if (m_panels.textures) DrawTextures();
    if (m_panels.skeleton) DrawSkeleton();
    if (m_panels.animation) DrawAnimation();
    if (m_panels.console) DrawConsole();
    if (m_panels.validation) DrawValidation();
    if (m_panels.stats) DrawStats();
    if (m_panels.sceneInfo) DrawSceneInfo();
    if (m_panels.history) DrawHistory();
    if (m_panels.uvViewer) DrawUVViewer();
    if (m_panels.exportWin) DrawExportWindow();
    if (m_panels.importWin) DrawImportWindow();
    if (m_panels.batchRename) DrawBatchRename();
    if (m_panels.settings) DrawSettings();
    if (m_panels.help) DrawHelp();
    if (m_panels.dump) DrawDump();
    if (m_panels.measure) DrawMeasure();
    if (m_panels.imguiDemo) ImGui::ShowDemoWindow(&m_panels.imguiDemo);
    if (m_panels.imguiMetrics) ImGui::ShowMetricsWindow(&m_panels.imguiMetrics);
    DrawImportingPopup();
    DrawQuitConfirm();
    if (m_smokeTest) SmokeTestStep();
    // デバッグ用: 指定ウィンドウを前面に (タブを切り替える)
    if (++m_frameIndex > 3 && !m_focusWindows.empty()) {
        for (auto& w : m_focusWindows) ImGui::SetWindowFocus(w.c_str());
        m_focusWindows.clear();
    }

    m_lastFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
}

void Editor::SetupDockLayout(unsigned dockId) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);
    ImGuiID center = dockId;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42f, nullptr, &left);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
    ImGui::DockBuilderDockWindow("ビューポート###Viewport", center);
    ImGui::DockBuilderDockWindow("階層###Hierarchy", left);
    ImGui::DockBuilderDockWindow("スケルトン###Skeleton", leftBottom);
    ImGui::DockBuilderDockWindow("シーン情報###SceneInfo", leftBottom);
    ImGui::DockBuilderDockWindow("単位 / 定規###Measure", leftBottom);
    ImGui::DockBuilderDockWindow("インスペクター###Inspector", right);
    ImGui::DockBuilderDockWindow("メッシュ###Meshes", rightBottom);
    ImGui::DockBuilderDockWindow("マテリアル###Materials", rightBottom);
    ImGui::DockBuilderDockWindow("テクスチャ###Textures", rightBottom);
    ImGui::DockBuilderDockWindow("アニメーション###Animation", bottom);
    ImGui::DockBuilderDockWindow("コンソール###Console", bottom);
    ImGui::DockBuilderDockWindow("検証###Validation", bottom);
    ImGui::DockBuilderDockWindow("統計 / デバッグ###Stats", bottom);
    ImGui::DockBuilderDockWindow("履歴###History", bottom);
    ImGui::DockBuilderFinish(dockId);
}

void Editor::HandleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (shift) { if (m_undo.Redo(m_scene)) { ValidateSelection(); MarkChanged(); LOG_INFO("やり直し"); } }
        else if (m_undo.Undo(m_scene)) { ValidateSelection(); MarkChanged(); LOG_INFO("元に戻す"); }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && m_undo.Redo(m_scene)) { ValidateSelection(); MarkChanged(); LOG_INFO("やり直し"); }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) OpenDialog();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) { if (shift) ExportDialog(); else QuickExport(); }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_E, false)) m_panels.exportWin = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) NewScene();

    // テキスト入力中は単キーのショートカットを無効化
    if (io.WantTextInput) return;
    bool viewportCtx = m_viewportHovered || m_viewportFocused;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) DuplicateSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && (viewportCtx || ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))) {
        // 階層かビューポートにフォーカスがある時のみ
        ImGuiWindow* w = GImGui->NavWindow;
        if (w && (strstr(w->Name, "Hierarchy") || strstr(w->Name, "Viewport"))) DeleteSelected();
    }
    // 表示/非表示 (H: 選択の切替, Ctrl+H: 非表示, Shift+H: すべて表示, I: 選択のみ表示)
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
        if (shift) ShowAll();
        else if (ctrl) SetVisibility(EditTargets(), false, "非表示");
        else ToggleSelectionVisibility();
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false)) IsolateSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && m_primary >= 0) { m_renameNode = m_primary; m_renameBuf = m_scene.nodes[m_primary].name; }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && viewportCtx && m_anim >= 0) m_playing = !m_playing;
    if (!viewportCtx || ctrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmoOp = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmoOp = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoOp = 2;
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) m_gizmoOp = 3;
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) m_gizmoLocal = !m_gizmoLocal;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) FrameSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) FrameAll();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ClearSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_4, false)) m_rs.wireframe = !m_rs.wireframe;
    if (ImGui::IsKeyPressed(ImGuiKey_5, false)) { m_rs.wireframe = false; m_rs.wireOnShaded = false; }
    if (ImGui::IsKeyPressed(ImGuiKey_6, false)) m_rs.useTextures = !m_rs.useTextures;
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad1, false)) m_camera.SetView(0, 0);
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad3, false)) m_camera.SetView(90, 0);
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad7, false)) m_camera.SetView(0, -89.5f);
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad5, false)) m_camera.ortho = !m_camera.ortho;
}

// ---------------------------------------------------------------------------
// メインメニュー
// ---------------------------------------------------------------------------
void Editor::DrawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("ファイル")) {
        if (ImGui::MenuItem("新規シーン", "Ctrl+N")) NewScene();
        if (ImGui::MenuItem("開く...", "Ctrl+O")) { m_importMode = 0; OpenDialog(); }
        if (ImGui::MenuItem("シーンに追加読み込み...")) {
            std::string p = Dialogs::OpenFile(ImportFileFilter(), "追加で読み込む");
            if (!p.empty()) StartImport(p, 1);
        }
        if (ImGui::MenuItem("アニメーションのみ取り込み (名前で対応付け)...")) {
            std::string p = Dialogs::OpenFile(ImportFileFilter(), "アニメーションを取り込む");
            if (!p.empty()) StartImport(p, 2);
        }
        if (ImGui::BeginMenu("最近使ったファイル", !m_recent.empty())) {
            for (auto& r : std::vector<std::string>(m_recent))
                if (ImGui::MenuItem(r.c_str())) StartImport(r, 0);
            ImGui::Separator();
            if (ImGui::MenuItem("履歴をクリア")) m_recent.clear();
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("再読み込み", nullptr, false, !m_currentPath.empty())) StartImport(m_currentPath, 0);
        ImGui::Separator();
        ImGui::MenuItem("インポート設定...", nullptr, &m_panels.importWin);
        ImGui::MenuItem("エクスポート設定...", "Ctrl+E", &m_panels.exportWin);
        if (ImGui::MenuItem("書き出し (前回のパス)", "Ctrl+S", false, !m_scene.Empty())) QuickExport();
        if (ImGui::MenuItem("名前を付けて書き出し...", "Ctrl+Shift+S", false, !m_scene.Empty())) ExportDialog();
        if (ImGui::BeginMenu("クイック書き出し", !m_scene.Empty())) {
            std::string base = m_currentPath.empty() ? FileUtil::Join(FileUtil::DocumentsDir(), "untitled") : FileUtil::ReplaceExt(m_currentPath, "");
            for (auto [fmt, suffix] : { std::pair{ ExportFormat::GLB, "_export.glb" }, std::pair{ ExportFormat::GLTF, "_export.gltf" }, std::pair{ ExportFormat::FBX, "_export.fbx" } }) {
                std::string p = base + suffix;
                if (ImGui::MenuItem(FileUtil::Filename(p).c_str())) {
                    ExportFormat prev = m_exportOpts.format;
                    m_exportOpts.format = fmt;
                    DoExport(p);
                    m_exportOpts.format = prev;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("エクスプローラーで表示", nullptr, false, !m_currentPath.empty())) Dialogs::OpenInExplorer(m_currentPath);
        ImGui::Separator();
        if (ImGui::MenuItem("終了", "Alt+F4")) RequestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("編集")) {
        std::string undoLabel = m_undo.CanUndo() ? "元に戻す: " + m_undo.UndoEntries().back().label : "元に戻す";
        std::string redoLabel = m_undo.CanRedo() ? "やり直し: " + m_undo.RedoEntries().back().label : "やり直し";
        if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, m_undo.CanUndo())) { m_undo.Undo(m_scene); ValidateSelection(); MarkChanged(); }
        if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, m_undo.CanRedo())) { m_undo.Redo(m_scene); ValidateSelection(); MarkChanged(); }
        ImGui::MenuItem("履歴パネル", nullptr, &m_panels.history);
        ImGui::Separator();
        if (ImGui::MenuItem("複製", "Ctrl+D", false, !m_selection.empty())) DuplicateSelected();
        if (ImGui::MenuItem("削除", "Delete", false, !m_selection.empty())) DeleteSelected();
        if (ImGui::MenuItem("名前変更", "F2", false, m_primary >= 0)) { m_renameNode = m_primary; m_renameBuf = m_scene.nodes[m_primary].name; }
        if (ImGui::MenuItem("すべて選択")) { m_selection.clear(); for (int i = 0; i < (int)m_scene.nodes.size(); ++i) m_selection.push_back(i); m_primary = m_selection.empty() ? -1 : 0; }
        if (ImGui::MenuItem("選択解除", "Esc")) ClearSelection();
        if (ImGui::MenuItem("子を選択に追加", nullptr, false, !m_selection.empty())) {
            std::function<void(int)> rec = [&](int i) { if (!IsSelected(i)) m_selection.push_back(i); for (int c : m_scene.nodes[i].children) rec(c); };
            for (int s : std::vector<int>(m_selection)) rec(s);
        }
        ImGui::Separator();
        ImGui::MenuItem("環境設定...", nullptr, &m_panels.settings);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("作成")) {
        if (ImGui::MenuItem("空のノード")) {
            Checkpoint("ノード作成");
            int parent = m_primary;
            int n = m_scene.AddNode("Node", parent);
            Select(n);
            MarkChanged();
        }
        if (ImGui::MenuItem("空のノード (ルート)")) {
            Checkpoint("ノード作成");
            int n = m_scene.AddNode("Node", -1);
            Select(n);
            MarkChanged();
        }
        if (ImGui::MenuItem("選択をグループ化 (新しい親)", nullptr, false, !m_selection.empty())) {
            Checkpoint("グループ化");
            int parent = m_scene.nodes[m_selection[0]].parent;
            int g = m_scene.AddNode("Group", parent);
            for (int s : std::vector<int>(m_selection)) if (s != g) m_scene.Reparent(s, g, true);
            Select(g);
            MarkChanged();
        }
        if (ImGui::BeginMenu("基本形状 (1 m)")) {
            for (int t = 0; t < Ops::Prim_Count; ++t)
                if (ImGui::MenuItem(Ops::PrimitiveName(t))) CreatePrimitiveObject(t);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("マテリアル")) {
            Checkpoint("マテリアル作成");
            m_selMaterial = Ops::AddMaterial(m_scene, "Material_" + std::to_string(m_scene.materials.size()));
            MarkChanged();
        }
        if (ImGui::MenuItem("テクスチャを読み込み...")) {
            std::string p = Dialogs::OpenFile("Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif\0All\0*.*\0", "テクスチャ");
            if (!p.empty()) LoadTextureFile(p);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("ツール")) {
        ImGui::TextDisabled("トランスフォーム");
        ImGui::Checkbox("移動", &m_freezeT); ImGui::SameLine();
        ImGui::Checkbox("回転", &m_freezeR); ImGui::SameLine();
        ImGui::Checkbox("スケール", &m_freezeS);
        if (ImGui::MenuItem("選択をフリーズ (Freeze Transformations)", nullptr, false, m_primary >= 0))
            for (int s : std::vector<int>(m_selection)) ApplyFreeze(s, false);
        if (ImGui::MenuItem("選択を階層ごとフリーズ", nullptr, false, m_primary >= 0))
            for (int s : std::vector<int>(m_selection)) ApplyFreeze(s, true);
        if (ImGui::MenuItem("全ノードをフリーズ (ジョイント除く)", nullptr, false, !m_scene.Empty())) {
            Checkpoint("全体をフリーズ");
            int n = 0;
            for (int r : std::vector<int>(m_scene.roots)) n += Ops::FreezeHierarchy(m_scene, r, m_freezeT, m_freezeR, m_freezeS, nullptr);
            LOG_INFO("%d 個のノードをフリーズしました", n);
            MarkChanged();
        }
        if (ImGui::MenuItem("スケールを階層に適用 (リグ対応)", nullptr, false, m_primary >= 0)) ApplyRootScale(m_primary);
        if (ImGui::MenuItem("全ルートのスケールを階層に適用", nullptr, false, !m_scene.Empty())) {
            Scene backup = m_scene;
            for (int r : std::vector<int>(m_scene.roots)) {
                std::string e;
                if (!Ops::ApplyUniformScaleToHierarchy(m_scene, r, &e) && !e.empty()) LOG_WARN("%s", e.c_str());
            }
            m_undo.Push(backup, "全ルートのスケール適用");
            MarkChanged();
        }
        if (ImGui::MenuItem("ピボットをメッシュ中心へ", nullptr, false, m_primary >= 0)) {
            Checkpoint("ピボット中心");
            std::string e;
            if (!Ops::CenterPivot(m_scene, m_primary, &e)) LOG_ERROR("%s", e.c_str());
            MarkChanged();
        }
        ImGui::Separator();
        ImGui::TextDisabled("シーン");
        ImGui::SetNextItemWidth(100);
        ImGui::InputFloat("##unit", &m_unitScale, 0, 0, "%.6g");
        ImGui::SameLine();
        if (ImGui::MenuItem("シーン全体をスケール")) { Checkpoint("シーンスケール"); Ops::ScaleScene(m_scene, m_unitScale); MarkChanged(); FrameAll(); }
        if (ImGui::MenuItem("cm → m (x0.01)")) { Checkpoint("cm→m"); Ops::ScaleScene(m_scene, 0.01f); MarkChanged(); FrameAll(); }
        if (ImGui::MenuItem("m → cm (x100)")) { Checkpoint("m→cm"); Ops::ScaleScene(m_scene, 100.0f); MarkChanged(); FrameAll(); }
        if (ImGui::MenuItem("Z-up → Y-up")) { Checkpoint("Z-up→Y-up"); Ops::ConvertUpAxis(m_scene, 2, 1); MarkChanged(); }
        if (ImGui::MenuItem("Y-up → Z-up")) { Checkpoint("Y-up→Z-up"); Ops::ConvertUpAxis(m_scene, 1, 2); MarkChanged(); }
        if (ImGui::MenuItem("原点に移動 (接地)")) { Checkpoint("原点へ"); Ops::CenterSceneToOrigin(m_scene, true); MarkChanged(); }
        if (ImGui::MenuItem("地面に置く (Y=0)")) { Checkpoint("接地"); Ops::PlaceOnGround(m_scene); MarkChanged(); }
        ImGui::Separator();
        ImGui::TextDisabled("整理");
        if (ImGui::MenuItem("未使用のメッシュ/マテリアル/テクスチャを削除")) { Checkpoint("未使用削除"); LOG_INFO("%d 個削除", Ops::RemoveUnusedResources(m_scene)); ValidateSelection(); MarkChanged(); }
        if (ImGui::MenuItem("空ノードを削除")) { Checkpoint("空ノード削除"); LOG_INFO("%d 個削除", Ops::RemoveEmptyNodes(m_scene)); ClearSelection(); ValidateSelection(); MarkChanged(); }
        if (ImGui::MenuItem("重複したノード名を修正")) { Checkpoint("名前修正"); LOG_INFO("%d 個修正", Ops::UniquifyNames(m_scene)); MarkChanged(); }
        if (ImGui::MenuItem("名前空間を除去 (xxx:Name → Name)")) { Checkpoint("名前空間除去"); LOG_INFO("%d 個修正", Ops::StripNamespaces(m_scene)); MarkChanged(); }
        ImGui::MenuItem("一括リネーム...", nullptr, &m_panels.batchRename);
        ImGui::Separator();
        ImGui::TextDisabled("全メッシュ");
        auto allPrims = [&](const char* label, auto&& fn) {
            Checkpoint(label);
            for (auto& m : m_scene.meshes) { for (auto& p : m.primitives) fn(p); m.Touch(); }
            MarkChanged();
        };
        if (ImGui::MenuItem("法線を再計算 (スムーズ)")) allPrims("法線再計算", [&](Primitive& p) { Ops::RecalculateNormals(p, true, m_normalAngle); });
        if (ImGui::MenuItem("接線を生成")) allPrims("接線生成", [](Primitive& p) { Ops::GenerateTangents(p); });
        if (ImGui::MenuItem("ウェイトを正規化")) allPrims("ウェイト正規化", [](Primitive& p) { if (p.HasSkin()) Ops::NormalizeWeights(p); });
        if (ImGui::MenuItem("縮退三角形を削除")) allPrims("縮退削除", [](Primitive& p) { Ops::RemoveDegenerateTriangles(p); });
        if (ImGui::MenuItem("頂点を結合 (Weld)")) allPrims("頂点結合", [&](Primitive& p) { Ops::WeldVertices(p, m_weldEps); });
        if (ImGui::MenuItem("UV の V を反転")) allPrims("UV反転", [](Primitive& p) { Ops::FlipUV(p, false, true); });
        if (ImGui::MenuItem("検証の自動修正をすべて適用")) {
            Checkpoint("自動修正");
            int n = 0;
            for (auto& is : ValidateScene(m_scene)) if (is.fix != FixAction::None && ApplyFix(m_scene, is)) ++n;
            LOG_INFO("%d 件の問題を修正しました", n);
            ValidateSelection();
            MarkChanged();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("表示")) {
        ImGui::MenuItem("ビューポート", nullptr, &m_panels.viewport);
        ImGui::MenuItem("階層", nullptr, &m_panels.hierarchy);
        ImGui::MenuItem("インスペクター", nullptr, &m_panels.inspector);
        ImGui::MenuItem("メッシュ", nullptr, &m_panels.meshes);
        ImGui::MenuItem("マテリアル", nullptr, &m_panels.materials);
        ImGui::MenuItem("テクスチャ", nullptr, &m_panels.textures);
        ImGui::MenuItem("スケルトン / ボーン", nullptr, &m_panels.skeleton);
        ImGui::MenuItem("アニメーション", nullptr, &m_panels.animation);
        ImGui::MenuItem("UV ビューア", nullptr, &m_panels.uvViewer);
        ImGui::MenuItem("シーン情報", nullptr, &m_panels.sceneInfo);
        ImGui::MenuItem("単位 / 実寸 / 定規", nullptr, &m_panels.measure);
        ImGui::Separator();
        if (ImGui::BeginMenu("表示モード")) {
            for (int i = 0; i < RM_Count; ++i)
                if (ImGui::MenuItem(RenderModeName(i), nullptr, m_rs.mode == i)) m_rs.mode = i;
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("選択の表示を切替", "H")) ToggleSelectionVisibility();
        if (ImGui::MenuItem("選択を非表示", "Ctrl+H")) SetVisibility(EditTargets(), false, "非表示");
        if (ImGui::MenuItem("選択のみ表示 (分離)", "I")) IsolateSelection();
        if (ImGui::MenuItem("すべて表示", "Shift+H")) ShowAll();
        ImGui::Separator();
        ImGui::MenuItem("ワイヤーフレーム", "4", &m_rs.wireframe);
        ImGui::MenuItem("シェード + ワイヤー", nullptr, &m_rs.wireOnShaded);
        ImGui::MenuItem("テクスチャ", "6", &m_rs.useTextures);
        ImGui::MenuItem("グリッド", nullptr, &m_rs.showGrid);
        ImGui::MenuItem("ボーン", nullptr, &m_rs.showBones);
        ImGui::MenuItem("ボーン名", nullptr, &m_showBoneNames);
        ImGui::MenuItem("法線", nullptr, &m_rs.showNormals);
        ImGui::MenuItem("接線", nullptr, &m_rs.showTangents);
        ImGui::MenuItem("バウンディングボックス", nullptr, &m_rs.showBounds);
        ImGui::MenuItem("ノード軸", nullptr, &m_rs.showNodeAxes);
        ImGui::Separator();
        if (ImGui::MenuItem("全体を表示", "A")) FrameAll();
        if (ImGui::MenuItem("選択を表示", "F")) FrameSelected();
        if (ImGui::MenuItem("正面 (Num1)")) m_camera.SetView(0, 0);
        if (ImGui::MenuItem("右 (Num3)")) m_camera.SetView(90, 0);
        if (ImGui::MenuItem("上 (Num7)")) m_camera.SetView(0, -89.5f);
        ImGui::MenuItem("平行投影 (Num5)", nullptr, &m_camera.ortho);
        ImGui::Separator();
        if (ImGui::MenuItem("レイアウトをリセット")) m_resetLayout = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("デバッグ")) {
        ImGui::MenuItem("コンソール", nullptr, &m_panels.console);
        ImGui::MenuItem("検証", nullptr, &m_panels.validation);
        ImGui::MenuItem("統計 / デバッグ", nullptr, &m_panels.stats);
        ImGui::MenuItem("シーンダンプ", nullptr, &m_panels.dump);
        ImGui::MenuItem("自動検証", nullptr, &m_autoValidate);
        ImGui::Separator();
        if (ImGui::MenuItem("GPU キャッシュをクリア")) m_renderer.ClearCaches();
        if (ImGui::MenuItem("ログをファイルに保存...")) {
            std::string p = Dialogs::SaveFile("Text\0*.txt\0", "editor_log.txt", ".txt");
            if (!p.empty()) {
                std::string s;
                Log::Get().Visit([&](const std::vector<LogEntry>& es) { for (auto& e : es) s += e.text + "\n"; });
                FileUtil::WriteText(p, s);
            }
        }
        ImGui::Separator();
        ImGui::MenuItem("ImGui デモ", nullptr, &m_panels.imguiDemo);
        ImGui::MenuItem("ImGui メトリクス", nullptr, &m_panels.imguiMetrics);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("ヘルプ")) {
        ImGui::MenuItem("操作方法", "F1", &m_panels.help);
        ImGui::EndMenu();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) m_panels.help = !m_panels.help;

    // 右端にステータス
    std::string status = m_currentPath.empty() ? "(無題)" : FileUtil::Filename(m_currentPath);
    if (m_dirty) status += " *";
    if (m_importing) status = "読み込み中... " + FileUtil::Filename(m_importingPath);
    int errors = 0, warns = 0;
    for (auto& is : m_issues) { if (is.severity == Severity::Error) ++errors; else if (is.severity == Severity::Warning) ++warns; }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s  |  エラー %d 警告 %d  |  %.0f FPS", status.c_str(), errors, warns, ImGui::GetIO().Framerate);
    float w = ImGui::CalcTextSize(buf).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - 20);
    ImGui::TextColored(errors ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(0.7f, 0.8f, 0.7f, 1), "%s", buf);
    ImGui::EndMainMenuBar();
}

void Editor::DrawImportingPopup() {
    if (!m_importing) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##importing", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
    static const char spin[] = "|/-\\";
    ImGui::Text("%c 読み込み中: %s", spin[(int)(ImGui::GetTime() * 8) % 4], FileUtil::Filename(m_importingPath).c_str());
    ImGui::End();
}

void Editor::DrawQuitConfirm() {
    if (m_confirmQuitOpen) {
        ImGui::OpenPopup("終了の確認");
        m_confirmQuitOpen = false;
    }
    if (ImGui::BeginPopupModal("終了の確認", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("書き出していない変更があります。終了しますか?");
        if (ImGui::Button("書き出してから終了")) {
            QuickExport();
            if (!m_dirty) m_quitRequested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("破棄して終了")) { m_quitRequested = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// スモークテスト: エディターの操作を自動で順番に実行してクラッシュや GL エラーを検出する
// ---------------------------------------------------------------------------
void Editor::SmokeTestStep() {
    if (m_importing) return;
    int f = m_smokeFrame++;
    auto step = [&](int at, const char* name, auto&& fn) {
        if (f != at) return;
        LOG_INFO("[smoke] %s", name);
        fn();
        if (m_renderer.DeviceLost()) { LOG_ERROR("[smoke] GPU デバイスが失われました (%s の後)", name); ++m_smokeErrors; }
    };
    const bool has = !m_scene.Empty();
    step(2, "全パネルを開く", [&] {
        m_panels.stats = m_panels.sceneInfo = m_panels.history = m_panels.uvViewer = m_panels.exportWin = true;
        m_panels.importWin = m_panels.batchRename = m_panels.settings = m_panels.help = m_panels.dump = true;
    });
    step(4, "全ノードを順に選択", [&] { for (int i = 0; i < (int)m_scene.nodes.size(); ++i) Select(i); });
    step(6, "表示モード: ウェイト", [&] { m_rs.mode = RM_Weights; });
    for (int m = 0; m < RM_Count; ++m) step(8 + m, RenderModeName(m), [&] { m_rs.mode = m; });
    step(20, "デバッグ表示を全て ON", [&] {
        m_rs.showNormals = m_rs.showTangents = m_rs.showBounds = m_rs.showNodeAxes = m_showBoneNames = true;
        m_rs.debugSelectedOnly = false;
        m_rs.wireOnShaded = true;
    });
    step(24, "ワイヤーフレーム", [&] { m_rs.wireframe = true; });
    step(26, "ワイヤー OFF", [&] { m_rs.wireframe = false; m_rs.mode = RM_Lit; });
    auto finish = [&] {
        LOG_INFO("[smoke] 完了: 失敗 %d (ログ上のエラー %d 件は操作の拒否など想定内のものを含む)", m_smokeErrors, Log::Get().CountOf(LogLevel::Error));
        m_quitRequested = true;
    };
    if (!has && f < 66) { if (f > 30) finish(); return; }
    step(28, "アニメーション再生", [&] { if (!m_scene.animations.empty()) { m_anim = 0; m_playing = true; } });
    step(34, "ルートを選択して階層フリーズ", [&] { Select(m_scene.roots[0]); ApplyFreeze(m_primary, true); });
    step(36, "Undo", [&] { m_undo.Undo(m_scene); ValidateSelection(); MarkChanged(); });
    step(38, "Redo", [&] { m_undo.Redo(m_scene); ValidateSelection(); MarkChanged(); });
    step(40, "スケールを階層に適用", [&] { Select(m_scene.roots[0]); ApplyRootScale(m_primary); });
    step(42, "複製", [&] { DuplicateSelected(); });
    step(44, "削除", [&] { DeleteSelected(); });
    step(46, "Undo x2", [&] { m_undo.Undo(m_scene); m_undo.Undo(m_scene); ValidateSelection(); MarkChanged(); });
    step(48, "全メッシュの法線再計算/接線生成/結合", [&] {
        Checkpoint("smoke");
        for (auto& m : m_scene.meshes) {
            for (auto& p : m.primitives) {
                Ops::RecalculateNormals(p, true, 45.0f);
                Ops::GenerateTangents(p);
                Ops::WeldVertices(p, 1e-5f);
                if (p.HasSkin()) Ops::NormalizeWeights(p);
            }
            m.Touch();
        }
        MarkChanged();
    });
    step(50, "フラット法線", [&] {
        Checkpoint("smoke");
        for (auto& m : m_scene.meshes) { for (auto& p : m.primitives) Ops::RecalculateNormals(p, false); m.Touch(); }
        MarkChanged();
    });
    step(52, "マテリアル追加とテクスチャ割り当て", [&] {
        Checkpoint("smoke");
        int mat = Ops::AddMaterial(m_scene, "SmokeMaterial");
        if (!m_scene.textures.empty()) m_scene.materials[mat].slots[Slot_BaseColor].texture = 0;
        m_scene.materials[mat].alphaMode = AlphaMode::Blend;
        m_scene.materials[mat].baseColor.a = 0.5f;
        for (auto& m : m_scene.meshes) { for (auto& p : m.primitives) p.material = mat; m.Touch(); }
        m_selMaterial = mat;
        MarkChanged();
    });
    step(54, "シーン単位変換 / 軸変換 / 原点", [&] {
        Checkpoint("smoke");
        Ops::ScaleScene(m_scene, 100.0f);
        Ops::ConvertUpAxis(m_scene, 1, 2);
        Ops::CenterSceneToOrigin(m_scene, true);
        MarkChanged();
        FrameAll();
    });
    step(56, "整理 (名前空間/重複名/未使用/空ノード)", [&] {
        Checkpoint("smoke");
        Ops::StripNamespaces(m_scene);
        Ops::UniquifyNames(m_scene);
        Ops::RemoveUnusedResources(m_scene);
        Ops::RemoveEmptyNodes(m_scene);
        ClearSelection();
        ValidateSelection();
        MarkChanged();
    });
    step(57, "定規 / 単位変更", [&] {
        int r = AddRuler();
        m_rulers[r].height = -m_rulers[r].height;
        m_rulerGizmo = true;
        int before = m_scene.info.modelUnit;
        ChangeModelUnit(Unit_CM);
        ChangeModelUnit(before);
        m_rulerGizmo = false;
    });
    step(58, "自動修正", [&] { for (auto& is : ValidateScene(m_scene)) ApplyFix(m_scene, is); ValidateSelection(); MarkChanged(); });
    step(60, "履歴を全て Undo", [&] { while (m_undo.Undo(m_scene)) {} ValidateSelection(); MarkChanged(); });
    step(61, "複数オブジェクト / 表示切替 / 矩形選択 / 定規フィット", [&] {
        // アニメーション付きモデル: 表示中の姿勢で大きさが変わることを確認 (キーに上書きされない)
        if (!m_scene.animations.empty()) {
            if (m_anim < 0) m_anim = 0;
            ClearSelection();
            for (int i = 0; i < (int)m_scene.nodes.size(); ++i)
                if (m_scene.nodes[i].mesh >= 0) { Select(i); break; }
            auto tg = FitTargetsForSelection();
            glm::vec3 mn, mx;
            ComputeTargetsBounds(tg, mn, mx);
            float h0 = mx.y - mn.y;
            ScaleTargetsToSize(tg, 1, h0 * 2.0f, nullptr, false, false, "smoke anim fit");
            UpdateWorld();
            ComputeTargetsBounds(tg, mn, mx);
            LOG_INFO("[smoke] アニメ付き: 高さ %.4f → %.4f (目標 %.4f)", h0, mx.y - mn.y, h0 * 2.0f);
            if (std::abs((mx.y - mn.y) - h0 * 2.0f) > h0 * 1e-3f) { LOG_ERROR("[smoke] アニメーション付きモデルの拡大が表示に反映されていません"); ++m_smokeErrors; }
            // 再生中の別の時刻でも 2 倍のままか (ルートモーションも同じ倍率か)
            float tmid = m_scene.animations[m_anim].duration * 0.5f;
            float saved = m_animTime;
            m_animTime = tmid;
            UpdateWorld();
            glm::vec3 a, b;
            ComputeTargetsBounds(tg, a, b, true);
            m_undo.Undo(m_scene);
            ValidateSelection();
            UpdateWorld();
            glm::vec3 a0, b0;
            ComputeTargetsBounds(tg, a0, b0, true);
            // アニメーション途中のフレームで定規フィットしても、時刻 0 で実行したのと同じ位置になるか
            {
                // 全ノードの T/S を文字列にして比較する (小数 4 桁)
                auto SceneSignature = [&] {
                    std::string sig;
                    char buf[160];
                    for (auto& n : m_scene.nodes) {
                        snprintf(buf, sizeof(buf), "%s %.4f %.4f %.4f %.4f|", n.name.c_str(), n.translation.x, n.translation.y, n.translation.z, n.scale.x);
                        sig += buf;
                    }
                    return sig;
                };
                glm::vec3 anchor(0.3f, 0.2f, -0.1f);
                ScaleTargetsToSize(FitTargetsForSelection(), 1, h0 * 1.5f, &anchor, false, true, "smoke fit mid");
                glm::vec3 m1, x1;
                ComputeTargetsBounds(FitTargetsForSelection(), m1, x1);
                std::string snapMid = SceneSignature();
                m_undo.Undo(m_scene);
                m_animTime = 0.0f;
                UpdateWorld();
                ScaleTargetsToSize(FitTargetsForSelection(), 1, h0 * 1.5f, &anchor, false, true, "smoke fit t0");
                std::string snapT0 = SceneSignature();
                m_undo.Undo(m_scene);
                ValidateSelection();
                m_animTime = tmid;
                UpdateWorld();
                float footErr = std::max(std::abs(m1.y - anchor.y), std::max(std::abs((m1.x + x1.x) * 0.5f - anchor.x), std::abs((m1.z + x1.z) * 0.5f - anchor.z)));
                LOG_INFO("[smoke] 途中フレームで定規フィット: 足元の誤差 %.3g, 高さ %.4f (目標 %.4f), 時刻 0 と %s", footErr, x1.y - m1.y, h0 * 1.5f,
                         snapMid == snapT0 ? "一致" : "不一致");
                if (footErr > h0 * 1e-4f || std::abs((x1.y - m1.y) - h0 * 1.5f) > h0 * 1e-4f || snapMid != snapT0) {
                    LOG_ERROR("[smoke] 定規フィットの位置がフレームによってずれます");
                    ++m_smokeErrors;
                }
            }
            // 倍率指定 (x3) で高さが 3 倍になり、足元の中心が動かないか
            {
                glm::vec3 m0, x0, m1, x1;
                ComputeTargetsBounds(FitTargetsForSelection(), m0, x0);
                ScaleTargetsToSize(FitTargetsForSelection(), 1, 0.0f, nullptr, false, false, "smoke factor", 3.0f);
                ComputeTargetsBounds(FitTargetsForSelection(), m1, x1);
                m_undo.Undo(m_scene);
                ValidateSelection();
                UpdateWorld();
                float r = (x0.y - m0.y) > 1e-9f ? (x1.y - m1.y) / (x0.y - m0.y) : 0.0f;
                float footMove = glm::length(glm::vec3((m1.x + x1.x) * 0.5f, m1.y, (m1.z + x1.z) * 0.5f) - glm::vec3((m0.x + x0.x) * 0.5f, m0.y, (m0.z + x0.z) * 0.5f));
                LOG_INFO("[smoke] 倍率 x3: 高さの比 %.4f, 足元の移動 %.3g", r, footMove);
                if (std::abs(r - 3.0f) > 1e-3f || footMove > h0 * 1e-4f) { LOG_ERROR("[smoke] 倍率指定の結果が不正です"); ++m_smokeErrors; }
            }
            float ratio = (b0.y - a0.y) > 1e-9f ? (b.y - a.y) / (b0.y - a0.y) : 0.0f;
            LOG_INFO("[smoke] アニメ付き (t=%.2f): 倍率 %.4f", tmid, ratio);
            if (std::abs(ratio - 2.0f) > 2e-3f) { LOG_ERROR("[smoke] アニメーション中の倍率が不正です"); ++m_smokeErrors; }
            m_animTime = saved;
            MarkChanged();
        }
        for (int t = 0; t < Ops::Prim_Count; ++t) CreatePrimitiveObject(t);
        int cube = m_scene.FindNode("Cube1");
        ToggleSelectionVisibility();
        ToggleSelectionVisibility();
        IsolateSelection();
        ShowAll();
        UpdateWorld();
        SelectInRect(m_viewportPos, ImVec2(m_viewportPos.x + m_viewportSize.x, m_viewportPos.y + m_viewportSize.y), m_viewportPos, m_viewportSize, false, false);
        LOG_INFO("[smoke] 矩形選択で %zu 個選択", m_selection.size());
        if (m_selection.empty()) { LOG_ERROR("[smoke] 矩形選択で何も選択されませんでした"); ++m_smokeErrors; }
        // 定規の高さ (2.5 m) に Cube1 を合わせる
        int r = AddRuler();
        m_rulers[r].origin = glm::vec3(3, 0.5f, 0);
        m_rulers[r].height = 2.5f;
        ClearSelection();
        Select(cube);
        glm::vec3 anchor = m_rulers[r].origin;
        ScaleTargetsToSize(FitTargetsForSelection(), 1, 2.5f, &anchor, false, true, "smoke fit");
        glm::vec3 mn, mx;
        ComputeTargetsBounds({ cube }, mn, mx);
        const Node& c = m_scene.nodes[cube];
        bool ok = std::abs((mx.y - mn.y) - 2.5f) < 1e-3f && std::abs(mn.y - 0.5f) < 1e-3f && std::abs(c.scale.x - 2.5f) < 1e-3f &&
                  std::abs((mn.x + mx.x) * 0.5f - 3.0f) < 1e-3f;
        LOG_INFO("[smoke] Cube1 高さ %.4f 下端 %.4f スケール %.4f", mx.y - mn.y, mn.y, c.scale.x);
        if (!ok) { LOG_ERROR("[smoke] 定規フィットの結果が不正です"); ++m_smokeErrors; }
        // マイナスの定規: 上端を原点に合わせる
        m_rulers[r].height = -1.0f;
        anchor = m_rulers[r].origin;
        ScaleTargetsToSize(FitTargetsForSelection(), 1, 1.0f, &anchor, true, false, "smoke fit neg");
        ComputeTargetsBounds({ cube }, mn, mx);
        if (std::abs(mx.y - 0.5f) > 1e-3f || std::abs((mx.y - mn.y) - 1.0f) > 1e-3f) { LOG_ERROR("[smoke] マイナス定規のフィットが不正です"); ++m_smokeErrors; }
        // サイズを変えても XZ (足元の中心) が一番元の位置に残るか。ピボット (原点) とモデルの中心が離れたオブジェクトで確認
        {
            Checkpoint("smoke xz");
            int g = m_scene.AddNode("SmokeXZ", -1);
            m_scene.nodes[g].objectRoot = true;
            int cb = Ops::CreatePrimitive(m_scene, Ops::Prim_Cube, 1.0f, g);
            m_scene.nodes[cb].translation = glm::vec3(3.0f, 0.5f, 1.0f);
            MarkChanged();
            auto centerXZ = [&](glm::vec2 expect, const char* what) {
                glm::vec3 a, b;
                ComputeTargetsBounds({ g }, a, b);
                glm::vec2 cxz((a.x + b.x) * 0.5f, (a.z + b.z) * 0.5f);
                float e = glm::length(cxz - expect);
                LOG_INFO("[smoke] XZ 保持 (%s): 中心 %.4f, %.4f (期待 %.4f, %.4f) スケール %.4g", what, cxz.x, cxz.y, expect.x, expect.y, m_scene.nodes[g].scale.x);
                if (e > 1e-4f) { LOG_ERROR("[smoke] サイズ変更で XZ がずれました (%s)", what); ++m_smokeErrors; }
            };
            ScaleNodeKeepXZ(g, glm::vec3(2.0f));
            centerXZ({ 3, 1 }, "インスペクター x2");
            ScaleNodeKeepXZ(g, glm::vec3(0.37f));
            ScaleNodeKeepXZ(g, glm::vec3(1.9f));
            centerXZ({ 3, 1 }, "インスペクター x0.37 → x1.9");
            ScaleTargetsToSize({ g }, 1, 0.0f, nullptr, false, false, "smoke xz factor", 3.0f);
            centerXZ({ 3, 1 }, "倍率 x3");
            glm::vec3 anchor(0.0f, 0.25f, 0.0f);
            ScaleTargetsToSize({ g }, 1, 0.8f, &anchor, false, false, "smoke xz ruler");
            centerXZ({ 3, 1 }, "定規 (XZ は合わせない)");
            m_scene.nodes[g].translation.x += 1.0f; // ユーザーが動かした → そこが新しい元の位置
            MarkChanged();
            ScaleNodeKeepXZ(g, glm::vec3(1.5f));
            centerXZ({ 4, 1 }, "移動後に x1.5");
            anchor = glm::vec3(-2.0f, 0.0f, 5.0f);
            ScaleTargetsToSize({ g }, 1, 1.2f, &anchor, false, true, "smoke xz ruler xz");
            centerXZ({ -2, 5 }, "定規に XZ を合わせる");
            ScaleTargetsToSize({ g }, 1, 0.0f, nullptr, false, false, "smoke xz factor 2", 0.5f);
            centerXZ({ -2, 5 }, "定規に合わせた後 x0.5");
        }
        // 選択なし = 全オブジェクトを高さ 1.7 m に
        ClearSelection();
        auto all = FitTargetsForSelection();
        ScaleTargetsToSize(all, 1, 1.7f, nullptr, false, false, "smoke fit all");
        ComputeTargetsBounds(all, mn, mx);
        if (std::abs((mx.y - mn.y) - 1.7f) > 1e-3f) { LOG_ERROR("[smoke] 全体のサイズ合わせが不正です (%.4f)", mx.y - mn.y); ++m_smokeErrors; }
    });
    step(62, "書き出し (glb / gltf / fbx / obj / x / 3mf / 3ds)", [&] {
        auto dir = std::filesystem::temp_directory_path() / "ModelEditorSmoke";
        std::filesystem::create_directories(dir);
        for (const char* n : { "smoke.glb", "smoke.gltf", "smoke.fbx", "smoke.obj", "smoke.x", "smoke.3mf", "smoke.3ds" })
            if (!DoExport(FileUtil::FromPath(dir / n))) ++m_smokeErrors;
    });
    step(64, "ピック", [&] { m_pickRequested = true; m_pickPos = ImVec2(m_viewportPos.x + m_viewportSize.x * 0.5f, m_viewportPos.y + m_viewportSize.y * 0.5f); });
    step(66, "新規シーン", [&] { NewScene(); });
    step(70, "終了", finish);
}

// ---------------------------------------------------------------------------
// 複数オブジェクト / 表示
// ---------------------------------------------------------------------------
void Editor::CreatePrimitiveObject(int type) {
    Checkpoint(std::string("作成: ") + Ops::PrimitiveName(type));
    int node = Ops::CreatePrimitive(m_scene, type, 1.0f);
    ClearSelection();
    Select(node);
    MarkChanged();
    LOG_INFO("'%s' を作成しました (1 m)", m_scene.nodes[node].name.c_str());
}

void Editor::SetVisibility(const std::vector<int>& nodes, bool visible, const char* label) {
    if (nodes.empty()) return;
    Checkpoint(label);
    for (int n : nodes)
        if (n >= 0 && n < (int)m_scene.nodes.size()) m_scene.nodes[n].visible = visible;
    MarkChanged();
}

void Editor::ToggleSelectionVisibility() {
    auto targets = EditTargets();
    if (targets.empty()) return;
    bool anyVisible = false;
    for (int t : targets) if (m_scene.nodes[t].visible) anyVisible = true;
    SetVisibility(targets, !anyVisible, anyVisible ? "非表示" : "表示");
}

void Editor::IsolateSelection() {
    auto targets = EditTargets();
    if (targets.empty()) return;
    Checkpoint("選択のみ表示");
    std::vector<bool> keep(m_scene.nodes.size(), false);
    std::function<void(int)> down = [&](int i) { keep[i] = true; for (int c : m_scene.nodes[i].children) down(c); };
    for (int t : targets) {
        down(t);
        for (int p = m_scene.nodes[t].parent; p >= 0; p = m_scene.nodes[p].parent) keep[p] = true; // 親が隠れると子も見えない
    }
    for (size_t i = 0; i < m_scene.nodes.size(); ++i) m_scene.nodes[i].visible = keep[i];
    MarkChanged();
}

void Editor::ShowAll() {
    std::vector<int> all(m_scene.nodes.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = (int)i;
    SetVisibility(all, true, "すべて表示");
}

std::vector<int> Editor::EditTargets() const {
    std::vector<int> out;
    if (m_primary >= 0 && m_primary < (int)m_scene.nodes.size()) out.push_back(m_primary);
    for (int s : m_selection)
        if (s >= 0 && s < (int)m_scene.nodes.size() && s != m_primary) out.push_back(s);
    return out;
}

std::vector<int> Editor::TopLevelTargets() const {
    auto all = EditTargets();
    std::vector<int> out;
    for (int t : all) {
        bool under = false;
        for (int o : all) if (o != t && m_scene.IsAncestor(o, t)) under = true;
        if (!under) out.push_back(t);
    }
    return out;
}

int Editor::ScaleTargetOf(int node, bool create) {
    if (node < 0 || node >= (int)m_scene.nodes.size() || !Ops::NodeHasAnimation(m_scene, node)) return node;
    // 祖先がアニメーションしているボーンは、そのボーン自身のスケール (子の移動キーも一緒に伸びる) として扱う
    for (int p = m_scene.nodes[node].parent; p >= 0; p = m_scene.nodes[p].parent) {
        if (m_scene.nodes[p].objectRoot) break;
        if (Ops::NodeHasAnimation(m_scene, p)) return node;
    }
    return create ? Ops::EnsureObjectNode(m_scene, node) : Ops::FindObjectNode(m_scene, node);
}

bool Editor::ResizeHomeXZ(int node, glm::vec2& home) {
    if (node < 0 || node >= (int)m_scene.nodes.size()) return false;
    glm::vec3 mn, mx;
    if (!ComputeTargetsBounds({ node }, mn, mx)) return false;
    const Node& n = m_scene.nodes[node];
    // 前回のサイズ変更の後に動かされていなければ、最初に記録した位置を使う (何度変えても一番元に戻る)
    float tol = 1e-5f * std::max(1.0f, glm::length(n.translation));
    if (n.hasResizeHome && glm::length(n.translation - n.resizeHomeT) <= tol) home = n.resizeHomeXZ;
    else home = glm::vec2((mn.x + mx.x) * 0.5f, (mn.z + mx.z) * 0.5f);
    return true;
}

void Editor::RestoreResizeHomeXZ(int node, const glm::vec2& home) {
    glm::vec3 mn, mx;
    if (!ComputeTargetsBounds({ node }, mn, mx)) return;
    glm::vec3 d(home.x - (mn.x + mx.x) * 0.5f, 0.0f, home.y - (mn.z + mx.z) * 0.5f);
    if (glm::length(d) > 0.0f) {
        auto world = m_scene.ComputeWorldMatrices();
        const Node& n = m_scene.nodes[node];
        glm::mat3 pw = n.parent >= 0 ? glm::mat3(world[n.parent]) : glm::mat3(1.0f);
        Ops::OffsetNodeTRS(m_scene, node, glm::inverse(pw) * d, glm::quat(1, 0, 0, 0), glm::vec3(1.0f));
    }
    Node& n = m_scene.nodes[node];
    n.hasResizeHome = true;
    n.resizeHomeXZ = home;
    n.resizeHomeT = n.translation;
}

// XZ を保つ対象: オブジェクト (またはルート) で、自身が動かず (移動キーがずれない)、スキンメッシュでもない
// (スキンメッシュはノードの変換が無視されるので動かしても意味がない)
static bool KeepsResizeXZ(const Scene& scene, int node) {
    const Node& n = scene.nodes[node];
    return (n.objectRoot || n.parent < 0) && !Ops::NodeHasAnimation(scene, node) && !(n.mesh >= 0 && n.skin >= 0);
}

void Editor::ScaleNodeKeepXZ(int node, const glm::vec3& mul) {
    if (node < 0 || node >= (int)m_scene.nodes.size()) return;
    glm::vec2 home;
    bool keep = KeepsResizeXZ(m_scene, node) && ResizeHomeXZ(node, home);
    Ops::OffsetNodeTRS(m_scene, node, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), mul);
    if (keep) RestoreResizeHomeXZ(node, home);
}

// ---------------------------------------------------------------------------
// サイズ合わせ (トランスフォームのスケールで変更する。頂点には焼き込まない)
// ---------------------------------------------------------------------------
std::vector<glm::mat4> Editor::PosedWorld() const {
    auto pose = m_scene.RestPose();
    if (m_anim >= 0 && m_anim < (int)m_scene.animations.size()) SampleAnimation(m_scene, m_scene.animations[m_anim], m_animTime, pose);
    return m_scene.ComputeWorldMatrices(pose);
}

bool Editor::ComputeTargetsBounds(const std::vector<int>& targets, glm::vec3& mn, glm::vec3& mx, bool posed) {
    // レストポーズで測る: アニメーションのどのフレームで操作しても同じ結果になる (ルートモーションでずれない)
    auto world = posed ? PosedWorld() : m_scene.ComputeWorldMatrices();
    std::vector<bool> inSub(m_scene.nodes.size(), false);
    std::function<void(int)> mark = [&](int i) { inSub[i] = true; for (int c : m_scene.nodes[i].children) mark(c); };
    for (int t : targets) if (t >= 0 && t < (int)m_scene.nodes.size()) mark(t);
    mn = glm::vec3(1e30f);
    mx = glm::vec3(-1e30f);
    bool any = false;
    for (int i = 0; i < (int)m_scene.nodes.size(); ++i) {
        const Node& n = m_scene.nodes[i];
        if (n.mesh < 0) continue;
        bool include = inSub[i];
        // スキンメッシュはジョイントが対象に含まれていれば対象とみなす
        if (!include && n.skin >= 0 && n.skin < (int)m_scene.skins.size() && !m_scene.skins[n.skin].joints.empty()) {
            int j = m_scene.skins[n.skin].joints[0];
            include = j >= 0 && j < (int)inSub.size() && inSub[j];
        }
        if (!include) continue;
        glm::vec3 a, b;
        if (m_scene.ComputeNodeBounds(i, world, a, b, SIZE_MAX)) { // 間引くと足元の頂点を見落として位置がずれる
            mn = glm::min(mn, a);
            mx = glm::max(mx, b);
            any = true;
        }
    }
    return any;
}

std::vector<int> Editor::FitTargetsForSelection() {
    std::vector<int> targets = TopLevelTargets();
    if (targets.empty()) targets = m_scene.roots;
    // スキンメッシュはメッシュノードの変換が無視されるので、スケルトンのルートも対象に加える
    std::vector<int> extra;
    for (int t : targets) {
        std::function<void(int)> visit = [&](int i) {
            const Node& n = m_scene.nodes[i];
            if (n.mesh >= 0 && n.skin >= 0 && n.skin < (int)m_scene.skins.size() && !m_scene.skins[n.skin].joints.empty()) {
                int root = m_scene.skins[n.skin].joints[0];
                while (root >= 0 && m_scene.nodes[root].parent >= 0) root = m_scene.nodes[root].parent;
                bool covered = false;
                for (int o : targets) if (o == root || m_scene.IsAncestor(o, root)) covered = true;
                for (int o : extra) if (o == root) covered = true;
                if (root >= 0 && !covered) extra.push_back(root);
            }
            for (int c : n.children) visit(c);
        };
        visit(t);
    }
    targets.insert(targets.end(), extra.begin(), extra.end());
    // 他の対象の子孫は除く (二重に拡大しないため)
    std::vector<int> out;
    for (int t : targets) {
        bool under = false;
        for (int o : targets) if (o != t && m_scene.IsAncestor(o, t)) under = true;
        if (!under && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    return out;
}

bool Editor::ScaleTargetsToSize(std::vector<int> targets, int axis, float targetSize, const glm::vec3* anchor, bool anchorTop, bool alignXZ, const char* label,
                                float factor) {
    if (targets.empty()) return false;
    Checkpoint(label);
    // アニメーション付きノードを直接拡大すると自身の移動キー (ルートモーション) が拡大されずズレるので、
    // アニメーションを持たないオブジェクトノード (親) のスケールで拡大する
    for (auto& t : targets)
        if (Ops::NodeHasAnimation(m_scene, t)) {
            int o = Ops::EnsureObjectNode(m_scene, t);
            if (o >= 0) t = o;
        }
    {
        std::vector<int> uniq;
        for (int t : targets) {
            bool under = false;
            for (int o : targets) if (o != t && m_scene.IsAncestor(o, t)) under = true;
            if (!under && std::find(uniq.begin(), uniq.end(), t) == uniq.end()) uniq.push_back(t);
        }
        targets = std::move(uniq);
    }
    for (int t : targets) m_scene.nodes[t].objectRoot = true; // 書き出し時に、このスケールを焼き込むか親として残すかを選べる
    glm::vec3 mn, mx;
    if (!ComputeTargetsBounds(targets, mn, mx)) {
        LOG_ERROR("対象にメッシュがありません");
        return false;
    }
    float cur = mx[axis] - mn[axis];
    if (cur <= 1e-9f || (factor <= 0.0f && targetSize <= 0)) {
        LOG_ERROR("現在のサイズまたは目標サイズが 0 です");
        return false;
    }
    float f = factor > 0.0f ? factor : targetSize / cur;
    targetSize = cur * f;
    // トランスフォームのスケールで拡大縮小。
    // 複数オブジェクトは、全体の足元中心 c を基準に拡大されるよう各基準点も移動する。
    // 測定と同じレストポーズの行列で位置を補正する (表示中の姿勢を使うとフレームによって位置がずれる)
    const glm::vec3 c((mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f);
    // 各オブジェクトの「一番元」の足元の中心 (XZ)。サイズを変えた後にずれていたら戻す
    std::vector<glm::vec2> homes(targets.size());
    std::vector<char> keepXZ(targets.size(), 0);
    for (size_t i = 0; i < targets.size(); ++i) keepXZ[i] = KeepsResizeXZ(m_scene, targets[i]) && ResizeHomeXZ(targets[i], homes[i]);
    {
        auto w0 = m_scene.ComputeWorldMatrices();
        std::vector<glm::vec3> pivots;
        for (int t : targets) pivots.push_back(glm::vec3(w0[t][3]));
        for (int t : targets) Ops::OffsetNodeTRS(m_scene, t, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::vec3(f));
        for (size_t i = 0; i < targets.size(); ++i) {
            const Node& n = m_scene.nodes[targets[i]];
            glm::vec3 d = (c + (pivots[i] - c) * f) - pivots[i];
            glm::mat3 pw = n.parent >= 0 ? glm::mat3(w0[n.parent]) : glm::mat3(1.0f);
            Ops::OffsetNodeTRS(m_scene, targets[i], glm::inverse(pw) * d, glm::quat(1, 0, 0, 0), glm::vec3(1.0f));
        }
    }
    glm::vec3 mn2, mx2;
    ComputeTargetsBounds(targets, mn2, mx2);
    glm::vec3 delta(0.0f);
    if (anchor) {
        delta.y = anchor->y - (anchorTop ? mx2.y : mn2.y);
        if (alignXZ) {
            delta.x = anchor->x - (mn2.x + mx2.x) * 0.5f;
            delta.z = anchor->z - (mn2.z + mx2.z) * 0.5f;
        }
    } else {
        // 足元の中心を元の位置に保つ
        glm::vec3 c1((mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f), c2((mn2.x + mx2.x) * 0.5f, mn2.y, (mn2.z + mx2.z) * 0.5f);
        delta = c1 - c2;
    }
    auto world = m_scene.ComputeWorldMatrices();
    for (int t : targets) {
        const Node& n = m_scene.nodes[t];
        glm::mat3 pw = n.parent >= 0 ? glm::mat3(world[n.parent]) : glm::mat3(1.0f);
        Ops::OffsetNodeTRS(m_scene, t, glm::inverse(pw) * delta, glm::quat(1, 0, 0, 0), glm::vec3(1.0f));
    }
    // 対象同士が別の親を持つ場合などに残る誤差を、もう一度測って詰める
    if (anchor) {
        glm::vec3 mn3, mx3;
        ComputeTargetsBounds(targets, mn3, mx3);
        float off = anchor->y - (anchorTop ? mx3.y : mn3.y);
        if (std::abs(off) > 1e-6f * std::max(1.0f, targetSize)) {
            auto w2 = m_scene.ComputeWorldMatrices();
            for (int t : targets) {
                const Node& n = m_scene.nodes[t];
                glm::mat3 pw = n.parent >= 0 ? glm::mat3(w2[n.parent]) : glm::mat3(1.0f);
                Ops::OffsetNodeTRS(m_scene, t, glm::inverse(pw) * glm::vec3(0, off, 0), glm::quat(1, 0, 0, 0), glm::vec3(1.0f));
            }
        }
    }
    // XZ を一番元の位置に戻す。定規に XZ を合わせたときは、その位置を新しい元の位置として記録する
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!keepXZ[i]) continue;
        if (anchor && alignXZ) {
            glm::vec3 a, b;
            if (ComputeTargetsBounds({ targets[i] }, a, b)) RestoreResizeHomeXZ(targets[i], glm::vec2((a.x + b.x) * 0.5f, (a.z + b.z) * 0.5f));
        } else {
            RestoreResizeHomeXZ(targets[i], homes[i]);
        }
    }
    m_eulerNode = -1;
    MarkChanged();
    UpdateWorld();
    const Node& p = m_scene.nodes[targets[0]];
    LOG_INFO("%zu 個のオブジェクトを x%.4g しました (スケール %.4g, %.4g, %.4g / サイズ %s)", targets.size(), f, p.scale.x, p.scale.y, p.scale.z,
             FormatLength(targetSize, m_displayUnit).c_str());
    return true;
}