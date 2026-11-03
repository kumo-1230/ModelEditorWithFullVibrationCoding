#include "ModelOps.h"
#include "Log.h"
#include "SceneOps.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <unordered_map>

namespace Ops {

// ---------------------------------------------------------------------------
// モデル
// ---------------------------------------------------------------------------
int ModelRootOf(const Scene& scene, int node) {
    if (node < 0 || node >= (int)scene.nodes.size()) return -1;
    while (scene.nodes[node].parent >= 0) node = scene.nodes[node].parent;
    return node;
}

std::vector<char> SubtreeMask(const Scene& scene, int root) {
    std::vector<char> m(scene.nodes.size(), 0);
    if (root < 0 || root >= (int)scene.nodes.size()) return m;
    std::function<void(int)> mark = [&](int i) { m[i] = 1; for (int c : scene.nodes[i].children) mark(c); };
    mark(root);
    return m;
}

bool ScaleModelContents(Scene& scene, int root, float f, std::string* err) {
    if (root < 0 || root >= (int)scene.nodes.size()) return false;
    // ルートのスケールを一時的に f にして階層へ適用すると、子の移動・移動キー・頂点・逆バインド行列だけが f 倍になる
    glm::vec3 keep = scene.nodes[root].scale;
    scene.nodes[root].scale = glm::vec3(f);
    bool ok = ApplyUniformScaleToHierarchy(scene, root, err);
    scene.nodes[root].scale = keep;
    return ok;
}

Scene ExtractModel(const Scene& src, int root) {
    Scene s = src;
    if (root < 0 || root >= (int)s.nodes.size()) return s;
    std::vector<int> remove;
    for (int r : s.roots) if (r != root) remove.push_back(r);
    // root が最上位でない (どこかの子になっている) 場合は、root の部分木以外を全部消す
    if (s.nodes[root].parent >= 0) {
        int top = ModelRootOf(s, root);
        s.Reparent(root, -1, true);
        remove.push_back(top);
    }
    s.RemoveNodes(remove);
    s.animations.erase(std::remove_if(s.animations.begin(), s.animations.end(), [](const Animation& a) { return a.channels.empty(); }), s.animations.end());
    RemoveUnusedResources(s);
    return s;
}

int GroupRootsAsModel(Scene& scene, const std::string& nameIn, size_t firstRoot) {
    std::vector<int> newRoots(scene.roots.begin() + std::min(firstRoot, scene.roots.size()), scene.roots.end());
    std::string name = nameIn.empty() ? "Model" : nameIn;
    // 既にある (他のファイルの) モデルと同じ名前なら _2, _3 ... を付ける。今回のファイル自身のノードとは比べない
    auto taken = [&](const std::string& n) {
        for (int r : scene.roots)
            if (std::find(newRoots.begin(), newRoots.end(), r) == newRoots.end() && scene.nodes[r].name == n) return true;
        return false;
    };
    std::string unique = name;
    for (int k = 2; taken(unique); ++k) unique = name + "_" + std::to_string(k);
    int group = scene.AddNode(unique, -1);
    for (int nr : newRoots) scene.Reparent(nr, group, false);
    scene.nodes[group].objectRoot = true;
    scene.nodes[group].objectWrapper = true; // 焼き込み書き出しでは取り除かれ、元のファイルと同じ階層になる
    return group;
}

int BakeSkinsToRestPose(Scene& scene) {
    const auto world = scene.ComputeWorldMatrices();
    int baked = 0;
    for (int ni = 0; ni < (int)scene.nodes.size(); ++ni) {
        Node& n = scene.nodes[ni];
        if (n.mesh < 0 || n.skin < 0 || n.skin >= (int)scene.skins.size() || !scene.meshes[n.mesh].HasSkin()) continue;
        const Skin sk = scene.skins[n.skin];
        std::vector<glm::mat4> jm(sk.joints.size(), glm::mat4(1.0f));
        for (size_t j = 0; j < sk.joints.size(); ++j)
            if (sk.joints[j] >= 0 && sk.joints[j] < (int)world.size() && j < sk.inverseBind.size()) jm[j] = world[sk.joints[j]] * sk.inverseBind[j];
        const glm::mat4 toLocal = glm::inverse(world[ni]); // 変形後 (ワールド) → メッシュノードのローカル
        int m = MakeMeshUnique(scene, ni);
        for (auto& p : scene.meshes[m].primitives) {
            if (p.joints.size() != p.positions.size() || p.weights.size() != p.positions.size()) continue;
            for (size_t v = 0; v < p.positions.size(); ++v) {
                glm::mat4 M(0.0f);
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    float w = p.weights[v][k];
                    int j = p.joints[v][k];
                    if (w <= 0.0f || j >= (int)jm.size()) continue;
                    M += jm[j] * w;
                    sum += w;
                }
                if (sum <= 0.0f) M = world[ni];
                else M /= sum;
                M = toLocal * M;
                p.positions[v] = glm::vec3(M * glm::vec4(p.positions[v], 1.0f));
                glm::mat3 N = glm::transpose(glm::inverse(glm::mat3(M)));
                if (v < p.normals.size()) p.normals[v] = glm::normalize(N * p.normals[v]);
                if (v < p.tangents.size()) p.tangents[v] = glm::vec4(glm::normalize(glm::mat3(M) * glm::vec3(p.tangents[v])), p.tangents[v].w);
            }
            p.joints.clear();
            p.weights.clear();
        }
        scene.meshes[m].Touch();
        scene.nodes[ni].skin = -1;
        ++baked;
    }
    if (baked) RemoveUnusedResources(scene);
    return baked;
}

// ---------------------------------------------------------------------------
// 抜き取り
// ---------------------------------------------------------------------------
void StripAnimations(Scene& scene) { scene.animations.clear(); }

void StripToAnimationOnly(Scene& scene) {
    const int n = (int)scene.nodes.size();
    std::vector<char> needed(n, 0);
    for (auto& a : scene.animations)
        for (auto& c : a.channels) if (c.node >= 0 && c.node < n) needed[c.node] = 1;
    for (auto& s : scene.skins)
        for (int j : s.joints) if (j >= 0 && j < n) needed[j] = 1;
    // 必要なノードの祖先も残す
    for (int i = 0; i < n; ++i)
        if (needed[i]) for (int p = scene.nodes[i].parent; p >= 0 && !needed[p]; p = scene.nodes[p].parent) needed[p] = 1;
    // メッシュだけを持つ末端のノードは消し、残りのノードからはメッシュを外す
    std::vector<int> remove;
    for (int i = 0; i < n; ++i)
        if (!needed[i] && scene.nodes[i].mesh >= 0 && scene.nodes[i].children.empty()) remove.push_back(i);
    scene.RemoveNodes(remove);
    for (auto& node : scene.nodes) { node.mesh = -1; node.skin = -1; }
    RemoveUnusedResources(scene);
}

// ---------------------------------------------------------------------------
// 結合
// ---------------------------------------------------------------------------
std::string NormalizeBoneName(const std::string& name) {
    std::string s = name;
    size_t p = s.find_last_of(":|");
    if (p != std::string::npos && p + 1 < s.size()) s = s.substr(p + 1);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    for (const char* pre : { "mixamorig", "bip001 ", "bip001_", "bip01 ", "bip01_", "armature_", "def-", "def_", "cc_base_", "jnt_", "bone_", "b_", "j_" }) {
        size_t len = strlen(pre);
        if (s.size() > len && s.compare(0, len, pre) == 0) { s = s.substr(len); break; }
    }
    std::string out;
    for (char c : s)
        if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80) out += c;
    return out.empty() ? s : out;
}

std::vector<std::pair<int, int>> AutoBoneMap(const Scene& scene, int srcModel, int dstModel) {
    std::vector<std::pair<int, int>> out;
    if (srcModel < 0 || dstModel < 0 || srcModel >= (int)scene.nodes.size() || dstModel >= (int)scene.nodes.size()) return out;
    auto srcMask = SubtreeMask(scene, srcModel), dstMask = SubtreeMask(scene, dstModel);
    std::unordered_map<std::string, int> dstExact, dstNorm;
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        if (!dstMask[i] || i == dstModel) continue;
        dstExact.emplace(scene.nodes[i].name, i);
        dstNorm.emplace(NormalizeBoneName(scene.nodes[i].name), i);
    }
    // 元のボーン: 動くノードとスキンのジョイント
    std::vector<char> isBone(scene.nodes.size(), 0);
    for (auto& a : scene.animations) for (auto& c : a.channels) if (c.node >= 0 && c.node < (int)isBone.size()) isBone[c.node] = 1;
    for (auto& s : scene.skins) for (int j : s.joints) if (j >= 0 && j < (int)isBone.size()) isBone[j] = 1;
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        if (!srcMask[i] || i == srcModel || !isBone[i]) continue;
        int d = -1;
        auto it = dstExact.find(scene.nodes[i].name);
        if (it != dstExact.end()) d = it->second;
        else {
            auto jt = dstNorm.find(NormalizeBoneName(scene.nodes[i].name));
            if (jt != dstNorm.end()) d = jt->second;
        }
        out.push_back({ i, d });
    }
    return out;
}

static glm::quat WorldRot(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) {
        float l = glm::length(r[i]);
        if (l > 1e-12f) r[i] /= l;
    }
    return glm::normalize(glm::quat_cast(r));
}

std::vector<int> CopyAnimationsBetweenModels(Scene& scene, int srcModel, int dstModel, const std::vector<int>& animsIn, const RetargetOptions& opt,
                                             RetargetReport* report) {
    std::vector<int> added;
    RetargetReport rep;
    if (srcModel < 0 || dstModel < 0 || srcModel == dstModel || srcModel >= (int)scene.nodes.size() || dstModel >= (int)scene.nodes.size()) {
        if (report) *report = rep;
        return added;
    }
    auto srcMask = SubtreeMask(scene, srcModel), dstMask = SubtreeMask(scene, dstModel);
    // 対応表: 同じ名前を優先し、無ければ正規化した名前で探す
    std::unordered_map<std::string, int> dstExact, dstNorm;
    for (int i = 0; i < (int)scene.nodes.size(); ++i) {
        if (!dstMask[i] || i == dstModel) continue;
        dstExact.emplace(scene.nodes[i].name, i);
        dstNorm.emplace(NormalizeBoneName(scene.nodes[i].name), i);
    }
    std::unordered_map<std::string, std::string> manual(opt.boneMap.begin(), opt.boneMap.end());
    // 先のノード番号。-1 = 見つからない、-2 = 手で「使わない」にした
    auto mapNode = [&](int s) -> int {
        auto m = manual.find(scene.nodes[s].name);
        if (m != manual.end()) {
            if (m->second.empty()) return -2;
            auto it = dstExact.find(m->second);
            return it != dstExact.end() ? it->second : -1;
        }
        if (!opt.autoMatch) return -1;
        auto it = dstExact.find(scene.nodes[s].name);
        if (it != dstExact.end()) return it->second;
        auto jt = dstNorm.find(NormalizeBoneName(scene.nodes[s].name));
        return jt != dstNorm.end() ? jt->second : -1;
    };
    std::vector<int> anims = animsIn;
    if (anims.empty())
        for (int a = 0; a < (int)scene.animations.size(); ++a)
            for (auto& c : scene.animations[a].channels)
                if (c.node >= 0 && c.node < (int)srcMask.size() && srcMask[c.node]) { anims.push_back(a); break; }

    const auto world = scene.ComputeWorldMatrices();
    std::vector<int> depth(scene.nodes.size(), 0);
    for (int i = 0; i < (int)scene.nodes.size(); ++i)
        for (int p = scene.nodes[i].parent; p >= 0; p = scene.nodes[p].parent) ++depth[i];

    // 身長の比: 対応が取れたノードの高さ (Y の範囲) で比べる
    {
        float sMin = 1e30f, sMax = -1e30f, dMin = 1e30f, dMax = -1e30f;
        for (int i = 0; i < (int)scene.nodes.size(); ++i) {
            if (!srcMask[i] || i == srcModel) continue;
            int d = mapNode(i);
            if (d < 0) continue;
            sMin = std::min(sMin, world[i][3].y); sMax = std::max(sMax, world[i][3].y);
            dMin = std::min(dMin, world[d][3].y); dMax = std::max(dMax, world[d][3].y);
        }
        if (sMax - sMin > 1e-6f && dMax - dMin > 1e-6f) rep.heightRatio = (dMax - dMin) / (sMax - sMin);
    }

    std::set<std::string> unmatched;
    std::set<int> matched;
    const int n0 = (int)scene.animations.size();
    for (int a : anims) {
        if (a < 0 || a >= n0) continue;
        const Animation src = scene.animations[a]; // push_back で参照が無効になるのでコピー
        // 腰 (移動キーを持つ一番上のボーン)
        int rootBone = -1;
        for (auto& c : src.channels)
            if (c.path == AnimPath::Translation && c.node >= 0 && srcMask[c.node] && (rootBone < 0 || depth[c.node] < depth[rootBone])) rootBone = c.node;
        Animation na;
        na.name = src.name;
        for (const auto& ch : src.channels) {
            if (ch.node < 0 || ch.node >= (int)srcMask.size() || !srcMask[ch.node] || ch.node == srcModel) continue;
            int d = mapNode(ch.node);
            if (d == -2) continue; // 手で「使わない」にしたボーン
            if (d < 0) { unmatched.insert(scene.nodes[ch.node].name); continue; }
            matched.insert(d);
            AnimChannel c = ch;
            c.node = d;
            const Node& sn = scene.nodes[ch.node];
            const Node& dn = scene.nodes[d];
            if (ch.path == AnimPath::Translation) {
                bool isRoot = ch.node == rootBone;
                if (isRoot ? !opt.rootTranslation : !opt.otherTranslations) continue;
                for (auto& v : c.values) {
                    glm::vec3 t(v);
                    if (opt.mode == 1) t = dn.translation + (t - sn.translation) * rep.heightRatio;
                    else t *= rep.heightRatio;
                    v = glm::vec4(t, 0.0f);
                }
                for (auto* tans : { &c.inTangents, &c.outTangents }) for (auto& d2 : *tans) d2 *= rep.heightRatio; // 曲線の傾きも同じ倍率
            } else if (ch.path == AnimPath::Rotation) {
                if (opt.mode == 1) {
                    // ワールドでの「レストポーズからの回転」が同じになるように移す:
                    // q_d = inv(Pd) * Ps * q_s * inv(Ws) * Wd   (P = 親のレストのワールド回転, W = 自分のレストのワールド回転)
                    glm::quat Ps = sn.parent >= 0 ? WorldRot(world[sn.parent]) : glm::quat(1, 0, 0, 0);
                    glm::quat Pd = dn.parent >= 0 ? WorldRot(world[dn.parent]) : glm::quat(1, 0, 0, 0);
                    glm::quat Ws = WorldRot(world[ch.node]), Wd = WorldRot(world[d]);
                    for (auto& v : c.values) {
                        glm::quat q(v.w, v.x, v.y, v.z);
                        glm::quat r = glm::normalize(glm::inverse(Pd) * Ps * q * glm::inverse(Ws) * Wd);
                        v = glm::vec4(r.x, r.y, r.z, r.w);
                    }
                    // 曲線の接線にも同じ左右の掛け算をする (q → A q B は線形)
                    for (auto* tans : { &c.inTangents, &c.outTangents })
                        for (auto& d2 : *tans) {
                            glm::quat q(d2.w, d2.x, d2.y, d2.z);
                            glm::quat r = glm::inverse(Pd) * Ps * q * glm::inverse(Ws) * Wd;
                            d2 = glm::vec4(r.x, r.y, r.z, r.w);
                        }
                }
            } else if (!opt.scaleKeys) {
                continue;
            }
            na.channels.push_back(std::move(c));
        }
        if (na.channels.empty()) continue;
        na.duration = src.duration;
        // 名前が重なる場合は区別できるようにする
        for (auto& e : scene.animations) if (e.name == na.name) { na.name += "_" + scene.nodes[dstModel].name; break; }
        scene.animations.push_back(std::move(na));
        added.push_back((int)scene.animations.size() - 1);
        ++rep.animations;
    }
    rep.matchedBones = (int)matched.size();
    rep.unmatched.assign(unmatched.begin(), unmatched.end());
    if (report) *report = rep;
    return added;
}

bool AttachModel(Scene& scene, int srcModel, int dstNode, bool keepWorld, std::string* err) {
    if (srcModel < 0 || dstNode < 0 || srcModel >= (int)scene.nodes.size() || dstNode >= (int)scene.nodes.size()) return false;
    if (srcModel == dstNode || scene.IsAncestor(srcModel, dstNode)) {
        if (err) *err = "自分自身や自分の子には付けられません";
        return false;
    }
    return scene.Reparent(srcModel, dstNode, keepWorld);
}

int MergeModels(Scene& scene, int srcModel, int dstModel) {
    if (srcModel < 0 || dstModel < 0 || srcModel == dstModel || srcModel >= (int)scene.nodes.size() || dstModel >= (int)scene.nodes.size()) return -1;
    if (scene.IsAncestor(srcModel, dstModel)) return -1;
    const Node& s = scene.nodes[srcModel];
    bool dissolve = s.mesh < 0 && s.skin < 0;
    bool animated = false;
    for (auto& a : scene.animations) for (auto& c : a.channels) if (c.node == srcModel) animated = true;
    if (dissolve && !animated) {
        // グループは消し、中身だけを見た目を保ったまま移す
        for (int c : std::vector<int>(s.children)) scene.Reparent(c, dstModel, true);
        scene.RemoveNodes({ srcModel });
        return dstModel > srcModel ? dstModel - 1 : dstModel;
    }
    scene.Reparent(srcModel, dstModel, true);
    return dstModel;
}

// ---------------------------------------------------------------------------
// アニメーション編集
// ---------------------------------------------------------------------------
glm::vec4 SampleChannel(const AnimChannel& ch, float t) { return EvalChannel(ch, t); }

glm::vec4 ChannelDerivative(const AnimChannel& ch, float t) {
    const size_t n = std::min(ch.times.size(), ch.values.size());
    if (n < 2 || ch.interp == Interp::Step) return glm::vec4(0.0f);
    if (t <= ch.times.front()) t = ch.times.front();
    if (t >= ch.times[n - 1]) t = ch.times[n - 1];
    size_t k = std::upper_bound(ch.times.begin(), ch.times.begin() + n, t) - ch.times.begin();
    k = k == 0 ? 0 : std::min(k - 1, n - 2);
    const float t0 = ch.times[k], t1 = ch.times[k + 1], dt = t1 - t0;
    if (dt <= 0.0f) return glm::vec4(0.0f);
    const glm::vec4 &a = ch.values[k], &b = ch.values[k + 1];
    if (ch.IsCubic()) {
        float f = (t - t0) / dt, f2 = f * f;
        return ((6 * f2 - 6 * f) * a + (3 * f2 - 4 * f + 1) * dt * ch.outTangents[k] + (-6 * f2 + 6 * f) * b + (3 * f2 - 2 * f) * dt * ch.inTangents[k + 1]) / dt;
    }
    return (b - a) / dt;
}

int InsertKey(AnimChannel& ch, float t) {
    const float eps = 1e-5f;
    for (size_t k = 0; k < ch.times.size(); ++k)
        if (std::abs(ch.times[k] - t) <= eps) return (int)k;
    glm::vec4 v = EvalChannel(ch, t);
    const bool cubic = ch.IsCubic();
    glm::vec4 d = cubic ? ChannelDerivative(ch, t) : glm::vec4(0.0f);
    // 最初のキーより前・最後のキーより後は値が変わらない (平らに延びる) ので、そこに打つキーは傾き 0。
    // 端のキーの外側の傾きも 0 にして、間が平らなままになるようにする
    const bool before = !ch.times.empty() && t < ch.times.front(), after = !ch.times.empty() && t > ch.times.back();
    if (cubic && (before || after)) {
        d = glm::vec4(0.0f);
        if (after) ch.outTangents.back() = glm::vec4(0.0f);
        if (before) ch.inTangents.front() = glm::vec4(0.0f);
    }
    if (ch.path == AnimPath::Rotation && !ch.values.empty()) {
        // 前のキーと同じ向きの四元数にそろえる (符号が逆だと補間が遠回りになる)
        size_t prev = std::upper_bound(ch.times.begin(), ch.times.end(), t) - ch.times.begin();
        const glm::vec4& ref = ch.values[prev > 0 ? prev - 1 : 0];
        if (glm::dot(ref, v) < 0.0f) { v = -v; d = -d; }
    }
    size_t idx = std::upper_bound(ch.times.begin(), ch.times.end(), t) - ch.times.begin();
    ch.times.insert(ch.times.begin() + idx, t);
    ch.values.insert(ch.values.begin() + idx, v);
    if (cubic) {
        ch.inTangents.insert(ch.inTangents.begin() + idx, d);
        ch.outTangents.insert(ch.outTangents.begin() + idx, d);
    }
    return (int)idx;
}

void EraseKey(AnimChannel& ch, int k) {
    if (k < 0 || k >= (int)ch.times.size()) return;
    ch.times.erase(ch.times.begin() + k);
    if (k < (int)ch.values.size()) ch.values.erase(ch.values.begin() + k);
    if (k < (int)ch.inTangents.size()) ch.inTangents.erase(ch.inTangents.begin() + k);
    if (k < (int)ch.outTangents.size()) ch.outTangents.erase(ch.outTangents.begin() + k);
}

void LinearizeChannel(AnimChannel& ch, float fps) {
    if (ch.interp != Interp::CubicSpline) return;
    if (ch.IsCubic() && ch.times.size() >= 2) {
        std::vector<float> times;
        std::vector<glm::vec4> values;
        for (size_t k = 0; k < ch.times.size(); ++k) {
            times.push_back(ch.times[k]);
            values.push_back(ch.values[k]);
            if (k + 1 >= ch.times.size()) break;
            const float t0 = ch.times[k], t1 = ch.times[k + 1];
            const int n = std::max(1, (int)std::ceil((t1 - t0) * fps - 1e-4f));
            for (int i = 1; i < n; ++i) {
                float t = t0 + (t1 - t0) * i / n;
                times.push_back(t);
                values.push_back(EvalChannel(ch, t));
            }
        }
        ch.times = std::move(times);
        ch.values = std::move(values);
    }
    ch.interp = Interp::Linear;
    ch.inTangents.clear();
    ch.outTangents.clear();
}

void LinearizeAnimation(Animation& anim, float fps) {
    for (auto& ch : anim.channels) LinearizeChannel(ch, fps);
}

void MakeCubic(AnimChannel& ch) {
    if (ch.IsCubic()) return;
    const size_t n = std::min(ch.times.size(), ch.values.size());
    ch.inTangents.assign(n, glm::vec4(0.0f));
    ch.outTangents.assign(n, glm::vec4(0.0f));
    if (ch.interp != Interp::Step) {
        // 前後の区間の傾きを接線にすると、形は今の直線のまま変わらない
        for (size_t k = 0; k + 1 < n; ++k) {
            float dt = ch.times[k + 1] - ch.times[k];
            glm::vec4 s = dt > 0 ? (ch.values[k + 1] - ch.values[k]) / dt : glm::vec4(0.0f);
            ch.outTangents[k] = s;
            ch.inTangents[k + 1] = s;
        }
    }
    ch.interp = Interp::CubicSpline;
}

void SetKeyTangents(AnimChannel& ch, const std::vector<int>& keys, TangentMode mode) {
    if (mode == TangentMode::Step) {
        // ステップはチャンネル全体の補間方法 (glTF の仕様)
        ch.interp = Interp::Step;
        ch.inTangents.clear();
        ch.outTangents.clear();
        return;
    }
    if (ch.interp == Interp::Step) ch.interp = Interp::Linear;
    MakeCubic(ch);
    const int n = (int)ch.times.size();
    auto slope = [&](int a, int b) {
        float dt = ch.times[b] - ch.times[a];
        return dt > 0 ? (ch.values[b] - ch.values[a]) / dt : glm::vec4(0.0f);
    };
    for (int k : keys) {
        if (k < 0 || k >= n) continue;
        glm::vec4 in(0.0f), out(0.0f);
        switch (mode) {
        case TangentMode::Linear:
            in = k > 0 ? slope(k - 1, k) : (k + 1 < n ? slope(k, k + 1) : glm::vec4(0.0f));
            out = k + 1 < n ? slope(k, k + 1) : in;
            break;
        case TangentMode::Flat:
            break;
        case TangentMode::Spline: {
            // Catmull-Rom (Maya の「スプライン」): 前後のキーを結ぶ傾き。端は隣への傾き
            glm::vec4 s = (k > 0 && k + 1 < n) ? slope(k - 1, k + 1) : (k + 1 < n ? slope(k, k + 1) : (k > 0 ? slope(k - 1, k) : glm::vec4(0.0f)));
            in = out = s;
            break;
        }
        case TangentMode::Clamped: {
            // 前後のどちらかと同じ値なら平ら (行き過ぎない)、それ以外はスプライン
            bool flatPrev = k > 0 && glm::length(ch.values[k] - ch.values[k - 1]) < 1e-6f;
            bool flatNext = k + 1 < n && glm::length(ch.values[k + 1] - ch.values[k]) < 1e-6f;
            glm::vec4 s = (k > 0 && k + 1 < n) ? slope(k - 1, k + 1) : (k + 1 < n ? slope(k, k + 1) : (k > 0 ? slope(k - 1, k) : glm::vec4(0.0f)));
            in = out = (flatPrev || flatNext) ? glm::vec4(0.0f) : s;
            break;
        }
        default:
            break;
        }
        ch.inTangents[k] = in;
        ch.outTangents[k] = out;
    }
}

// t0〜t1 の外側のキーを消し、t0 を 0 秒にする (両端は形を保ったまま分割したキー)
static void TrimChannel(AnimChannel& ch, float t0, float t1) {
    if (ch.times.empty()) return;
    if (ch.times.size() == 1) { ch.times[0] = 0.0f; return; }
    const float eps = 1e-5f;
    InsertKey(ch, t0);
    if (t1 - t0 > eps) InsertKey(ch, t1);
    for (int k = (int)ch.times.size() - 1; k >= 0; --k)
        if (ch.times[k] < t0 - eps || ch.times[k] > t1 + eps) EraseKey(ch, k);
    for (auto& t : ch.times) t = std::max(0.0f, t - t0);
}

void TrimAnimation(Animation& anim, float t0, float t1) {
    t0 = std::max(0.0f, t0);
    t1 = std::max(t0, t1);
    for (auto& ch : anim.channels) TrimChannel(ch, t0, t1);
    anim.duration = t1 - t0;
}

void CutAnimationRange(Animation& anim, float t0, float t1) {
    t0 = std::max(0.0f, t0);
    t1 = std::min(std::max(t0, t1), anim.duration);
    const float d = t1 - t0, eps = 1e-5f;
    if (d <= eps) return;
    // 切った位置: t0 までは元のまま、ほんの少し後 (gap) から t1 以降の動きがそのまま続く
    const float gap = 1e-4f;
    for (auto& ch : anim.channels) {
        if (ch.times.size() < 2) continue;
        InsertKey(ch, t0);
        InsertKey(ch, t1 + gap);
        for (int k = (int)ch.times.size() - 1; k >= 0; --k)
            if (ch.times[k] > t0 + eps && ch.times[k] < t1 + gap - eps) EraseKey(ch, k);
        for (auto& t : ch.times)
            if (t > t0 + eps) t -= d;
    }
    anim.duration = std::max(0.0f, anim.duration - d);
}

Animation ExtractAnimationRange(const Animation& anim, float t0, float t1, const std::string& name) {
    Animation a = anim;
    a.name = name;
    TrimAnimation(a, t0, t1);
    return a;
}

int RemoveChannelsForNodes(Animation& anim, const std::vector<int>& nodes, int pathMask) {
    std::set<int> ns(nodes.begin(), nodes.end());
    size_t before = anim.channels.size();
    anim.channels.erase(std::remove_if(anim.channels.begin(), anim.channels.end(),
                                       [&](const AnimChannel& c) { return ns.count(c.node) && (pathMask & (1 << (int)c.path)); }),
                        anim.channels.end());
    return (int)(before - anim.channels.size());
}

int DeleteKeysInRange(Animation& anim, const std::vector<int>& nodes, float t0, float t1) {
    std::set<int> ns(nodes.begin(), nodes.end());
    int removed = 0;
    const float eps = 1e-5f;
    for (auto& ch : anim.channels) {
        if (!ns.empty() && !ns.count(ch.node)) continue;
        if (ch.times.size() < 2) continue;
        // 範囲の両端には元の値 (と曲線なら傾き) のキーを残し、範囲内は両端をつなぐ補間にする
        if (t0 > ch.times.front() + eps && t0 < ch.times.back() - eps) InsertKey(ch, t0);
        if (t1 > ch.times.front() + eps && t1 < ch.times.back() - eps) InsertKey(ch, t1);
        for (int k = (int)ch.times.size() - 1; k >= 0; --k)
            if (ch.times[k] > t0 + eps && ch.times[k] < t1 - eps) { EraseKey(ch, k); ++removed; }
    }
    return removed;
}

int ReduceKeys(Animation& anim, float tolerance) {
    int removed = 0;
    for (auto& ch : anim.channels) {
        if (ch.times.size() < 3 || ch.interp != Interp::Linear) continue; // 曲線とステップは間引かない
        const size_t n = std::min(ch.times.size(), ch.values.size());
        std::vector<float> times{ ch.times.front() };
        std::vector<glm::vec4> values{ ch.values.front() };
        size_t lastKept = 0;
        // 値の差 (回転は角度 ≒ 2|q1 - ±q2|。acos は 1 付近で精度が落ちるので使わない)
        auto diff = [&](const glm::vec4& a, const glm::vec4& b) {
            if (ch.path == AnimPath::Rotation) return 2.0f * std::min(glm::length(a - b), glm::length(a + b));
            return glm::length(glm::vec3(a) - glm::vec3(b));
        };
        for (size_t k = 1; k + 1 < n; ++k) {
            // 直前に残したキーと次のキーを結んだ補間で、その間のキー (これまでに消したものも含む) がすべて再現できるなら消す
            AnimChannel seg;
            seg.path = ch.path;
            seg.times = { ch.times[lastKept], ch.times[k + 1] };
            seg.values = { ch.values[lastKept], ch.values[k + 1] };
            bool removable = true;
            for (size_t i = lastKept + 1; i <= k && removable; ++i)
                if (diff(SampleChannel(seg, ch.times[i]), ch.values[i]) > tolerance) removable = false;
            if (removable) { ++removed; continue; }
            times.push_back(ch.times[k]);
            values.push_back(ch.values[k]);
            lastKept = k;
        }
        times.push_back(ch.times[n - 1]);
        values.push_back(ch.values[n - 1]);
        ch.times = std::move(times);
        ch.values = std::move(values);
    }
    return removed;
}

} // namespace Ops
