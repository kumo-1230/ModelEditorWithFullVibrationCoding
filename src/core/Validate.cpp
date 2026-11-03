#include "Validate.h"
#include "Diagnostics.h"
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

    int nonAscii = 0, firstNonAscii = -1, maxDepth = 0, deepest = -1;
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        if (HasNonAscii(scene.nodes[i].name)) { if (firstNonAscii < 0) firstNonAscii = i; ++nonAscii; }
        int d = 0;
        for (int p = scene.nodes[i].parent; p >= 0 && d < 100000; p = scene.nodes[p].parent) ++d;
        if (d > maxDepth) { maxDepth = d; deepest = i; }
    }
    if (nonAscii)
        add(Severity::Info, "ノード", Fmt("日本語などの ASCII 以外の文字を含むノード名が %d 個あります (例: '%s')。ツールやエンジンによっては文字化けや対応付けの失敗の原因になります",
                                         nonAscii, scene.nodes[firstNonAscii].name.c_str())).node = firstNonAscii;
    if (maxDepth > 64)
        add(Severity::Info, "ノード", Fmt("階層の深さが %d 段あります ('%s')。深すぎる階層はエンジンで遅くなったり読み込めなかったりします", maxDepth, scene.nodes[deepest].name.c_str())).node = deepest;

    auto joints = scene.JointMask();
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        const Node& n = scene.nodes[i];
        if (n.mesh >= 0 && n.skin >= 0 && n.mesh < (int)scene.meshes.size() && scene.meshes[n.mesh].HasSkin() && !joints[i]) {
            glm::mat4 L = n.LocalMatrix();
            bool identity = true;
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) if (std::abs(L[c][r] - (c == r ? 1.0f : 0.0f)) > 1e-5f) identity = false;
            if (!identity)
                add(Severity::Info, "スキン", Fmt("スキンメッシュ '%s' のノードに移動・回転・スケールがあります。glTF では無視されるため、エンジンによって位置や大きさが変わることがあります",
                                                n.name.c_str())).node = i;
        }
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

            // トポロジー (穴・非多様体・面の向き・裏返し・重なり)
            if (!badIdx && p.indices.size() >= 3) {
                TopologyInfo topo = AnalyzeTopology(p);
                auto addP = [&](Severity s, const char* cat, std::string msg, FixAction fix = FixAction::None) {
                    Issue& is = add(s, cat, std::move(msg), fix);
                    is.mesh = mi;
                    is.prim = (int)pi;
                };
                if (topo.nonManifoldEdges)
                    addP(Severity::Warning, "トポロジー", Fmt("'%s'[%zu] に 3 枚以上の面が共有する辺 (非多様体) が %d 本あります。法線の計算・UV 展開・ブーリアンや物理で問題になります",
                                                        mn, pi, topo.nonManifoldEdges));
                if (topo.windingMismatch)
                    addP(Severity::Warning, "トポロジー", Fmt("'%s'[%zu] で、隣り合う面の向きが逆になっている辺が %d 本あります (一部の面だけ裏返っています。「表示モード: 裏面」で赤く見えます)",
                                                        mn, pi, topo.windingMismatch));
                if (topo.closed && topo.signedVolume < 0.0)
                    addP(Severity::Warning, "トポロジー", Fmt("'%s'[%zu] は閉じた形状ですが、面が内側を向いています (裏返し)。背面カリングで消えて見えます", mn, pi), FixAction::FlipInsideOut);
                if (topo.openEdges)
                    addP(Severity::Info, "トポロジー", Fmt("'%s'[%zu] に開いた辺 (穴や縁) が %d 本あります。板ポリゴン以外では穴の可能性があります (「表示: 開いた辺」で確認)",
                                                     mn, pi, topo.openEdges));
                if (topo.duplicateTriangles)
                    addP(Severity::Warning, "トポロジー", Fmt("'%s'[%zu] に同じ位置で重なった三角形が %d 個あります (ちらつきの原因)", mn, pi, topo.duplicateTriangles));
                if (topo.isolatedVertices)
                    addP(Severity::Info, "メッシュ", Fmt("'%s'[%zu] にどの面にも使われていない頂点が %d 個あります", mn, pi, topo.isolatedVertices));
                if (p.HasNormals() && topo.triangles > 0 && topo.normalsAgainstFaces > std::max(1, topo.triangles / 10))
                    addP(Severity::Warning, "メッシュ", Fmt("'%s'[%zu] の %d / %d 面で、頂点法線が面の向きと逆です (法線が裏返っています。「表示モード: 法線と面の向き」で確認)",
                                                     mn, pi, topo.normalsAgainstFaces, topo.triangles), FixAction::RecalcNormals);
            }
            // UV
            if (p.HasUV0()) {
                UVInfo uv = AnalyzeUV(p);
                if (uv.nan) add(Severity::Error, "UV", Fmt("'%s'[%zu] に NaN/Inf の UV が %d 個あります", mn, pi, uv.nan)).mesh = mi;
                if (uv.outside01)
                    add(Severity::Info, "UV", Fmt("'%s'[%zu] の UV が 0〜1 の外にあります (%.3g〜%.3g, %.3g〜%.3g)。タイリングやベイクを考えていなければ確認してください",
                                                mn, pi, uv.min.x, uv.max.x, uv.min.y, uv.max.y)).mesh = mi;
                if (uv.zeroAreaTris)
                    add(Severity::Info, "UV", Fmt("'%s'[%zu] に UV 上で面積 0 の三角形が %d 個あります (テクスチャが伸び、接線が作れません)", mn, pi, uv.zeroAreaTris)).mesh = mi;
            }
            if (p.HasTangents()) {
                int badT = 0;
                for (auto& t : p.tangents) {
                    float l = glm::length(glm::vec3(t));
                    if (!std::isfinite(l) || l < 0.5f || std::abs(std::abs(t.w) - 1.0f) > 1e-3f) ++badT;
                }
                if (badT) add(Severity::Warning, "メッシュ", Fmt("'%s'[%zu] に壊れた接線 (長さ 0 / NaN) が %d 個あります (ノーマルマップが乱れます)", mn, pi, badT), FixAction::GenerateTangents).mesh = mi;
            }
            if (p.HasColors()) {
                int badC = 0;
                for (auto& c : p.colors) if (!std::isfinite(c.x + c.y + c.z + c.w)) ++badC;
                if (badC) add(Severity::Error, "メッシュ", Fmt("'%s'[%zu] に NaN/Inf の頂点カラーが %d 個あります", mn, pi, badC)).mesh = mi;
            }

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
    glm::vec3 sceneMin, sceneMax;
    scene.ComputeBounds(scene.ComputeWorldMatrices(), sceneMin, sceneMax);
    const float sceneDiag = std::max(glm::length(sceneMax - sceneMin), 1e-6f);
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        const Node& n = scene.nodes[i];
        if (n.mesh < 0 || n.skin < 0) continue;
        float dev = BindPoseDeviation(scene, i);
        if (dev > sceneDiag * 0.01f)
            add(Severity::Info, "スキン", Fmt("'%s' はレストポーズがバインドポーズと違います (最大 %.3g m 変形しています)。アニメーションを止めた状態で形が崩れて見えるのはこのためです",
                                            n.name.c_str(), dev)).node = i;
    }
    for (size_t si = 0; si < scene.skins.size(); ++si) {
        const Skin& s = scene.skins[si];
        // ウェイトを持たないジョイント / 複数のスケルトンルート
        auto counts = JointVertexCounts(scene, (int)si);
        int unusedJ = 0;
        for (int c : counts) if (c == 0) ++unusedJ;
        if (unusedJ && unusedJ < (int)counts.size())
            add(Severity::Info, "スキン", Fmt("スキン '%s' の %d / %zu 本のジョイントは、どの頂点も動かしていません (末端ボーンや補助ボーンなら問題ありません)",
                                            s.name.c_str(), unusedJ, counts.size()));
        if (!counts.empty() && unusedJ == (int)counts.size())
            add(Severity::Warning, "スキン", Fmt("スキン '%s' のジョイントがどの頂点も動かしていません (ウェイトが失われています)", s.name.c_str()));
        std::set<int> roots;
        std::set<int> jointSet(s.joints.begin(), s.joints.end());
        for (int j : s.joints) {
            if (j < 0 || j >= (int)scene.nodes.size()) continue;
            int r = j;
            while (scene.nodes[r].parent >= 0 && jointSet.count(scene.nodes[r].parent)) r = scene.nodes[r].parent;
            roots.insert(r);
        }
        if (roots.size() > 1)
            add(Severity::Info, "スキン", Fmt("スキン '%s' のジョイントが %zu 個の別々の階層に分かれています。Unity の Humanoid や Unreal ではルートボーンが 1 本である必要があります",
                                            s.name.c_str(), roots.size()));
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
        // テクスチャの割り当て間違い (平均色から推測)
        auto texAvg = [&](int slot, glm::vec4& avg, float* amin) {
            int t = mat.slots[slot].texture;
            return t >= 0 && t < (int)scene.textures.size() && AverageTextureColor(scene.textures[t], avg, amin);
        };
        glm::vec4 avg;
        float amin = 1.0f;
        if (texAvg(Slot_Normal, avg, nullptr) && !LooksLikeNormalMap(avg))
            add(Severity::Warning, "マテリアル", Fmt("'%s' の法線スロットの画像はノーマルマップに見えません (平均色 %.2f, %.2f, %.2f)。別の画像が割り当てられていないか確認してください",
                                                 mat.name.c_str(), avg.r, avg.g, avg.b)).material = mi;
        if (texAvg(Slot_BaseColor, avg, &amin)) {
            if (LooksLikeNormalMap(avg))
                add(Severity::Warning, "マテリアル", Fmt("'%s' のベースカラーにノーマルマップのような画像 (青紫) が割り当てられています", mat.name.c_str())).material = mi;
            if (mat.alphaMode == AlphaMode::Blend && amin > 0.99f && mat.baseColor.a > 0.999f)
                add(Severity::Info, "マテリアル", Fmt("'%s' は Blend ですが透明な部分がありません。Opaque にすると軽くなり、描画順の問題も起きません", mat.name.c_str())).material = mi;
            if (mat.alphaMode == AlphaMode::Opaque && amin < 0.5f)
                add(Severity::Info, "マテリアル", Fmt("'%s' のベースカラーに透明な部分 (アルファ %.2f) がありますが Opaque です。切り抜くなら Mask にしてください", mat.name.c_str(), amin)).material = mi;
        }
        if (mat.metallic > 0.99f && !mat.slots[Slot_MetallicRoughness].Valid() && !mat.slots[Slot_BaseColor].Valid() && glm::vec3(mat.baseColor) == glm::vec3(0.0f))
            add(Severity::Info, "マテリアル", Fmt("'%s' はメタリック 1 でベースカラーが黒のため、真っ黒に見えます", mat.name.c_str())).material = mi;
    }
    for (int ti = 0; ti < (int)scene.textures.size(); ++ti) {
        const Texture& t = scene.textures[ti];
        if (t.missing) add(Severity::Error, "テクスチャ", Fmt("テクスチャ '%s' が見つかりません (%s)", t.name.c_str(), t.originalUri.c_str())).texture = ti;
        else if (!t.image) add(Severity::Error, "テクスチャ", Fmt("テクスチャ '%s' の画像を読み込めていません (対応していない形式の可能性)", t.name.c_str())).texture = ti;
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
    for (int ai = 0; ai < (int)scene.animations.size(); ++ai) {
        const Animation& a = scene.animations[ai];
        AnimationReport r = AnalyzeAnimation(scene, ai);
        auto addA = [&](Severity s, std::string msg, FixAction fix = FixAction::None) { add(s, "アニメーション", std::move(msg), fix).animation = ai; };
        const char* an = a.name.c_str();
        if (a.duration <= 0.0f && !a.channels.empty()) addA(Severity::Warning, Fmt("'%s' の長さが 0 秒です (1 フレームだけのポーズ)", an));
        if (r.nanKeys) addA(Severity::Error, Fmt("'%s' に NaN/Inf のキーが %d 個あります (モデルが消えたり飛んだりします)", an, r.nanKeys));
        if (r.negativeTimes) addA(Severity::Error, Fmt("'%s' に負の時刻のキーが %d 個あります", an, r.negativeTimes));
        if (r.quatFlips)
            addA(Severity::Warning, Fmt("'%s' で、回転キーの符号が反転している箇所が %d あります。キーの間で遠回り (一回転) して見えます", an, r.quatFlips), FixAction::FixQuaternions);
        if (r.unnormalizedQuats) addA(Severity::Warning, Fmt("'%s' に長さが 1 でない回転キーが %d 個あります", an, r.unnormalizedQuats), FixAction::FixQuaternions);
        if (r.zeroScaleKeys) addA(Severity::Warning, Fmt("'%s' にスケール 0 のキーが %d 個あります (その間メッシュが消えます)", an, r.zeroScaleKeys));
        if (r.missingNodeChannels) addA(Severity::Warning, Fmt("'%s' に存在しないノードへのチャンネルが %d 本あります", an, r.missingNodeChannels));
        if (r.duplicateChannels) addA(Severity::Warning, Fmt("'%s' に同じノード・同じ種類のチャンネルが重複して %d 本あります (どちらが使われるかはツール次第です)", an, r.duplicateChannels));
        if (r.maxBoundsRatio > 3.0f)
            addA(Severity::Warning, Fmt("'%s' の %.2f 秒付近で、モデルの範囲がレストポーズの %.1f 倍になります。頂点が飛んでいる (ウェイトや逆バインド行列の不整合) 可能性があります",
                                        an, r.maxBoundsTime, r.maxBoundsRatio));
        if (a.duration > 0.0f && r.loopError > sceneDiag * 0.02f)
            addA(Severity::Info, Fmt("'%s' は最初と最後の姿勢が違います (最大 %.3g m)。ループ再生すると継ぎ目で跳ねます (ループしないモーションなら問題ありません)", an, r.loopError));
    }

    // ---- 単位系のヒント ---------------------------------------------------
    auto world = scene.ComputeWorldMatrices();
    glm::vec3 mn, mx;
    scene.ComputeBounds(world, mn, mx);
    float size = glm::length(mx - mn);
    if (size > 1000.0f) add(Severity::Info, "単位", Fmt("シーンサイズが %.1f と大きいです。cm 単位の可能性があります (x0.01 で m に)", size));
    else if (size > 0 && size < 0.01f) add(Severity::Info, "単位", Fmt("シーンサイズが %.5f と小さいです。単位を確認してください", size));
    glm::vec3 center = (mn + mx) * 0.5f;
    if (size > 0 && glm::length(center) > std::max(size * 10.0f, 100.0f))
        add(Severity::Info, "単位", Fmt("モデルが原点から %.1f m 離れています。エンジンに置いたとき見つからない・ピボットが離れる原因になります (「原点へ移動」で直せます)",
                                       glm::length(center)));
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
    case FixAction::FlipInsideOut: { // 法線を反転し、巻き順も逆にする
        if (issue.mesh < 0 || issue.mesh >= (int)scene.meshes.size()) return false;
        auto& prims = scene.meshes[issue.mesh].primitives;
        for (int pi = 0; pi < (int)prims.size(); ++pi)
            if (issue.prim < 0 || issue.prim == pi) Ops::FlipNormals(prims[pi]);
        scene.meshes[issue.mesh].Touch();
        return true;
    }
    case FixAction::FixQuaternions:
        if (issue.animation < 0 || issue.animation >= (int)scene.animations.size()) return false;
        return FixQuaternionKeys(scene.animations[issue.animation]) > 0;
    case FixAction::UniquifyNames: return Ops::UniquifyNames(scene) > 0;
    case FixAction::RemoveUnused: return Ops::RemoveUnusedResources(scene) > 0;
    default: return false;
    }
}
