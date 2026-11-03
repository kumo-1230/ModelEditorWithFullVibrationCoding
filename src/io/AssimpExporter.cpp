// 内部シーン -> aiScene に変換して Assimp で書き出す (FBX / OBJ / DAE / STL / PLY)
#include "Exporter.h"
#include "ImageIO.h"
#include "core/Log.h"
#include "core/ModelOps.h"
#include "core/SceneOps.h"
#include "util/FileUtil.h"

#include <assimp/Exporter.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>

static aiMatrix4x4 ToAi(const glm::mat4& m) {
    return aiMatrix4x4(m[0][0], m[1][0], m[2][0], m[3][0],
                       m[0][1], m[1][1], m[2][1], m[3][1],
                       m[0][2], m[1][2], m[2][2], m[3][2],
                       m[0][3], m[1][3], m[2][3], m[3][3]);
}

namespace {

struct AiBuilder {
    const Scene& scene;
    const ExportOptions& opts;
    std::string outDir;
    aiScene* ai = nullptr;
    std::vector<std::vector<unsigned>> meshToAi; // Scene メッシュ -> aiMesh 番号群
    std::vector<std::string> texturePaths;       // Scene テクスチャ -> マテリアルに書くパス

    AiBuilder(const Scene& s, const ExportOptions& o) : scene(s), opts(o) {}

    void BuildTextures() {
        texturePaths.assign(scene.textures.size(), "");
        bool embed = opts.embedTexturesFBX && (opts.format == ExportFormat::FBX || opts.format == ExportFormat::FBX_ASCII);
        std::vector<aiTexture*> embedded;
        std::set<std::string> used;
        for (size_t i = 0; i < scene.textures.size(); ++i) {
            const Texture& t = scene.textures[i];
            std::string mime;
            std::vector<uint8_t> bytes = ImageIO::GetEncodedBytes(t, mime);
            if (bytes.empty()) {
                // データが無ければ元のパスをそのまま書く
                texturePaths[i] = !t.sourcePath.empty() ? t.sourcePath : t.originalUri;
                continue;
            }
            std::string ext = ImageIO::ExtensionFromMime(mime);
            std::string base = FileUtil::SanitizeFilename(t.name.empty() ? "texture_" + std::to_string(i) : t.name);
            std::string fname = base + ext;
            int k = 1;
            while (used.count(fname)) fname = base + "_" + std::to_string(k++) + ext;
            used.insert(fname);
            if (embed) {
                aiTexture* at = new aiTexture();
                at->mWidth = (unsigned)bytes.size();
                at->mHeight = 0;
                std::string hint = ext.substr(1);
                strncpy(at->achFormatHint, hint.c_str(), sizeof(at->achFormatHint) - 1);
                at->pcData = (aiTexel*)new uint8_t[bytes.size()];
                memcpy(at->pcData, bytes.data(), bytes.size());
                at->mFilename = aiString(fname);
                texturePaths[i] = "*" + std::to_string(embedded.size());
                embedded.push_back(at);
            } else {
                std::string rel = opts.textureSubdir.empty() ? fname : opts.textureSubdir + "/" + fname;
                if (opts.copyTextures) {
                    if (!opts.textureSubdir.empty()) FileUtil::CreateDirs(FileUtil::Join(outDir, opts.textureSubdir));
                    FileUtil::WriteBinary(FileUtil::Join(outDir, rel), bytes.data(), bytes.size());
                }
                texturePaths[i] = rel;
            }
        }
        if (!embedded.empty()) {
            ai->mNumTextures = (unsigned)embedded.size();
            ai->mTextures = new aiTexture*[embedded.size()];
            for (size_t i = 0; i < embedded.size(); ++i) ai->mTextures[i] = embedded[i];
        }
    }

    void AddTex(aiMaterial* m, aiTextureType type, const TextureSlot& slot) {
        if (slot.texture < 0 || slot.texture >= (int)texturePaths.size() || texturePaths[slot.texture].empty()) return;
        aiString p(texturePaths[slot.texture]);
        m->AddProperty(&p, AI_MATKEY_TEXTURE(type, 0));
        int uv = slot.uvSet;
        m->AddProperty(&uv, 1, AI_MATKEY_UVWSRC(type, 0));
        int mode = (int)aiTextureMapMode_Wrap;
        m->AddProperty(&mode, 1, AI_MATKEY_MAPPINGMODE_U(type, 0));
        m->AddProperty(&mode, 1, AI_MATKEY_MAPPINGMODE_V(type, 0));
    }

    void BuildMaterials() {
        size_t count = std::max<size_t>(1, scene.materials.size());
        ai->mNumMaterials = (unsigned)count;
        ai->mMaterials = new aiMaterial*[count];
        for (size_t i = 0; i < count; ++i) {
            aiMaterial* m = new aiMaterial();
            Material src;
            if (i < scene.materials.size()) src = scene.materials[i];
            else src.name = "DefaultMaterial";
            aiString name(src.name);
            m->AddProperty(&name, AI_MATKEY_NAME);
            aiColor4D base(src.baseColor.r, src.baseColor.g, src.baseColor.b, src.baseColor.a);
            aiColor3D diffuse(src.baseColor.r, src.baseColor.g, src.baseColor.b);
            m->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
            m->AddProperty(&base, 1, AI_MATKEY_BASE_COLOR);
            aiColor3D emissive(src.emissive.r * src.emissiveStrength, src.emissive.g * src.emissiveStrength, src.emissive.b * src.emissiveStrength);
            m->AddProperty(&emissive, 1, AI_MATKEY_COLOR_EMISSIVE);
            aiColor3D ambient(0, 0, 0);
            m->AddProperty(&ambient, 1, AI_MATKEY_COLOR_AMBIENT);
            aiColor3D spec(0.04f + src.metallic * 0.96f, 0.04f + src.metallic * 0.96f, 0.04f + src.metallic * 0.96f);
            m->AddProperty(&spec, 1, AI_MATKEY_COLOR_SPECULAR);
            float opacity = src.alphaMode == AlphaMode::Opaque ? 1.0f : src.baseColor.a;
            m->AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
            float metallic = src.metallic, rough = src.roughness;
            m->AddProperty(&metallic, 1, AI_MATKEY_METALLIC_FACTOR);
            m->AddProperty(&rough, 1, AI_MATKEY_ROUGHNESS_FACTOR);
            float shininess = std::clamp(2.0f / std::max(rough * rough, 1e-4f) - 2.0f, 0.0f, 1024.0f);
            m->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
            int twoSided = src.doubleSided ? 1 : 0;
            m->AddProperty(&twoSided, 1, AI_MATKEY_TWOSIDED);
            int shading = src.unlit ? aiShadingMode_Unlit : aiShadingMode_Phong;
            m->AddProperty(&shading, 1, AI_MATKEY_SHADING_MODEL);
            aiString alpha(src.alphaMode == AlphaMode::Mask ? "MASK" : src.alphaMode == AlphaMode::Blend ? "BLEND" : "OPAQUE");
            m->AddProperty(&alpha, AI_MATKEY_GLTF_ALPHAMODE);
            float cutoff = src.alphaCutoff;
            m->AddProperty(&cutoff, 1, AI_MATKEY_GLTF_ALPHACUTOFF);

            AddTex(m, aiTextureType_DIFFUSE, src.slots[Slot_BaseColor]);
            AddTex(m, aiTextureType_BASE_COLOR, src.slots[Slot_BaseColor]);
            AddTex(m, aiTextureType_NORMALS, src.slots[Slot_Normal]);
            AddTex(m, aiTextureType_EMISSIVE, src.slots[Slot_Emissive]);
            AddTex(m, aiTextureType_LIGHTMAP, src.slots[Slot_Occlusion]);
            AddTex(m, aiTextureType_METALNESS, src.slots[Slot_MetallicRoughness]);
            ai->mMaterials[i] = m;
        }
    }

    aiMesh* BuildMesh(const Mesh& mesh, const Primitive& p, const Skin* skin, const std::string& name) {
        aiMesh* am = new aiMesh();
        am->mName = aiString(name);
        am->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        am->mMaterialIndex = (p.material >= 0 && p.material < (int)ai->mNumMaterials) ? p.material : 0;
        const unsigned nv = (unsigned)p.positions.size();
        am->mNumVertices = nv;
        am->mVertices = new aiVector3D[nv];
        for (unsigned v = 0; v < nv; ++v) am->mVertices[v] = aiVector3D(p.positions[v].x, p.positions[v].y, p.positions[v].z);
        if (p.HasNormals()) {
            am->mNormals = new aiVector3D[nv];
            for (unsigned v = 0; v < nv; ++v) am->mNormals[v] = aiVector3D(p.normals[v].x, p.normals[v].y, p.normals[v].z);
        }
        if (p.HasTangents() && p.HasNormals()) {
            am->mTangents = new aiVector3D[nv];
            am->mBitangents = new aiVector3D[nv];
            for (unsigned v = 0; v < nv; ++v) {
                glm::vec3 t(p.tangents[v]);
                // cross(N,T)*w は画像の「上」方向 = V 上向き UV での dP/dv
                glm::vec3 b = glm::cross(p.normals[v], t) * p.tangents[v].w;
                am->mTangents[v] = aiVector3D(t.x, t.y, t.z);
                am->mBitangents[v] = aiVector3D(b.x, b.y, b.z);
            }
        }
        const std::vector<glm::vec2>* uvs[2] = { p.HasUV0() ? &p.uv0 : nullptr, p.HasUV1() ? &p.uv1 : nullptr };
        for (int s = 0; s < 2; ++s) {
            if (!uvs[s]) continue;
            am->mTextureCoords[s] = new aiVector3D[nv];
            am->mNumUVComponents[s] = 2;
            for (unsigned v = 0; v < nv; ++v) am->mTextureCoords[s][v] = aiVector3D((*uvs[s])[v].x, 1.0f - (*uvs[s])[v].y, 0);
        }
        if (p.HasColors()) {
            am->mColors[0] = new aiColor4D[nv];
            for (unsigned v = 0; v < nv; ++v) am->mColors[0][v] = aiColor4D(p.colors[v].r, p.colors[v].g, p.colors[v].b, p.colors[v].a);
        }
        const unsigned nf = (unsigned)(p.indices.size() / 3);
        am->mNumFaces = nf;
        am->mFaces = new aiFace[nf];
        for (unsigned f = 0; f < nf; ++f) {
            am->mFaces[f].mNumIndices = 3;
            am->mFaces[f].mIndices = new unsigned[3]{ p.indices[f * 3], p.indices[f * 3 + 1], p.indices[f * 3 + 2] };
        }

        if (skin && p.HasSkin()) {
            std::map<int, std::vector<aiVertexWeight>> perJoint;
            for (unsigned v = 0; v < nv; ++v)
                for (int k = 0; k < 4; ++k)
                    if (p.weights[v][k] > 0 && p.joints[v][k] < skin->joints.size())
                        perJoint[p.joints[v][k]].push_back(aiVertexWeight(v, p.weights[v][k]));
            if (!perJoint.empty()) {
                am->mNumBones = (unsigned)perJoint.size();
                am->mBones = new aiBone*[perJoint.size()];
                unsigned bi = 0;
                for (auto& [j, weights] : perJoint) {
                    aiBone* b = new aiBone();
                    int node = skin->joints[j];
                    b->mName = aiString(scene.nodes[node].name);
                    b->mOffsetMatrix = ToAi(skin->inverseBind[j]);
                    b->mNumWeights = (unsigned)weights.size();
                    b->mWeights = new aiVertexWeight[weights.size()];
                    std::copy(weights.begin(), weights.end(), b->mWeights);
                    am->mBones[bi++] = b;
                }
            }
        }

        if (opts.exportMorphTargets && !p.targets.empty()) {
            am->mNumAnimMeshes = (unsigned)p.targets.size();
            am->mAnimMeshes = new aiAnimMesh*[p.targets.size()];
            am->mMethod = aiMorphingMethod_MORPH_NORMALIZED;
            for (size_t t = 0; t < p.targets.size(); ++t) {
                const MorphTarget& mt = p.targets[t];
                aiAnimMesh* a = new aiAnimMesh();
                a->mName = aiString(mt.name);
                a->mNumVertices = nv;
                a->mVertices = new aiVector3D[nv];
                for (unsigned v = 0; v < nv; ++v) {
                    glm::vec3 pos = p.positions[v] + (v < mt.dPositions.size() ? mt.dPositions[v] : glm::vec3(0));
                    a->mVertices[v] = aiVector3D(pos.x, pos.y, pos.z);
                }
                if (p.HasNormals()) {
                    a->mNormals = new aiVector3D[nv];
                    for (unsigned v = 0; v < nv; ++v) {
                        glm::vec3 n = p.normals[v] + (v < mt.dNormals.size() ? mt.dNormals[v] : glm::vec3(0));
                        a->mNormals[v] = aiVector3D(n.x, n.y, n.z);
                    }
                }
                a->mWeight = t < mesh.morphWeights.size() ? mesh.morphWeights[t] : 0.0f;
                am->mAnimMeshes[t] = a;
            }
        }
        return am;
    }

    void BuildMeshes() {
        std::vector<aiMesh*> meshes;
        meshToAi.assign(scene.meshes.size(), {});
        // メッシュごとに使用するスキン (最初に見つかったノードのもの)
        std::vector<int> meshSkin(scene.meshes.size(), -1);
        for (auto& n : scene.nodes) if (n.mesh >= 0 && n.skin >= 0 && meshSkin[n.mesh] < 0) meshSkin[n.mesh] = n.skin;
        for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
            const Mesh& m = scene.meshes[mi];
            const Skin* skin = meshSkin[mi] >= 0 ? &scene.skins[meshSkin[mi]] : nullptr;
            for (size_t pi = 0; pi < m.primitives.size(); ++pi) {
                const Primitive& p = m.primitives[pi];
                if (p.positions.empty() || p.indices.empty()) continue;
                std::string name = m.primitives.size() > 1 ? m.name + "_" + std::to_string(pi) : m.name;
                meshToAi[mi].push_back((unsigned)meshes.size());
                meshes.push_back(BuildMesh(m, p, skin, name));
            }
        }
        ai->mNumMeshes = (unsigned)meshes.size();
        ai->mMeshes = new aiMesh*[std::max<size_t>(1, meshes.size())];
        for (size_t i = 0; i < meshes.size(); ++i) ai->mMeshes[i] = meshes[i];
    }

    aiNode* BuildNode(int idx, aiNode* parent) {
        const Node& n = scene.nodes[idx];
        aiNode* an = new aiNode(n.name);
        an->mParent = parent;
        an->mTransformation = ToAi(n.LocalMatrix());
        if (n.mesh >= 0 && n.mesh < (int)meshToAi.size() && !meshToAi[n.mesh].empty()) {
            auto& list = meshToAi[n.mesh];
            an->mNumMeshes = (unsigned)list.size();
            an->mMeshes = new unsigned[list.size()];
            std::copy(list.begin(), list.end(), an->mMeshes);
        }
        if (!n.children.empty()) {
            an->mNumChildren = (unsigned)n.children.size();
            an->mChildren = new aiNode*[n.children.size()];
            for (size_t c = 0; c < n.children.size(); ++c) an->mChildren[c] = BuildNode(n.children[c], an);
        }
        return an;
    }

    void BuildHierarchy() {
        aiNode* root = new aiNode("RootNode");
        root->mNumChildren = (unsigned)scene.roots.size();
        if (!scene.roots.empty()) {
            root->mChildren = new aiNode*[scene.roots.size()];
            for (size_t i = 0; i < scene.roots.size(); ++i) root->mChildren[i] = BuildNode(scene.roots[i], root);
        }
        ai->mRootNode = root;
    }

    void BuildAnimations() {
        if (!opts.exportAnimations || scene.animations.empty()) return;
        const double tps = 1000.0; // ミリ秒単位のティック
        std::vector<aiAnimation*> anims;
        for (const Animation& a : scene.animations) {
            std::map<int, std::vector<const AnimChannel*>> byNode;
            for (auto& c : a.channels) if (c.node >= 0 && !c.times.empty()) byNode[c.node].push_back(&c);
            if (byNode.empty()) continue;
            aiAnimation* aa = new aiAnimation();
            aa->mName = aiString(a.name);
            aa->mTicksPerSecond = tps;
            aa->mDuration = a.duration * tps;
            aa->mNumChannels = (unsigned)byNode.size();
            aa->mChannels = new aiNodeAnim*[byNode.size()];
            unsigned ci = 0;
            for (auto& [node, chans] : byNode) {
                const Node& n = scene.nodes[node];
                aiNodeAnim* na = new aiNodeAnim();
                na->mNodeName = aiString(n.name);
                const AnimChannel *T = nullptr, *R = nullptr, *S = nullptr;
                for (auto* c : chans) {
                    if (c->path == AnimPath::Translation) T = c;
                    else if (c->path == AnimPath::Rotation) R = c;
                    else S = c;
                }
                auto fillVec = [&](const AnimChannel* c, const glm::vec3& rest, aiVectorKey*& keys, unsigned& count) {
                    if (c) {
                        count = (unsigned)c->times.size();
                        keys = new aiVectorKey[count];
                        for (unsigned k = 0; k < count; ++k)
                            keys[k] = aiVectorKey(c->times[k] * tps, aiVector3D(c->values[k].x, c->values[k].y, c->values[k].z));
                    } else {
                        count = 1;
                        keys = new aiVectorKey[1];
                        keys[0] = aiVectorKey(0.0, aiVector3D(rest.x, rest.y, rest.z));
                    }
                };
                fillVec(T, n.translation, na->mPositionKeys, na->mNumPositionKeys);
                fillVec(S, n.scale, na->mScalingKeys, na->mNumScalingKeys);
                if (R) {
                    na->mNumRotationKeys = (unsigned)R->times.size();
                    na->mRotationKeys = new aiQuatKey[R->times.size()];
                    for (size_t k = 0; k < R->times.size(); ++k) {
                        const glm::vec4& q = R->values[k];
                        na->mRotationKeys[k] = aiQuatKey(R->times[k] * tps, aiQuaternion(q.w, q.x, q.y, q.z));
                    }
                } else {
                    na->mNumRotationKeys = 1;
                    na->mRotationKeys = new aiQuatKey[1];
                    na->mRotationKeys[0] = aiQuatKey(0.0, aiQuaternion(n.rotation.w, n.rotation.x, n.rotation.y, n.rotation.z));
                }
                aa->mChannels[ci++] = na;
            }
            anims.push_back(aa);
        }
        if (anims.empty()) return;
        ai->mNumAnimations = (unsigned)anims.size();
        ai->mAnimations = new aiAnimation*[anims.size()];
        for (size_t i = 0; i < anims.size(); ++i) ai->mAnimations[i] = anims[i];
    }
};

const char* AssimpFormatId(ExportFormat f) {
    switch (f) {
    case ExportFormat::FBX: return "fbx";
    case ExportFormat::FBX_ASCII: return "fbxa";
    case ExportFormat::OBJ: return "obj";
    case ExportFormat::DAE: return "collada";
    case ExportFormat::STL: return "stlb";
    case ExportFormat::PLY: return "plyb";
    case ExportFormat::X: return "x";
    case ExportFormat::ThreeMF: return "3mf";
    case ExportFormat::ThreeDS: return "3ds";
    default: return nullptr;
    }
}

} // namespace

// Assimp (特に FBX エクスポーター) は「レストポーズ = バインドポーズ」かつ
// 「スキンメッシュの頂点はメッシュノードのローカル空間」を前提にしている。
// 内部表現 (glTF 準拠: 頂点はバインド空間、変形は joint * IBM) をその前提に合わせる。
static void ConvertSkinsForAssimp(Scene& s) {
    auto maxDiff = [](const glm::mat4& a, const glm::mat4& b) {
        float d = 0;
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) d = std::max(d, std::abs(a[c][r] - b[c][r]));
        return d;
    };
    // 1) スキンをノードごとに専有化
    std::vector<int> skinUsers(s.skins.size(), 0);
    for (auto& n : s.nodes) {
        if (n.skin < 0 || n.skin >= (int)s.skins.size()) continue;
        if (skinUsers[n.skin]++ > 0) {
            s.skins.push_back(s.skins[n.skin]);
            n.skin = (int)s.skins.size() - 1;
        }
    }
    // 2) バインドポーズとレストポーズが食い違うスキンは、ジョイントのレストをバインドに合わせる
    for (auto& sk : s.skins) {
        if (sk.joints.empty()) continue;
        auto world = s.ComputeWorldMatrices();
        glm::mat4 B0 = world[sk.joints[0]] * sk.inverseBind[0];
        float scale = std::max(1.0f, glm::length(glm::vec3(B0[3])));
        bool consistent = true;
        for (size_t j = 1; j < sk.joints.size(); ++j)
            if (maxDiff(world[sk.joints[j]] * sk.inverseBind[j], B0) > 1e-3f * scale) { consistent = false; break; }
        if (consistent) continue;
        LOG_WARN("スキン '%s': レストポーズがバインドポーズと異なるため、書き出し用にジョイントをバインドポーズへ戻しました", sk.name.c_str());
        // 親から順に設定する
        std::vector<int> order(sk.joints.size());
        for (size_t j = 0; j < order.size(); ++j) order[j] = (int)j;
        auto depth = [&](int node) { int d = 0; while (node >= 0) { node = s.nodes[node].parent; ++d; } return d; };
        std::sort(order.begin(), order.end(), [&](int a, int b) { return depth(sk.joints[a]) < depth(sk.joints[b]); });
        for (int j : order) {
            int node = sk.joints[j];
            world = s.ComputeWorldMatrices();
            glm::mat4 desired = B0 * glm::inverse(sk.inverseBind[j]);
            int parent = s.nodes[node].parent;
            glm::mat4 pw = parent >= 0 ? world[parent] : glm::mat4(1.0f);
            s.nodes[node].SetLocalMatrix(glm::inverse(pw) * desired);
        }
    }
    // 3) 頂点をメッシュノードのローカル空間へ移し、IBM を Assimp のオフセット行列にする
    auto world = s.ComputeWorldMatrices();
    for (int ni = 0; ni < (int)s.nodes.size(); ++ni) {
        Node& n = s.nodes[ni];
        if (n.mesh < 0 || n.skin < 0 || n.skin >= (int)s.skins.size() || !s.meshes[n.mesh].HasSkin()) continue;
        Skin& sk = s.skins[n.skin];
        if (sk.joints.empty()) continue;
        glm::mat4 B = world[sk.joints[0]] * sk.inverseBind[0];
        glm::mat4 X = glm::inverse(world[ni]) * B;
        if (maxDiff(X, glm::mat4(1.0f)) < 1e-6f) continue;
        int m = Ops::MakeMeshUnique(s, ni);
        for (auto& p : s.meshes[m].primitives) Ops::TransformPrimitive(p, X);
        glm::mat4 Xinv = glm::inverse(X);
        for (auto& ibm : sk.inverseBind) ibm = ibm * Xinv;
    }
}

namespace {

// バイナリ FBX のレコードを 1 つずつ書き直す (文字列の長さが変わるので、終端オフセットを計算し直す)
struct FbxBinaryRewriter {
    const std::vector<uint8_t>& in;
    std::vector<uint8_t> out;
    bool is64 = true; // 7500 以降はオフセットが 64bit
    bool ok = true;
    int fixed = 0;

    size_t NullSize() const { return is64 ? 25 : 13; }
    uint64_t Rd(size_t pos, size_t bytes) {
        uint64_t v = 0;
        if (pos + bytes > in.size()) { ok = false; return 0; }
        std::memcpy(&v, &in[pos], bytes);
        return v;
    }
    void Wr(uint64_t v, size_t bytes) {
        size_t p = out.size();
        out.resize(p + bytes);
        std::memcpy(&out[p], &v, bytes);
    }
    void Copy(size_t pos, size_t n) {
        if (pos + n > in.size()) { ok = false; return; }
        out.insert(out.end(), in.begin() + pos, in.begin() + pos + n);
    }

    // pos のレコードを書き直して pos を進める。null レコード (リストの終端) かエラーなら false
    bool Record(size_t& pos) {
        const size_t ob = is64 ? 8 : 4;
        const uint64_t end = Rd(pos, ob), numProps = Rd(pos + ob, ob);
        const size_t nameLen = (size_t)Rd(pos + 3 * ob, 1);
        if (!ok) return false;
        if (end == 0) { pos += NullSize(); return false; }
        if (end > in.size() || end <= pos) { ok = false; return false; }
        const size_t hdr = out.size();
        Wr(0, ob);
        Wr(numProps, ob);
        Wr(0, ob);
        out.push_back((uint8_t)nameLen);
        size_t p = pos + 3 * ob + 1;
        if (p + nameLen > in.size()) { ok = false; return false; }
        const std::string name((const char*)&in[p], nameLen);
        Copy(p, nameLen);
        p += nameLen;
        const size_t propStart = out.size();
        for (uint64_t i = 0; i < numProps && ok; ++i) {
            const char t = (char)Rd(p, 1);
            size_t sz = 0; // 型コードを除いた大きさ
            switch (t) {
            case 'Y': sz = 2; break;
            case 'C': sz = 1; break;
            case 'I': case 'F': sz = 4; break;
            case 'D': case 'L': sz = 8; break;
            case 'f': case 'd': case 'l': case 'i': case 'b': sz = 12 + (size_t)Rd(p + 9, 4); break; // 要素数, 圧縮, 圧縮後の長さ
            case 'S': case 'R': sz = 4 + (size_t)Rd(p + 1, 4); break;
            default: ok = false; return false;
            }
            // 接続 (C: "OP", カーブノード, モデル, "Lcl Scale") のプロパティ名を直す
            if (t == 'S' && name == "C" && sz == 4 + 9 && p + 1 + sz <= in.size() && std::memcmp(&in[p + 5], "Lcl Scale", 9) == 0) {
                out.push_back('S');
                Wr(11, 4);
                out.insert(out.end(), "Lcl Scaling", "Lcl Scaling" + 11);
                ++fixed;
            } else {
                Copy(p, 1 + sz);
            }
            p += 1 + sz;
        }
        if (!ok) return false;
        const uint64_t propLen = out.size() - propStart;
        if (p < end) { // 入れ子のレコード (null レコードで終わる)
            while (ok && Record(p)) {}
            out.insert(out.end(), NullSize(), 0);
        }
        if (!ok || p != end) { ok = false; return false; }
        pos = (size_t)end;
        uint64_t newEnd = out.size();
        std::memcpy(&out[hdr], &newEnd, ob);
        std::memcpy(&out[hdr + 2 * ob], &propLen, ob);
        return true;
    }
};

} // namespace

int FixFbxScaleCurveLinks(std::vector<uint8_t>& data) {
    static const char kMagic[] = "Kaydara FBX Binary  ";
    if (data.size() >= 27 && std::memcmp(data.data(), kMagic, 20) == 0) {
        static const uint8_t pat[] = { 'S', 9, 0, 0, 0, 'L', 'c', 'l', ' ', 'S', 'c', 'a', 'l', 'e' };
        if (std::search(data.begin(), data.end(), std::begin(pat), std::end(pat)) == data.end()) return 0;
        FbxBinaryRewriter w{ data };
        uint32_t version = 0;
        std::memcpy(&version, &data[23], 4);
        w.is64 = version >= 7500;
        w.Copy(0, 27);
        size_t pos = 27;
        while (w.ok && w.Record(pos)) {}
        if (!w.ok) return -1;
        w.out.insert(w.out.end(), w.NullSize(), 0);
        // フッター: ID の後ろは 16 バイト境界に揃えるパディング (位置が変わったので詰め直す)
        static const uint8_t footId[16] = { 0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e };
        if (pos + 16 <= data.size() && std::memcmp(&data[pos], footId, 16) == 0) {
            w.Copy(pos, 16);
            pos += 16;
            size_t inPad = 16 - (pos % 16), outPad = 16 - (w.out.size() % 16);
            if (pos + inPad <= data.size()) {
                pos += inPad;
                w.out.insert(w.out.end(), outPad, 0);
            }
        }
        if (pos < data.size()) w.Copy(pos, data.size() - pos);
        if (!w.ok) return -1;
        data = std::move(w.out);
        return w.fixed;
    }
    // ASCII
    static const std::string from = "\"Lcl Scale\"", to = "\"Lcl Scaling\"";
    std::string s(data.begin(), data.end());
    int fixed = 0;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) {
        s.replace(p, from.size(), to);
        ++fixed;
    }
    if (fixed) data.assign(s.begin(), s.end());
    return fixed;
}

// 全メッシュをレストポーズのワールド座標に焼き込み、ルート直下の単位行列ノードに並べる (スキン・アニメーションは捨てる)。
// Assimp の 3MF 書き出しはルート直下のノードのメッシュしか書かないので、aiProcess_PreTransformVertices (メッシュがルートに付く) は使えない
static void FlattenToWorld(Scene& s) {
    auto world = s.ComputeWorldMatrices();
    Scene out;
    out.materials = s.materials;
    out.textures = s.textures;
    out.info = s.info;
    for (int i = 0; i < (int)s.nodes.size(); ++i) {
        const Node& n = s.nodes[i];
        if (n.mesh < 0) continue;
        Mesh m = s.meshes[n.mesh];
        const Skin* skin = (n.skin >= 0 && n.skin < (int)s.skins.size() && m.HasSkin()) ? &s.skins[n.skin] : nullptr;
        for (auto& p : m.primitives) {
            p.targets.clear();
            p.tangents.clear();
            if (skin && p.HasSkin()) {
                for (size_t v = 0; v < p.positions.size(); ++v) {
                    glm::mat4 M(0.0f);
                    float sum = 0;
                    for (int k = 0; k < 4; ++k) {
                        float w = p.weights[v][k];
                        int j = p.joints[v][k];
                        if (w <= 0 || j >= (int)skin->joints.size() || skin->joints[j] < 0) continue;
                        M += world[skin->joints[j]] * skin->inverseBind[j] * w;
                        sum += w;
                    }
                    if (sum <= 0) M = world[i];
                    p.positions[v] = glm::vec3(M * glm::vec4(p.positions[v], 1.0f));
                    if (v < p.normals.size()) {
                        glm::vec3 nv = glm::mat3(M) * p.normals[v];
                        float l = glm::length(nv);
                        p.normals[v] = l > 0 ? nv / l : glm::vec3(0, 1, 0);
                    }
                }
            } else {
                Ops::TransformPrimitive(p, world[i]);
            }
            p.joints.clear();
            p.weights.clear();
        }
        m.Touch();
        out.meshes.push_back(std::move(m));
        int node = out.AddNode(n.name, -1);
        out.nodes[node].mesh = (int)out.meshes.size() - 1;
    }
    s = std::move(out);
}

bool ExportWithAssimp(const Scene& sceneIn, const std::string& path, const ExportOptions& opts, std::string& err) {
    const char* id = AssimpFormatId(opts.format);
    if (!id) { err = "Assimp では未対応の形式です"; return false; }

    Scene scene = sceneIn;
    // ボーンを持てない形式は、画面に表示しているレストポーズの形で頂点に焼き込む
    // (焼き込まないと、レストポーズとバインドポーズが違うモデルでは別の姿勢で書き出される)
    const bool staticFormat = opts.format == ExportFormat::OBJ || opts.format == ExportFormat::STL || opts.format == ExportFormat::PLY ||
                              opts.format == ExportFormat::ThreeMF || opts.format == ExportFormat::ThreeDS || opts.format == ExportFormat::X;
    if (staticFormat) {
        int n = Ops::BakeSkinsToRestPose(scene);
        if (n) LOG_INFO("ボーンを持てない形式のため、スキンメッシュ %d 個を表示中のレストポーズで固定しました", n);
    }
    // 3MF / 3DS はノードの変換 (回転・拡大) を正しく持てないので、メッシュをワールド座標に焼き込む
    if (opts.format == ExportFormat::ThreeMF || opts.format == ExportFormat::ThreeDS) FlattenToWorld(scene);
    ConvertSkinsForAssimp(scene);

    std::unique_ptr<aiScene> ai(new aiScene());
    AiBuilder b(scene, opts);
    b.ai = ai.get();
    b.outDir = FileUtil::Directory(path);
    b.BuildTextures();
    b.BuildMaterials();
    b.BuildMeshes();
    b.BuildHierarchy();
    b.BuildAnimations();
    if (ai->mNumMeshes == 0 && (opts.format == ExportFormat::STL || opts.format == ExportFormat::PLY || opts.format == ExportFormat::OBJ ||
                                opts.format == ExportFormat::ThreeMF || opts.format == ExportFormat::ThreeDS)) {
        err = "書き出すメッシュがありません";
        return false;
    }

    // FBX の単位: 内部はメートル。cm 出力時は PrepareSceneForExport で x100 済み
    if (opts.format == ExportFormat::FBX || opts.format == ExportFormat::FBX_ASCII) {
        double unit = opts.fbxUnitCentimeters ? 1.0 : 100.0;
        ai->mMetaData = aiMetadata::Alloc(4);
        ai->mMetaData->Set(0, "UnitScaleFactor", unit);
        ai->mMetaData->Set(1, "OriginalUnitScaleFactor", unit);
        ai->mMetaData->Set(2, "UpAxis", (int32_t)1);
        ai->mMetaData->Set(3, "OriginalUpAxis", (int32_t)1);
    }

    unsigned pp = 0;
    if (opts.format == ExportFormat::STL || opts.format == ExportFormat::PLY) pp |= aiProcess_PreTransformVertices;
    Assimp::Exporter exporter;
    aiReturn r = exporter.Export(ai.get(), id, path.c_str(), pp);
    if (r != aiReturn_SUCCESS) {
        err = std::string("Assimp: ") + exporter.GetErrorString();
        return false;
    }
    if (opts.format == ExportFormat::FBX || opts.format == ExportFormat::FBX_ASCII) {
        // スケールキーが読み込み側で無視されないよう、Assimp が書いた接続名を直す
        std::vector<uint8_t> data;
        if (FileUtil::ReadBinary(path, data)) {
            int n = FixFbxScaleCurveLinks(data);
            if (n > 0 && FileUtil::WriteBinary(path, data.data(), data.size())) LOG_INFO("FBX: スケールのアニメーションカーブ %d 個の接続を修正しました", n);
            else if (n < 0) LOG_WARN("FBX: スケールのアニメーションカーブの接続を修正できませんでした (スケールキーが読み込まれない可能性があります)");
        }
    }
    return true;
}
