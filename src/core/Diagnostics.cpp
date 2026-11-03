#include "Diagnostics.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace {

bool Finite4(const glm::vec4& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }

// 位置で頂点を溶接した番号 (UV / 法線の継ぎ目で分かれた頂点を同じ点として扱う)
std::vector<uint32_t> WeldByPosition(const std::vector<glm::vec3>& pos) {
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (auto& p : pos) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
    double eps = std::max((double)glm::length(mx - mn) * 1e-6, 1e-9);
    struct Key {
        long long x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return (size_t)(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL); }
    };
    std::unordered_map<Key, uint32_t, KeyHash> ids;
    ids.reserve(pos.size());
    std::vector<uint32_t> out(pos.size());
    for (size_t i = 0; i < pos.size(); ++i) {
        Key k{ (long long)std::llround(pos[i].x / eps), (long long)std::llround(pos[i].y / eps), (long long)std::llround(pos[i].z / eps) };
        auto it = ids.emplace(k, (uint32_t)ids.size()).first;
        out[i] = it->second;
    }
    return out;
}

// ノードのジョイント行列 (world[joint] * IBM)。スキンでなければ空
std::vector<glm::mat4> JointMatrices(const Scene& s, int node, const std::vector<glm::mat4>& world) {
    std::vector<glm::mat4> jm;
    const Node& n = s.nodes[node];
    if (n.skin < 0 || n.skin >= (int)s.skins.size() || n.mesh < 0 || !s.meshes[n.mesh].HasSkin()) return jm;
    const Skin& sk = s.skins[n.skin];
    jm.resize(sk.joints.size(), glm::mat4(1.0f));
    for (size_t j = 0; j < sk.joints.size(); ++j) {
        int jn = sk.joints[j];
        glm::mat4 ibm = j < sk.inverseBind.size() ? sk.inverseBind[j] : glm::mat4(1.0f);
        if (jn >= 0 && jn < (int)world.size()) jm[j] = world[jn] * ibm;
    }
    return jm;
}

glm::mat4 VertexMatrix(const Primitive& p, size_t v, const std::vector<glm::mat4>& jm, const glm::mat4& nodeWorld) {
    if (jm.empty() || !p.HasSkin()) return nodeWorld;
    glm::mat4 m(0.0f);
    float sum = 0;
    for (int k = 0; k < 4; ++k) {
        float w = p.weights[v][k];
        if (w <= 0) continue;
        int j = p.joints[v][k];
        m += (j < (int)jm.size() ? jm[j] : glm::mat4(1.0f)) * w;
        sum += w;
    }
    return sum > 0 ? m : nodeWorld;
}

float RestDiagonal(const Scene& s, const std::vector<glm::mat4>& world, glm::vec3* outMin = nullptr, glm::vec3* outMax = nullptr) {
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (int i = 0; i < (int)s.nodes.size(); ++i) {
        glm::vec3 a, b;
        if (s.ComputeNodeBounds(i, world, a, b, 2000)) { mn = glm::min(mn, a); mx = glm::max(mx, b); }
    }
    if (outMin) *outMin = mn;
    if (outMax) *outMax = mx;
    return mn.x <= mx.x ? glm::length(mx - mn) : 0.0f;
}

int Depth(const Scene& s, int n) {
    int d = 0;
    for (int p = s.nodes[n].parent; p >= 0 && d < 100000; p = s.nodes[p].parent) ++d;
    return d;
}

} // namespace

TopologyInfo AnalyzeTopology(const Primitive& p, bool collectEdges) {
    TopologyInfo r;
    if (p.positions.empty()) return r;
    auto weld = WeldByPosition(p.positions);
    std::vector<char> used(p.positions.size(), 0);
    struct EdgeInfo {
        int count = 0, dirSum = 0;
        uint32_t va = 0, vb = 0;
    };
    std::unordered_map<uint64_t, EdgeInfo> edges;
    edges.reserve(p.indices.size());
    std::vector<std::array<uint32_t, 3>> tris;
    tris.reserve(p.indices.size() / 3);
    const bool hasN = p.HasNormals();
    for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
        uint32_t v[3] = { p.indices[i], p.indices[i + 1], p.indices[i + 2] };
        if (v[0] >= p.positions.size() || v[1] >= p.positions.size() || v[2] >= p.positions.size()) continue;
        ++r.triangles;
        for (auto x : v) used[x] = 1;
        uint32_t w[3] = { weld[v[0]], weld[v[1]], weld[v[2]] };
        const glm::vec3 &a = p.positions[v[0]], &b = p.positions[v[1]], &c = p.positions[v[2]];
        glm::vec3 fn = glm::cross(b - a, c - a);
        if (w[0] == w[1] || w[1] == w[2] || w[0] == w[2] || glm::dot(fn, fn) < 1e-24f) { ++r.degenerateTriangles; continue; }
        r.signedVolume += glm::dot(a, glm::cross(b, c)) / 6.0;
        if (hasN && glm::dot(p.normals[v[0]] + p.normals[v[1]] + p.normals[v[2]], fn) < 0.0f) ++r.normalsAgainstFaces;
        std::array<uint32_t, 3> key = { w[0], w[1], w[2] };
        std::sort(key.begin(), key.end());
        tris.push_back(key);
        for (int e = 0; e < 3; ++e) {
            uint32_t A = w[e], B = w[(e + 1) % 3];
            uint64_t k = ((uint64_t)std::min(A, B) << 32) | std::max(A, B);
            auto& ei = edges[k];
            if (ei.count == 0) { ei.va = v[e]; ei.vb = v[(e + 1) % 3]; }
            ++ei.count;
            ei.dirSum += A < B ? 1 : -1;
        }
    }
    for (auto& [k, ei] : edges) {
        if (ei.count == 1) {
            ++r.openEdges;
            if (collectEdges) { r.openEdgeVerts.push_back(ei.va); r.openEdgeVerts.push_back(ei.vb); }
        } else if (ei.count > 2) {
            ++r.nonManifoldEdges;
            if (collectEdges) { r.nonManifoldEdgeVerts.push_back(ei.va); r.nonManifoldEdgeVerts.push_back(ei.vb); }
        } else if (ei.dirSum != 0) {
            ++r.windingMismatch; // 正しく向きが揃っていれば、共有する 2 面で辺の向きが逆になる
        }
    }
    std::sort(tris.begin(), tris.end());
    for (size_t i = 1; i < tris.size(); ++i) if (tris[i] == tris[i - 1]) ++r.duplicateTriangles;
    for (auto u : used) if (!u) ++r.isolatedVertices;
    r.closed = r.triangles > 0 && r.openEdges == 0 && r.nonManifoldEdges == 0;
    return r;
}

UVInfo AnalyzeUV(const Primitive& p) {
    UVInfo r;
    if (!p.HasUV0()) return r;
    r.min = glm::vec2(1e30f);
    r.max = glm::vec2(-1e30f);
    for (auto& uv : p.uv0) {
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) { ++r.nan; continue; }
        r.min = glm::min(r.min, uv);
        r.max = glm::max(r.max, uv);
        if (uv.x < -1e-3f || uv.x > 1.001f || uv.y < -1e-3f || uv.y > 1.001f) ++r.outside01;
    }
    for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
        uint32_t a = p.indices[i], b = p.indices[i + 1], c = p.indices[i + 2];
        if (a >= p.uv0.size() || b >= p.uv0.size() || c >= p.uv0.size()) continue;
        glm::vec3 fn = glm::cross(p.positions[b] - p.positions[a], p.positions[c] - p.positions[a]);
        if (glm::dot(fn, fn) < 1e-24f) continue; // 形状として縮退している面は別に数える
        glm::vec2 e1 = p.uv0[b] - p.uv0[a], e2 = p.uv0[c] - p.uv0[a];
        if (std::abs(e1.x * e2.y - e1.y * e2.x) < 1e-12f) ++r.zeroAreaTris;
    }
    if (r.min.x > r.max.x) r.min = r.max = glm::vec2(0.0f);
    return r;
}

int FindRootMotionNode(const Scene& scene, const Animation& anim) {
    int best = -1, bestDepth = 1 << 30;
    for (auto& c : anim.channels) {
        if (c.path != AnimPath::Translation || c.node < 0 || c.node >= (int)scene.nodes.size() || c.values.size() < 2) continue;
        glm::vec3 mn(1e30f), mx(-1e30f);
        for (auto& v : c.values) { mn = glm::min(mn, glm::vec3(v)); mx = glm::max(mx, glm::vec3(v)); }
        if (glm::length(mx - mn) < 1e-5f) continue;
        int d = Depth(scene, c.node);
        if (d < bestDepth) { bestDepth = d; best = c.node; }
    }
    return best;
}

std::vector<glm::vec3> SampleNodeTrajectory(const Scene& scene, const Animation& anim, int node, float fps) {
    std::vector<glm::vec3> out;
    if (node < 0 || node >= (int)scene.nodes.size()) return out;
    int frames = std::clamp((int)std::ceil(anim.duration * fps), 1, 2000) + 1;
    for (int k = 0; k < frames; ++k) {
        float t = anim.duration * k / (frames - 1);
        auto pose = scene.RestPose();
        SampleAnimation(scene, anim, t, pose);
        out.push_back(glm::vec3(scene.ComputeWorldMatrices(pose)[node][3]));
    }
    return out;
}

AnimationReport AnalyzeAnimation(const Scene& scene, int ai, float fps) {
    AnimationReport r;
    if (ai < 0 || ai >= (int)scene.animations.size()) return r;
    const Animation& a = scene.animations[ai];
    r.duration = a.duration;
    r.channels = (int)a.channels.size();
    std::set<std::pair<int, int>> seen;
    std::set<int> animated;
    for (auto& c : a.channels) {
        r.keys += (int)c.times.size();
        if (c.node < 0 || c.node >= (int)scene.nodes.size()) { ++r.missingNodeChannels; continue; }
        animated.insert(c.node);
        if (!seen.insert({ c.node, (int)c.path }).second) ++r.duplicateChannels;
        for (float t : c.times) {
            if (!std::isfinite(t)) ++r.nanKeys;
            else if (t < 0) ++r.negativeTimes;
        }
        bool constant = true;
        for (size_t k = 0; k < c.values.size(); ++k) {
            const glm::vec4& v = c.values[k];
            if (!Finite4(v)) { ++r.nanKeys; continue; }
            if (k > 0 && glm::length(v - c.values[0]) > 1e-6f) constant = false;
            if (c.path == AnimPath::Rotation) {
                if (std::abs(glm::length(v) - 1.0f) > 1e-3f) ++r.unnormalizedQuats;
                if (k > 0 && glm::dot(c.values[k - 1], v) < 0.0f) ++r.quatFlips;
            } else if (c.path == AnimPath::Scale) {
                if (std::abs(v.x) < 1e-8f || std::abs(v.y) < 1e-8f || std::abs(v.z) < 1e-8f) ++r.zeroScaleKeys;
            }
        }
        if (constant && c.values.size() > 1) ++r.constantChannels;
    }

    const auto restWorld = scene.ComputeWorldMatrices();
    const float restDiag = std::max(RestDiagonal(scene, restWorld), 1e-6f);
    r.rootNode = FindRootMotionNode(scene, a);
    const int frames = std::clamp((int)std::ceil(a.duration * fps), 1, 600) + 1;
    const int boundsStride = std::max(1, frames / 60); // AABB は最大 60 フレームで調べる
    glm::vec3 prevRoot(0.0f);
    std::vector<glm::mat4> firstWorld, lastWorld;
    for (int k = 0; k < frames; ++k) {
        float t = frames > 1 ? a.duration * k / (frames - 1) : 0.0f;
        auto pose = scene.RestPose();
        SampleAnimation(scene, a, t, pose);
        auto world = scene.ComputeWorldMatrices(pose);
        if (k == 0) firstWorld = world;
        if (k == frames - 1) lastWorld = world;
        if (r.rootNode >= 0) {
            glm::vec3 p(world[r.rootNode][3]);
            if (k == 0) { r.rootMinY = r.rootMaxY = p.y; }
            else {
                float d = glm::length(p - prevRoot);
                r.rootPathLength += d;
                float dt = a.duration / (frames - 1);
                if (dt > 0) r.rootMaxSpeed = std::max(r.rootMaxSpeed, d / dt);
            }
            r.rootMinY = std::min(r.rootMinY, p.y);
            r.rootMaxY = std::max(r.rootMaxY, p.y);
            prevRoot = p;
        }
        if (k % boundsStride == 0 || k == frames - 1) {
            float ratio = RestDiagonal(scene, world) / restDiag;
            if (ratio > r.maxBoundsRatio) { r.maxBoundsRatio = ratio; r.maxBoundsTime = t; }
        }
    }
    if (r.rootNode >= 0 && !firstWorld.empty())
        r.rootDisplacement = glm::length(glm::vec3(lastWorld[r.rootNode][3]) - glm::vec3(firstWorld[r.rootNode][3]));
    // ループの継ぎ目: ルートから見た各ノードの位置が最初と最後で一致するか
    auto joints = scene.JointMask();
    if (!firstWorld.empty()) {
        glm::mat4 inv0 = r.rootNode >= 0 ? glm::inverse(firstWorld[r.rootNode]) : glm::mat4(1.0f);
        glm::mat4 inv1 = r.rootNode >= 0 ? glm::inverse(lastWorld[r.rootNode]) : glm::mat4(1.0f);
        for (int i = 0; i < (int)scene.nodes.size(); ++i) {
            if (!joints[i] && !animated.count(i)) continue;
            glm::vec3 p0(inv0 * firstWorld[i][3]), p1(inv1 * lastWorld[i][3]);
            r.loopError = std::max(r.loopError, glm::length(p1 - p0));
        }
    }
    return r;
}

int FixQuaternionKeys(Animation& anim) {
    int fixedCount = 0;
    for (auto& c : anim.channels) {
        if (c.path != AnimPath::Rotation) continue;
        for (size_t k = 0; k < c.values.size(); ++k) {
            glm::vec4& v = c.values[k];
            float l = glm::length(v);
            if (!std::isfinite(l) || l < 1e-12f) { v = glm::vec4(0, 0, 0, 1); ++fixedCount; continue; }
            if (std::abs(l - 1.0f) > 1e-6f) { v /= l; if (std::abs(l - 1.0f) > 1e-3f) ++fixedCount; }
            if (k > 0 && glm::dot(c.values[k - 1], v) < 0.0f) {
                v = -v;
                // 曲線の接線も同じ向きに反転する
                if (k < c.inTangents.size()) c.inTangents[k] = -c.inTangents[k];
                if (k < c.outTangents.size()) c.outTangents[k] = -c.outTangents[k];
                ++fixedCount;
            }
        }
    }
    return fixedCount;
}

float BindPoseDeviation(const Scene& scene, int node, size_t maxSamples) {
    if (node < 0 || node >= (int)scene.nodes.size()) return 0.0f;
    const Node& n = scene.nodes[node];
    if (n.mesh < 0 || n.mesh >= (int)scene.meshes.size()) return 0.0f;
    auto world = scene.ComputeWorldMatrices();
    auto jm = JointMatrices(scene, node, world);
    if (jm.empty()) return 0.0f;
    // 頂点がバインド空間 (glTF 準拠) でもメッシュノードの空間でも、ずれが小さい方を採用する
    float devBind = 0.0f, devNode = 0.0f;
    for (auto& p : scene.meshes[n.mesh].primitives) {
        if (!p.HasSkin()) continue;
        size_t stride = std::max<size_t>(1, p.positions.size() / std::max<size_t>(1, maxSamples));
        for (size_t v = 0; v < p.positions.size(); v += stride) {
            glm::vec3 s(VertexMatrix(p, v, jm, world[node]) * glm::vec4(p.positions[v], 1.0f));
            devBind = std::max(devBind, glm::length(s - p.positions[v]));
            devNode = std::max(devNode, glm::length(s - glm::vec3(world[node] * glm::vec4(p.positions[v], 1.0f))));
        }
    }
    return std::min(devBind, devNode);
}

std::vector<int> JointVertexCounts(const Scene& scene, int skin) {
    std::vector<int> counts;
    if (skin < 0 || skin >= (int)scene.skins.size()) return counts;
    counts.assign(scene.skins[skin].joints.size(), 0);
    std::set<int> meshes;
    for (auto& n : scene.nodes) if (n.skin == skin && n.mesh >= 0 && n.mesh < (int)scene.meshes.size()) meshes.insert(n.mesh);
    for (int m : meshes)
        for (auto& p : scene.meshes[m].primitives) {
            if (!p.HasSkin()) continue;
            for (size_t v = 0; v < p.positions.size(); ++v)
                for (int k = 0; k < 4; ++k)
                    if (p.weights[v][k] > 1e-4f && p.joints[v][k] < counts.size()) ++counts[p.joints[v][k]];
        }
    return counts;
}

void DeformPositions(const Scene& scene, const std::vector<glm::mat4>& world, int node, const Primitive& p, std::vector<glm::vec3>& out) {
    auto jm = JointMatrices(scene, node, world);
    out.resize(p.positions.size());
    for (size_t v = 0; v < p.positions.size(); ++v) out[v] = glm::vec3(VertexMatrix(p, v, jm, world[node]) * glm::vec4(p.positions[v], 1.0f));
}

glm::vec3 DeformedVertex(const Scene& scene, const std::vector<glm::mat4>& world, int node, const Primitive& p, uint32_t v, glm::vec3* normal) {
    auto jm = JointMatrices(scene, node, world);
    glm::mat4 M = VertexMatrix(p, v, jm, world[node]);
    if (normal) {
        glm::vec3 nv = v < p.normals.size() ? glm::mat3(glm::inverseTranspose(glm::mat3(M))) * p.normals[v] : glm::vec3(0.0f);
        float l = glm::length(nv);
        *normal = l > 0 ? nv / l : glm::vec3(0.0f);
    }
    return glm::vec3(M * glm::vec4(p.positions[v], 1.0f));
}

bool RaycastMeshes(const Scene& scene, const std::vector<glm::mat4>& world, const glm::vec3& o, const glm::vec3& dirIn, MeshHit& hit) {
    glm::vec3 d = glm::normalize(dirIn);
    // 祖先が非表示なら描画されない
    std::vector<char> visible(scene.nodes.size(), 1);
    for (int i = 0; i < (int)scene.nodes.size(); ++i)
        for (int p = i; p >= 0; p = scene.nodes[p].parent)
            if (!scene.nodes[p].visible) { visible[i] = 0; break; }
    bool any = false;
    hit.t = 1e30f;
    std::vector<glm::vec3> wp;
    for (int i = 0; i < (int)scene.nodes.size() && i < (int)world.size(); ++i) {
        const Node& n = scene.nodes[i];
        if (n.mesh < 0 || n.mesh >= (int)scene.meshes.size() || !visible[i]) continue;
        auto jm = JointMatrices(scene, i, world);
        const Mesh& mesh = scene.meshes[n.mesh];
        for (int pi = 0; pi < (int)mesh.primitives.size(); ++pi) {
            const Primitive& p = mesh.primitives[pi];
            wp.resize(p.positions.size());
            for (size_t v = 0; v < p.positions.size(); ++v) wp[v] = glm::vec3(VertexMatrix(p, v, jm, world[i]) * glm::vec4(p.positions[v], 1.0f));
            for (size_t k = 0; k + 2 < p.indices.size(); k += 3) {
                uint32_t a = p.indices[k], b = p.indices[k + 1], c = p.indices[k + 2];
                if (a >= wp.size() || b >= wp.size() || c >= wp.size()) continue;
                // Moller-Trumbore (両面)
                glm::vec3 e1 = wp[b] - wp[a], e2 = wp[c] - wp[a];
                glm::vec3 pv = glm::cross(d, e2);
                float det = glm::dot(e1, pv);
                if (std::abs(det) < 1e-20f) continue;
                float inv = 1.0f / det;
                glm::vec3 tv = o - wp[a];
                float u = glm::dot(tv, pv) * inv;
                if (u < 0 || u > 1) continue;
                glm::vec3 qv = glm::cross(tv, e1);
                float v = glm::dot(d, qv) * inv;
                if (v < 0 || u + v > 1) continue;
                float t = glm::dot(e2, qv) * inv;
                if (t <= 0 || t >= hit.t) continue;
                hit.t = t;
                hit.node = i;
                hit.prim = pi;
                hit.tri = (uint32_t)(k / 3);
                hit.bary = glm::vec3(1 - u - v, u, v);
                hit.pos = o + d * t;
                hit.vertex = hit.bary.x >= hit.bary.y && hit.bary.x >= hit.bary.z ? a : hit.bary.y >= hit.bary.z ? b : c;
                any = true;
            }
        }
    }
    return any;
}

bool AverageTextureColor(const Texture& tex, glm::vec4& avg, float* minAlpha) {
    if (!tex.image || tex.image->width <= 0 || tex.image->height <= 0) return false;
    const auto& px = tex.image->rgba;
    size_t count = (size_t)tex.image->width * tex.image->height;
    if (px.size() < count * 4) return false;
    size_t stride = std::max<size_t>(1, count / 65536);
    glm::dvec4 sum(0.0);
    double n = 0;
    float amin = 1.0f;
    for (size_t i = 0; i < count; i += stride) {
        glm::dvec4 c(px[i * 4], px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]);
        sum += c / 255.0;
        amin = std::min(amin, (float)(c.w / 255.0));
        ++n;
    }
    avg = glm::vec4(sum / std::max(n, 1.0));
    if (minAlpha) *minAlpha = amin;
    return true;
}

bool LooksLikeNormalMap(const glm::vec4& avg) {
    return avg.b > 0.7f && std::abs(avg.r - 0.5f) < 0.12f && std::abs(avg.g - 0.5f) < 0.12f;
}

bool HasNonAscii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return true;
    return false;
}
