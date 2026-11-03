// tinygltf による glTF 2.0 / GLB 書き出し
#include "Exporter.h"
#include "ImageIO.h"
#include "core/Log.h"
#include "util/FileUtil.h"
#include <tiny_gltf.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace {

struct GltfWriter {
    const Scene& scene;
    const ExportOptions& opts;
    tinygltf::Model model;
    std::vector<unsigned char>* bin = nullptr;
    std::vector<int> textureMap; // Scene テクスチャ -> glTF テクスチャ
    std::string outDir;
    bool binary = false;
    bool embedImages = false;

    GltfWriter(const Scene& s, const ExportOptions& o) : scene(s), opts(o) {}

    void Align4() {
        while (bin->size() % 4) bin->push_back(0);
    }

    int AddView(const void* data, size_t bytes, int target, int stride = 0) {
        Align4();
        tinygltf::BufferView v;
        v.buffer = 0;
        v.byteOffset = bin->size();
        v.byteLength = bytes;
        v.target = target;
        if (stride) v.byteStride = stride;
        const unsigned char* p = (const unsigned char*)data;
        bin->insert(bin->end(), p, p + bytes);
        model.bufferViews.push_back(v);
        return (int)model.bufferViews.size() - 1;
    }

    template<class T>
    int AddAccessor(const std::vector<T>& data, int componentType, int type, int target,
                    bool minmax = false, bool normalized = false) {
        if (data.empty()) return -1;
        tinygltf::Accessor a;
        a.bufferView = AddView(data.data(), data.size() * sizeof(T), target);
        a.byteOffset = 0;
        a.componentType = componentType;
        a.type = type;
        a.count = data.size();
        a.normalized = normalized;
        if (minmax && componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
            const int comps = (int)(sizeof(T) / sizeof(float));
            a.minValues.assign(comps, std::numeric_limits<double>::max());
            a.maxValues.assign(comps, -std::numeric_limits<double>::max());
            for (const T& e : data) {
                const float* f = (const float*)&e;
                for (int c = 0; c < comps; ++c) {
                    a.minValues[c] = std::min(a.minValues[c], (double)f[c]);
                    a.maxValues[c] = std::max(a.maxValues[c], (double)f[c]);
                }
            }
        }
        model.accessors.push_back(a);
        return (int)model.accessors.size() - 1;
    }

    // ---- 画像 / テクスチャ -------------------------------------------------
    void WriteTextures() {
        textureMap.assign(scene.textures.size(), -1);
        std::set<std::string> usedNames;
        for (size_t i = 0; i < scene.textures.size(); ++i) {
            const Texture& t = scene.textures[i];
            std::string mime;
            std::vector<uint8_t> bytes = ImageIO::GetEncodedBytes(t, mime);
            if (bytes.empty()) {
                LOG_WARN("テクスチャ '%s' はデータが無いため書き出しません", t.name.c_str());
                continue;
            }
            tinygltf::Image img;
            img.name = t.name;
            img.mimeType = mime;
            if (binary || embedImages) {
                img.bufferView = AddView(bytes.data(), bytes.size(), 0);
            } else {
                std::string base = FileUtil::SanitizeFilename(t.name.empty() ? "texture_" + std::to_string(i) : t.name);
                std::string fname = base + ImageIO::ExtensionFromMime(mime);
                int k = 1;
                while (usedNames.count(fname)) fname = base + "_" + std::to_string(k++) + ImageIO::ExtensionFromMime(mime);
                usedNames.insert(fname);
                std::string rel = opts.textureSubdir.empty() ? fname : opts.textureSubdir + "/" + fname;
                std::string full = FileUtil::Join(outDir, rel);
                if (!opts.textureSubdir.empty()) FileUtil::CreateDirs(FileUtil::Join(outDir, opts.textureSubdir));
                if (!FileUtil::WriteBinary(full, bytes.data(), bytes.size()))
                    LOG_ERROR("画像を書き込めません: %s", full.c_str());
                img.uri = rel;
            }
            model.images.push_back(img);

            tinygltf::Sampler s;
            s.wrapS = (int)t.wrapS;
            s.wrapT = (int)t.wrapT;
            s.magFilter = t.linearFilter ? TINYGLTF_TEXTURE_FILTER_LINEAR : TINYGLTF_TEXTURE_FILTER_NEAREST;
            s.minFilter = t.linearFilter ? (t.generateMips ? TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR : TINYGLTF_TEXTURE_FILTER_LINEAR)
                                         : (t.generateMips ? TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST : TINYGLTF_TEXTURE_FILTER_NEAREST);
            model.samplers.push_back(s);

            tinygltf::Texture tex;
            tex.name = t.name;
            tex.source = (int)model.images.size() - 1;
            tex.sampler = (int)model.samplers.size() - 1;
            model.textures.push_back(tex);
            textureMap[i] = (int)model.textures.size() - 1;
        }
    }

    int MapTex(const TextureSlot& s) const {
        if (s.texture < 0 || s.texture >= (int)textureMap.size()) return -1;
        return textureMap[s.texture];
    }

    void WriteMaterials() {
        bool usesUnlit = false, usesStrength = false;
        for (const Material& m : scene.materials) {
            tinygltf::Material g;
            g.name = m.name;
            g.pbrMetallicRoughness.baseColorFactor = { m.baseColor.r, m.baseColor.g, m.baseColor.b, m.baseColor.a };
            g.pbrMetallicRoughness.metallicFactor = m.metallic;
            g.pbrMetallicRoughness.roughnessFactor = m.roughness;
            if (int t = MapTex(m.slots[Slot_BaseColor]); t >= 0) {
                g.pbrMetallicRoughness.baseColorTexture.index = t;
                g.pbrMetallicRoughness.baseColorTexture.texCoord = m.slots[Slot_BaseColor].uvSet;
            }
            if (int t = MapTex(m.slots[Slot_MetallicRoughness]); t >= 0) {
                g.pbrMetallicRoughness.metallicRoughnessTexture.index = t;
                g.pbrMetallicRoughness.metallicRoughnessTexture.texCoord = m.slots[Slot_MetallicRoughness].uvSet;
            }
            if (int t = MapTex(m.slots[Slot_Normal]); t >= 0) {
                g.normalTexture.index = t;
                g.normalTexture.texCoord = m.slots[Slot_Normal].uvSet;
                g.normalTexture.scale = m.normalScale;
            }
            if (int t = MapTex(m.slots[Slot_Occlusion]); t >= 0) {
                g.occlusionTexture.index = t;
                g.occlusionTexture.texCoord = m.slots[Slot_Occlusion].uvSet;
                g.occlusionTexture.strength = m.occlusionStrength;
            }
            if (int t = MapTex(m.slots[Slot_Emissive]); t >= 0) {
                g.emissiveTexture.index = t;
                g.emissiveTexture.texCoord = m.slots[Slot_Emissive].uvSet;
            }
            g.emissiveFactor = { std::clamp(m.emissive.r, 0.0f, 1.0f), std::clamp(m.emissive.g, 0.0f, 1.0f), std::clamp(m.emissive.b, 0.0f, 1.0f) };
            // 1 を超える発光色は strength に逃がす
            float maxE = std::max(m.emissive.r, std::max(m.emissive.g, m.emissive.b));
            float strength = m.emissiveStrength;
            if (maxE > 1.0f) {
                g.emissiveFactor = { m.emissive.r / maxE, m.emissive.g / maxE, m.emissive.b / maxE };
                strength *= maxE;
            }
            if (strength > 1.0f + 1e-5f) {
                tinygltf::Value::Object o;
                o["emissiveStrength"] = tinygltf::Value((double)strength);
                g.extensions["KHR_materials_emissive_strength"] = tinygltf::Value(o);
                usesStrength = true;
            }
            g.alphaMode = m.alphaMode == AlphaMode::Mask ? "MASK" : m.alphaMode == AlphaMode::Blend ? "BLEND" : "OPAQUE";
            g.alphaCutoff = m.alphaCutoff;
            g.doubleSided = m.doubleSided;
            if (m.unlit) {
                g.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
                usesUnlit = true;
            }
            model.materials.push_back(g);
        }
        if (usesUnlit) model.extensionsUsed.push_back("KHR_materials_unlit");
        if (usesStrength) model.extensionsUsed.push_back("KHR_materials_emissive_strength");
    }

    // ---- メッシュ ---------------------------------------------------------
    void WriteMeshes() {
        for (const Mesh& mesh : scene.meshes) {
            tinygltf::Mesh gm;
            gm.name = mesh.name;
            const int targetCount = opts.exportMorphTargets ? mesh.MorphTargetCount() : 0;
            for (const Primitive& p : mesh.primitives) {
                if (p.positions.empty() || p.indices.empty()) continue;
                tinygltf::Primitive gp;
                gp.mode = TINYGLTF_MODE_TRIANGLES;
                gp.material = (p.material >= 0 && p.material < (int)scene.materials.size()) ? p.material : -1;
                gp.attributes["POSITION"] = AddAccessor(p.positions, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, TINYGLTF_TARGET_ARRAY_BUFFER, true);
                if (p.HasNormals()) gp.attributes["NORMAL"] = AddAccessor(p.normals, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, TINYGLTF_TARGET_ARRAY_BUFFER);
                if (p.HasTangents() && p.HasNormals()) gp.attributes["TANGENT"] = AddAccessor(p.tangents, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4, TINYGLTF_TARGET_ARRAY_BUFFER);
                if (p.HasUV0()) gp.attributes["TEXCOORD_0"] = AddAccessor(p.uv0, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, TINYGLTF_TARGET_ARRAY_BUFFER);
                if (p.HasUV1()) gp.attributes["TEXCOORD_1"] = AddAccessor(p.uv1, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, TINYGLTF_TARGET_ARRAY_BUFFER);
                if (p.HasColors()) gp.attributes["COLOR_0"] = AddAccessor(p.colors, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4, TINYGLTF_TARGET_ARRAY_BUFFER);
                if (p.HasSkin()) {
                    gp.attributes["JOINTS_0"] = AddAccessor(p.joints, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC4, TINYGLTF_TARGET_ARRAY_BUFFER);
                    gp.attributes["WEIGHTS_0"] = AddAccessor(p.weights, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4, TINYGLTF_TARGET_ARRAY_BUFFER);
                }
                if (p.positions.size() <= 65535) {
                    std::vector<uint16_t> idx(p.indices.begin(), p.indices.end());
                    gp.indices = AddAccessor(idx, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_SCALAR, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
                } else {
                    gp.indices = AddAccessor(p.indices, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
                }
                // モーフターゲット (全プリミティブで数を揃える)
                for (int t = 0; t < targetCount; ++t) {
                    std::map<std::string, int> tg;
                    std::vector<glm::vec3> dp(p.positions.size(), glm::vec3(0));
                    if (t < (int)p.targets.size() && p.targets[t].dPositions.size() == p.positions.size()) dp = p.targets[t].dPositions;
                    tg["POSITION"] = AddAccessor(dp, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, TINYGLTF_TARGET_ARRAY_BUFFER, true);
                    if (p.HasNormals() && t < (int)p.targets.size() && p.targets[t].dNormals.size() == p.positions.size())
                        tg["NORMAL"] = AddAccessor(p.targets[t].dNormals, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, TINYGLTF_TARGET_ARRAY_BUFFER);
                    gp.targets.push_back(tg);
                }
                gm.primitives.push_back(gp);
            }
            if (targetCount) {
                gm.weights.resize(targetCount, 0.0);
                for (int t = 0; t < targetCount && t < (int)mesh.morphWeights.size(); ++t) gm.weights[t] = mesh.morphWeights[t];
                tinygltf::Value::Array names;
                for (int t = 0; t < targetCount; ++t) {
                    std::string n = "target_" + std::to_string(t);
                    for (auto& p : mesh.primitives) if (t < (int)p.targets.size() && !p.targets[t].name.empty()) { n = p.targets[t].name; break; }
                    names.push_back(tinygltf::Value(n));
                }
                tinygltf::Value::Object extras;
                extras["targetNames"] = tinygltf::Value(names);
                gm.extras = tinygltf::Value(extras);
            }
            if (gm.primitives.empty()) {
                // 空のメッシュは glTF では不正なのでダミーを入れない代わりに警告
                LOG_WARN("メッシュ '%s' は空のため参照を外します", mesh.name.c_str());
            }
            model.meshes.push_back(gm);
        }
    }

    // ---- ノード / スキン / アニメーション ---------------------------------
    void WriteNodes() {
        for (size_t i = 0; i < scene.nodes.size(); ++i) {
            const Node& n = scene.nodes[i];
            tinygltf::Node g;
            g.name = n.name;
            if (glm::length(n.translation) > 0) g.translation = { n.translation.x, n.translation.y, n.translation.z };
            glm::quat q = glm::normalize(n.rotation);
            if (std::abs(q.w - 1.0f) > 1e-7f) g.rotation = { q.x, q.y, q.z, q.w };
            if (n.scale != glm::vec3(1)) g.scale = { n.scale.x, n.scale.y, n.scale.z };
            if (n.mesh >= 0 && n.mesh < (int)model.meshes.size() && !model.meshes[n.mesh].primitives.empty()) {
                g.mesh = n.mesh;
                if (n.skin >= 0 && n.skin < (int)scene.skins.size()) {
                    bool meshSkinned = false;
                    for (auto& p : scene.meshes[n.mesh].primitives) if (p.HasSkin()) meshSkinned = true;
                    if (meshSkinned) g.skin = n.skin;
                }
            }
            g.children = n.children;
            model.nodes.push_back(g);
        }
        tinygltf::Scene s;
        s.name = "Scene";
        s.nodes = scene.roots;
        model.scenes.push_back(s);
        model.defaultScene = 0;
    }

    void WriteSkins() {
        for (const Skin& s : scene.skins) {
            tinygltf::Skin g;
            g.name = s.name;
            g.joints = s.joints;
            if (s.skeleton >= 0) g.skeleton = s.skeleton;
            std::vector<glm::mat4> ibm = s.inverseBind;
            ibm.resize(s.joints.size(), glm::mat4(1));
            g.inverseBindMatrices = AddAccessor(ibm, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_MAT4, 0);
            model.skins.push_back(g);
        }
    }

    void WriteAnimations() {
        for (const Animation& a : scene.animations) {
            tinygltf::Animation g;
            g.name = a.name;
            for (const AnimChannel& ch : a.channels) {
                if (ch.node < 0 || ch.times.empty() || ch.values.size() < ch.times.size()) continue;
                tinygltf::AnimationSampler s;
                s.input = AddAccessor(ch.times, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_SCALAR, 0, true);
                if (ch.IsCubic()) {
                    // 曲線: glTF の CUBICSPLINE (キーごとに 入る側の接線 / 値 / 出る側の接線 の 3 つを並べる)
                    const size_t n = ch.times.size();
                    if (ch.path == AnimPath::Rotation) {
                        std::vector<glm::vec4> out;
                        for (size_t k = 0; k < n; ++k) { out.push_back(ch.inTangents[k]); out.push_back(ch.values[k]); out.push_back(ch.outTangents[k]); }
                        s.output = AddAccessor(out, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4, 0);
                    } else {
                        std::vector<glm::vec3> out;
                        for (size_t k = 0; k < n; ++k) {
                            out.push_back(glm::vec3(ch.inTangents[k]));
                            out.push_back(glm::vec3(ch.values[k]));
                            out.push_back(glm::vec3(ch.outTangents[k]));
                        }
                        s.output = AddAccessor(out, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, 0);
                    }
                    s.interpolation = "CUBICSPLINE";
                    g.samplers.push_back(s);
                    tinygltf::AnimationChannel c;
                    c.sampler = (int)g.samplers.size() - 1;
                    c.target_node = ch.node;
                    c.target_path = ch.path == AnimPath::Translation ? "translation" : ch.path == AnimPath::Rotation ? "rotation" : "scale";
                    g.channels.push_back(c);
                    continue;
                }
                if (ch.path == AnimPath::Rotation) {
                    std::vector<glm::vec4> q(ch.values.begin(), ch.values.begin() + ch.times.size());
                    for (auto& v : q) { float l = glm::length(v); v = l > 0 ? v / l : glm::vec4(0, 0, 0, 1); }
                    s.output = AddAccessor(q, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4, 0);
                } else {
                    std::vector<glm::vec3> v;
                    for (size_t k = 0; k < ch.times.size(); ++k) v.push_back(glm::vec3(ch.values[k]));
                    s.output = AddAccessor(v, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, 0);
                }
                s.interpolation = ch.interp == Interp::Step ? "STEP" : "LINEAR";
                g.samplers.push_back(s);
                tinygltf::AnimationChannel c;
                c.sampler = (int)g.samplers.size() - 1;
                c.target_node = ch.node;
                c.target_path = ch.path == AnimPath::Translation ? "translation" : ch.path == AnimPath::Rotation ? "rotation" : "scale";
                g.channels.push_back(c);
            }
            if (!g.channels.empty()) model.animations.push_back(g);
        }
    }
};

} // namespace

bool ExportGltf(const Scene& scene, const std::string& path, const ExportOptions& opts, std::string& err) {
    GltfWriter w(scene, opts);
    w.binary = opts.format == ExportFormat::GLB;
    w.embedImages = opts.format == ExportFormat::GLTF_Embedded;
    w.outDir = FileUtil::Directory(path);
    w.model.asset.version = "2.0";
    w.model.asset.generator = "ModelEditor (ImGui + Assimp + tinygltf)";
    w.model.buffers.resize(1);
    w.bin = &w.model.buffers[0].data;

    w.WriteTextures();
    w.WriteMaterials();
    w.WriteMeshes();
    w.WriteNodes();
    if (opts.exportSkins) w.WriteSkins();
    if (opts.exportAnimations) w.WriteAnimations();
    w.Align4();
    if (w.bin->empty()) w.model.buffers.clear();

    tinygltf::TinyGLTF gltf;
    gltf.SetImageWriter(nullptr, nullptr); // 画像は自前で処理済み
    bool embedBuffers = opts.format != ExportFormat::GLTF;
    if (opts.format == ExportFormat::GLTF && !w.model.buffers.empty())
        w.model.buffers[0].uri = FileUtil::Stem(path) + ".bin";
    if (!gltf.WriteGltfSceneToFile(&w.model, path, false, embedBuffers, opts.prettyPrintJson, w.binary)) {
        err = "tinygltf: ファイルの書き込みに失敗しました: " + path;
        return false;
    }
    return true;
}

bool VerifyGltf(const std::string& path, std::string& report) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    bool ok = FileUtil::LowerExt(path) == ".glb" ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
                                                 : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    char buf[512];
    snprintf(buf, sizeof(buf), "nodes=%zu meshes=%zu materials=%zu textures=%zu images=%zu skins=%zu animations=%zu accessors=%zu",
             model.nodes.size(), model.meshes.size(), model.materials.size(), model.textures.size(), model.images.size(),
             model.skins.size(), model.animations.size(), model.accessors.size());
    report = buf;
    // 簡単な整合性チェック
    int problems = 0;
    for (auto& m : model.meshes)
        for (auto& p : m.primitives) {
            auto it = p.attributes.find("POSITION");
            if (it == p.attributes.end()) { report += "\n  POSITION の無いプリミティブ"; ++problems; continue; }
            size_t count = model.accessors[it->second].count;
            for (auto& [name, acc] : p.attributes)
                if (model.accessors[acc].count != count) { report += "\n  属性 " + name + " の要素数が不一致 (" + m.name + ")"; ++problems; }
        }
    for (auto& img : model.images)
        if (img.image.empty()) { report += "\n  画像 '" + img.name + "' を読み込めません"; ++problems; }
    if (!warn.empty()) report += "\n  warn: " + warn;
    if (!err.empty()) report += "\n  err: " + err;
    return ok && problems == 0 && err.empty();
}
