#include "Importer.h"
#include "ImageIO.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include "core/Units.h"
#include "io/Exporter.h"
#include "util/FileUtil.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <assimp/GltfMaterial.h>
#include <assimp/version.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <unordered_map>

static glm::mat4 ToGlm(const aiMatrix4x4& a) {
    // aiMatrix4x4 は行優先, glm は列優先
    return glm::mat4(a.a1, a.b1, a.c1, a.d1,
                     a.a2, a.b2, a.c2, a.d2,
                     a.a3, a.b3, a.c3, a.d3,
                     a.a4, a.b4, a.c4, a.d4);
}

// Assimp が読める拡張子 (".fbx" の形、小文字)。VRM は GLB として読む
const std::vector<std::string>& SupportedModelExtensions() {
    static const std::vector<std::string> exts = [] {
        std::vector<std::string> v;
        aiString list;
        Assimp::Importer().GetExtensionList(list); // "*.3ds;*.obj;..."
        std::string s = list.C_Str();
        size_t p = 0;
        while (p <= s.size()) {
            size_t q = s.find(';', p);
            if (q == std::string::npos) q = s.size();
            std::string e = s.substr(p, q - p);
            if (!e.empty() && e[0] == '*') e.erase(0, 1);
            std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (e.size() > 1 && e[0] == '.') v.push_back(e);
            p = q + 1;
        }
        v.push_back(".vrm");
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }();
    return exts;
}

bool IsSupportedModelExtension(const std::string& ext) {
    const auto& exts = SupportedModelExtensions();
    return std::binary_search(exts.begin(), exts.end(), ext);
}

const char* ImportFileFilter() {
    static const std::string filter = [] {
        std::string f;
        auto add = [&](const std::string& label, const std::string& pattern) {
            f += label;
            f.push_back('\0');
            f += pattern;
            f.push_back('\0');
        };
        std::string all;
        for (auto& e : SupportedModelExtensions()) all += (all.empty() ? "*" : ";*") + e;
        add("3D モデル (対応形式すべて)", all);
        // よく使う形式 (このビルドで読めるものだけ)
        static const std::pair<const char*, const char*> groups[] = {
            { "FBX (*.fbx)", ".fbx" }, { "glTF / GLB / VRM (*.gltf;*.glb;*.vrm)", ".gltf;.glb;.vrm" }, { "Wavefront OBJ (*.obj)", ".obj" },
            { "Blender (*.blend)", ".blend" }, { "Collada (*.dae)", ".dae" }, { "3ds Max 3DS (*.3ds)", ".3ds" }, { "DirectX (*.x)", ".x" },
            { "3MF (*.3mf)", ".3mf" }, { "STL (*.stl)", ".stl" }, { "PLY (*.ply)", ".ply" }, { "BVH モーション (*.bvh)", ".bvh" },
        };
        for (auto& [label, list] : groups) {
            std::string pat, l = list;
            bool any = false;
            for (size_t p = 0; p < l.size();) {
                size_t q = l.find(';', p);
                if (q == std::string::npos) q = l.size();
                std::string e = l.substr(p, q - p);
                if (IsSupportedModelExtension(e)) { pat += (pat.empty() ? "*" : ";*") + e; any = true; }
                p = q + 1;
            }
            if (any) add(label, pat);
        }
        add("すべてのファイル (*.*)", "*.*");
        return f; // 末尾は c_str() の終端と合わせて \0\0 になる
    }();
    return filter.c_str();
}

namespace {

struct Converter {
    const aiScene* ai = nullptr;
    const ImportOptions& opts;
    Scene& scene;
    std::string modelDir;
    std::map<std::string, int> textureByKey;
    std::unordered_map<std::string, int> nodeByName;
    std::vector<const aiNode*> aiNodes;

    Converter(const ImportOptions& o, Scene& s) : opts(o), scene(s) {}

    // ---- ノード -----------------------------------------------------------
    int ConvertNode(const aiNode* an, int parent) {
        int idx = scene.AddNode(an->mName.C_Str(), parent);
        scene.nodes[idx].SetLocalMatrix(ToGlm(an->mTransformation));
        aiNodes.push_back(an);
        if (!nodeByName.count(scene.nodes[idx].name)) nodeByName[scene.nodes[idx].name] = idx;
        for (unsigned i = 0; i < an->mNumChildren; ++i) ConvertNode(an->mChildren[i], idx);
        return idx;
    }

    // ---- テクスチャ -------------------------------------------------------
    int GetTexture(const aiString& aiPath) {
        std::string path = aiPath.C_Str();
        if (path.empty()) return -1;
        const aiTexture* emb = ai->GetEmbeddedTexture(path.c_str());
        std::string key;
        std::string resolved;
        if (emb) {
            key = "embedded:" + path;
        } else {
            resolved = FileUtil::ResolveTexturePath(path, modelDir, opts.textureSearchDirs);
            key = resolved.empty() ? "missing:" + path : resolved;
        }
        auto it = textureByKey.find(key);
        if (it != textureByKey.end()) return it->second;

        Texture tex;
        tex.originalUri = path;
        tex.name = FileUtil::Stem(path);
        if (emb) {
            tex.embedded = true;
            if (emb->mFilename.length) tex.name = FileUtil::Stem(emb->mFilename.C_Str());
            if (tex.name.empty() || tex.name[0] == '*') tex.name = "embedded_" + std::to_string(scene.textures.size());
            std::string e;
            if (emb->mHeight == 0) {
                if (!ImageIO::LoadMemory((const uint8_t*)emb->pcData, emb->mWidth, tex, &e))
                    LOG_WARN("埋め込みテクスチャ '%s': %s", tex.name.c_str(), e.c_str());
            } else {
                auto img = std::make_shared<ImageData>();
                img->width = emb->mWidth;
                img->height = emb->mHeight;
                img->rgba.resize((size_t)emb->mWidth * emb->mHeight * 4);
                for (size_t i = 0; i < (size_t)emb->mWidth * emb->mHeight; ++i) {
                    const aiTexel& t = emb->pcData[i];
                    img->rgba[i * 4 + 0] = t.r;
                    img->rgba[i * 4 + 1] = t.g;
                    img->rgba[i * 4 + 2] = t.b;
                    img->rgba[i * 4 + 3] = t.a;
                }
                tex.image = img;
                tex.mimeType = "image/png";
            }
        } else if (!resolved.empty()) {
            std::string e;
            if (!ImageIO::LoadFile(resolved, tex, &e)) LOG_WARN("テクスチャ '%s': %s", resolved.c_str(), e.c_str());
            tex.name = FileUtil::Stem(path);
        } else {
            tex.missing = true;
            LOG_WARN("テクスチャが見つかりません: %s", path.c_str());
        }
        scene.textures.push_back(std::move(tex));
        int idx = (int)scene.textures.size() - 1;
        textureByKey[key] = idx;
        return idx;
    }

    bool ReadSlot(const aiMaterial* m, std::initializer_list<aiTextureType> types, TextureSlot& slot, Texture** outTex = nullptr) {
        for (aiTextureType t : types) {
            if (m->GetTextureCount(t) == 0) continue;
            aiString path;
            unsigned uv = 0;
            aiTextureMapMode modes[3] = { aiTextureMapMode_Wrap, aiTextureMapMode_Wrap, aiTextureMapMode_Wrap };
            if (m->GetTexture(t, 0, &path, nullptr, &uv, nullptr, nullptr, modes) != aiReturn_SUCCESS) continue;
            int idx = GetTexture(path);
            if (idx < 0) continue;
            slot.texture = idx;
            slot.uvSet = (int)std::min(uv, 1u);
            auto conv = [](aiTextureMapMode mm) {
                return mm == aiTextureMapMode_Clamp ? WrapMode::ClampToEdge : mm == aiTextureMapMode_Mirror ? WrapMode::MirroredRepeat : WrapMode::Repeat;
            };
            scene.textures[idx].wrapS = conv(modes[0]);
            scene.textures[idx].wrapT = conv(modes[1]);
            if (outTex) *outTex = &scene.textures[idx];
            return true;
        }
        return false;
    }

    // ---- マテリアル -------------------------------------------------------
    void ConvertMaterials() {
        for (unsigned i = 0; i < ai->mNumMaterials; ++i) {
            const aiMaterial* am = ai->mMaterials[i];
            Material m;
            aiString name;
            if (am->Get(AI_MATKEY_NAME, name) == aiReturn_SUCCESS) m.name = name.C_Str();
            if (m.name.empty()) m.name = "Material_" + std::to_string(i);

            aiColor4D c4;
            aiColor3D c3;
            if (am->Get(AI_MATKEY_BASE_COLOR, c4) == aiReturn_SUCCESS) m.baseColor = { c4.r, c4.g, c4.b, c4.a };
            else if (am->Get(AI_MATKEY_COLOR_DIFFUSE, c4) == aiReturn_SUCCESS) m.baseColor = { c4.r, c4.g, c4.b, 1.0f };

            float opacity = 1.0f;
            if (am->Get(AI_MATKEY_OPACITY, opacity) == aiReturn_SUCCESS) {
                if (opacity <= 0.001f) {
                    LOG_WARN("マテリアル '%s' の不透明度が 0 でした。FBX の TransparencyFactor の誤設定とみなし 1 として扱います", m.name.c_str());
                    opacity = 1.0f;
                }
                if (opacity < 0.999f) m.baseColor.a = opacity;
            }

            float f;
            bool hasMetal = am->Get(AI_MATKEY_METALLIC_FACTOR, f) == aiReturn_SUCCESS;
            if (hasMetal) m.metallic = f;
            if (am->Get(AI_MATKEY_ROUGHNESS_FACTOR, f) == aiReturn_SUCCESS) m.roughness = f;
            else if (am->Get(AI_MATKEY_SHININESS, f) == aiReturn_SUCCESS && f > 0) m.roughness = std::clamp(std::sqrt(2.0f / (f + 2.0f)), 0.04f, 1.0f);
            else m.roughness = hasMetal ? 1.0f : 0.6f;

            if (am->Get(AI_MATKEY_COLOR_EMISSIVE, c3) == aiReturn_SUCCESS) m.emissive = { c3.r, c3.g, c3.b };
            if (am->Get(AI_MATKEY_EMISSIVE_INTENSITY, f) == aiReturn_SUCCESS && f > 0) m.emissiveStrength = f;

            int twoSided = 0;
            if (am->Get(AI_MATKEY_TWOSIDED, twoSided) == aiReturn_SUCCESS) m.doubleSided = twoSided != 0;
            int shading = 0;
            if (am->Get(AI_MATKEY_SHADING_MODEL, shading) == aiReturn_SUCCESS && shading == aiShadingMode_Unlit) m.unlit = true;

            aiString alphaMode;
            if (am->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == aiReturn_SUCCESS) {
                std::string a = alphaMode.C_Str();
                m.alphaMode = a == "MASK" ? AlphaMode::Mask : a == "BLEND" ? AlphaMode::Blend : AlphaMode::Opaque;
            } else if (m.baseColor.a < 0.999f) {
                m.alphaMode = AlphaMode::Blend;
            }
            if (am->Get(AI_MATKEY_GLTF_ALPHACUTOFF, f) == aiReturn_SUCCESS) m.alphaCutoff = f;

            Texture* baseTex = nullptr;
            ReadSlot(am, { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE }, m.slots[Slot_BaseColor], &baseTex);
            ReadSlot(am, { aiTextureType_GLTF_METALLIC_ROUGHNESS, aiTextureType_METALNESS, aiTextureType_DIFFUSE_ROUGHNESS }, m.slots[Slot_MetallicRoughness]);
            if (!ReadSlot(am, { aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA }, m.slots[Slot_Normal])) {
                if (ReadSlot(am, { aiTextureType_HEIGHT }, m.slots[Slot_Normal]))
                    LOG_INFO("マテリアル '%s': Bump/Height マップを法線マップとして割り当てました", m.name.c_str());
            }
            ReadSlot(am, { aiTextureType_LIGHTMAP, aiTextureType_AMBIENT_OCCLUSION }, m.slots[Slot_Occlusion]);
            if (ReadSlot(am, { aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR }, m.slots[Slot_Emissive])) {
                if (m.emissive == glm::vec3(0)) m.emissive = glm::vec3(1);
            }
            float ns;
            if (am->Get(AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0), ns) == aiReturn_SUCCESS) m.normalScale = ns;
            if (am->Get(AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_LIGHTMAP, 0), ns) == aiReturn_SUCCESS) m.occlusionStrength = ns;
            // テクスチャがある場合、FBX の拡散色が黒/灰色のままだと暗くなるので白に
            if (baseTex && scene.info.sourceFormat == "FBX" && glm::vec3(m.baseColor) == glm::vec3(0)) m.baseColor = glm::vec4(1, 1, 1, m.baseColor.a);
            if (baseTex && baseTex->image && m.alphaMode == AlphaMode::Opaque && baseTex->mimeType == "image/png") {
                // 透明ピクセルの有無をざっくり確認してヒントを出す
                const auto& px = baseTex->image->rgba;
                size_t transparent = 0;
                for (size_t p = 3; p < px.size(); p += 4 * 16) if (px[p] < 128) ++transparent;
                if (transparent > 0) LOG_INFO("マテリアル '%s' のベースカラーに透明ピクセルがあります (Mask/Blend を検討)", m.name.c_str());
            }
            scene.materials.push_back(std::move(m));
        }
    }

    // ---- メッシュ ---------------------------------------------------------
    Primitive ConvertPrimitive(const aiMesh* am, std::map<std::string, int>& jointIndex, Skin* skin, int fallbackJoint) {
        Primitive p;
        p.material = (int)am->mMaterialIndex < (int)scene.materials.size() ? (int)am->mMaterialIndex : -1;
        const unsigned nv = am->mNumVertices;
        p.positions.resize(nv);
        for (unsigned v = 0; v < nv; ++v) p.positions[v] = { am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z };
        if (am->HasNormals()) {
            p.normals.resize(nv);
            for (unsigned v = 0; v < nv; ++v) {
                glm::vec3 n(am->mNormals[v].x, am->mNormals[v].y, am->mNormals[v].z);
                float l = glm::length(n);
                p.normals[v] = (l > 0 && std::isfinite(l)) ? n / l : glm::vec3(0, 1, 0);
            }
        }
        for (int set = 0; set < 2; ++set) {
            if (!am->HasTextureCoords(set)) continue;
            auto& dst = set == 0 ? p.uv0 : p.uv1;
            dst.resize(nv);
            for (unsigned v = 0; v < nv; ++v) {
                // aiProcess_FlipUVs で glTF と同じ左上原点にしている
                float u = am->mTextureCoords[set][v].x, vv = am->mTextureCoords[set][v].y;
                if (opts.flipUV) vv = 1.0f - vv;
                dst[v] = { u, vv };
            }
        }
        if (am->HasVertexColors(0)) {
            p.colors.resize(nv);
            for (unsigned v = 0; v < nv; ++v) {
                const aiColor4D& c = am->mColors[0][v];
                p.colors[v] = { c.r, c.g, c.b, c.a };
            }
        }
        p.indices.reserve(am->mNumFaces * 3);
        for (unsigned f = 0; f < am->mNumFaces; ++f) {
            const aiFace& face = am->mFaces[f];
            if (face.mNumIndices != 3) continue;
            p.indices.insert(p.indices.end(), { face.mIndices[0], face.mIndices[1], face.mIndices[2] });
        }

        // スキン
        if (skin) {
            std::vector<std::vector<std::pair<float, int>>> influences(nv);
            if (am->HasBones()) {
                for (unsigned b = 0; b < am->mNumBones; ++b) {
                    const aiBone* bone = am->mBones[b];
                    std::string bname = bone->mName.C_Str();
                    int ji;
                    auto it = jointIndex.find(bname);
                    if (it == jointIndex.end()) {
                        auto nit = nodeByName.find(bname);
                        int node = nit != nodeByName.end() ? nit->second : -1;
                        if (node < 0) {
                            LOG_WARN("ボーン '%s' に対応するノードがありません", bname.c_str());
                            continue;
                        }
                        ji = (int)skin->joints.size();
                        skin->joints.push_back(node);
                        skin->inverseBind.push_back(ToGlm(bone->mOffsetMatrix));
                        jointIndex[bname] = ji;
                    } else {
                        ji = it->second;
                    }
                    for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                        const aiVertexWeight& vw = bone->mWeights[w];
                        if (vw.mVertexId < nv && vw.mWeight > 0) influences[vw.mVertexId].push_back({ vw.mWeight, ji });
                    }
                }
            } else if (fallbackJoint >= 0) {
                // スキンメッシュ内の剛体パーツ: メッシュノード自身をジョイントにする
                for (auto& inf : influences) inf.push_back({ 1.0f, fallbackJoint });
            }
            p.joints.resize(nv, glm::u16vec4(0));
            p.weights.resize(nv, glm::vec4(0));
            for (unsigned v = 0; v < nv; ++v) {
                auto& inf = influences[v];
                std::sort(inf.begin(), inf.end(), [](auto& a, auto& b) { return a.first > b.first; });
                if (inf.size() > 4) p.droppedInfluences += (int)inf.size() - 4;
                float sum = 0;
                for (size_t k = 0; k < inf.size() && k < 4; ++k) {
                    p.joints[v][k] = (uint16_t)inf[k].second;
                    p.weights[v][k] = inf[k].first;
                    sum += inf[k].first;
                }
                if (sum > 0) p.weights[v] /= sum;
            }
        }

        // モーフターゲット
        if (opts.importMorphTargets) {
            for (unsigned a = 0; a < am->mNumAnimMeshes; ++a) {
                const aiAnimMesh* anim = am->mAnimMeshes[a];
                if (!anim->HasPositions() || anim->mNumVertices != nv) continue;
                MorphTarget t;
                t.name = anim->mName.length ? anim->mName.C_Str() : ("target_" + std::to_string(a));
                t.dPositions.resize(nv);
                for (unsigned v = 0; v < nv; ++v)
                    t.dPositions[v] = glm::vec3(anim->mVertices[v].x, anim->mVertices[v].y, anim->mVertices[v].z) - p.positions[v];
                if (anim->HasNormals() && p.HasNormals()) {
                    t.dNormals.resize(nv);
                    for (unsigned v = 0; v < nv; ++v)
                        t.dNormals[v] = glm::vec3(anim->mNormals[v].x, anim->mNormals[v].y, anim->mNormals[v].z) - p.normals[v];
                }
                p.targets.push_back(std::move(t));
            }
        }
        return p;
    }

    void ConvertMeshes() {
        std::map<std::vector<unsigned>, std::pair<int, int>> groupToMesh; // -> (mesh, skin)
        for (int ni = 0; ni < (int)aiNodes.size(); ++ni) {
            const aiNode* an = aiNodes[ni];
            if (an->mNumMeshes == 0) continue;
            std::vector<unsigned> group(an->mMeshes, an->mMeshes + an->mNumMeshes);
            auto it = groupToMesh.find(group);
            if (it != groupToMesh.end()) {
                scene.nodes[ni].mesh = it->second.first;
                scene.nodes[ni].skin = it->second.second;
                continue;
            }
            bool anyBones = false;
            for (unsigned mi : group) if (ai->mMeshes[mi]->HasBones()) anyBones = true;

            Mesh mesh;
            mesh.name = group.size() == 1 && ai->mMeshes[group[0]]->mName.length ? ai->mMeshes[group[0]]->mName.C_Str() : an->mName.C_Str();
            Skin skin;
            skin.name = std::string(an->mName.C_Str()) + "_skin";
            std::map<std::string, int> jointIndex;
            int fallbackJoint = -1;
            for (unsigned mi : group) {
                const aiMesh* am = ai->mMeshes[mi];
                if (!(am->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
                if (anyBones && !am->HasBones() && fallbackJoint < 0) {
                    fallbackJoint = (int)skin.joints.size();
                    skin.joints.push_back(ni);
                    skin.inverseBind.push_back(glm::mat4(1.0f));
                    jointIndex[scene.nodes[ni].name + "#self"] = fallbackJoint;
                }
                mesh.primitives.push_back(ConvertPrimitive(am, jointIndex, anyBones ? &skin : nullptr, fallbackJoint));
            }
            if (mesh.primitives.empty()) continue;
            int morphs = mesh.MorphTargetCount();
            if (morphs) {
                mesh.morphWeights.assign(morphs, 0.0f);
                for (unsigned mi : group) {
                    const aiMesh* am = ai->mMeshes[mi];
                    for (unsigned a = 0; a < am->mNumAnimMeshes && a < (unsigned)morphs; ++a) mesh.morphWeights[a] = am->mAnimMeshes[a]->mWeight;
                }
            }
            scene.meshes.push_back(std::move(mesh));
            int meshIdx = (int)scene.meshes.size() - 1;
            int skinIdx = -1;
            if (anyBones && !skin.joints.empty()) {
                // スケルトンルート: ジョイント群の共通の祖先に近いもの
                int root = skin.joints[0];
                while (root >= 0) {
                    bool allUnder = true;
                    for (int j : skin.joints) if (j != root && !scene.IsAncestor(root, j)) { allUnder = false; break; }
                    if (allUnder) break;
                    root = scene.nodes[root].parent;
                }
                skin.skeleton = root;
                scene.skins.push_back(std::move(skin));
                skinIdx = (int)scene.skins.size() - 1;
            }
            scene.nodes[ni].mesh = meshIdx;
            scene.nodes[ni].skin = skinIdx;
            groupToMesh[group] = { meshIdx, skinIdx };
        }
    }

    // ---- アニメーション ---------------------------------------------------
    void ConvertAnimations() {
        for (unsigned a = 0; a < ai->mNumAnimations; ++a) {
            const aiAnimation* aa = ai->mAnimations[a];
            Animation anim;
            anim.name = aa->mName.length ? aa->mName.C_Str() : ("Animation_" + std::to_string(a));
            double tps = aa->mTicksPerSecond > 0 ? aa->mTicksPerSecond : 25.0;
            for (unsigned c = 0; c < aa->mNumChannels; ++c) {
                const aiNodeAnim* na = aa->mChannels[c];
                auto it = nodeByName.find(na->mNodeName.C_Str());
                if (it == nodeByName.end()) {
                    LOG_WARN("アニメーション '%s': ノード '%s' が見つかりません", anim.name.c_str(), na->mNodeName.C_Str());
                    continue;
                }
                int node = it->second;
                if (na->mNumPositionKeys) {
                    AnimChannel ch;
                    ch.node = node;
                    ch.path = AnimPath::Translation;
                    for (unsigned k = 0; k < na->mNumPositionKeys; ++k) {
                        ch.times.push_back((float)(na->mPositionKeys[k].mTime / tps));
                        auto& v = na->mPositionKeys[k].mValue;
                        ch.values.push_back({ v.x, v.y, v.z, 0 });
                    }
                    anim.channels.push_back(std::move(ch));
                }
                if (na->mNumRotationKeys) {
                    AnimChannel ch;
                    ch.node = node;
                    ch.path = AnimPath::Rotation;
                    glm::vec4 prev(0, 0, 0, 1);
                    for (unsigned k = 0; k < na->mNumRotationKeys; ++k) {
                        ch.times.push_back((float)(na->mRotationKeys[k].mTime / tps));
                        auto& q = na->mRotationKeys[k].mValue;
                        glm::vec4 v(q.x, q.y, q.z, q.w);
                        float l = glm::length(v);
                        v = l > 0 ? v / l : glm::vec4(0, 0, 0, 1);
                        if (k > 0 && glm::dot(prev, v) < 0) v = -v; // 補間の最短経路
                        ch.values.push_back(v);
                        prev = v;
                    }
                    anim.channels.push_back(std::move(ch));
                }
                if (na->mNumScalingKeys) {
                    AnimChannel ch;
                    ch.node = node;
                    ch.path = AnimPath::Scale;
                    for (unsigned k = 0; k < na->mNumScalingKeys; ++k) {
                        ch.times.push_back((float)(na->mScalingKeys[k].mTime / tps));
                        auto& v = na->mScalingKeys[k].mValue;
                        ch.values.push_back({ v.x, v.y, v.z, 0 });
                    }
                    anim.channels.push_back(std::move(ch));
                }
            }
            if (aa->mNumMorphMeshChannels || aa->mNumMeshChannels)
                LOG_WARN("アニメーション '%s': モーフ/メッシュアニメーションのチャンネルは未対応のためスキップしました", anim.name.c_str());
            anim.RecomputeDuration();
            // 最初のキーが 0 から離れている場合は開始を 0 に揃える
            float start = std::numeric_limits<float>::max();
            for (auto& ch : anim.channels) if (!ch.times.empty()) start = std::min(start, ch.times.front());
            if (start > 1e-4f && start < std::numeric_limits<float>::max()) {
                for (auto& ch : anim.channels) for (auto& t : ch.times) t -= start;
                anim.RecomputeDuration();
                LOG_INFO("アニメーション '%s' の開始時刻 %.3fs を 0 に揃えました", anim.name.c_str(), start);
            }
            scene.animations.push_back(std::move(anim));
        }
    }

    void ReadMetadata() {
        const aiMetadata* md = ai->mMetaData;
        if (!md) return;
        for (unsigned i = 0; i < md->mNumProperties; ++i) {
            std::string key = md->mKeys[i].C_Str();
            const aiMetadataEntry& e = md->mValues[i];
            std::string val;
            double num = 0;
            bool isNum = false;
            switch (e.mType) {
            case AI_BOOL: val = *(bool*)e.mData ? "true" : "false"; break;
            case AI_INT32: num = *(int32_t*)e.mData; isNum = true; break;
            case AI_UINT64: num = (double)*(uint64_t*)e.mData; isNum = true; break;
            case AI_FLOAT: num = *(float*)e.mData; isNum = true; break;
            case AI_DOUBLE: num = *(double*)e.mData; isNum = true; break;
            case AI_AISTRING: val = ((aiString*)e.mData)->C_Str(); break;
            case AI_AIVECTOR3D: {
                auto* v = (aiVector3D*)e.mData;
                char b[128];
                snprintf(b, sizeof(b), "(%g, %g, %g)", v->x, v->y, v->z);
                val = b;
                break;
            }
            default: val = "<...>"; break;
            }
            if (isNum) {
                char b[64];
                snprintf(b, sizeof(b), "%g", num);
                val = b;
                if (key == "UnitScaleFactor") scene.info.unitScaleFactor = num;
                if (key == "UpAxis") scene.info.upAxis = (int)num;
            }
            scene.info.metadata.push_back({ key, val });
        }
    }
};

bool IsIdentity(const Node& n) {
    return glm::length(n.translation) < 1e-6f && std::abs(std::abs(n.rotation.w) - 1.0f) < 1e-6f && glm::length(n.scale - glm::vec3(1)) < 1e-6f;
}

} // namespace

bool ImportModel(const std::string& path, const ImportOptions& opts, Scene& out, std::string& err) {
    auto t0 = std::chrono::steady_clock::now();
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, opts.preservePivots);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ANIMATIONS, opts.importAnimations);
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, opts.smoothingAngle);
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, 4);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_REMOVE_EMPTY_BONES, false);

    unsigned flags = aiProcess_FlipUVs | aiProcess_SortByPType;
    if (opts.triangulate) flags |= aiProcess_Triangulate;
    if (opts.generateNormals) flags |= aiProcess_GenSmoothNormals;
    if (opts.joinVertices) flags |= aiProcess_JoinIdenticalVertices;
    if (opts.limitBoneWeights) flags |= aiProcess_LimitBoneWeights;
    if (opts.optimizeMeshes) flags |= aiProcess_OptimizeMeshes;
    if (opts.optimizeGraph) flags |= aiProcess_OptimizeGraph;
    if (opts.removeRedundantMaterials) flags |= aiProcess_RemoveRedundantMaterials;
    if (opts.fixInfacingNormals) flags |= aiProcess_FixInfacingNormals;
    if (opts.validateData) flags |= aiProcess_ValidateDataStructure;

    LOG_INFO("インポート開始: %s (Assimp %u.%u.%u)", path.c_str(), aiGetVersionMajor(), aiGetVersionMinor(), aiGetVersionPatch());
    // 以前に Assimp で書き出した FBX は、スケールカーブの接続名が "Lcl Scale" でスケールキーが失われるので、直してから読む
    std::vector<uint8_t> fileData;
    const char* memoryHint = nullptr;
    const std::string ext = FileUtil::LowerExt(path);
    if (ext == ".fbx" && FileUtil::ReadBinary(path, fileData) && FixFbxScaleCurveLinks(fileData) > 0) {
        LOG_INFO("FBX: スケールのアニメーションカーブの接続 (Lcl Scale) を修正して読み込みます");
        memoryHint = "fbx";
    } else if (ext == ".vrm") {
        // VRM は中身が GLB (glTF バイナリ)
        if (!FileUtil::ReadBinary(path, fileData)) {
            err = "ファイルを読めません";
            LOG_ERROR("インポート失敗: %s", err.c_str());
            return false;
        }
        memoryHint = "glb";
    }
    const aiScene* ai = memoryHint ? importer.ReadFileFromMemory(fileData.data(), fileData.size(), flags, memoryHint) : importer.ReadFile(path.c_str(), flags);
    if (!ai || !ai->mRootNode) {
        err = importer.GetErrorString();
        LOG_ERROR("インポート失敗: %s", err.c_str());
        return false;
    }
    if (ai->mFlags & AI_SCENE_FLAGS_INCOMPLETE) LOG_WARN("シーンが不完全です (AI_SCENE_FLAGS_INCOMPLETE)");

    Scene scene;
    scene.info.sourcePath = path;
    scene.info.sourceFormat = ext == ".fbx" ? "FBX" : ext == ".gltf" || ext == ".glb" || ext == ".vrm" ? "glTF" : ext.size() > 1 ? ext.substr(1) : "?";
    Converter conv(opts, scene);
    conv.ai = ai;
    conv.modelDir = FileUtil::Directory(path);
    conv.ReadMetadata();
    conv.ConvertNode(ai->mRootNode, -1);
    conv.ConvertMaterials();
    conv.ConvertMeshes();
    if (opts.importAnimations) conv.ConvertAnimations();

    // 単位行列のダミールートを除去
    if (opts.removeDummyRoot && scene.roots.size() == 1) {
        int r = scene.roots[0];
        auto joints = scene.JointMask();
        bool animated = false;
        for (auto& a : scene.animations) for (auto& c : a.channels) if (c.node == r) animated = true;
        bool skelRoot = false;
        for (auto& s : scene.skins) if (s.skeleton == r) skelRoot = true;
        if (IsIdentity(scene.nodes[r]) && scene.nodes[r].mesh < 0 && !joints[r] && !animated && !skelRoot && !scene.nodes[r].children.empty()) {
            auto children = scene.nodes[r].children;
            for (int c : children) scene.Reparent(c, -1, false);
            std::string name = scene.nodes[r].name;
            scene.RemoveNodes({ r });
            LOG_INFO("ダミールート '%s' を除去しました", name.c_str());
        }
    }

    // 単位 / 軸の変換 (FBX)
    scene.info.modelUnit = Unit_M;
    if (scene.info.sourceFormat == "glTF") scene.info.sourceUnit = Unit_M; // glTF の仕様でメートル
    if (scene.info.sourceFormat == "FBX") {
        scene.info.sourceUnit = UnitFromFbxScaleFactor(scene.info.unitScaleFactor);
        if (opts.convertUnitsToMeters) {
            if (std::abs(scene.info.unitScaleFactor - 100.0) > 1e-6) {
                float f = (float)(scene.info.unitScaleFactor / 100.0);
                LOG_INFO("FBX UnitScaleFactor=%g → メートルへ変換 (x%g)", scene.info.unitScaleFactor, f);
                Ops::ScaleScene(scene, f);
            }
            scene.info.unitConverted = true;
            scene.info.modelUnit = scene.info.sourceUnit >= 0 ? scene.info.sourceUnit : Unit_M;
        }
        if (opts.convertUpAxisToY && scene.info.upAxis == 2) Ops::ConvertUpAxis(scene, 2, 1);
    }
    if (ext == ".3mf") {
        // 3MF は Z-up・ミリメートル (Assimp は unit 属性を読まないので、仕様の既定値 mm として扱う)
        scene.info.sourceUnit = Unit_MM;
        scene.info.upAxis = 2;
        if (opts.convertUnitsToMeters) {
            Ops::ScaleScene(scene, 0.001f);
            scene.info.unitConverted = true;
            scene.info.modelUnit = Unit_MM;
        }
        if (opts.convertUpAxisToY) Ops::ConvertUpAxis(scene, 2, 1);
    }
    if (std::abs(opts.extraScale - 1.0f) > 1e-7f) Ops::ScaleScene(scene, opts.extraScale);
    // 接線は glTF の規約 (UV 左上原点, bitangent = cross(N,T)*w) で自前計算する
    if (opts.calcTangents)
        for (auto& m : scene.meshes)
            for (auto& p : m.primitives)
                if (p.HasUV0() && p.HasNormals()) Ops::GenerateTangents(p);

    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("インポート完了 (%.1f ms): ノード %zu, メッシュ %zu, 頂点 %zu, 三角形 %zu, マテリアル %zu, テクスチャ %zu, スキン %zu, アニメーション %zu",
             ms, scene.nodes.size(), scene.meshes.size(), scene.TotalVertices(), scene.TotalTriangles(),
             scene.materials.size(), scene.textures.size(), scene.skins.size(), scene.animations.size());
    out = std::move(scene);
    return true;
}
