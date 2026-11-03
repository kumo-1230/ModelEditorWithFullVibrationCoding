#include "SceneOps.h"
#include "Log.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace Ops {

static bool IsUniformScale(const glm::vec3& s, float eps = 1e-4f) {
    float m = std::max(std::abs(s.x), std::max(std::abs(s.y), std::abs(s.z)));
    return std::abs(s.x - s.y) <= eps * std::max(1.0f, m) && std::abs(s.y - s.z) <= eps * std::max(1.0f, m);
}

void TransformPrimitive(Primitive& prim, const glm::mat4& m) {
    glm::mat3 lin(m);
    glm::mat3 nrm = glm::inverseTranspose(lin);
    for (auto& p : prim.positions) p = glm::vec3(m * glm::vec4(p, 1.0f));
    for (auto& n : prim.normals) {
        glm::vec3 v = nrm * n;
        float l = glm::length(v);
        n = l > 0 ? v / l : glm::vec3(0, 1, 0);
    }
    for (auto& t : prim.tangents) {
        glm::vec3 v = lin * glm::vec3(t);
        float l = glm::length(v);
        if (l > 0) v /= l;
        t = glm::vec4(v, t.w);
    }
    for (auto& tg : prim.targets) {
        for (auto& d : tg.dPositions) d = lin * d;
        for (auto& d : tg.dNormals) d = nrm * d;
    }
    if (glm::determinant(lin) < 0.0f) ReverseWinding(prim);
}

void ResetTransform(Node& node, bool t, bool r, bool s) {
    if (t) node.translation = glm::vec3(0);
    if (r) node.rotation = glm::quat(1, 0, 0, 0);
    if (s) node.scale = glm::vec3(1);
}

bool NodeHasAnimation(const Scene& scene, int node) {
    for (auto& a : scene.animations)
        for (auto& c : a.channels)
            if (c.node == node && !c.times.empty()) return true;
    return false;
}

void OffsetNodeTRS(Scene& scene, int nodeIdx, const glm::vec3& dT, const glm::quat& dR, const glm::vec3& sMul, bool includeRest) {
    if (nodeIdx < 0 || nodeIdx >= (int)scene.nodes.size()) return;
    const bool hasT = glm::length(dT) > 0.0f;
    const bool hasR = std::abs(std::abs(dR.w) - 1.0f) > 1e-7f;
    const bool hasS = sMul != glm::vec3(1.0f);
    if (includeRest) {
        Node& n = scene.nodes[nodeIdx];
        if (hasT) n.translation += dT;
        if (hasR) n.rotation = glm::normalize(dR * n.rotation);
        if (hasS) n.scale *= sMul;
    }
    for (auto& a : scene.animations)
        for (auto& ch : a.channels) {
            if (ch.node != nodeIdx) continue;
            for (auto& v : ch.values) {
                if (ch.path == AnimPath::Translation && hasT) {
                    v = glm::vec4(glm::vec3(v) + dT, 0.0f);
                } else if (ch.path == AnimPath::Rotation && hasR) {
                    glm::quat q = glm::normalize(dR * glm::quat(v.w, v.x, v.y, v.z));
                    v = glm::vec4(q.x, q.y, q.z, q.w);
                } else if (ch.path == AnimPath::Scale && hasS) {
                    v = glm::vec4(glm::vec3(v) * sMul, 0.0f);
                }
            }
            // 曲線の接線: 移動の足し算は傾きを変えない。回転・拡大は値と同じ掛け算
            for (auto* tans : { &ch.inTangents, &ch.outTangents })
                for (auto& d : *tans) {
                    if (ch.path == AnimPath::Rotation && hasR) {
                        glm::quat q = dR * glm::quat(d.w, d.x, d.y, d.z);
                        d = glm::vec4(q.x, q.y, q.z, q.w);
                    } else if (ch.path == AnimPath::Scale && hasS) {
                        d = glm::vec4(glm::vec3(d) * sMul, 0.0f);
                    }
                }
        }
}

int MakeMeshUnique(Scene& scene, int node) {
    if (node < 0 || node >= (int)scene.nodes.size()) return -1;
    int mesh = scene.nodes[node].mesh;
    if (mesh < 0) return -1;
    if (scene.NodesUsingMesh(mesh).size() <= 1) return mesh;
    Mesh copy = scene.meshes[mesh];
    copy.Touch();
    copy.name += "_unique";
    scene.meshes.push_back(std::move(copy));
    scene.nodes[node].mesh = (int)scene.meshes.size() - 1;
    LOG_INFO("共有メッシュを複製しました: %s", scene.meshes.back().name.c_str());
    return scene.nodes[node].mesh;
}

// 子ノードのローカル変換 (とそのアニメーションキー) に左から X を掛ける
static void PremultiplyChildTransform(Scene& scene, int child, const glm::mat4& X) {
    Node& c = scene.nodes[child];
    c.SetLocalMatrix(X * c.LocalMatrix());

    // アニメーションキーの補正: X = T * R * uniform(u) の場合のみ正確
    glm::vec3 xt, xs;
    glm::quat xr;
    DecomposeTRS(X, xt, xr, xs);
    bool uniform = IsUniformScale(xs);
    float u = xs.x;
    bool warned = false;
    for (auto& anim : scene.animations) {
        for (auto& ch : anim.channels) {
            if (ch.node != child) continue;
            if (!uniform && !warned) {
                LOG_WARN("ノード '%s' はアニメーションを持ちますが非均等スケールのため完全には補正できません", c.name.c_str());
                warned = true;
            }
            for (auto& v : ch.values) {
                if (ch.path == AnimPath::Translation) {
                    glm::vec3 t = xt + (xr * (glm::vec3(v) * u));
                    v = glm::vec4(t, 0);
                } else if (ch.path == AnimPath::Rotation) {
                    glm::quat q = xr * glm::quat(v.w, v.x, v.y, v.z);
                    v = glm::vec4(q.x, q.y, q.z, q.w);
                } else {
                    v = glm::vec4(glm::vec3(v) * u, 0);
                }
            }
            // 曲線の接線 (傾き) には移動 xt は効かない
            for (auto* tans : { &ch.inTangents, &ch.outTangents })
                for (auto& d : *tans) {
                    if (ch.path == AnimPath::Translation) {
                        d = glm::vec4(xr * (glm::vec3(d) * u), 0);
                    } else if (ch.path == AnimPath::Rotation) {
                        glm::quat q = xr * glm::quat(d.w, d.x, d.y, d.z);
                        d = glm::vec4(q.x, q.y, q.z, q.w);
                    } else {
                        d = glm::vec4(glm::vec3(d) * u, 0);
                    }
                }
        }
    }
}

bool FreezeTransform(Scene& scene, int nodeIdx, bool t, bool r, bool s, std::string* err) {
    if (nodeIdx < 0 || nodeIdx >= (int)scene.nodes.size()) return false;
    auto joints = scene.JointMask();
    Node& node = scene.nodes[nodeIdx];
    if (joints[nodeIdx]) {
        if (err) *err = "'" + node.name + "' はジョイントです。ジョイントには「スケールを階層に適用」を使用してください。";
        return false;
    }
    for (auto& anim : scene.animations)
        for (auto& ch : anim.channels)
            if (ch.node == nodeIdx) {
                if (err) *err = "'" + node.name + "' 自身がアニメーションしているためフリーズできません。";
                return false;
            }

    glm::mat4 L = node.LocalMatrix();
    Node target = node;
    ResetTransform(target, t, r, s);
    glm::mat4 Lnew = target.LocalMatrix();
    glm::mat4 X = glm::inverse(Lnew) * L; // 焼き込む行列
    if (glm::all(glm::epsilonEqual(X[0], glm::vec4(1, 0, 0, 0), 1e-6f)) &&
        glm::all(glm::epsilonEqual(X[1], glm::vec4(0, 1, 0, 0), 1e-6f)) &&
        glm::all(glm::epsilonEqual(X[2], glm::vec4(0, 0, 1, 0), 1e-6f)) &&
        glm::all(glm::epsilonEqual(X[3], glm::vec4(0, 0, 0, 1), 1e-6f)))
        return true; // 既に単位行列

    // メッシュに焼き込み (スキンメッシュはジョイントで変形されるのでノード変換は無視される)
    if (node.mesh >= 0) {
        bool skinned = node.skin >= 0 && scene.meshes[node.mesh].HasSkin();
        if (!skinned) {
            int m = MakeMeshUnique(scene, nodeIdx);
            for (auto& p : scene.meshes[m].primitives) TransformPrimitive(p, X);
            scene.meshes[m].Touch();
        }
    }
    for (int c : scene.nodes[nodeIdx].children) PremultiplyChildTransform(scene, c, X);
    scene.nodes[nodeIdx].translation = target.translation;
    scene.nodes[nodeIdx].rotation = target.rotation;
    scene.nodes[nodeIdx].scale = target.scale;
    return true;
}

int FreezeHierarchy(Scene& scene, int node, bool t, bool r, bool s, std::string* err) {
    int count = 0;
    std::function<void(int)> rec = [&](int i) {
        std::string e;
        if (FreezeTransform(scene, i, t, r, s, &e)) ++count;
        else if (!e.empty()) LOG_WARN("%s", e.c_str());
        auto children = scene.nodes[i].children;
        for (int c : children) rec(c);
    };
    rec(node);
    if (err) err->clear();
    return count;
}

bool ApplyUniformScaleToHierarchy(Scene& scene, int nodeIdx, std::string* err) {
    if (nodeIdx < 0 || nodeIdx >= (int)scene.nodes.size()) return false;
    Node& node = scene.nodes[nodeIdx];
    if (!IsUniformScale(node.scale)) {
        if (err) *err = "非均等スケールです。通常のフリーズを使用してください。";
        return false;
    }
    float f = node.scale.x;
    if (std::abs(f) < 1e-12f) {
        if (err) *err = "スケールが 0 のため適用できません。";
        return false;
    }
    for (auto& anim : scene.animations)
        for (auto& ch : anim.channels)
            if (ch.node == nodeIdx && ch.path == AnimPath::Scale) {
                if (err) *err = "このノードはスケールアニメーションを持つため適用できません。";
                return false;
            }

    // 子孫を集める
    std::vector<int> desc;
    std::function<void(int)> collect = [&](int i) {
        for (int c : scene.nodes[i].children) { desc.push_back(c); collect(c); }
    };
    collect(nodeIdx);
    std::vector<bool> inSub(scene.nodes.size(), false);
    inSub[nodeIdx] = true;
    for (int d : desc) inSub[d] = true;

    // 子孫の移動量とアニメーションの移動キーをスケール
    for (int d : desc) scene.nodes[d].translation *= f;
    for (auto& anim : scene.animations)
        for (auto& ch : anim.channels)
            if (ch.node >= 0 && ch.node != nodeIdx && inSub[ch.node] && ch.path == AnimPath::Translation)
                for (auto* arr : { &ch.values, &ch.inTangents, &ch.outTangents }) for (auto& v : *arr) v = glm::vec4(glm::vec3(v) * f, 0); // 値と曲線の接線

    // 非スキンメッシュ (ノード自身と子孫) の頂点をスケール
    glm::mat4 S = glm::scale(glm::mat4(1), glm::vec3(f));
    std::set<int> scaledMeshes;
    auto scaleMeshOf = [&](int n) {
        Node& nd = scene.nodes[n];
        if (nd.mesh < 0) return;
        if (nd.skin >= 0 && scene.meshes[nd.mesh].HasSkin()) return;
        int m = nd.mesh;
        // 部分木の外でも使われているなら複製
        for (int user : scene.NodesUsingMesh(m))
            if (!inSub[user]) { m = MakeMeshUnique(scene, n); break; }
        if (scaledMeshes.count(m)) return;
        scaledMeshes.insert(m);
        for (auto& p : scene.meshes[m].primitives) TransformPrimitive(p, S);
        scene.meshes[m].Touch();
    };
    scaleMeshOf(nodeIdx);
    for (int d : desc) scaleMeshOf(d);

    // 部分木内のジョイントを参照するスキンの逆バインド行列: IBM' = S * IBM
    for (auto& skin : scene.skins)
        for (size_t j = 0; j < skin.joints.size(); ++j) {
            int jn = skin.joints[j];
            if (jn >= 0 && jn != nodeIdx && inSub[jn] && j < skin.inverseBind.size())
                skin.inverseBind[j] = S * skin.inverseBind[j];
            else if (jn == nodeIdx && j < skin.inverseBind.size())
                skin.inverseBind[j] = S * skin.inverseBind[j];
        }

    scene.nodes[nodeIdx].scale = glm::vec3(1.0f);
    LOG_INFO("スケール %.6g を '%s' の階層に適用しました", f, scene.nodes[nodeIdx].name.c_str());
    return true;
}

static int TopRoot(const Scene& scene, int n) {
    while (n >= 0 && scene.nodes[n].parent >= 0) n = scene.nodes[n].parent;
    return n;
}

int FindObjectNode(const Scene& scene, int node) {
    if (node < 0 || node >= (int)scene.nodes.size()) return -1;
    for (int p = node; p >= 0; p = scene.nodes[p].parent)
        if (scene.nodes[p].objectRoot) return p;
    int r = TopRoot(scene, node);
    return NodeHasAnimation(scene, r) ? -1 : r;
}

int EnsureObjectNode(Scene& scene, int node) {
    int o = FindObjectNode(scene, node);
    if (o >= 0) { scene.nodes[o].objectRoot = true; return o; }
    if (node < 0 || node >= (int)scene.nodes.size()) return -1;
    // 最上位ルートがアニメーションしている (ルートモーション等) → アニメーションを持たない親を作る
    int r = TopRoot(scene, node);
    size_t pos = std::find(scene.roots.begin(), scene.roots.end(), r) - scene.roots.begin();
    int w = scene.AddNode(scene.nodes[r].name + "_Object", -1);
    scene.Reparent(r, w, false);
    scene.roots.erase(std::remove(scene.roots.begin(), scene.roots.end(), w), scene.roots.end());
    scene.roots.insert(scene.roots.begin() + std::min(pos, scene.roots.size()), w); // ルートの並び順を保つ
    scene.nodes[w].objectRoot = true;
    scene.nodes[w].objectWrapper = true;
    LOG_INFO("'%s' 用のオブジェクトノード '%s' を作成しました", scene.nodes[r].name.c_str(), scene.nodes[w].name.c_str());
    return w;
}

int BakeObjectScales(Scene& scene, bool dissolveWrappers) {
    int baked = 0;
    std::vector<int> wrappers;
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        if (!scene.nodes[i].objectRoot) continue;
        if (NodeHasAnimation(scene, i)) {
            LOG_WARN("オブジェクト '%s' はアニメーションを持つため、親ノードとして残します", scene.nodes[i].name.c_str());
            continue;
        }
        if (scene.nodes[i].scale != glm::vec3(1.0f)) {
            if (!IsUniformScale(scene.nodes[i].scale)) {
                LOG_WARN("オブジェクト '%s' は非均等スケールのため、焼き込まずに親ノードとして残します", scene.nodes[i].name.c_str());
                continue;
            }
            std::string e;
            if (!ApplyUniformScaleToHierarchy(scene, i, &e)) { LOG_WARN("%s", e.c_str()); continue; }
            ++baked;
        }
        if (dissolveWrappers && scene.nodes[i].objectWrapper && scene.nodes[i].mesh < 0) wrappers.push_back(i);
    }
    // 番号がずれないよう後ろから取り除く
    std::sort(wrappers.rbegin(), wrappers.rend());
    for (int w : wrappers) {
        glm::mat4 L = scene.nodes[w].LocalMatrix(); // 移動 + 回転のみ (スケールは焼き込み済み)
        int parent = scene.nodes[w].parent;
        auto children = scene.nodes[w].children;
        for (int c : children) {
            PremultiplyChildTransform(scene, c, L);
            scene.Reparent(c, parent, false);
        }
        scene.RemoveNodes({ w });
    }
    return baked;
}

// ジョイントのワールド位置と、メッシュ頂点 (stride 個ごと) のワールド位置
static void CollectWorldPoints(const Scene& s, const std::vector<NodePose>& pose, size_t stride, std::vector<glm::vec3>& out) {
    out.clear();
    auto world = s.ComputeWorldMatrices(pose);
    for (auto& skin : s.skins)
        for (int j : skin.joints) out.push_back(j >= 0 && j < (int)world.size() ? glm::vec3(world[j][3]) : glm::vec3(0.0f));
    for (int i = 0; i < (int)s.nodes.size(); ++i) {
        const Node& n = s.nodes[i];
        if (n.mesh < 0) continue;
        const Mesh& m = s.meshes[n.mesh];
        const Skin* skin = (n.skin >= 0 && m.HasSkin()) ? &s.skins[n.skin] : nullptr;
        for (auto& p : m.primitives)
            for (size_t v = 0; v < p.positions.size(); v += stride) {
                glm::mat4 M = world[i];
                if (skin && p.HasSkin()) {
                    glm::mat4 acc(0.0f);
                    float sum = 0;
                    for (int k = 0; k < 4; ++k) {
                        float w = p.weights[v][k];
                        int j = p.joints[v][k];
                        if (w <= 0 || j >= (int)skin->joints.size() || skin->joints[j] < 0) continue;
                        acc += world[skin->joints[j]] * skin->inverseBind[j] * w;
                        sum += w;
                    }
                    if (sum > 0) M = acc;
                }
                out.push_back(glm::vec3(M * glm::vec4(p.positions[v], 1.0f)));
            }
    }
}

float MaxMotionError(const Scene& a, const Scene& b, float fps, int* frames) {
    if (frames) *frames = 0;
    if (a.animations.size() != b.animations.size() || a.skins.size() != b.skins.size()) return -1.0f;
    size_t verts = 0;
    for (auto& n : a.nodes) if (n.mesh >= 0) verts += a.meshes[n.mesh].VertexCount();
    const size_t stride = std::max<size_t>(1, verts / 20000);
    std::vector<glm::vec3> pa, pb;
    float err = 0.0f;
    int count = 0;
    auto compare = [&](int anim, float t) {
        auto poseA = a.RestPose(), poseB = b.RestPose();
        if (anim >= 0) {
            SampleAnimation(a, a.animations[anim], t, poseA);
            SampleAnimation(b, b.animations[anim], t, poseB);
        }
        CollectWorldPoints(a, poseA, stride, pa);
        CollectWorldPoints(b, poseB, stride, pb);
        if (pa.size() != pb.size()) return false;
        for (size_t i = 0; i < pa.size(); ++i) err = std::max(err, glm::length(pa[i] - pb[i]));
        ++count;
        return true;
    };
    if (!compare(-1, 0.0f)) return -1.0f;
    for (int ai = 0; ai < (int)a.animations.size(); ++ai) {
        float dur = std::max(a.animations[ai].duration, b.animations[ai].duration);
        int n = std::clamp((int)std::ceil(dur * fps), 0, 599) + 1;
        for (int k = 0; k < n; ++k)
            if (!compare(ai, n > 1 ? dur * k / (n - 1) : 0.0f)) return -1.0f;
    }
    if (frames) *frames = count;
    return err;
}

void ScaleScene(Scene& scene, float f) {
    for (auto& n : scene.nodes) n.translation *= f;
    for (auto& m : scene.meshes) {
        for (auto& p : m.primitives) {
            for (auto& v : p.positions) v *= f;
            for (auto& t : p.targets) for (auto& d : t.dPositions) d *= f;
        }
        m.Touch();
    }
    for (auto& s : scene.skins)
        for (auto& ibm : s.inverseBind) ibm[3] = glm::vec4(glm::vec3(ibm[3]) * f, 1.0f);
    for (auto& a : scene.animations)
        for (auto& ch : a.channels)
            if (ch.path == AnimPath::Translation)
                for (auto* arr : { &ch.values, &ch.inTangents, &ch.outTangents }) for (auto& v : *arr) v = glm::vec4(glm::vec3(v) * f, 0); // 値と曲線の接線
    LOG_INFO("シーン全体を %.6g 倍しました", f);
}

void TransformSceneRoots(Scene& scene, const glm::mat4& M) {
    glm::vec3 mt, ms;
    glm::quat mr;
    DecomposeTRS(M, mt, mr, ms);
    glm::mat4 Minv = glm::inverse(M);
    scene.RebuildRoots();
    for (int r : scene.roots) PremultiplyChildTransform(scene, r, M);
    // スキンメッシュ: 頂点はバインド空間(=ワールド)にあるので v' = M v, IBM' = IBM * M^-1
    std::set<int> done;
    for (auto& n : scene.nodes) {
        if (n.mesh < 0 || n.skin < 0 || done.count(n.mesh)) continue;
        if (!scene.meshes[n.mesh].HasSkin()) continue;
        done.insert(n.mesh);
        for (auto& p : scene.meshes[n.mesh].primitives) TransformPrimitive(p, M);
        scene.meshes[n.mesh].Touch();
    }
    for (auto& s : scene.skins)
        for (auto& ibm : s.inverseBind) ibm = ibm * Minv;
}

void ConvertUpAxis(Scene& scene, int fromAxis, int toAxis) {
    if (fromAxis == toAxis) return;
    glm::mat4 R(1.0f);
    if (fromAxis == 2 && toAxis == 1) R = glm::rotate(glm::mat4(1), glm::radians(-90.0f), glm::vec3(1, 0, 0));
    else if (fromAxis == 1 && toAxis == 2) R = glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(1, 0, 0));
    else if (fromAxis == 0 && toAxis == 1) R = glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 0, 1));
    else if (fromAxis == 1 && toAxis == 0) R = glm::rotate(glm::mat4(1), glm::radians(-90.0f), glm::vec3(0, 0, 1));
    else if (fromAxis == 0 && toAxis == 2) R = glm::rotate(glm::mat4(1), glm::radians(-90.0f), glm::vec3(0, 1, 0));
    else if (fromAxis == 2 && toAxis == 0) R = glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 1, 0));
    TransformSceneRoots(scene, R);
    static const char* ax[] = { "X", "Y", "Z" };
    LOG_INFO("上方向軸を %s-up から %s-up に変換しました", ax[fromAxis], ax[toAxis]);
}

bool CenterPivot(Scene& scene, int nodeIdx, std::string* err) {
    if (nodeIdx < 0 || nodeIdx >= (int)scene.nodes.size()) return false;
    Node& node = scene.nodes[nodeIdx];
    if (node.mesh < 0) { if (err) *err = "メッシュがありません。"; return false; }
    if (node.skin >= 0) { if (err) *err = "スキンメッシュのピボットは変更できません。"; return false; }
    // アニメーションで回転・拡大するノードは、ピボットを動かすと動きが変わってしまうので対象外。
    // 移動だけのアニメーションなら、移動キーもピボットの移動分ずらす
    bool animT = false;
    for (auto& a : scene.animations)
        for (auto& ch : a.channels)
            if (ch.node == nodeIdx) {
                if (ch.path != AnimPath::Translation) {
                    if (err) *err = "アニメーションで回転・スケールするノードのピボットは変更できません。";
                    return false;
                }
                animT = true;
            }
    glm::vec3 mn(std::numeric_limits<float>::max()), mx(-std::numeric_limits<float>::max());
    for (auto& p : scene.meshes[node.mesh].primitives) {
        glm::vec3 a, b;
        ComputePrimitiveBounds(p, a, b);
        mn = glm::min(mn, a);
        mx = glm::max(mx, b);
    }
    glm::vec3 c = (mn + mx) * 0.5f;
    int m = MakeMeshUnique(scene, nodeIdx);
    glm::mat4 Tm = glm::translate(glm::mat4(1), -c);
    for (auto& p : scene.meshes[m].primitives) TransformPrimitive(p, Tm);
    scene.meshes[m].Touch();
    Node& nd = scene.nodes[nodeIdx];
    // ローカル行列 T R S に右から T(c) を掛ける = 移動に R S c を足す (回転・拡大は変わらない)
    const glm::vec3 offset = nd.rotation * (nd.scale * c);
    nd.translation += offset;
    if (animT)
        for (auto& a : scene.animations)
            for (auto& ch : a.channels)
                if (ch.node == nodeIdx && ch.path == AnimPath::Translation)
                    for (auto& v : ch.values) v = glm::vec4(glm::vec3(v) + offset, 0.0f);
    for (int ch : nd.children) PremultiplyChildTransform(scene, ch, glm::translate(glm::mat4(1), -c));
    return true;
}

void CenterSceneToOrigin(Scene& scene, bool keepOnGround) {
    auto world = scene.ComputeWorldMatrices();
    glm::vec3 mn, mx;
    scene.ComputeBounds(world, mn, mx);
    glm::vec3 c = (mn + mx) * 0.5f;
    if (keepOnGround) c.y = mn.y;
    TransformSceneRoots(scene, glm::translate(glm::mat4(1), -c));
}

void PlaceOnGround(Scene& scene) {
    auto world = scene.ComputeWorldMatrices();
    glm::vec3 mn, mx;
    scene.ComputeBounds(world, mn, mx);
    TransformSceneRoots(scene, glm::translate(glm::mat4(1), glm::vec3(0, -mn.y, 0)));
}

// ---------------------------------------------------------------------------
void ComputePrimitiveBounds(const Primitive& prim, glm::vec3& mn, glm::vec3& mx) {
    mn = glm::vec3(std::numeric_limits<float>::max());
    mx = glm::vec3(-std::numeric_limits<float>::max());
    for (auto& p : prim.positions) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
    if (prim.positions.empty()) { mn = mx = glm::vec3(0); }
}

void ReverseWinding(Primitive& prim) {
    for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) std::swap(prim.indices[i + 1], prim.indices[i + 2]);
}

void FlipNormals(Primitive& prim) {
    for (auto& n : prim.normals) n = -n;
    for (auto& t : prim.targets) for (auto& d : t.dNormals) d = -d;
    ReverseWinding(prim);
}

void FlipUV(Primitive& prim, bool u, bool v) {
    for (auto* set : { &prim.uv0, &prim.uv1 })
        for (auto& t : *set) {
            if (u) t.x = 1.0f - t.x;
            if (v) t.y = 1.0f - t.y;
        }
}

// 頂点を複製するヘルパー (全属性)
static uint32_t CopyVertex(const Primitive& src, size_t i, Primitive& dst) {
    dst.positions.push_back(src.positions[i]);
    if (src.HasNormals()) dst.normals.push_back(src.normals[i]);
    if (src.HasTangents()) dst.tangents.push_back(src.tangents[i]);
    if (src.HasUV0()) dst.uv0.push_back(src.uv0[i]);
    if (src.HasUV1()) dst.uv1.push_back(src.uv1[i]);
    if (src.HasColors()) dst.colors.push_back(src.colors[i]);
    if (src.HasSkin()) { dst.joints.push_back(src.joints[i]); dst.weights.push_back(src.weights[i]); }
    for (size_t t = 0; t < src.targets.size(); ++t) {
        if (dst.targets.size() <= t) { dst.targets.push_back({}); dst.targets[t].name = src.targets[t].name; }
        if (i < src.targets[t].dPositions.size()) dst.targets[t].dPositions.push_back(src.targets[t].dPositions[i]);
        if (i < src.targets[t].dNormals.size()) dst.targets[t].dNormals.push_back(src.targets[t].dNormals[i]);
    }
    return (uint32_t)dst.positions.size() - 1;
}

static Primitive EmptyLike(const Primitive& src) {
    Primitive p;
    p.material = src.material;
    p.droppedInfluences = src.droppedInfluences;
    for (auto& t : src.targets) { MorphTarget mt; mt.name = t.name; p.targets.push_back(mt); }
    return p;
}

void RecalculateNormals(Primitive& prim, bool smooth, float angleDeg) {
    const size_t triCount = prim.indices.size() / 3;
    if (triCount == 0) return;
    std::vector<glm::vec3> faceN(triCount);
    for (size_t f = 0; f < triCount; ++f) {
        const glm::vec3& a = prim.positions[prim.indices[f * 3 + 0]];
        const glm::vec3& b = prim.positions[prim.indices[f * 3 + 1]];
        const glm::vec3& c = prim.positions[prim.indices[f * 3 + 2]];
        faceN[f] = glm::cross(b - a, c - a); // 面積で重み付け
    }

    if (!smooth) {
        // フラット: 三角形ごとに頂点を分離
        Primitive out = EmptyLike(prim);
        out.normals.clear();
        for (size_t f = 0; f < triCount; ++f) {
            glm::vec3 n = glm::length(faceN[f]) > 0 ? glm::normalize(faceN[f]) : glm::vec3(0, 1, 0);
            for (int k = 0; k < 3; ++k) {
                uint32_t v = CopyVertex(prim, prim.indices[f * 3 + k], out);
                if (out.normals.size() < out.positions.size()) out.normals.resize(out.positions.size());
                out.normals[v] = n;
                out.indices.push_back(v);
            }
        }
        out.normals.resize(out.positions.size());
        prim = std::move(out);
        return;
    }

    // 位置で頂点をグループ化 (UV シームを跨いで滑らかにする)
    auto key = [](const glm::vec3& p) {
        const float q = 1e5f;
        return std::tuple<long long, long long, long long>((long long)std::llround(p.x * q), (long long)std::llround(p.y * q), (long long)std::llround(p.z * q));
    };
    std::map<std::tuple<long long, long long, long long>, std::vector<size_t>> posToFaces;
    for (size_t f = 0; f < triCount; ++f)
        for (int k = 0; k < 3; ++k) posToFaces[key(prim.positions[prim.indices[f * 3 + k]])].push_back(f);

    const float cosThr = std::cos(glm::radians(std::clamp(angleDeg, 0.0f, 180.0f)));
    const bool useAngle = angleDeg < 179.9f;

    // コーナーごとの法線
    std::vector<glm::vec3> cornerN(prim.indices.size());
    for (size_t f = 0; f < triCount; ++f) {
        glm::vec3 fn = glm::length(faceN[f]) > 0 ? glm::normalize(faceN[f]) : glm::vec3(0);
        for (int k = 0; k < 3; ++k) {
            const auto& faces = posToFaces[key(prim.positions[prim.indices[f * 3 + k]])];
            glm::vec3 sum(0);
            for (size_t g : faces) {
                if (useAngle) {
                    glm::vec3 gn = glm::length(faceN[g]) > 0 ? glm::normalize(faceN[g]) : glm::vec3(0);
                    if (glm::dot(fn, gn) < cosThr) continue;
                }
                sum += faceN[g];
            }
            if (glm::length(sum) <= 0) sum = fn;
            cornerN[f * 3 + k] = glm::length(sum) > 0 ? glm::normalize(sum) : glm::vec3(0, 1, 0);
        }
    }

    // 同じ頂点で法線が異なるコーナーは頂点を分割する
    Primitive out = EmptyLike(prim);
    std::map<std::pair<uint32_t, std::tuple<int, int, int>>, uint32_t> remap;
    for (size_t c = 0; c < prim.indices.size(); ++c) {
        uint32_t vi = prim.indices[c];
        const glm::vec3& n = cornerN[c];
        auto nk = std::make_tuple((int)std::lround(n.x * 1000), (int)std::lround(n.y * 1000), (int)std::lround(n.z * 1000));
        auto k = std::make_pair(vi, nk);
        auto it = remap.find(k);
        uint32_t ni;
        if (it == remap.end()) {
            ni = CopyVertex(prim, vi, out);
            out.normals.resize(out.positions.size());
            out.normals[ni] = n;
            remap[k] = ni;
        } else {
            ni = it->second;
        }
        out.indices.push_back(ni);
    }
    prim = std::move(out);
}

bool GenerateTangents(Primitive& prim) {
    if (!prim.HasUV0() || !prim.HasNormals()) return false;
    size_t n = prim.positions.size();
    std::vector<glm::vec3> tan(n, glm::vec3(0)), bit(n, glm::vec3(0));
    for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
        uint32_t i0 = prim.indices[i], i1 = prim.indices[i + 1], i2 = prim.indices[i + 2];
        glm::vec3 e1 = prim.positions[i1] - prim.positions[i0];
        glm::vec3 e2 = prim.positions[i2] - prim.positions[i0];
        glm::vec2 d1 = prim.uv0[i1] - prim.uv0[i0];
        glm::vec2 d2 = prim.uv0[i2] - prim.uv0[i0];
        float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) continue;
        float r = 1.0f / det;
        glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        glm::vec3 b = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t v : { i0, i1, i2 }) { tan[v] += t; bit[v] += b; }
    }
    prim.tangents.resize(n);
    for (size_t v = 0; v < n; ++v) {
        glm::vec3 nn = prim.normals[v];
        glm::vec3 t = tan[v] - nn * glm::dot(nn, tan[v]); // グラム・シュミット
        if (glm::length(t) < 1e-8f) {
            t = std::abs(nn.x) < 0.9f ? glm::cross(nn, glm::vec3(1, 0, 0)) : glm::cross(nn, glm::vec3(0, 1, 0));
        }
        t = glm::normalize(t);
        // UV は左上原点なので dP/dv は画像の「下」向き。法線マップの +Y(上) はその逆
        float w = glm::dot(glm::cross(nn, t), bit[v]) < 0.0f ? 1.0f : -1.0f;
        prim.tangents[v] = glm::vec4(t, w);
    }
    return true;
}

int WeldVertices(Primitive& prim, float eps) {
    if (prim.positions.empty()) return 0;
    const float q = eps > 0 ? 1.0f / eps : 1e6f;
    auto qf = [&](float f) { return (long long)std::llround(f * q); };
    std::unordered_map<std::string, uint32_t> map;
    Primitive out = EmptyLike(prim);
    std::vector<uint32_t> remap(prim.positions.size());
    std::string k;
    for (size_t i = 0; i < prim.positions.size(); ++i) {
        k.clear();
        auto add = [&](const float* f, int cnt) {
            for (int c = 0; c < cnt; ++c) { long long v = qf(f[c]); k.append((const char*)&v, sizeof(v)); }
        };
        add(&prim.positions[i].x, 3);
        if (prim.HasNormals()) add(&prim.normals[i].x, 3);
        if (prim.HasTangents()) add(&prim.tangents[i].x, 4);
        if (prim.HasUV0()) add(&prim.uv0[i].x, 2);
        if (prim.HasUV1()) add(&prim.uv1[i].x, 2);
        if (prim.HasColors()) add(&prim.colors[i].x, 4);
        if (prim.HasSkin()) {
            k.append((const char*)&prim.joints[i], sizeof(glm::u16vec4));
            add(&prim.weights[i].x, 4);
        }
        for (auto& t : prim.targets) if (i < t.dPositions.size()) add(&t.dPositions[i].x, 3);
        auto it = map.find(k);
        if (it == map.end()) {
            uint32_t ni = CopyVertex(prim, i, out);
            map.emplace(k, ni);
            remap[i] = ni;
        } else {
            remap[i] = it->second;
        }
    }
    int removed = (int)(prim.positions.size() - out.positions.size());
    for (uint32_t idx : prim.indices) out.indices.push_back(remap[idx]);
    prim = std::move(out);
    return removed;
}

int RemoveDegenerateTriangles(Primitive& prim) {
    std::vector<uint32_t> out;
    int removed = 0;
    for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
        uint32_t a = prim.indices[i], b = prim.indices[i + 1], c = prim.indices[i + 2];
        bool degenerate = a == b || b == c || a == c;
        if (!degenerate) {
            glm::vec3 cr = glm::cross(prim.positions[b] - prim.positions[a], prim.positions[c] - prim.positions[a]);
            degenerate = glm::dot(cr, cr) < 1e-24f;
        }
        if (degenerate) { ++removed; continue; }
        out.insert(out.end(), { a, b, c });
    }
    prim.indices = std::move(out);
    return removed;
}

int NormalizeWeights(Primitive& prim) {
    int fixedCount = 0;
    for (auto& w : prim.weights) {
        float s = w.x + w.y + w.z + w.w;
        if (std::abs(s - 1.0f) > 1e-4f) {
            if (s > 0) w /= s;
            else w = glm::vec4(1, 0, 0, 0);
            ++fixedCount;
        }
    }
    return fixedCount;
}

int PruneWeights(Primitive& prim, float threshold) {
    int pruned = 0;
    for (size_t i = 0; i < prim.weights.size(); ++i) {
        glm::vec4& w = prim.weights[i];
        for (int k = 0; k < 4; ++k)
            if (w[k] > 0 && w[k] < threshold) { w[k] = 0; prim.joints[i][k] = 0; ++pruned; }
    }
    NormalizeWeights(prim);
    return pruned;
}

// ---------------------------------------------------------------------------
int RemoveUnusedResources(Scene& scene) {
    int removed = 0;
    // メッシュ
    for (int m = (int)scene.meshes.size() - 1; m >= 0; --m)
        if (scene.NodesUsingMesh(m).empty()) { scene.RemoveMesh(m); ++removed; }
    // マテリアル
    for (int mat = (int)scene.materials.size() - 1; mat >= 0; --mat) {
        bool used = false;
        for (auto& m : scene.meshes) for (auto& p : m.primitives) if (p.material == mat) used = true;
        if (!used) { scene.RemoveMaterial(mat); ++removed; }
    }
    // テクスチャ
    for (int t = (int)scene.textures.size() - 1; t >= 0; --t) {
        bool used = false;
        for (auto& m : scene.materials) for (auto& s : m.slots) if (s.texture == t) used = true;
        if (!used) { scene.RemoveTexture(t); ++removed; }
    }
    // スキン
    for (int s = (int)scene.skins.size() - 1; s >= 0; --s) {
        bool used = false;
        for (auto& n : scene.nodes) if (n.skin == s) used = true;
        if (!used) {
            scene.skins.erase(scene.skins.begin() + s);
            for (auto& n : scene.nodes) if (n.skin > s) n.skin--;
            ++removed;
        }
    }
    return removed;
}

int RemoveEmptyNodes(Scene& scene) {
    int total = 0;
    while (true) {
        auto joints = scene.JointMask();
        std::vector<bool> animated(scene.nodes.size(), false);
        for (auto& a : scene.animations) for (auto& c : a.channels) if (c.node >= 0) animated[c.node] = true;
        std::vector<int> rm;
        for (int i = 0; i < (int)scene.nodes.size(); ++i) {
            const Node& n = scene.nodes[i];
            bool isSkeletonRoot = false;
            for (auto& s : scene.skins) if (s.skeleton == i) isSkeletonRoot = true;
            if (n.mesh < 0 && n.children.empty() && !joints[i] && !animated[i] && !isSkeletonRoot) rm.push_back(i);
        }
        if (rm.empty()) break;
        total += (int)rm.size();
        scene.RemoveNodes(rm);
    }
    return total;
}

int UniquifyNames(Scene& scene) {
    int changed = 0;
    std::map<std::string, int> count;
    std::set<std::string> used;
    for (auto& n : scene.nodes) used.insert(n.name);
    std::set<std::string> seen;
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        Node& n = scene.nodes[i];
        if (n.name.empty()) { n.name = "Node_" + std::to_string(i); ++changed; }
        if (seen.count(n.name)) {
            std::string base = n.name;
            int k = ++count[base];
            std::string candidate;
            do { candidate = base + "_" + std::to_string(k++); } while (used.count(candidate) || seen.count(candidate));
            count[base] = k;
            n.name = candidate;
            used.insert(candidate);
            ++changed;
        }
        seen.insert(n.name);
    }
    return changed;
}

int BatchRename(Scene& scene, const std::string& find, const std::string& replace,
                const std::string& prefix, const std::string& suffix, const std::vector<int>* subset) {
    int changed = 0;
    auto apply = [&](Node& n) {
        std::string name = n.name;
        if (!find.empty()) {
            size_t pos = 0;
            while ((pos = name.find(find, pos)) != std::string::npos) {
                name.replace(pos, find.size(), replace);
                pos += replace.size();
            }
        }
        name = prefix + name + suffix;
        if (name != n.name) { n.name = name; ++changed; }
    };
    if (subset) { for (int i : *subset) if (i >= 0 && i < (int)scene.nodes.size()) apply(scene.nodes[i]); }
    else for (auto& n : scene.nodes) apply(n);
    return changed;
}

int StripNamespaces(Scene& scene) {
    int changed = 0;
    for (auto& n : scene.nodes) {
        size_t p = n.name.rfind(':');
        if (p != std::string::npos && p + 1 < n.name.size()) { n.name = n.name.substr(p + 1); ++changed; }
    }
    return changed;
}

int DuplicateNode(Scene& scene, int node, bool deepCopyMesh) {
    if (node < 0 || node >= (int)scene.nodes.size()) return -1;
    std::function<int(int, int)> copy = [&](int src, int parent) -> int {
        Node n = scene.nodes[src];
        n.children.clear();
        n.parent = parent;
        n.name += parent == scene.nodes[node].parent ? "_copy" : "";
        if (deepCopyMesh && n.mesh >= 0) {
            Mesh m = scene.meshes[n.mesh];
            m.Touch();
            m.name += "_copy";
            scene.meshes.push_back(std::move(m));
            n.mesh = (int)scene.meshes.size() - 1;
        }
        int idx = (int)scene.nodes.size();
        scene.nodes.push_back(n);
        if (parent >= 0) scene.nodes[parent].children.push_back(idx);
        else scene.roots.push_back(idx);
        auto children = scene.nodes[src].children;
        for (int c : children) copy(c, idx);
        return idx;
    };
    return copy(node, scene.nodes[node].parent);
}

int AddMaterial(Scene& scene, const std::string& name) {
    Material m;
    m.name = name;
    scene.materials.push_back(m);
    return (int)scene.materials.size() - 1;
}

// ---------------------------------------------------------------------------
// 基本形状
// ---------------------------------------------------------------------------
const char* PrimitiveName(int type) {
    static const char* n[] = { "Cube", "Sphere", "Cylinder", "Cone", "Plane" };
    return type >= 0 && type < Prim_Count ? n[type] : "Primitive";
}

static Primitive BuildPrimitiveGeometry(int type, float size) {
    Primitive p;
    const float r = size * 0.5f;
    const float PI = 3.14159265358979f;
    auto vert = [&](glm::vec3 pos, glm::vec3 n, glm::vec2 uv) {
        p.positions.push_back(pos);
        p.normals.push_back(glm::normalize(n));
        p.uv0.push_back(uv);
        return (uint32_t)p.positions.size() - 1;
    };
    auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) { p.indices.insert(p.indices.end(), { a, b, c, a, c, d }); };
    switch (type) {
    case Prim_Cube: {
        const glm::vec3 n[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (auto& f : n) {
            glm::vec3 u = std::abs(f.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            glm::vec3 v = glm::cross(f, u);
            uint32_t a = vert((f - u - v) * r, f, { 0, 1 }), b = vert((f + u - v) * r, f, { 1, 1 });
            uint32_t c = vert((f + u + v) * r, f, { 1, 0 }), d = vert((f - u + v) * r, f, { 0, 0 });
            quad(a, b, c, d);
        }
        break;
    }
    case Prim_Sphere: {
        const int seg = 32, rings = 16;
        for (int y = 0; y <= rings; ++y)
            for (int x = 0; x <= seg; ++x) {
                float th = PI * y / rings, ph = 2 * PI * x / seg;
                glm::vec3 n(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
                vert(n * r, n, { (float)x / seg, (float)y / rings });
            }
        for (int y = 0; y < rings; ++y)
            for (int x = 0; x < seg; ++x) {
                uint32_t a = y * (seg + 1) + x, b = a + 1, c = a + seg + 2, d = a + seg + 1;
                quad(a, b, c, d);
            }
        break;
    }
    case Prim_Cylinder:
    case Prim_Cone: {
        const int seg = 32;
        bool cone = type == Prim_Cone;
        float slope = cone ? r / size : 0.0f; // 側面法線の Y 成分
        for (int x = 0; x <= seg; ++x) {
            float ph = 2 * PI * x / seg;
            glm::vec3 d(std::cos(ph), 0, std::sin(ph));
            glm::vec3 n = glm::normalize(d + glm::vec3(0, slope, 0));
            vert(d * r + glm::vec3(0, -r, 0), n, { (float)x / seg, 1 });
            vert(cone ? glm::vec3(0, r, 0) : d * r + glm::vec3(0, r, 0), n, { (float)x / seg, 0 });
        }
        for (int x = 0; x < seg; ++x) quad(x * 2, x * 2 + 2, x * 2 + 3, x * 2 + 1);
        for (int cap = 0; cap < (cone ? 1 : 2); ++cap) {
            float yy = cap == 0 ? -r : r;
            glm::vec3 n(0, cap == 0 ? -1 : 1, 0);
            uint32_t center = vert({ 0, yy, 0 }, n, { 0.5f, 0.5f });
            uint32_t first = (uint32_t)p.positions.size();
            for (int x = 0; x <= seg; ++x) {
                float ph = 2 * PI * x / seg;
                vert({ std::cos(ph) * r, yy, std::sin(ph) * r }, n, { 0.5f + std::cos(ph) * 0.5f, 0.5f + std::sin(ph) * 0.5f });
            }
            for (int x = 0; x < seg; ++x) p.indices.insert(p.indices.end(), { center, first + x, first + x + 1 });
        }
        break;
    }
    case Prim_Plane: {
        const int div = 10;
        for (int z = 0; z <= div; ++z)
            for (int x = 0; x <= div; ++x)
                vert({ -r + size * x / div, 0, -r + size * z / div }, { 0, 1, 0 }, { (float)x / div, (float)z / div });
        for (int z = 0; z < div; ++z)
            for (int x = 0; x < div; ++x) {
                uint32_t a = z * (div + 1) + x;
                quad(a, a + 1, a + div + 2, a + div + 1);
            }
        break;
    }
    }
    // 巻き順を法線に合わせる (反時計回りが表)
    for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
        uint32_t a = p.indices[i], b = p.indices[i + 1], c = p.indices[i + 2];
        glm::vec3 fn = glm::cross(p.positions[b] - p.positions[a], p.positions[c] - p.positions[a]);
        glm::vec3 vn = p.normals[a] + p.normals[b] + p.normals[c];
        if (glm::dot(fn, vn) < 0) std::swap(p.indices[i + 1], p.indices[i + 2]);
    }
    GenerateTangents(p);
    return p;
}

int CreatePrimitive(Scene& scene, int type, float size, int parent) {
    // 既定マテリアル (なければ作成)
    int mat = -1;
    for (int i = 0; i < (int)scene.materials.size(); ++i) if (scene.materials[i].name == "Default") { mat = i; break; }
    if (mat < 0) {
        mat = AddMaterial(scene, "Default");
        scene.materials[mat].baseColor = glm::vec4(0.75f, 0.75f, 0.75f, 1.0f);
        scene.materials[mat].roughness = 0.6f;
    }
    Mesh mesh;
    // 名前の番号を決める (Cube1, Cube2, ...)
    int number = 1;
    std::string base = PrimitiveName(type);
    while (scene.FindNode(base + std::to_string(number)) >= 0) ++number;
    mesh.name = base + std::to_string(number) + "Shape";
    mesh.primitives.push_back(BuildPrimitiveGeometry(type, std::max(size, 1e-5f)));
    mesh.primitives[0].material = mat;
    scene.meshes.push_back(std::move(mesh));
    int node = scene.AddNode(base + std::to_string(number), parent);
    scene.nodes[node].mesh = (int)scene.meshes.size() - 1;
    if (type != Prim_Plane) scene.nodes[node].translation.y = size * 0.5f; // 床に乗せる
    return node;
}

void ScaleAnimationSpeed(Animation& anim, float speed) {
    if (!(speed > 0.0f)) return;
    for (auto& c : anim.channels) {
        for (auto& t : c.times) t /= speed;
        // 曲線の接線は 1 秒あたりの変化なので、速くした分だけ大きくなる
        for (auto* tans : { &c.inTangents, &c.outTangents }) for (auto& d : *tans) d *= speed;
    }
    anim.RecomputeDuration();
}

void AppendScene(Scene& dst, const Scene& src) {
    const int nOff = (int)dst.nodes.size(), mOff = (int)dst.meshes.size(), matOff = (int)dst.materials.size();
    const int tOff = (int)dst.textures.size(), sOff = (int)dst.skins.size();
    for (auto n : src.nodes) {
        if (n.parent >= 0) n.parent += nOff;
        for (auto& c : n.children) c += nOff;
        if (n.mesh >= 0) n.mesh += mOff;
        if (n.skin >= 0) n.skin += sOff;
        dst.nodes.push_back(std::move(n));
    }
    for (int r : src.roots) dst.roots.push_back(r + nOff);
    for (auto m : src.meshes) {
        for (auto& p : m.primitives) if (p.material >= 0) p.material += matOff;
        m.Touch();
        dst.meshes.push_back(std::move(m));
    }
    for (auto m : src.materials) {
        for (auto& s : m.slots) if (s.texture >= 0) s.texture += tOff;
        dst.materials.push_back(std::move(m));
    }
    for (auto& t : src.textures) dst.textures.push_back(t);
    for (auto s : src.skins) {
        for (auto& j : s.joints) if (j >= 0) j += nOff;
        if (s.skeleton >= 0) s.skeleton += nOff;
        dst.skins.push_back(std::move(s));
    }
    for (auto a : src.animations) {
        for (auto& c : a.channels) if (c.node >= 0) c.node += nOff;
        dst.animations.push_back(std::move(a));
    }
}

int ImportAnimationsByName(Scene& dst, const Scene& src, int* unmatched) {
    std::unordered_map<std::string, int> byName;
    for (int i = 0; i < (int)dst.nodes.size(); ++i) byName.emplace(dst.nodes[i].name, i);
    auto stripNs = [](const std::string& s) { size_t p = s.rfind(':'); return p == std::string::npos ? s : s.substr(p + 1); };
    std::unordered_map<std::string, int> byShortName;
    for (int i = 0; i < (int)dst.nodes.size(); ++i) byShortName.emplace(stripNs(dst.nodes[i].name), i);
    int matched = 0, missed = 0;
    for (const auto& a : src.animations) {
        Animation na;
        na.name = a.name;
        for (const auto& c : a.channels) {
            if (c.node < 0 || c.node >= (int)src.nodes.size()) continue;
            const std::string& name = src.nodes[c.node].name;
            int target = -1;
            if (auto it = byName.find(name); it != byName.end()) target = it->second;
            else if (auto it2 = byShortName.find(stripNs(name)); it2 != byShortName.end()) target = it2->second;
            if (target < 0) { ++missed; continue; }
            AnimChannel nc = c;
            nc.node = target;
            na.channels.push_back(std::move(nc));
            ++matched;
        }
        na.RecomputeDuration();
        if (!na.channels.empty()) dst.animations.push_back(std::move(na));
    }
    if (unmatched) *unmatched = missed;
    return matched;
}

} // namespace Ops
