#include "Validate.h"
#include "SceneOps.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>

static bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

static std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    return buf;
}

std::vector<Issue> ValidateScene(const Scene& scene) {
    std::vector<Issue> out;
    auto add = [&](Severity s, const char* cat, std::string msg, FixAction fix = FixAction::None) -> Issue& {
        Issue is;
        is.severity = s;
        is.category = cat;
        is.message = std::move(msg);
        is.fix = fix;
        out.push_back(is);
        return out.back();
    };

    if (scene.Empty()) {
        add(Severity::Info, "シーン", "シーンが空です");
        return out;
    }

    // ---- ノード -----------------------------------------------------------
    std::map<std::string, int> nameCount;
    for (auto& n : scene.nodes) nameCount[n.name]++;
    int dupNames = 0, emptyNames = 0;
    for (auto& [name, c] : nameCount) {
        if (name.empty()) emptyNames += c;
        else if (c > 1) dupNames += c;
    }
    if (dupNames) add(Severity::Warning, "ノード", Fmt("重複したノード名が %d 個あります (FBX のボーン対応付けで問題になります)", dupNames), FixAction::UniquifyNames);
    if (emptyNames) add(Severity::Warning, "ノード", Fmt("名前の無いノードが %d 個あります", emptyNames), FixAction::UniquifyNames);

    auto joints = scene.JointMask();
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        const Node& n = scene.nodes[i];
        if (!Finite(n.translation) || !Finite(n.scale) || !std::isfinite(n.rotation.w))
            add(Severity::Error, "ノード", Fmt("'%s' のトランスフォームに NaN/Inf があります", n.name.c_str())).node = i;
        if (std::abs(n.scale.x) < 1e-8f || std::abs(n.scale.y) < 1e-8f || std::abs(n.scale.z) < 1e-8f)
            add(Severity::Error, "ノード", Fmt("'%s' のスケールが 0 です", n.name.c_str())).node = i;
        if (n.scale.x * n.scale.y * n.scale.z < 0)
            add(Severity::Warning, "ノード", Fmt("'%s' は負のスケールを持っています (面の向きが反転します)", n.name.c_str())).node = i;
        bool nonUniform = std::abs(n.scale.x - n.scale.y) > 1e-4f || std::abs(n.scale.y - n.scale.z) > 1e-4f;
        if (nonUniform && joints[i])
            add(Severity::Warning, "スキン", Fmt("ジョイント '%s' が非均等スケールを持っています", n.name.c_str())).node = i;
        if (std::abs(n.scale.x - 1) > 1e-4f && !nonUniform && !n.children.empty() && n.parent < 0)
            add(Severity::Info, "ノード", Fmt("ルート '%s' にスケール %.4g があります (フリーズ/階層に適用で正規化できます)", n.name.c_str(), n.scale.x)).node = i;
        if (n.mesh >= (int)scene.meshes.size())
            add(Severity::Error, "ノード", Fmt("'%s' のメッシュ参照が範囲外です", n.name.c_str())).node = i;
        if (n.skin >= (int)scene.skins.size())
            add(Severity::Error, "ノード", Fmt("'%s' のスキン参照が範囲外です", n.name.c_str())).node = i;
        if (n.skin >= 0 && n.mesh >= 0 && n.mesh < (int)scene.meshes.size() && !scene.meshes[n.mesh].HasSkin())
            add(Severity::Warning, "スキン", Fmt("'%s' はスキンを持ちますがメッシュにウェイトがありません", n.name.c_str())).node = i;
        if (n.mesh >= 0 && n.mesh < (int)scene.meshes.size() && scene.meshes[n.mesh].HasSkin() && n.skin < 0)
            add(Severity::Warning, "スキン", Fmt("'%s' のメッシュはウェイトを持ちますがスキンが割り当てられていません", n.name.c_str())).node = i;
    }

    // ---- メッシュ ---------------------------------------------------------
    for (int mi = 0; mi < (int)scene.meshes.size(); ++mi) {
        const Mesh& m = scene.meshes[mi];
        if (scene.NodesUsingMesh(mi).empty())
            add(Severity::Info, "メッシュ", Fmt("メッシュ '%s' はどのノードからも使われていません", m.name.c_str()), FixAction::RemoveUnused).mesh = mi;
        int maxJoint = -1;
        for (auto& nidx : scene.NodesUsingMesh(mi)) {
            int s = scene.nodes[nidx].skin;
            if (s >= 0 && s < (int)scene.skins.size()) maxJoint = std::max(maxJoint, (int)scene.skins[s].joints.size());
        }
        for (size_t pi = 0; pi < m.primitives.size(); ++pi) {
            const Primitive& p = m.primitives[pi];
            const char* mn = m.name.c_str();
            if (p.positions.empty()) { add(Severity::Warning, "メッシュ", Fmt("'%s'[%zu] に頂点がありません", mn, pi)).mesh = mi; continue; }
            int nan = 0;
            for (auto& v : p.positions) if (!Finite(v)) ++nan;
            if (nan) add(Severity::Error, "メッシュ", Fmt("'%s'[%zu] に NaN/Inf の頂点が %d 個あります", mn, pi, nan)).mesh = mi;
            int badIdx = 0;
            for (auto i : p.indices) if (i >= p.positions.size()) ++badIdx;
            if (badIdx) add(Severity::Error, "メッシュ", Fmt("'%s'[%zu] に範囲外のインデックスが %d 個あります", mn, pi, badIdx)).mesh = mi;
            if (p.indices.size() % 3) add(Severity::Error, "メッシュ", Fmt("'%s'[%zu] のインデックス数が 3 の倍数ではありません", mn, pi)).mesh = mi;
            if (!badIdx) {
                int degenerate = 0;
                for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
                    uint32_t a = p.indices[i], b = p.indices[i + 1], c = p.indices[i + 2];
                    glm::vec3 cr = glm::cross(p.positions[b] - p.positions[a], p.positions[c] - p.positions[a]);
                    if (a == b || b == c || a == c || glm::dot(cr, cr) < 1e-24f) ++degenerate;
                }
                if (degenerate) add(Severity::Warning, "メッシュ", Fmt("'%s'[%zu] に縮退した三角形が %d 個あります", mn, pi, degenerate), FixAction::RemoveDegenerate).mesh = mi;
            }
            if (!p.HasNormals()) add(Severity::Warning, "メッシュ", Fmt("'%s'[%zu] に法線がありません", mn, pi), FixAction::RecalcNormals).mesh = mi;
            else {
                int bad = 0;
                for (auto& n : p.normals) { float l = glm::length(n); if (!std::isfinite(l) || std::abs(l - 1) > 0.01f) ++bad; }
                if (bad) add(Severity::Warning, "メッシュ", Fmt("'%s'[%zu] に正規化されていない法線が %d 個あります", mn, pi, bad), FixAction::RecalcNormals).mesh = mi;
            }
            if (!p.HasUV0()) add(Severity::Info, "メッシュ", Fmt("'%s'[%zu] に UV がありません", mn, pi)).mesh = mi;
            if (p.HasUV0() && p.HasNormals() && !p.HasTangents()) {
                const Material* mat = (p.material >= 0 && p.material < (int)scene.materials.size()) ? &scene.materials[p.material] : nullptr;
                if (mat && mat->slots[Slot_Normal].Valid())
                    add(Severity::Info, "メッシュ", Fmt("'%s'[%zu] はノーマルマップを使いますが接線がありません", mn, pi), FixAction::GenerateTangents).mesh = mi;
            }
            if (p.material < 0) add(Severity::Info, "メッシュ", Fmt("'%s'[%zu] にマテリアルがありません", mn, pi)).mesh = mi;
            else if (p.material >= (int)scene.materials.size()) add(Severity::Error, "メッシュ", Fmt("'%s'[%zu] のマテリアル参照が範囲外です", mn, pi)).mesh = mi;
            if (p.positions.size() > 65535) add(Severity::Info, "メッシュ", Fmt("'%s'[%zu] は 65535 頂点を超えています (32bit インデックス)", mn, pi)).mesh = mi;

            if (p.HasSkin()) {
                int unnormalized = 0, zero = 0, oob = 0;
                for (size_t v = 0; v < p.weights.size(); ++v) {
                    const auto& w = p.weights[v];
                    float s = w.x + w.y + w.z + w.w;
                    if (s <= 1e-6f) ++zero;
                    else if (std::abs(s - 1.0f) > 1e-3f) ++unnormalized;
                    if (maxJoint >= 0)
                        for (int k = 0; k < 4; ++k) if (w[k] > 0 && p.joints[v][k] >= maxJoint) ++oob;
                }
                if (unnormalized) add(Severity::Warning, "スキン", Fmt("'%s'[%zu] に合計が 1 でないウェイトが %d 頂点あります", mn, pi, unnormalized), FixAction::NormalizeWeights).mesh = mi;
                if (zero) add(Severity::Error, "スキン", Fmt("'%s'[%zu] にウェイトが 0 の頂点が %d 個あります", mn, pi, zero), FixAction::NormalizeWeights).mesh = mi;
                if (oob) add(Severity::Error, "スキン", Fmt("'%s'[%zu] にスキンのジョイント数を超える番号が %d 個あります", mn, pi, oob), FixAction::ClampJointIndices).mesh = mi;
                if (p.droppedInfluences) add(Severity::Info, "スキン", Fmt("'%s'[%zu] はインポート時に 5 本目以降のウェイト %d 個を切り捨てました", mn, pi, p.droppedInfluences)).mesh = mi;
            }
            for (size_t t = 0; t < p.targets.size(); ++t)
                if (p.targets[t].dPositions.size() != p.positions.size())
                    add(Severity::Error, "モーフ", Fmt("'%s'[%zu] のモーフ %zu の頂点数が一致しません", mn, pi, t)).mesh = mi;
        }
    }

    // ---- スキン -----------------------------------------------------------
    for (size_t si = 0; si < scene.skins.size(); ++si) {
        const Skin& s = scene.skins[si];
        if (s.inverseBind.size() != s.joints.size())
            add(Severity::Error, "スキン", Fmt("スキン '%s' の逆バインド行列数がジョイント数と一致しません", s.name.c_str()));
        int missing = 0;
        for (int j : s.joints) if (j < 0 || j >= (int)scene.nodes.size()) ++missing;
        if (missing) add(Severity::Error, "スキン", Fmt("スキン '%s' に存在しないジョイントが %d 個あります", s.name.c_str(), missing));
        std::set<int> uniq(s.joints.begin(), s.joints.end());
        if (uniq.size() != s.joints.size()) add(Severity::Warning, "スキン", Fmt("スキン '%s' に重複したジョイントがあります", s.name.c_str()));
        if (s.joints.size() > 256) add(Severity::Info, "スキン", Fmt("スキン '%s' のジョイント数 %zu はゲームエンジンによっては多すぎます", s.name.c_str(), s.joints.size()));
        for (auto& ibm : s.inverseBind) {
            if (std::abs(glm::determinant(ibm)) < 1e-12f) {
                add(Severity::Error, "スキン", Fmt("スキン '%s' に特異な逆バインド行列があります", s.name.c_str()));
                break;
            }
        }
    }

    // ---- マテリアル / テクスチャ -----------------------------------------
    std::vector<bool> matUsed(scene.materials.size(), false), texUsed(scene.textures.size(), false);
    for (auto& m : scene.meshes) for (auto& p : m.primitives) if (p.material >= 0 && p.material < (int)matUsed.size()) matUsed[p.material] = true;
    for (int mi = 0; mi < (int)scene.materials.size(); ++mi) {
        const Material& mat = scene.materials[mi];
        if (!matUsed[mi]) add(Severity::Info, "マテリアル", Fmt("マテリアル '%s' は未使用です", mat.name.c_str()), FixAction::RemoveUnused).material = mi;
        for (int s = 0; s < Slot_Count; ++s) {
            int t = mat.slots[s].texture;
            if (t >= (int)scene.textures.size()) add(Severity::Error, "マテリアル", Fmt("'%s' の %s が範囲外のテクスチャを参照しています", mat.name.c_str(), TexSlotName(s))).material = mi;
            else if (t >= 0) texUsed[t] = true;
        }
        if (mat.alphaMode == AlphaMode::Opaque && mat.baseColor.a < 0.999f)
            add(Severity::Info, "マテリアル", Fmt("'%s' はアルファ < 1 ですが Opaque です", mat.name.c_str())).material = mi;
    }
    for (int ti = 0; ti < (int)scene.textures.size(); ++ti) {
        const Texture& t = scene.textures[ti];
        if (t.missing) add(Severity::Error, "テクスチャ", Fmt("テクスチャ '%s' が見つかりません (%s)", t.name.c_str(), t.originalUri.c_str())).texture = ti;
        if (!texUsed[ti]) add(Severity::Info, "テクスチャ", Fmt("テクスチャ '%s' は未使用です", t.name.c_str()), FixAction::RemoveUnused).texture = ti;
        int w = t.Width(), h = t.Height();
        if (w > 0 && h > 0 && ((w & (w - 1)) || (h & (h - 1))))
            add(Severity::Info, "テクスチャ", Fmt("テクスチャ '%s' (%dx%d) は 2 のべき乗サイズではありません", t.name.c_str(), w, h)).texture = ti;
        if (w > 4096 || h > 4096)
            add(Severity::Info, "テクスチャ", Fmt("テクスチャ '%s' (%dx%d) は大きすぎる可能性があります", t.name.c_str(), w, h)).texture = ti;
    }

    // ---- アニメーション -----------------------------------------------------
    for (auto& a : scene.animations) {
        if (a.channels.empty()) add(Severity::Warning, "アニメーション", Fmt("'%s' にチャンネルがありません", a.name.c_str()));
        for (auto& c : a.channels) {
            bool sorted = true;
            for (size_t i = 1; i < c.times.size(); ++i) if (c.times[i] < c.times[i - 1]) sorted = false;
            if (!sorted) { add(Severity::Error, "アニメーション", Fmt("'%s' のキー時間が昇順ではありません", a.name.c_str())); break; }
            if (c.values.size() != c.times.size()) { add(Severity::Error, "アニメーション", Fmt("'%s' のキー数と値の数が一致しません", a.name.c_str())); break; }
        }
    }

    // ---- 単位系のヒント ---------------------------------------------------
    auto world = scene.ComputeWorldMatrices();
    glm::vec3 mn, mx;
    scene.ComputeBounds(world, mn, mx);
    float size = glm::length(mx - mn);
    if (size > 1000.0f) add(Severity::Info, "単位", Fmt("シーンサイズが %.1f と大きいです。cm 単位の可能性があります (x0.01 で m に)", size));
    else if (size > 0 && size < 0.01f) add(Severity::Info, "単位", Fmt("シーンサイズが %.5f と小さいです。単位を確認してください", size));
    return out;
}

bool ApplyFix(Scene& scene, const Issue& issue) {
    auto forMesh = [&](auto&& fn) {
        if (issue.mesh < 0 || issue.mesh >= (int)scene.meshes.size()) return false;
        for (auto& p : scene.meshes[issue.mesh].primitives) fn(p);
        scene.meshes[issue.mesh].Touch();
        return true;
    };
    switch (issue.fix) {
    case FixAction::RecalcNormals: return forMesh([](Primitive& p) { Ops::RecalculateNormals(p, true, 60.0f); });
    case FixAction::GenerateTangents: return forMesh([](Primitive& p) { Ops::GenerateTangents(p); });
    case FixAction::NormalizeWeights: return forMesh([](Primitive& p) { Ops::NormalizeWeights(p); });
    case FixAction::RemoveDegenerate: return forMesh([](Primitive& p) { Ops::RemoveDegenerateTriangles(p); });
    case FixAction::ClampJointIndices: {
        int maxJ = 0;
        for (int n : scene.NodesUsingMesh(issue.mesh)) {
            int s = scene.nodes[n].skin;
            if (s >= 0 && s < (int)scene.skins.size()) maxJ = std::max(maxJ, (int)scene.skins[s].joints.size());
        }
        return forMesh([&](Primitive& p) {
            for (size_t v = 0; v < p.joints.size(); ++v)
                for (int k = 0; k < 4; ++k)
                    if (p.joints[v][k] >= maxJ) { p.joints[v][k] = 0; p.weights[v][k] = 0; }
            Ops::NormalizeWeights(p);
        });
    }
    case FixAction::UniquifyNames: return Ops::UniquifyNames(scene) > 0;
    case FixAction::RemoveUnused: return Ops::RemoveUnusedResources(scene) > 0;
    default: return false;
    }
}
