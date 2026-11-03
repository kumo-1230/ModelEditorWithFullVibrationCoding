// --selftest <file>: 編集オペレーションがワールド上の見た目を変えないことを数値で検証する
#include "Cli.h"
#include "core/Diagnostics.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include "core/Validate.h"
#include "io/Exporter.h"
#include "io/Importer.h"
#include "util/FileUtil.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>

namespace {

// 全メッシュ頂点のワールド座標 (スキンは指定ポーズで変形)
std::vector<glm::vec3> WorldPositions(const Scene& s, const std::vector<NodePose>& pose) {
    auto world = s.ComputeWorldMatrices(pose);
    std::vector<glm::vec3> out;
    for (int i = 0; i < (int)s.nodes.size(); ++i) {
        const Node& n = s.nodes[i];
        if (n.mesh < 0) continue;
        const Mesh& m = s.meshes[n.mesh];
        const Skin* skin = (n.skin >= 0 && m.HasSkin()) ? &s.skins[n.skin] : nullptr;
        for (auto& p : m.primitives) {
            for (size_t v = 0; v < p.positions.size(); ++v) {
                glm::mat4 M = world[i];
                if (skin && p.HasSkin()) {
                    M = glm::mat4(0.0f);
                    float sum = 0;
                    for (int k = 0; k < 4; ++k) {
                        float w = p.weights[v][k];
                        if (w <= 0) continue;
                        int j = p.joints[v][k];
                        M += world[skin->joints[j]] * skin->inverseBind[j] * w;
                        sum += w;
                    }
                    if (sum <= 0) M = world[i];
                }
                out.push_back(glm::vec3(M * glm::vec4(p.positions[v], 1.0f)));
            }
        }
    }
    return out;
}

std::vector<NodePose> PoseAt(const Scene& s, float t) {
    auto pose = s.RestPose();
    if (!s.animations.empty()) SampleAnimation(s, s.animations[0], t, pose);
    return pose;
}

float MaxError(const std::vector<glm::vec3>& a, const std::vector<glm::vec3>& b) {
    if (a.size() != b.size()) return 1e30f;
    float e = 0;
    for (size_t i = 0; i < a.size(); ++i) e = std::max(e, glm::length(a[i] - b[i]));
    return e;
}

// テスト用リグ: ルートモーション (移動キー) を持つルートボーン + 回転・非均等スケールする子ボーン + スキンメッシュ
void BuildTestRig(Scene& s) {
    int root = s.AddNode("RootBone", -1);
    int spine = s.AddNode("Spine", root);
    s.nodes[spine].translation = glm::vec3(0, 0.5f, 0);
    int meshNode = Ops::CreatePrimitive(s, Ops::Prim_Cylinder, 1.0f);
    Node& mn = s.nodes[meshNode];
    Primitive& p = s.meshes[mn.mesh].primitives[0];
    Ops::TransformPrimitive(p, mn.LocalMatrix()); // バインド空間 (= ワールド) に置く
    mn.translation = glm::vec3(0.0f);
    for (auto& v : p.positions) {
        float w = std::clamp((v.y - 0.3f) / 0.4f, 0.0f, 1.0f);
        p.joints.push_back(glm::u16vec4(0, 1, 0, 0));
        p.weights.push_back(glm::vec4(1.0f - w, w, 0, 0));
    }
    Skin sk;
    sk.name = "Skin";
    sk.joints = { root, spine };
    sk.inverseBind = { glm::mat4(1.0f), glm::inverse(glm::translate(glm::mat4(1.0f), glm::vec3(0, 0.5f, 0))) };
    sk.skeleton = root;
    s.skins.push_back(sk);
    mn.skin = 0;
    Animation a;
    a.name = "Walk";
    AnimChannel t;
    t.node = root;
    t.path = AnimPath::Translation;
    t.times = { 0.0f, 1.0f, 2.0f };
    t.values = { glm::vec4(0, 0, 0, 0), glm::vec4(0, 0.2f, 1.5f, 0), glm::vec4(0, 0, 3.0f, 0) };
    AnimChannel r;
    r.node = spine;
    r.path = AnimPath::Rotation;
    r.times = { 0.0f, 1.0f, 2.0f };
    for (float deg : { 0.0f, 40.0f, -20.0f }) {
        glm::quat q = glm::angleAxis(glm::radians(deg), glm::vec3(0, 0, 1));
        r.values.push_back(glm::vec4(q.x, q.y, q.z, q.w));
    }
    AnimChannel sc;
    sc.node = spine;
    sc.path = AnimPath::Scale;
    sc.times = { 0.0f, 2.0f };
    sc.values = { glm::vec4(1, 1, 1, 0), glm::vec4(1.2f, 1, 1, 0) };
    a.channels = { t, r, sc };
    a.RecomputeDuration();
    s.animations.push_back(a);
}

} // namespace

bool RunSelfTest(const std::string& path, int& exitCode) {
    ImportOptions io;
    Scene base;
    std::string err;
    if (path == "@rig") BuildTestRig(base);
    else if (!ImportModel(path, io, base, err)) {
        printf("読み込み失敗: %s\n", err.c_str());
        exitCode = 1;
        return true;
    }
    glm::vec3 mn, mx;
    base.ComputeBounds(base.ComputeWorldMatrices(), mn, mx);
    float size = std::max(glm::length(mx - mn), 1e-6f);
    float tol = size * 1e-4f;
    float tMid = base.animations.empty() ? 0.0f : base.animations[0].duration * 0.5f;
    auto refRest = WorldPositions(base, PoseAt(base, 0));
    auto refAnim = WorldPositions(base, PoseAt(base, tMid));
    printf("モデル: %s  頂点 %zu  サイズ %.4g  アニメ時刻 %.3f\n", path.c_str(), refRest.size(), size, tMid);

    int failed = 0;
    auto check = [&](const char* name, std::function<bool(Scene&)> op, float scaleBack = 1.0f, glm::mat4 xformBack = glm::mat4(1)) {
        Scene s = base;
        bool ok = op(s);
        auto r = WorldPositions(s, PoseAt(s, 0));
        auto a = WorldPositions(s, PoseAt(s, tMid));
        for (auto& v : r) v = glm::vec3(xformBack * glm::vec4(v * scaleBack, 1));
        for (auto& v : a) v = glm::vec3(xformBack * glm::vec4(v * scaleBack, 1));
        float e1 = MaxError(refRest, r), e2 = MaxError(refAnim, a);
        bool pass = ok && e1 <= tol && e2 <= tol;
        if (!pass) ++failed;
        printf("[%s] %-40s rest誤差=%.3g anim誤差=%.3g\n", pass ? "PASS" : "FAIL", name, e1, e2);
    };

    check("ApplyUniformScaleToHierarchy (全ルート)", [](Scene& s) {
        for (int r : std::vector<int>(s.roots)) Ops::ApplyUniformScaleToHierarchy(s, r, nullptr);
        return true;
    });
    check("FreezeHierarchy T/R/S (全ルート)", [](Scene& s) {
        for (int r : std::vector<int>(s.roots)) Ops::FreezeHierarchy(s, r, true, true, true, nullptr);
        return true;
    });
    check("FreezeHierarchy S のみ", [](Scene& s) {
        for (int r : std::vector<int>(s.roots)) Ops::FreezeHierarchy(s, r, false, false, true, nullptr);
        return true;
    });
    check("ScaleScene x2 (比較時 x0.5)", [](Scene& s) { Ops::ScaleScene(s, 2.0f); return true; }, 0.5f);
    check("ConvertUpAxis Y→Z→Y", [](Scene& s) { Ops::ConvertUpAxis(s, 1, 2); Ops::ConvertUpAxis(s, 2, 1); return true; });
    check("CenterPivot (非スキン全ノード)", [](Scene& s) {
        for (int i = 0; i < (int)s.nodes.size(); ++i)
            if (s.nodes[i].mesh >= 0 && s.nodes[i].skin < 0) Ops::CenterPivot(s, i, nullptr);
        return true;
    });
    check("UniquifyNames + RemoveEmptyNodes", [](Scene& s) { Ops::UniquifyNames(s); Ops::RemoveEmptyNodes(s); return true; });

    // ---- 診断: わざと壊した形状を正しく見つけ、直せるか -------------------------
    {
        auto result = [&](const char* name, bool pass, const std::string& detail) {
            if (!pass) ++failed;
            printf("[%s] 診断: %-34s %s\n", pass ? "PASS" : "FAIL", name, detail.c_str());
        };
        auto hasIssue = [](const Scene& s, FixAction fix, const char* category) {
            for (auto& is : ValidateScene(s))
                if ((fix != FixAction::None && is.fix == fix) || (category && is.category == category)) return true;
            return false;
        };
        Scene cubeScene;
        int cube = Ops::CreatePrimitive(cubeScene, Ops::Prim_Cube, 1.0f);
        Primitive& cp = cubeScene.meshes[cubeScene.nodes[cube].mesh].primitives[0];
        TopologyInfo t0 = AnalyzeTopology(cp);
        result("立方体は閉じていて向きが揃う", t0.closed && t0.openEdges == 0 && t0.windingMismatch == 0 && t0.signedVolume > 0 && t0.normalsAgainstFaces == 0,
               "開いた辺 " + std::to_string(t0.openEdges) + " / 体積 " + std::to_string(t0.signedVolume));
        // 裏返し (法線も巻き順も内向き) → 検出 → 自動修正
        Scene inside = cubeScene;
        Ops::FlipNormals(inside.meshes[inside.nodes[cube].mesh].primitives[0]);
        bool found = hasIssue(inside, FixAction::FlipInsideOut, nullptr);
        for (auto& is : ValidateScene(inside)) ApplyFix(inside, is);
        TopologyInfo t1 = AnalyzeTopology(inside.meshes[inside.nodes[cube].mesh].primitives[0]);
        result("裏返った立方体を検出して修正", found && t1.signedVolume > 0 && t1.normalsAgainstFaces == 0, found ? "検出 OK" : "検出できず");
        // 1 枚だけ裏返った面
        Scene oneFlip = cubeScene;
        auto& oi = oneFlip.meshes[oneFlip.nodes[cube].mesh].primitives[0].indices;
        std::swap(oi[1], oi[2]);
        TopologyInfo t2 = AnalyzeTopology(oneFlip.meshes[oneFlip.nodes[cube].mesh].primitives[0]);
        result("1 枚だけ裏返った面を検出", t2.windingMismatch > 0, "向き不一致の辺 " + std::to_string(t2.windingMismatch));
        // 重なった三角形 (同じ面を 2 回)
        Scene dup = cubeScene;
        auto& di = dup.meshes[dup.nodes[cube].mesh].primitives[0].indices;
        di.insert(di.end(), { di[0], di[1], di[2] });
        TopologyInfo t3 = AnalyzeTopology(dup.meshes[dup.nodes[cube].mesh].primitives[0]);
        result("重なった三角形と非多様体の辺を検出", t3.duplicateTriangles == 1 && t3.nonManifoldEdges > 0,
               "重なり " + std::to_string(t3.duplicateTriangles) + " / 非多様体 " + std::to_string(t3.nonManifoldEdges));
        // 板 (開いた形状)
        Scene planeScene;
        int plane = Ops::CreatePrimitive(planeScene, Ops::Prim_Plane, 1.0f);
        TopologyInfo t4 = AnalyzeTopology(planeScene.meshes[planeScene.nodes[plane].mesh].primitives[0]);
        result("板の縁を開いた辺として検出", !t4.closed && t4.openEdges > 0, "開いた辺 " + std::to_string(t4.openEdges));
        // レイキャスト: 真上から立方体 (高さ 0〜1) の上面に当たる
        MeshHit hit;
        bool rayOk = RaycastMeshes(cubeScene, cubeScene.ComputeWorldMatrices(), glm::vec3(0.1f, 5.0f, 0.2f), glm::vec3(0, -1, 0), hit);
        result("レイキャストで上面に当たる", rayOk && hit.node == cube && std::abs(hit.pos.y - 1.0f) < 1e-4f, rayOk ? "y=" + std::to_string(hit.pos.y) : "当たらず");
        // テクスチャの割り当て間違いの推測
        result("ノーマルマップの色を判定", LooksLikeNormalMap(glm::vec4(0.5f, 0.5f, 1.0f, 1)) && !LooksLikeNormalMap(glm::vec4(0.8f, 0.3f, 0.2f, 1)), "");
    }
    if (path == "@rig") {
        auto result = [&](const char* name, bool pass, const std::string& detail) {
            if (!pass) ++failed;
            printf("[%s] 診断: %-34s %s\n", pass ? "PASS" : "FAIL", name, detail.c_str());
        };
        AnimationReport r = AnalyzeAnimation(base, 0);
        int root = base.FindNode("RootBone");
        result("ルートモーションを検出", r.rootNode == root && std::abs(r.rootDisplacement - 3.0f) < 1e-3f && r.maxBoundsRatio < 3.0f,
               "移動 " + std::to_string(r.rootDisplacement) + " m / 範囲の最大倍率 " + std::to_string(r.maxBoundsRatio));
        result("レストポーズ = バインドポーズ", BindPoseDeviation(base, base.FindNode("Cylinder1")) < 1e-5f, "");
        // 回転キーの符号を反転させる (同じ向きだが、補間すると遠回りになる)
        Scene flipped = base;
        for (auto& c : flipped.animations[0].channels)
            if (c.path == AnimPath::Rotation && c.values.size() >= 2) c.values[1] = -c.values[1];
        int before = AnalyzeAnimation(flipped, 0).quatFlips;
        auto keyPose = WorldPositions(flipped, PoseAt(flipped, 1.0f));
        int fixedKeys = FixQuaternionKeys(flipped.animations[0]);
        int after = AnalyzeAnimation(flipped, 0).quatFlips;
        float keyErr = MaxError(keyPose, WorldPositions(flipped, PoseAt(flipped, 1.0f)));
        result("回転キーの反転を検出して修正", before > 0 && fixedKeys > 0 && after == 0 && keyErr <= tol,
               "反転 " + std::to_string(before) + " → " + std::to_string(after) + " / キー時刻の姿勢誤差 " + std::to_string(keyErr));
    }
    // アニメーション速度 x2: 速くした後の時刻 t/2 の姿勢が、元の時刻 t の姿勢と一致するか
    if (!base.animations.empty()) {
        Scene fast = base;
        Ops::ScaleAnimationSpeed(fast.animations[0], 2.0f);
        float e = 0.0f;
        for (float k : { 0.0f, 0.3f, 0.5f, 0.8f, 1.0f }) {
            float t = base.animations[0].duration * k;
            e = std::max(e, MaxError(WorldPositions(base, PoseAt(base, t)), WorldPositions(fast, PoseAt(fast, t * 0.5f))));
        }
        bool lenOk = std::abs(fast.animations[0].duration - base.animations[0].duration * 0.5f) < 1e-5f;
        bool pass = e <= tol && lenOk;
        if (!pass) ++failed;
        printf("[%s] %-40s 長さ %.3f→%.3f 姿勢誤差=%.3g\n", pass ? "PASS" : "FAIL", "アニメーション速度 x2", base.animations[0].duration,
               fast.animations[0].duration, e);
    }
    // オブジェクトノード (親) で x1.7 → 書き出し用の焼き込み。全フレームのボーン・頂点で比較する
    const float objScale = 1.7f;
    Scene parentScaled = base;
    {
        int w = parentScaled.AddNode("SelfTest_Object", -1);
        for (int r : std::vector<int>(parentScaled.roots)) if (r != w) parentScaled.Reparent(r, w, false);
        parentScaled.nodes[w].objectRoot = parentScaled.nodes[w].objectWrapper = true;
        parentScaled.nodes[w].scale = glm::vec3(objScale);
    }
    auto checkMotion = [&](const char* name, const Scene& ref, const Scene& s) {
        int frames = 0;
        float e = Ops::MaxMotionError(ref, s, 30.0f, &frames);
        bool pass = e >= 0 && e <= tol * objScale;
        if (!pass) ++failed;
        printf("[%s] %-40s 全 %d フレーム 最大誤差=%.3g\n", pass ? "PASS" : "FAIL", name, frames, e);
    };
    {
        Scene baked = parentScaled;
        int n = Ops::BakeObjectScales(baked, true);
        bool removed = baked.nodes.size() == base.nodes.size() && baked.FindNode("SelfTest_Object") < 0;
        if (n != 1 || !removed) { ++failed; printf("[FAIL] 焼き込み数 %d / 親ノードの除去 %s\n", n, removed ? "OK" : "NG"); }
        checkMotion("親スケール x1.7 → 焼き込み (親を除去)", parentScaled, baked);
        Scene global = base;
        Ops::ScaleScene(global, objScale);
        checkMotion("焼き込み結果 = シーン全体 x1.7", global, baked);
        Scene kept = parentScaled;
        Ops::BakeObjectScales(kept, false);
        checkMotion("親スケール x1.7 → 焼き込み (親を残す)", parentScaled, kept);
    }
    // 参考: 従来の「ルートのノードを直接拡大」(移動キーは拡大されない) との差
    if (!base.animations.empty()) {
        Scene direct = base;
        for (int r : std::vector<int>(direct.roots)) Ops::OffsetNodeTRS(direct, r, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::vec3(objScale));
        float e = Ops::MaxMotionError(parentScaled, direct);
        printf("[INFO] %-40s 最大誤差=%.3g (ルートモーションがあると大きくなる)\n", "参考: ルートノードを直接 x1.7", e);
    }

    // 書き出し → 再読み込み (頂点数とバウンディングボックス)
    auto tmpDir = std::filesystem::temp_directory_path() / "ModelEditorSelfTest";
    std::filesystem::create_directories(tmpDir);
    for (const char* ext : { ".glb", ".gltf", ".fbx" }) {
        std::string out = FileUtil::FromPath(tmpDir / (std::string("selftest") + ext));
        ExportOptions eo;
        eo.verifyAfterExport = false;
        bool ok = ExportScene(base, out, eo, err);
        Scene re;
        ImportOptions io2;
        io2.convertUnitsToMeters = true;
        io2.calcTangents = false;
        ok = ok && ImportModel(out, io2, re, err);
        // FBX ではレストポーズがバインドポーズに置き換わることがあるので、アニメーション中の姿勢で比較する
        glm::vec3 mnA, mxA, mn2, mx2;
        base.ComputeBounds(base.ComputeWorldMatrices(PoseAt(base, tMid)), mnA, mxA);
        re.ComputeBounds(re.ComputeWorldMatrices(PoseAt(re, tMid)), mn2, mx2);
        float e = std::max(glm::length(mn2 - mnA), glm::length(mx2 - mxA));
        bool animOk = re.animations.size() == base.animations.size();
        bool skinOk = re.skins.size() == base.skins.size();
        bool pass = ok && e <= size * 1e-3f && animOk && skinOk;
        if (!pass) ++failed;
        printf("[%s] 書き出し→再読み込み %-6s AABB誤差=%.3g  頂点 %zu→%zu  スキン %zu→%zu  アニメ %zu→%zu %s\n", pass ? "PASS" : "FAIL", ext, e,
               base.TotalVertices(), re.TotalVertices(), base.skins.size(), re.skins.size(), base.animations.size(), re.animations.size(),
               ok ? "" : err.c_str());
    }
    // アニメーションを持てない形式 / 単位・軸を変換する形式: レストポーズの大きさと位置が保たれるか
    for (const char* ext : { ".obj", ".x", ".3mf", ".3ds" }) {
        std::string out = FileUtil::FromPath(tmpDir / (std::string("selftest_static") + ext));
        ExportOptions eo;
        eo.verifyAfterExport = false;
        bool ok = ExportScene(base, out, eo, err);
        Scene re;
        ImportOptions io2;
        io2.calcTangents = false;
        ok = ok && ImportModel(out, io2, re, err);
        glm::vec3 a0, a1, b0, b1;
        base.ComputeBounds(base.ComputeWorldMatrices(), a0, a1);
        re.ComputeBounds(re.ComputeWorldMatrices(), b0, b1);
        float e = std::max(glm::length(b0 - a0), glm::length(b1 - a1));
        bool pass = ok && e <= size * 1e-3f && re.TotalVertices() > 0;
        if (!pass) ++failed;
        printf("[%s] 書き出し→再読み込み %-6s (レスト) AABB誤差=%.3g  頂点 %zu→%zu %s\n", pass ? "PASS" : "FAIL", ext, e, base.TotalVertices(), re.TotalVertices(),
               ok ? "" : err.c_str());
    }
    // オブジェクトのスケール: 焼き込み / 親を残す の両方で書き出し → 再読み込みし、アニメーション中の複数時刻で比較
    for (int mode = 0; mode < 2; ++mode) {
        std::string out = FileUtil::FromPath(tmpDir / (std::string("selftest_obj") + std::to_string(mode) + ".glb"));
        ExportOptions eo;
        eo.verifyAfterExport = false;
        eo.objectScaleMode = mode;
        bool ok = ExportScene(parentScaled, out, eo, err);
        Scene re;
        ImportOptions io2;
        io2.calcTangents = false;
        ok = ok && ImportModel(out, io2, re, err);
        float e = 0;
        for (float k : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
            float t = parentScaled.animations.empty() ? 0.0f : parentScaled.animations[0].duration * k;
            glm::vec3 a0, a1, b0, b1;
            parentScaled.ComputeBounds(parentScaled.ComputeWorldMatrices(PoseAt(parentScaled, t)), a0, a1);
            re.ComputeBounds(re.ComputeWorldMatrices(PoseAt(re, t)), b0, b1);
            e = std::max(e, std::max(glm::length(b0 - a0), glm::length(b1 - a1)));
        }
        bool pass = ok && e <= size * objScale * 1e-3f;
        if (!pass) ++failed;
        printf("[%s] 書き出し→再読み込み オブジェクト x1.7 (%s) AABB誤差=%.3g %s\n", pass ? "PASS" : "FAIL", mode == 0 ? "焼き込み" : "親を残す", e,
               ok ? "" : err.c_str());
    }
    printf("結果: %s (失敗 %d)\n", failed ? "FAIL" : "PASS", failed);
    exitCode = failed ? 4 : 0;
    return true;
}
