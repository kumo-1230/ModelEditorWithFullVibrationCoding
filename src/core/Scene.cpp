#include "Scene.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>

uint64_t NewUid() {
    static std::atomic<uint64_t> counter{ 1 };
    return counter++;
}

const char* TexSlotName(int slot) {
    switch (slot) {
    case Slot_BaseColor: return "BaseColor (アルベド)";
    case Slot_MetallicRoughness: return "Metallic/Roughness";
    case Slot_Normal: return "Normal (法線)";
    case Slot_Occlusion: return "Occlusion (AO)";
    case Slot_Emissive: return "Emissive (発光)";
    }
    return "?";
}

glm::mat4 ComposeTRS(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    glm::mat4 m = glm::mat4_cast(glm::normalize(r));
    m[0] *= s.x;
    m[1] *= s.y;
    m[2] *= s.z;
    m[3] = glm::vec4(t, 1.0f);
    return m;
}

void DecomposeTRS(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    glm::vec3 skew;
    glm::vec4 persp;
    if (!glm::decompose(m, s, r, t, skew, persp)) {
        t = glm::vec3(m[3]);
        r = glm::quat(1, 0, 0, 0);
        s = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    }
    r = glm::normalize(r);
}

glm::mat4 Node::LocalMatrix() const { return ComposeTRS(translation, rotation, scale); }
void Node::SetLocalMatrix(const glm::mat4& m) { DecomposeTRS(m, translation, rotation, scale); }
glm::mat4 NodePose::Matrix() const { return ComposeTRS(t, r, s); }

size_t Mesh::VertexCount() const {
    size_t n = 0;
    for (auto& p : primitives) n += p.VertexCount();
    return n;
}
size_t Mesh::TriangleCount() const {
    size_t n = 0;
    for (auto& p : primitives) n += p.TriangleCount();
    return n;
}
bool Mesh::HasSkin() const {
    for (auto& p : primitives) if (p.HasSkin()) return true;
    return false;
}
int Mesh::MorphTargetCount() const {
    int n = 0;
    for (auto& p : primitives) n = std::max(n, (int)p.targets.size());
    return n;
}

void Animation::RecomputeDuration() {
    duration = 0.0f;
    for (auto& c : channels)
        if (!c.times.empty()) duration = std::max(duration, c.times.back());
}

// ---------------------------------------------------------------------------
void Scene::Clear() { *this = Scene(); }

void Scene::RebuildRoots() {
    std::vector<int> newRoots;
    // 既存の順番をできるだけ保つ
    for (int r : roots)
        if (r >= 0 && r < (int)nodes.size() && nodes[r].parent < 0 &&
            std::find(newRoots.begin(), newRoots.end(), r) == newRoots.end())
            newRoots.push_back(r);
    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i].parent < 0 && std::find(newRoots.begin(), newRoots.end(), i) == newRoots.end())
            newRoots.push_back(i);
    roots = std::move(newRoots);
}

int Scene::AddNode(const std::string& name, int parent) {
    Node n;
    n.name = name;
    n.parent = parent;
    int idx = (int)nodes.size();
    nodes.push_back(n);
    if (parent >= 0) nodes[parent].children.push_back(idx);
    else roots.push_back(idx);
    return idx;
}

bool Scene::IsAncestor(int ancestor, int node) const {
    int p = node >= 0 ? nodes[node].parent : -1;
    while (p >= 0) {
        if (p == ancestor) return true;
        p = nodes[p].parent;
    }
    return false;
}

bool Scene::Reparent(int node, int newParent, bool keepWorld) {
    if (node < 0 || node >= (int)nodes.size()) return false;
    if (node == newParent) return false;
    if (newParent >= 0 && IsAncestor(node, newParent)) return false; // 循環防止
    glm::mat4 world(1.0f);
    if (keepWorld) world = ComputeWorldMatrices()[node];

    Node& n = nodes[node];
    if (n.parent >= 0) {
        auto& ch = nodes[n.parent].children;
        ch.erase(std::remove(ch.begin(), ch.end(), node), ch.end());
    } else {
        roots.erase(std::remove(roots.begin(), roots.end(), node), roots.end());
    }
    n.parent = newParent;
    if (newParent >= 0) nodes[newParent].children.push_back(node);
    else roots.push_back(node);

    if (keepWorld) {
        glm::mat4 parentWorld(1.0f);
        if (newParent >= 0) parentWorld = ComputeWorldMatrices()[newParent];
        nodes[node].SetLocalMatrix(glm::inverse(parentWorld) * world);
    }
    return true;
}

void Scene::RemoveNodes(const std::vector<int>& nodesToRemove) {
    std::vector<bool> remove(nodes.size(), false);
    std::function<void(int)> mark = [&](int i) {
        if (i < 0 || i >= (int)nodes.size() || remove[i]) return;
        remove[i] = true;
        for (int c : nodes[i].children) mark(c);
    };
    for (int i : nodesToRemove) mark(i);

    std::vector<int> remap(nodes.size(), -1);
    std::vector<Node> kept;
    for (int i = 0; i < (int)nodes.size(); ++i) {
        if (!remove[i]) {
            remap[i] = (int)kept.size();
            kept.push_back(nodes[i]);
        }
    }
    for (auto& n : kept) {
        n.parent = n.parent >= 0 ? remap[n.parent] : -1;
        std::vector<int> ch;
        for (int c : n.children) if (remap[c] >= 0) ch.push_back(remap[c]);
        n.children = std::move(ch);
    }
    std::vector<int> newRoots;
    for (int r : roots) if (r >= 0 && r < (int)remap.size() && remap[r] >= 0) newRoots.push_back(remap[r]);
    nodes = std::move(kept);
    roots = std::move(newRoots);
    RebuildRoots();

    for (auto& s : skins) {
        for (auto& j : s.joints) j = (j >= 0 && j < (int)remap.size()) ? remap[j] : -1;
        s.skeleton = (s.skeleton >= 0 && s.skeleton < (int)remap.size()) ? remap[s.skeleton] : -1;
    }
    for (auto& a : animations) {
        std::vector<AnimChannel> ch;
        for (auto& c : a.channels) {
            if (c.node >= 0 && c.node < (int)remap.size() && remap[c.node] >= 0) {
                c.node = remap[c.node];
                ch.push_back(std::move(c));
            }
        }
        a.channels = std::move(ch);
    }
}

void Scene::RemoveMesh(int mesh) {
    if (mesh < 0 || mesh >= (int)meshes.size()) return;
    meshes.erase(meshes.begin() + mesh);
    for (auto& n : nodes) {
        if (n.mesh == mesh) { n.mesh = -1; n.skin = -1; }
        else if (n.mesh > mesh) n.mesh--;
    }
}

void Scene::RemoveMaterial(int material) {
    if (material < 0 || material >= (int)materials.size()) return;
    materials.erase(materials.begin() + material);
    for (auto& m : meshes) {
        bool changed = false;
        for (auto& p : m.primitives) {
            if (p.material == material) { p.material = -1; changed = true; }
            else if (p.material > material) { p.material--; changed = true; }
        }
        if (changed) m.Touch();
    }
}

void Scene::RemoveTexture(int texture) {
    if (texture < 0 || texture >= (int)textures.size()) return;
    textures.erase(textures.begin() + texture);
    for (auto& m : materials)
        for (auto& s : m.slots) {
            if (s.texture == texture) s.texture = -1;
            else if (s.texture > texture) s.texture--;
        }
}

void Scene::RemoveAnimation(int anim) {
    if (anim >= 0 && anim < (int)animations.size()) animations.erase(animations.begin() + anim);
}

std::vector<NodePose> Scene::RestPose() const {
    std::vector<NodePose> pose(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        pose[i].t = nodes[i].translation;
        pose[i].r = nodes[i].rotation;
        pose[i].s = nodes[i].scale;
    }
    return pose;
}

std::vector<glm::mat4> Scene::ComputeWorldMatrices() const { return ComputeWorldMatrices(RestPose()); }

std::vector<glm::mat4> Scene::ComputeWorldMatrices(const std::vector<NodePose>& pose) const {
    std::vector<glm::mat4> world(nodes.size(), glm::mat4(1.0f));
    std::vector<bool> done(nodes.size(), false);
    std::vector<std::pair<int, glm::mat4>> stack;
    for (int r : roots) stack.push_back({ r, glm::mat4(1.0f) });
    // roots に含まれない孤立ノードも処理
    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i].parent < 0 && std::find(roots.begin(), roots.end(), i) == roots.end())
            stack.push_back({ i, glm::mat4(1.0f) });
    while (!stack.empty()) {
        auto [idx, parentM] = stack.back();
        stack.pop_back();
        if (idx < 0 || idx >= (int)nodes.size() || done[idx]) continue;
        done[idx] = true;
        const glm::mat4 local = idx < (int)pose.size() ? pose[idx].Matrix() : nodes[idx].LocalMatrix();
        world[idx] = parentM * local;
        for (int c : nodes[idx].children) stack.push_back({ c, world[idx] });
    }
    return world;
}

std::vector<bool> Scene::JointMask() const {
    std::vector<bool> mask(nodes.size(), false);
    for (auto& s : skins)
        for (int j : s.joints)
            if (j >= 0 && j < (int)nodes.size()) mask[j] = true;
    return mask;
}

std::vector<int> Scene::NodesUsingMesh(int mesh) const {
    std::vector<int> out;
    for (int i = 0; i < (int)nodes.size(); ++i) if (nodes[i].mesh == mesh) out.push_back(i);
    return out;
}

int Scene::FindNode(const std::string& name) const {
    for (int i = 0; i < (int)nodes.size(); ++i) if (nodes[i].name == name) return i;
    return -1;
}

std::string Scene::NodePath(int node) const {
    std::string path;
    int guard = 0;
    while (node >= 0 && guard++ < 1024) {
        path = nodes[node].name + (path.empty() ? "" : "/" + path);
        node = nodes[node].parent;
    }
    return path;
}

size_t Scene::TotalVertices() const {
    size_t n = 0;
    for (auto& m : meshes) n += m.VertexCount();
    return n;
}
size_t Scene::TotalTriangles() const {
    size_t n = 0;
    for (auto& m : meshes) n += m.TriangleCount();
    return n;
}

bool Scene::ComputeNodeBounds(int i, const std::vector<glm::mat4>& world, glm::vec3& outMin, glm::vec3& outMax, size_t maxSamples) const {
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(-std::numeric_limits<float>::max());
    if (i < 0 || i >= (int)nodes.size()) return false;
    const Node& n = nodes[i];
    if (n.mesh < 0 || n.mesh >= (int)meshes.size()) return false;
    const Mesh& mesh = meshes[n.mesh];
    const Skin* skin = (n.skin >= 0 && n.skin < (int)skins.size() && mesh.HasSkin()) ? &skins[n.skin] : nullptr;
    std::vector<glm::mat4> jm;
    if (skin) {
        jm.resize(skin->joints.size(), glm::mat4(1.0f));
        for (size_t j = 0; j < skin->joints.size(); ++j) {
            int jn = skin->joints[j];
            glm::mat4 ibm = j < skin->inverseBind.size() ? skin->inverseBind[j] : glm::mat4(1.0f);
            if (jn >= 0 && jn < (int)world.size()) jm[j] = world[jn] * ibm;
        }
    }
    bool any = false;
    for (auto& p : mesh.primitives) {
        size_t stride = std::max<size_t>(1, p.positions.size() / std::max<size_t>(1, maxSamples));
        bool skinned = skin && p.HasSkin();
        for (size_t v = 0; v < p.positions.size(); v += stride) {
            glm::vec3 w;
            if (skinned) {
                glm::mat4 m(0.0f);
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    float wt = p.weights[v][k];
                    if (wt <= 0) continue;
                    int j = p.joints[v][k];
                    m += (j < (int)jm.size() ? jm[j] : glm::mat4(1.0f)) * wt;
                    sum += wt;
                }
                if (sum <= 0) m = world[i];
                w = glm::vec3(m * glm::vec4(p.positions[v], 1.0f));
            } else {
                w = glm::vec3(world[i] * glm::vec4(p.positions[v], 1.0f));
            }
            outMin = glm::min(outMin, w);
            outMax = glm::max(outMax, w);
            any = true;
        }
    }
    return any;
}

void Scene::ComputeBounds(const std::vector<glm::mat4>& world, glm::vec3& outMin, glm::vec3& outMax) const {
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(-std::numeric_limits<float>::max());
    bool any = false;
    for (int i = 0; i < (int)nodes.size(); ++i) {
        glm::vec3 a, b;
        if (ComputeNodeBounds(i, world, a, b, SIZE_MAX)) { // 全頂点 (間引くと最低点・最高点がずれる)
            outMin = glm::min(outMin, a);
            outMax = glm::max(outMax, b);
            any = true;
        }
    }
    if (!any) {
        // メッシュが無ければノード位置から
        for (auto& w : world) {
            glm::vec3 p(w[3]);
            outMin = glm::min(outMin, p);
            outMax = glm::max(outMax, p);
            any = true;
        }
    }
    if (!any) { outMin = glm::vec3(-1); outMax = glm::vec3(1); }
}
// ---------------------------------------------------------------------------
static size_t FindKey(const std::vector<float>& times, float t) {
    // times[i] <= t < times[i+1] となる i
    auto it = std::upper_bound(times.begin(), times.end(), t);
    if (it == times.begin()) return 0;
    return (size_t)(it - times.begin()) - 1;
}

glm::vec4 EvalChannel(const AnimChannel& ch, float time) {
    if (ch.times.empty() || ch.values.size() < ch.times.size()) return glm::vec4(0.0f);
    if (ch.times.size() == 1 || time <= ch.times.front()) return ch.values.front();
    if (time >= ch.times.back()) return ch.values[ch.times.size() - 1];
    size_t k = FindKey(ch.times, time);
    size_t k1 = std::min(k + 1, ch.times.size() - 1);
    float t0 = ch.times[k], t1 = ch.times[k1];
    float f = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
    if (ch.interp == Interp::Step) f = 0.0f;
    const glm::vec4& a = ch.values[k];
    const glm::vec4& b = ch.values[k1];
    if (ch.IsCubic()) {
        // エルミート補間 (glTF CUBICSPLINE と同じ式)。回転は 4 成分を補間して正規化する
        const float dt = t1 - t0, f2 = f * f, f3 = f2 * f;
        glm::vec4 v = (2 * f3 - 3 * f2 + 1) * a + (f3 - 2 * f2 + f) * dt * ch.outTangents[k] + (-2 * f3 + 3 * f2) * b + (f3 - f2) * dt * ch.inTangents[k1];
        if (ch.path == AnimPath::Rotation) {
            float l = glm::length(v);
            if (l > 1e-12f) v /= l;
        }
        return v;
    }
    if (ch.path == AnimPath::Rotation) {
        glm::quat qa(a.w, a.x, a.y, a.z), qb(b.w, b.x, b.y, b.z);
        glm::quat q = glm::slerp(qa, qb, f);
        return glm::vec4(q.x, q.y, q.z, q.w);
    }
    return glm::mix(a, b, f);
}

void SampleAnimation(const Scene& scene, const Animation& anim, float time, std::vector<NodePose>& pose) {
    for (const auto& ch : anim.channels) {
        if (ch.node < 0 || ch.node >= (int)pose.size() || ch.times.empty() || ch.values.size() < ch.times.size()) continue;
        glm::vec4 v = EvalChannel(ch, time);
        NodePose& p = pose[ch.node];
        switch (ch.path) {
        case AnimPath::Translation: p.t = glm::vec3(v); break;
        case AnimPath::Rotation: p.r = glm::normalize(glm::quat(v.w, v.x, v.y, v.z)); break;
        case AnimPath::Scale: p.s = glm::vec3(v); break;
        }
    }
    (void)scene;
}
