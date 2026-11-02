#include "SceneDump.h"
#include <glm/gtc/quaternion.hpp>
#include <cstdarg>
#include <cstdio>
#include <functional>

static void Appendf(std::string& s, const char* fmt, ...) {
    char buf[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    s += buf;
}

static std::string JsonEscape(const std::string& in) {
    std::string o;
    for (char c : in) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += c;
        }
    }
    return o;
}

std::string DumpSceneText(const Scene& scene) {
    std::string s;
    Appendf(s, "Source: %s (%s)\n", scene.info.sourcePath.c_str(), scene.info.sourceFormat.c_str());
    Appendf(s, "Nodes %zu / Meshes %zu / Materials %zu / Textures %zu / Skins %zu / Animations %zu\n",
            scene.nodes.size(), scene.meshes.size(), scene.materials.size(), scene.textures.size(), scene.skins.size(), scene.animations.size());
    Appendf(s, "Vertices %zu / Triangles %zu\n\n[Hierarchy]\n", scene.TotalVertices(), scene.TotalTriangles());
    auto joints = scene.JointMask();
    std::function<void(int, int)> rec = [&](int i, int depth) {
        const Node& n = scene.nodes[i];
        glm::vec3 e = glm::degrees(glm::eulerAngles(n.rotation));
        Appendf(s, "%*s- [%d] %s%s  T(%.4g %.4g %.4g) R(%.3g %.3g %.3g) S(%.4g %.4g %.4g)", depth * 2, "", i, n.name.c_str(),
                joints[i] ? " (joint)" : "", n.translation.x, n.translation.y, n.translation.z, e.x, e.y, e.z, n.scale.x, n.scale.y, n.scale.z);
        if (n.mesh >= 0) Appendf(s, " mesh=%d", n.mesh);
        if (n.skin >= 0) Appendf(s, " skin=%d", n.skin);
        s += "\n";
        for (int c : n.children) rec(c, depth + 1);
    };
    for (int r : scene.roots) rec(r, 0);

    s += "\n[Meshes]\n";
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const Mesh& m = scene.meshes[i];
        Appendf(s, "[%zu] %s  prims=%zu verts=%zu tris=%zu morphs=%d\n", i, m.name.c_str(), m.primitives.size(), m.VertexCount(), m.TriangleCount(), m.MorphTargetCount());
        for (size_t p = 0; p < m.primitives.size(); ++p) {
            const Primitive& pr = m.primitives[p];
            Appendf(s, "    prim %zu: v=%zu t=%zu mat=%d attrs:%s%s%s%s%s%s%s\n", p, pr.VertexCount(), pr.TriangleCount(), pr.material,
                    pr.HasNormals() ? " N" : "", pr.HasTangents() ? " T" : "", pr.HasUV0() ? " UV0" : "", pr.HasUV1() ? " UV1" : "",
                    pr.HasColors() ? " COL" : "", pr.HasSkin() ? " SKIN" : "", pr.targets.empty() ? "" : " MORPH");
        }
    }
    s += "\n[Materials]\n";
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const Material& m = scene.materials[i];
        Appendf(s, "[%zu] %s base(%.3g %.3g %.3g %.3g) metal=%.3g rough=%.3g alpha=%d%s%s\n", i, m.name.c_str(), m.baseColor.r, m.baseColor.g,
                m.baseColor.b, m.baseColor.a, m.metallic, m.roughness, (int)m.alphaMode, m.doubleSided ? " doubleSided" : "", m.unlit ? " unlit" : "");
        for (int t = 0; t < Slot_Count; ++t)
            if (m.slots[t].texture >= 0) Appendf(s, "    %s -> tex %d (uv%d)\n", TexSlotName(t), m.slots[t].texture, m.slots[t].uvSet);
    }
    s += "\n[Textures]\n";
    for (size_t i = 0; i < scene.textures.size(); ++i) {
        const Texture& t = scene.textures[i];
        Appendf(s, "[%zu] %s %dx%d %s %s%s uri=%s\n", i, t.name.c_str(), t.Width(), t.Height(), t.mimeType.c_str(),
                t.embedded ? "embedded" : t.sourcePath.c_str(), t.missing ? " MISSING" : "", t.originalUri.c_str());
    }
    s += "\n[Skins]\n";
    for (size_t i = 0; i < scene.skins.size(); ++i) {
        const Skin& sk = scene.skins[i];
        Appendf(s, "[%zu] %s joints=%zu skeleton=%d\n", i, sk.name.c_str(), sk.joints.size(), sk.skeleton);
    }
    s += "\n[Animations]\n";
    for (size_t i = 0; i < scene.animations.size(); ++i) {
        const Animation& a = scene.animations[i];
        Appendf(s, "[%zu] %s duration=%.3fs channels=%zu\n", i, a.name.c_str(), a.duration, a.channels.size());
    }
    if (!scene.info.metadata.empty()) {
        s += "\n[Metadata]\n";
        for (auto& [k, v] : scene.info.metadata) Appendf(s, "%s = %s\n", k.c_str(), v.c_str());
    }
    return s;
}

std::string DumpSceneJson(const Scene& scene) {
    std::string s = "{\n  \"nodes\": [\n";
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        const Node& n = scene.nodes[i];
        Appendf(s, "    {\"index\": %zu, \"name\": \"%s\", \"parent\": %d, \"mesh\": %d, \"skin\": %d, "
                   "\"t\": [%g, %g, %g], \"r\": [%g, %g, %g, %g], \"s\": [%g, %g, %g], \"children\": [",
                i, JsonEscape(n.name).c_str(), n.parent, n.mesh, n.skin, n.translation.x, n.translation.y, n.translation.z,
                n.rotation.x, n.rotation.y, n.rotation.z, n.rotation.w, n.scale.x, n.scale.y, n.scale.z);
        for (size_t c = 0; c < n.children.size(); ++c) Appendf(s, "%s%d", c ? ", " : "", n.children[c]);
        Appendf(s, "]}%s\n", i + 1 < scene.nodes.size() ? "," : "");
    }
    s += "  ],\n  \"meshes\": [\n";
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const Mesh& m = scene.meshes[i];
        Appendf(s, "    {\"name\": \"%s\", \"vertices\": %zu, \"triangles\": %zu, \"primitives\": %zu}%s\n", JsonEscape(m.name).c_str(),
                m.VertexCount(), m.TriangleCount(), m.primitives.size(), i + 1 < scene.meshes.size() ? "," : "");
    }
    s += "  ],\n  \"materials\": [\n";
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const Material& m = scene.materials[i];
        Appendf(s, "    {\"name\": \"%s\", \"baseColor\": [%g, %g, %g, %g], \"metallic\": %g, \"roughness\": %g, \"textures\": [",
                JsonEscape(m.name).c_str(), m.baseColor.r, m.baseColor.g, m.baseColor.b, m.baseColor.a, m.metallic, m.roughness);
        for (int t = 0; t < Slot_Count; ++t) Appendf(s, "%s%d", t ? ", " : "", m.slots[t].texture);
        Appendf(s, "]}%s\n", i + 1 < scene.materials.size() ? "," : "");
    }
    s += "  ],\n  \"textures\": [\n";
    for (size_t i = 0; i < scene.textures.size(); ++i) {
        const Texture& t = scene.textures[i];
        Appendf(s, "    {\"name\": \"%s\", \"width\": %d, \"height\": %d, \"uri\": \"%s\", \"missing\": %s}%s\n", JsonEscape(t.name).c_str(),
                t.Width(), t.Height(), JsonEscape(t.originalUri).c_str(), t.missing ? "true" : "false", i + 1 < scene.textures.size() ? "," : "");
    }
    s += "  ],\n  \"skins\": [\n";
    for (size_t i = 0; i < scene.skins.size(); ++i) {
        const Skin& sk = scene.skins[i];
        Appendf(s, "    {\"name\": \"%s\", \"skeleton\": %d, \"joints\": [", JsonEscape(sk.name).c_str(), sk.skeleton);
        for (size_t j = 0; j < sk.joints.size(); ++j) Appendf(s, "%s%d", j ? ", " : "", sk.joints[j]);
        Appendf(s, "]}%s\n", i + 1 < scene.skins.size() ? "," : "");
    }
    s += "  ],\n  \"animations\": [\n";
    for (size_t i = 0; i < scene.animations.size(); ++i) {
        const Animation& a = scene.animations[i];
        Appendf(s, "    {\"name\": \"%s\", \"duration\": %g, \"channels\": %zu}%s\n", JsonEscape(a.name).c_str(), a.duration,
                a.channels.size(), i + 1 < scene.animations.size() ? "," : "");
    }
    s += "  ]\n}\n";
    return s;
}
