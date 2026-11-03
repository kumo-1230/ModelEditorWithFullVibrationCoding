#include "Exporter.h"
#include "core/Log.h"
#include "core/ModelOps.h"
#include "core/SceneOps.h"
#include "util/FileUtil.h"
#include <chrono>
#include <algorithm>
#include <functional>
#include <cctype>

const char* ExportFormatName(ExportFormat f) {
    switch (f) {
    case ExportFormat::GLB: return "glTF Binary (.glb)";
    case ExportFormat::GLTF: return "glTF Separate (.gltf + .bin + 画像)";
    case ExportFormat::GLTF_Embedded: return "glTF Embedded (.gltf 単体)";
    case ExportFormat::FBX: return "FBX Binary (.fbx)";
    case ExportFormat::FBX_ASCII: return "FBX ASCII (.fbx)";
    case ExportFormat::OBJ: return "Wavefront OBJ (.obj)";
    case ExportFormat::DAE: return "Collada (.dae)";
    case ExportFormat::STL: return "STL (.stl)";
    case ExportFormat::PLY: return "PLY (.ply)";
    case ExportFormat::X: return "DirectX (.x)";
    case ExportFormat::ThreeMF: return "3MF (.3mf)";
    case ExportFormat::ThreeDS: return "3ds Max (.3ds)";
    default: return "?";
    }
}

const char* ExportFormatExt(ExportFormat f) {
    switch (f) {
    case ExportFormat::GLB: return ".glb";
    case ExportFormat::GLTF:
    case ExportFormat::GLTF_Embedded: return ".gltf";
    case ExportFormat::FBX:
    case ExportFormat::FBX_ASCII: return ".fbx";
    case ExportFormat::OBJ: return ".obj";
    case ExportFormat::DAE: return ".dae";
    case ExportFormat::STL: return ".stl";
    case ExportFormat::PLY: return ".ply";
    case ExportFormat::X: return ".x";
    case ExportFormat::ThreeMF: return ".3mf";
    case ExportFormat::ThreeDS: return ".3ds";
    default: return "";
    }
}

ExportFormat ExportFormatFromPath(const std::string& path, ExportFormat fallback) {
    std::string e = FileUtil::LowerExt(path);
    if (e == ".glb") return ExportFormat::GLB;
    if (e == ".gltf") return (fallback == ExportFormat::GLTF_Embedded) ? fallback : ExportFormat::GLTF;
    if (e == ".fbx") return (fallback == ExportFormat::FBX_ASCII) ? fallback : ExportFormat::FBX;
    if (e == ".obj") return ExportFormat::OBJ;
    if (e == ".dae") return ExportFormat::DAE;
    if (e == ".stl") return ExportFormat::STL;
    if (e == ".ply") return ExportFormat::PLY;
    if (e == ".x") return ExportFormat::X;
    if (e == ".3mf") return ExportFormat::ThreeMF;
    if (e == ".3ds") return ExportFormat::ThreeDS;
    if (e == ".vrm") return ExportFormat::GLB; // 中身は GLB (VRM 拡張は書き出さない)
    return fallback;
}

void PrepareSceneForExport(Scene& scene, const ExportOptions& opts) {
    if (opts.content == 1) Ops::StripAnimations(scene);
    if (opts.content == 2) Ops::StripToAnimationOnly(scene);
    if (opts.onlyVisible) {
        std::function<void(int, bool)> rec = [&](int i, bool parentHidden) {
            bool hidden = parentHidden || !scene.nodes[i].visible;
            if (hidden) { scene.nodes[i].mesh = -1; scene.nodes[i].skin = -1; }
            for (int c : scene.nodes[i].children) rec(c, hidden);
        };
        for (int r : scene.roots) rec(r, false);
    }
    if (opts.objectScaleMode == 0) {
        bool scaled = false;
        for (auto& n : scene.nodes) if (n.objectRoot && n.scale != glm::vec3(1.0f)) scaled = true;
        Scene ref;
        if (scaled && opts.verifyAfterExport) ref = scene;
        int n = Ops::BakeObjectScales(scene, opts.dissolveObjectWrappers);
        if (n) LOG_INFO("オブジェクト %d 個のスケールを中身 (ボーン・アニメーション・頂点) に焼き込みました", n);
        if (n && opts.verifyAfterExport) {
            // 焼き込み前 (親で拡大) と後で、全フレームのボーン・頂点のワールド位置が一致するか
            glm::vec3 mn, mx;
            ref.ComputeBounds(ref.ComputeWorldMatrices(), mn, mx);
            float tol = std::max(glm::length(mx - mn), 1e-6f) * 1e-4f;
            int frames = 0;
            float e = Ops::MaxMotionError(ref, scene, 30.0f, &frames);
            if (e < 0) LOG_WARN("焼き込みの検証: 構成が変わったため比較できませんでした");
            else if (e <= tol) LOG_INFO("焼き込みの検証 OK: %d フレームで最大誤差 %.3g (許容 %.3g)", frames, e, tol);
            else LOG_WARN("焼き込みの検証で誤差があります: %d フレームで最大誤差 %.3g (許容 %.3g)", frames, e, tol);
        }
    }
    if (opts.upAxisConversion == 1) Ops::ConvertUpAxis(scene, 1, 2);
    else if (opts.upAxisConversion == 2) Ops::ConvertUpAxis(scene, 2, 1);
    if (std::abs(opts.scale - 1.0f) > 1e-7f) Ops::ScaleScene(scene, opts.scale);
    if ((opts.format == ExportFormat::FBX || opts.format == ExportFormat::FBX_ASCII) && opts.fbxUnitCentimeters) Ops::ScaleScene(scene, 100.0f);
    if (opts.format == ExportFormat::ThreeMF) {
        // 3MF は Z-up・ミリメートル (Assimp は unit="millimeter" と書く)
        Ops::ConvertUpAxis(scene, 1, 2);
        Ops::ScaleScene(scene, 1000.0f);
    }
    // 3DS は Z-up (Assimp の読み込みも Z-up → Y-up に回転する)
    if (opts.format == ExportFormat::ThreeDS) Ops::ConvertUpAxis(scene, 1, 2);
    if (opts.uniquifyNames) {
        int n = Ops::UniquifyNames(scene);
        if (n) LOG_WARN("書き出し用に %d 個のノード名を一意に変更しました", n);
    }

    // 壊れたジョイント参照の修復
    for (auto& s : scene.skins) {
        int fallback = s.skeleton;
        for (int j : s.joints) if (fallback < 0 && j >= 0) fallback = j;
        for (auto& j : s.joints)
            if (j < 0 || j >= (int)scene.nodes.size()) {
                LOG_WARN("スキン '%s' の無効なジョイントを置き換えました", s.name.c_str());
                j = fallback >= 0 ? fallback : 0;
            }
        s.inverseBind.resize(s.joints.size(), glm::mat4(1.0f));
    }

    if (!opts.exportSkins) {
        for (auto& n : scene.nodes) n.skin = -1;
        scene.skins.clear();
    }
    if (!opts.exportAnimations) scene.animations.clear();
    for (auto& m : scene.meshes) {
        if (!opts.exportMorphTargets) m.morphWeights.clear();
        for (auto& p : m.primitives) {
            if (!opts.exportSkins) { p.joints.clear(); p.weights.clear(); }
            if (opts.normalizeWeights && p.HasSkin()) Ops::NormalizeWeights(p);
            if (!opts.exportMorphTargets) p.targets.clear();
            if (!opts.exportNormals) { p.normals.clear(); p.tangents.clear(); }
            if (!opts.exportTangents) p.tangents.clear();
            if (!opts.exportUV1) p.uv1.clear();
            if (!opts.exportColors) p.colors.clear();
        }
    }
    // 空のプリミティブ / メッシュを除去 (glTF では不正)
    for (auto& m : scene.meshes) {
        auto& prims = m.primitives;
        prims.erase(std::remove_if(prims.begin(), prims.end(), [](const Primitive& p) { return p.positions.empty() || p.indices.size() < 3; }), prims.end());
    }
    for (int mi = (int)scene.meshes.size() - 1; mi >= 0; --mi)
        if (scene.meshes[mi].primitives.empty()) scene.RemoveMesh(mi);

    // スキンが無いノードに付いたウェイトは除去 (glTF 検証エラーになる)
    for (auto& n : scene.nodes) {
        if (n.mesh >= 0 && n.skin < 0 && scene.meshes[n.mesh].HasSkin()) {
            bool usedSkinned = false;
            for (auto& o : scene.nodes) if (o.mesh == n.mesh && o.skin >= 0) usedSkinned = true;
            if (!usedSkinned)
                for (auto& p : scene.meshes[n.mesh].primitives) { p.joints.clear(); p.weights.clear(); }
        }
    }
}

namespace {
bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool IsLower(char c) { return c >= 'a' && c <= 'z'; }
// 英数字と UTF-8 のマルチバイト文字 (日本語など) は単語の一部
bool IsWordChar(char c) { return IsUpper(c) || IsLower(c) || (c >= '0' && c <= '9') || (unsigned char)c >= 0x80; }
// 0=小文字 1=先頭が大文字 2=全部大文字 -1=英字なし
int LetterCase(const std::string& s) {
    bool up = false, lo = false;
    char first = 0;
    for (char c : s) {
        if (IsUpper(c)) up = true;
        if (IsLower(c)) lo = true;
        if (!first && (IsUpper(c) || IsLower(c))) first = c;
    }
    if (!up && !lo) return -1;
    if (up && !lo) return 2;
    return IsUpper(first) ? 1 : 0;
}
const char* TextureWord(int letterCase) { return letterCase == 2 ? "TEXTURE" : letterCase == 1 ? "Texture" : "texture"; }
} // namespace

std::string TextureFolderName(const std::string& modelName) {
    std::string name = modelName;
    while (!name.empty() && (name.back() == ' ' || name.back() == '_' || name.back() == '-' || name.back() == '.')) name.pop_back();
    while (!name.empty() && (name.front() == ' ' || name.front() == '_' || name.front() == '-' || name.front() == '.')) name.erase(name.begin());
    if (name.empty()) return "modelTexture";
    bool underscore = false, dash = false, other = false;
    for (char c : name) {
        if (c == '_') underscore = true;
        else if (c == '-') dash = true;
        else if (!IsWordChar(c)) other = true;
    }
    std::string out;
    if (!other && underscore != dash) {
        // 区切り文字の命名 (snake_case / SCREAMING_CASE / SK_Hero / kebab-case): 同じ区切りと、最後の単語と同じ大文字小文字で付ける
        char sep = underscore ? '_' : '-';
        int c = LetterCase(name) == 2 ? 2 : LetterCase(name.substr(name.find_last_of(sep) + 1));
        if (c < 0) c = LetterCase(name); // 最後が数字だけ (Hero_01) なら全体に合わせる
        out = name + sep + TextureWord(c < 0 ? 0 : c);
    } else if (!other && !underscore && !dash &&
               (LetterCase(name) == -1 || (std::any_of(name.begin(), name.end(), IsUpper) && std::any_of(name.begin(), name.end(), IsLower)))) {
        // PascalCase / camelCase / 英字なし (数字や日本語): そのまま Texture を付ける
        out = name + "Texture";
    } else {
        // それ以外 (空白入り、小文字だけ、大文字だけ、区切りの混在) はキャメルケースに統一する
        std::vector<std::string> words;
        std::string w;
        for (char c : name) {
            if (IsWordChar(c)) w += c;
            else if (!w.empty()) { words.push_back(w); w.clear(); }
        }
        if (!w.empty()) words.push_back(w);
        for (size_t i = 0; i < words.size(); ++i) {
            std::string s = words[i];
            if (LetterCase(s) == 2) for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            if (i == 0 && IsUpper(s[0])) s[0] = (char)std::tolower((unsigned char)s[0]);
            if (i > 0 && IsLower(s[0])) s[0] = (char)std::toupper((unsigned char)s[0]);
            out += s;
        }
        if (out.empty()) out = "model";
        out += "Texture";
    }
    return FileUtil::SanitizeFilename(out);
}

std::string ResolveTextureSubdir(const std::string& exportPath, const ExportOptions& opts) {
    if (opts.textureFolderMode == 0) return TextureFolderName(FileUtil::Stem(exportPath));
    return opts.textureSubdir;
}

bool ExportScene(const Scene& src, const std::string& path, const ExportOptions& optsIn, std::string& err) {
    auto t0 = std::chrono::steady_clock::now();
    ExportOptions opts = optsIn;
    opts.format = ExportFormatFromPath(path, opts.format);
    opts.textureSubdir = ResolveTextureSubdir(path, opts);
    opts.textureFolderMode = 1;
    Scene scene = src;
    PrepareSceneForExport(scene, opts);
    LOG_INFO("書き出し開始: %s [%s]", path.c_str(), ExportFormatName(opts.format));
    bool ok;
    if (opts.format == ExportFormat::GLB || opts.format == ExportFormat::GLTF || opts.format == ExportFormat::GLTF_Embedded)
        ok = ExportGltf(scene, path, opts, err);
    else
        ok = ExportWithAssimp(scene, path, opts, err);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ok) {
        LOG_INFO("書き出し完了 (%.1f ms): %s", ms, path.c_str());
        if (opts.verifyAfterExport && (opts.format == ExportFormat::GLB || opts.format == ExportFormat::GLTF || opts.format == ExportFormat::GLTF_Embedded)) {
            std::string report;
            if (VerifyGltf(path, report)) LOG_INFO("検証 OK: %s", report.c_str());
            else LOG_WARN("検証で問題が見つかりました: %s", report.c_str());
        }
    } else {
        LOG_ERROR("書き出し失敗: %s", err.c_str());
    }
    return ok;
}
