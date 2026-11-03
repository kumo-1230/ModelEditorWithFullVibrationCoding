#pragma once
// エディター内部のシーン表現。
// glTF 2.0 のデータモデルに近い形で保持し、Assimp(FBX等)からの読み込み・
// glTF/GLB/FBX への書き出しの共通中間形式として使う。
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_precision.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

uint64_t NewUid();

// ---------------------------------------------------------------------------
// テクスチャ
// ---------------------------------------------------------------------------
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // RGBA8
};

enum class WrapMode { Repeat = 10497, ClampToEdge = 33071, MirroredRepeat = 33648 };

struct Texture {
    uint64_t uid = NewUid();
    std::string name;
    std::string sourcePath;   // 解決済みのファイルパス(埋め込みなら空)
    std::string originalUri;  // 元ファイル内に記録されていたパス
    std::string mimeType;     // "image/png" / "image/jpeg" など
    std::shared_ptr<const std::vector<uint8_t>> encoded; // 元のエンコード済みデータ(png/jpg)
    std::shared_ptr<const ImageData> image;              // デコード済みピクセル
    bool embedded = false;
    bool missing = false;     // ファイルが見つからなかった
    WrapMode wrapS = WrapMode::Repeat;
    WrapMode wrapT = WrapMode::Repeat;
    bool linearFilter = true;
    bool generateMips = true;

    int Width() const { return image ? image->width : 0; }
    int Height() const { return image ? image->height : 0; }
};

// ---------------------------------------------------------------------------
// マテリアル (glTF PBR Metallic-Roughness 準拠)
// ---------------------------------------------------------------------------
enum class AlphaMode { Opaque = 0, Mask = 1, Blend = 2 };

struct TextureSlot {
    int texture = -1;
    int uvSet = 0;
    bool Valid() const { return texture >= 0; }
};

enum TexSlotId {
    Slot_BaseColor = 0,
    Slot_MetallicRoughness,
    Slot_Normal,
    Slot_Occlusion,
    Slot_Emissive,
    Slot_Count
};
const char* TexSlotName(int slot);

struct Material {
    std::string name;
    glm::vec4 baseColor{ 1.0f };
    float metallic = 0.0f;
    float roughness = 0.5f;
    glm::vec3 emissive{ 0.0f };
    float emissiveStrength = 1.0f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    TextureSlot slots[Slot_Count];

    TextureSlot& Slot(int i) { return slots[i]; }
    const TextureSlot& Slot(int i) const { return slots[i]; }
};

// ---------------------------------------------------------------------------
// メッシュ
// ---------------------------------------------------------------------------
struct MorphTarget {
    std::string name;
    std::vector<glm::vec3> dPositions; // 差分
    std::vector<glm::vec3> dNormals;   // 差分(空でもよい)
};

struct Primitive {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;   // w = handedness
    std::vector<glm::vec2> uv0;
    std::vector<glm::vec2> uv1;
    std::vector<glm::vec4> colors;
    std::vector<glm::u16vec4> joints;  // スキンのジョイント番号 (最大4影響)
    std::vector<glm::vec4> weights;
    std::vector<uint32_t> indices;     // 三角形リスト
    std::vector<MorphTarget> targets;
    int material = -1;
    int droppedInfluences = 0;         // 5本目以降で切り捨てられた影響の数(インポート時)

    size_t VertexCount() const { return positions.size(); }
    size_t TriangleCount() const { return indices.size() / 3; }
    bool HasNormals() const { return normals.size() == positions.size() && !positions.empty(); }
    bool HasTangents() const { return tangents.size() == positions.size() && !positions.empty(); }
    bool HasUV0() const { return uv0.size() == positions.size() && !positions.empty(); }
    bool HasUV1() const { return uv1.size() == positions.size() && !positions.empty(); }
    bool HasColors() const { return colors.size() == positions.size() && !positions.empty(); }
    bool HasSkin() const { return joints.size() == positions.size() && weights.size() == positions.size() && !positions.empty(); }
};

struct Mesh {
    uint64_t uid = NewUid();  // 変更されるたびに更新 (GPUキャッシュ用)
    std::string name;
    std::vector<Primitive> primitives;
    std::vector<float> morphWeights;  // モーフの既定ウェイト

    void Touch() { uid = NewUid(); }
    size_t VertexCount() const;
    size_t TriangleCount() const;
    bool HasSkin() const;
    int MorphTargetCount() const;
};

// ---------------------------------------------------------------------------
// ノード / スキン / アニメーション
// ---------------------------------------------------------------------------
struct Node {
    std::string name;
    glm::vec3 translation{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f };
    int parent = -1;
    std::vector<int> children;
    int mesh = -1;
    int skin = -1;
    bool visible = true;
    bool objectRoot = false;    // オブジェクト (エンジンの GameObject 相当)。サイズ変更はこのノードのスケールで行う
    bool objectWrapper = false; // エディターが自動で作った親ノード (焼き込み書き出し時に取り除ける)
    // 読み込んだファイルごとのグループ (= 1 モデル) に付ける元ファイルの情報 (単位・パスなど)。それ以外のノードは空
    std::shared_ptr<struct SceneInfo> modelInfo;

    glm::mat4 LocalMatrix() const;
    void SetLocalMatrix(const glm::mat4& m);
};

struct Skin {
    std::string name;
    std::vector<int> joints;            // ノード番号
    std::vector<glm::mat4> inverseBind; // joints と同数
    int skeleton = -1;                  // スケルトンルート(任意)
};

enum class AnimPath { Translation = 0, Rotation = 1, Scale = 2 };
enum class Interp { Linear = 0, Step = 1 };

struct AnimChannel {
    int node = -1;
    AnimPath path = AnimPath::Translation;
    Interp interp = Interp::Linear;
    std::vector<float> times;       // 秒
    std::vector<glm::vec4> values;  // T/S は xyz, R は quat(x,y,z,w)
};

struct Animation {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimChannel> channels;
    void RecomputeDuration();
};

struct SceneInfo {
    std::string sourcePath;
    std::string sourceFormat;
    std::vector<std::pair<std::string, std::string>> metadata;
    double unitScaleFactor = 1.0; // FBX の UnitScaleFactor (cm=1)
    int upAxis = 1;               // 0=X 1=Y 2=Z
    int sourceUnit = -1;          // 元ファイルに記録されていた単位 (LengthUnit, -1=不明)
    int modelUnit = 2;            // 元ファイルの 1 単位を何として解釈しているか (LengthUnit, 既定 m)。内部の数値は常にメートル
    bool unitConverted = false;   // 読み込み時に単位変換したか
};

// ノードのローカル姿勢 (アニメーション再生時の上書き用)
struct NodePose {
    glm::vec3 t{ 0.0f };
    glm::quat r{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 s{ 1.0f };
    glm::mat4 Matrix() const;
};

struct Scene {
    std::vector<Node> nodes;
    std::vector<int> roots;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Skin> skins;
    std::vector<Animation> animations;
    SceneInfo info;

    void Clear();
    bool Empty() const { return nodes.empty(); }

    void RebuildRoots();
    int AddNode(const std::string& name, int parent);
    bool IsAncestor(int ancestor, int node) const;
    // keepWorld=true ならワールド位置を保つようにローカル変換を修正する
    bool Reparent(int node, int newParent, bool keepWorld);
    // 指定ノード群(子孫を含む)を削除し、すべての参照を詰め直す
    void RemoveNodes(const std::vector<int>& nodesToRemove);
    void RemoveMesh(int mesh);
    void RemoveMaterial(int material);
    void RemoveTexture(int texture);
    void RemoveAnimation(int anim);

    std::vector<glm::mat4> ComputeWorldMatrices() const;
    std::vector<glm::mat4> ComputeWorldMatrices(const std::vector<NodePose>& pose) const;
    std::vector<NodePose> RestPose() const;
    std::vector<bool> JointMask() const;
    std::vector<int> NodesUsingMesh(int mesh) const;
    int FindNode(const std::string& name) const;
    std::string NodePath(int node) const;

    size_t TotalVertices() const;
    size_t TotalTriangles() const;
    void ComputeBounds(const std::vector<glm::mat4>& world, glm::vec3& outMin, glm::vec3& outMax) const;
    // ノードのメッシュのワールド AABB (スキンメッシュは現在のポーズで変形して計算)。メッシュが無ければ false
    bool ComputeNodeBounds(int node, const std::vector<glm::mat4>& world, glm::vec3& outMin, glm::vec3& outMax, size_t maxSamples = 4000) const;
};

// 便利関数
glm::mat4 ComposeTRS(const glm::vec3& t, const glm::quat& r, const glm::vec3& s);
void DecomposeTRS(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s);
void SampleAnimation(const Scene& scene, const Animation& anim, float time, std::vector<NodePose>& pose);
