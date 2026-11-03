#pragma once
// モデルの問題を調べるための診断 (トポロジー、アニメーション、スキン、レイキャスト、テクスチャ)
#include "Scene.h"
#include <string>
#include <vector>

// ---- メッシュのトポロジー ----------------------------------------------------
// 辺は頂点の位置で溶接してから数える (UV や法線の継ぎ目は穴とみなさない)
struct TopologyInfo {
    int openEdges = 0;           // 1 枚の面にしか使われていない辺 (穴・開いた縁)
    int nonManifoldEdges = 0;    // 3 枚以上の面が共有する辺
    int windingMismatch = 0;     // 隣り合う 2 面で巻き順 (面の向き) が逆になっている辺
    int duplicateTriangles = 0;  // 同じ 3 頂点位置の三角形 (重なってちらつく)
    int degenerateTriangles = 0; // 面積 0 の三角形
    int isolatedVertices = 0;    // どの三角形にも使われていない頂点
    int normalsAgainstFaces = 0; // 頂点法線が面の向きと逆を向いている三角形
    int triangles = 0;
    double signedVolume = 0.0;   // 閉じたメッシュで負なら裏返し (内向き)
    bool closed = false;         // 開いた辺も非多様体の辺も無い
    std::vector<uint32_t> openEdgeVerts;        // 2 個ずつ (頂点番号)
    std::vector<uint32_t> nonManifoldEdgeVerts; // 2 個ずつ
};
TopologyInfo AnalyzeTopology(const Primitive& p, bool collectEdges = false);

// UV の範囲と異常値
struct UVInfo {
    glm::vec2 min{ 0.0f }, max{ 0.0f };
    int nan = 0;
    int outside01 = 0;      // 0〜1 の外にある頂点 (タイリング前提でなければ問題)
    int zeroAreaTris = 0;   // UV 上で面積 0 の三角形 (テクスチャが伸びる・接線が作れない)
};
UVInfo AnalyzeUV(const Primitive& p);

// ---- アニメーション ----------------------------------------------------------
struct AnimationReport {
    float duration = 0.0f;
    int channels = 0, keys = 0;
    int constantChannels = 0;    // 全キーが同じ値 (削除・最適化できる)
    int quatFlips = 0;           // 隣のキーとの内積が負 (遠回りに回転する)
    int unnormalizedQuats = 0;
    int nanKeys = 0;
    int zeroScaleKeys = 0;
    int negativeTimes = 0;
    int missingNodeChannels = 0; // 存在しないノードへのチャンネル
    int duplicateChannels = 0;   // 同じノード・同じ種類のチャンネルが複数
    int rootNode = -1;           // ルートモーションを持つノード (-1 = なし)
    float rootPathLength = 0.0f; // 軌跡の長さ (m)
    float rootDisplacement = 0.0f; // 最初と最後の位置の距離 (m)
    float rootMaxSpeed = 0.0f;   // m/s
    float rootMinY = 0.0f, rootMaxY = 0.0f;
    float loopError = 0.0f;      // 最初と最後のフレームの姿勢差 (ルートからの相対位置の最大差、m)
    float maxBoundsRatio = 1.0f; // フレームごとの AABB の対角 / レストポーズの AABB の対角 の最大
    float maxBoundsTime = 0.0f;
};
AnimationReport AnalyzeAnimation(const Scene& scene, int anim, float fps = 30.0f);
// 移動キーが変化する最上位のノード (ルートモーション)
int FindRootMotionNode(const Scene& scene, const Animation& anim);
// ノードのワールド位置の軌跡 (fps 刻み)
std::vector<glm::vec3> SampleNodeTrajectory(const Scene& scene, const Animation& anim, int node, float fps = 30.0f);
// 回転キーの符号を揃えて最短経路で補間されるようにし、正規化もする。直したキー数を返す
int FixQuaternionKeys(Animation& anim);

// ---- スキン ------------------------------------------------------------------
// スキンメッシュがレストポーズでどれだけ変形しているか (頂点の最大移動量、m)。0 ならレストポーズ = バインドポーズ
float BindPoseDeviation(const Scene& scene, int node, size_t maxSamples = 4000);
// スキンのジョイントごとに、ウェイトを受け持つ頂点数
std::vector<int> JointVertexCounts(const Scene& scene, int skin);

// ---- レイキャスト ------------------------------------------------------------
struct MeshHit {
    int node = -1, prim = -1;
    uint32_t tri = 0;      // 三角形の番号
    uint32_t vertex = 0;   // 当たった三角形のうち、一番近い頂点の番号
    float t = 0.0f;
    glm::vec3 pos{ 0.0f }, bary{ 0.0f };
};
// ワールド空間のレイで、指定の姿勢 (スキン変形後) のメッシュと交差判定する。非表示のノードは無視する
bool RaycastMeshes(const Scene& scene, const std::vector<glm::mat4>& world, const glm::vec3& origin, const glm::vec3& dir, MeshHit& hit);
// プリミティブの全頂点を変形後のワールド座標にする
void DeformPositions(const Scene& scene, const std::vector<glm::mat4>& world, int node, const Primitive& p, std::vector<glm::vec3>& out);
// 変形後の頂点位置 (と法線)
glm::vec3 DeformedVertex(const Scene& scene, const std::vector<glm::mat4>& world, int node, const Primitive& p, uint32_t v, glm::vec3* normal = nullptr);

// ---- テクスチャ --------------------------------------------------------------
// 平均色 (0〜1、ファイルのまま) と最小アルファ。画像が無ければ false
bool AverageTextureColor(const Texture& tex, glm::vec4& avg, float* minAlpha = nullptr);
// 平均色がノーマルマップらしいか (青紫: R,G ≈ 0.5, B が大きい)
bool LooksLikeNormalMap(const glm::vec4& avg);

// ---- 名前 --------------------------------------------------------------------
bool HasNonAscii(const std::string& s);
