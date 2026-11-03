#pragma once
// シーンに対する編集オペレーション群 (Maya の Freeze Transformations 等)
#include "Scene.h"
#include <string>

namespace Ops {

// --- トランスフォーム -------------------------------------------------------
// プリミティブの頂点を行列で変換 (法線は逆転置、負スケールなら巻き順を反転)
void TransformPrimitive(Primitive& prim, const glm::mat4& m);

// Maya の「フリーズトランスフォーム」。選択した成分 (T/R/S) をメッシュ頂点と子ノードに焼き込み、
// ノードの値を 0 / 0 / 1 に戻す。ワールド上の見た目は変わらない。
bool FreezeTransform(Scene& scene, int node, bool t, bool r, bool s, std::string* err);
// ノードと子孫すべてを上から順にフリーズ
int FreezeHierarchy(Scene& scene, int node, bool t, bool r, bool s, std::string* err);

// 均等スケールを子孫 (ジョイント/スキン/アニメーション含む) に伝播して、ノードのスケールを 1 に戻す。
// FBX の 0.01 / 100 倍スケールのリグを正規化するのに使う。
bool ApplyUniformScaleToHierarchy(Scene& scene, int node, std::string* err);

// --- オブジェクト (親ノードでのスケール) -----------------------------------
// node を含むオブジェクトノードを返す。祖先の objectRoot、無ければアニメーションの無い最上位ルート。
// 最上位ルート自身がアニメーションしていて親ノードの作成が必要なら -1
int FindObjectNode(const Scene& scene, int node);
// FindObjectNode と同じだが、必要ならアニメーションを持たない親ノードを自動で作る
int EnsureObjectNode(Scene& scene, int node);
// 全オブジェクトノードの均等スケールを中身 (ボーン位置・移動キー・頂点・逆バインド行列) に焼き込んで 1 に戻す。
// dissolveWrappers なら自動作成した親ノードを取り除く (移動・回転は子に焼き込む)。焼き込んだ数を返す
int BakeObjectScales(Scene& scene, bool dissolveWrappers);
// 2 つのシーンのボーンと頂点 (スキン変形後) のワールド位置を、レストポーズと全アニメーションの全フレーム
// (fps 刻み、1 アニメーション最大 600 フレーム) で比較して最大誤差を返す。構成が違って比較できなければ負の値
float MaxMotionError(const Scene& a, const Scene& b, float fps = 30.0f, int* frames = nullptr);

// シーン全体の単位を変換 (位置・メッシュ・逆バインド行列・アニメーションをスケール)
void ScaleScene(Scene& scene, float factor);

// ルートノード群に行列を前から掛ける (軸変換等)。スキンメッシュ頂点と逆バインド行列も補正する。
// m は回転 + 均等スケール + 平行移動であること。
void TransformSceneRoots(Scene& scene, const glm::mat4& m);
// 0=X,1=Y,2=Z の上方向軸を変換する
void ConvertUpAxis(Scene& scene, int fromAxis, int toAxis);

void ResetTransform(Node& node, bool t = true, bool r = true, bool s = true);
// ノードの変換を差分で動かす: 移動 += dT, 回転 = dR * 回転, スケール *= sMul。
// アニメーションのキーにも同じ差分を適用するので、アニメーション付きノードでも変更が反映される
void OffsetNodeTRS(Scene& scene, int node, const glm::vec3& dT, const glm::quat& dR, const glm::vec3& sMul, bool includeRest = true);
bool NodeHasAnimation(const Scene& scene, int node);
bool CenterPivot(Scene& scene, int node, std::string* err);
void CenterSceneToOrigin(Scene& scene, bool keepOnGround);
void PlaceOnGround(Scene& scene);

// --- メッシュ ---------------------------------------------------------------
int MakeMeshUnique(Scene& scene, int node); // 共有メッシュを複製して専有にする。新しいメッシュ番号を返す
void RecalculateNormals(Primitive& prim, bool smooth, float angleDeg = 180.0f);
void FlipNormals(Primitive& prim);
void ReverseWinding(Primitive& prim);
void FlipUV(Primitive& prim, bool u, bool v);
bool GenerateTangents(Primitive& prim);
int WeldVertices(Primitive& prim, float epsilon);
int RemoveDegenerateTriangles(Primitive& prim);
void ComputePrimitiveBounds(const Primitive& prim, glm::vec3& mn, glm::vec3& mx);

// --- スキン -----------------------------------------------------------------
int NormalizeWeights(Primitive& prim);           // 修正した頂点数
int PruneWeights(Primitive& prim, float threshold); // 閾値未満のウェイトを除去

// --- シーン整理 -------------------------------------------------------------
int RemoveUnusedResources(Scene& scene);  // 未使用メッシュ/マテリアル/テクスチャ
int RemoveEmptyNodes(Scene& scene);       // メッシュも子も無く、ジョイントでもないノード
int UniquifyNames(Scene& scene);          // 重複/空のノード名を修正
int BatchRename(Scene& scene, const std::string& find, const std::string& replace,
                const std::string& prefix, const std::string& suffix, const std::vector<int>* subset);
int StripNamespaces(Scene& scene);        // "mixamorig:Hips" -> "Hips"
int DuplicateNode(Scene& scene, int node, bool deepCopyMesh);
int AddMaterial(Scene& scene, const std::string& name);

// --- 基本形状 ---------------------------------------------------------------
enum PrimitiveType { Prim_Cube = 0, Prim_Sphere, Prim_Cylinder, Prim_Cone, Prim_Plane, Prim_Count };
const char* PrimitiveName(int type);
// 基本形状のノードを作成 (size = 一辺/直径/高さ [m])。底面が Y=0 に乗るように配置する。ノード番号を返す
int CreatePrimitive(Scene& scene, int type, float size, int parent = -1);

// --- アニメーション ---------------------------------------------------------
// 再生速度を speed 倍にする (キーの時刻を 1/speed 倍。2 なら 2 倍速で長さは半分)。書き出しにも反映される
void ScaleAnimationSpeed(Animation& anim, float speed);

// --- シーン結合 -------------------------------------------------------------
// src の全要素を dst に追加する (ルートは dst のルートとして追加)
void AppendScene(Scene& dst, const Scene& src);
// src のアニメーションを、ノード名で dst のノードに対応付けて追加する。対応したチャンネル数を返す
int ImportAnimationsByName(Scene& dst, const Scene& src, int* unmatched = nullptr);

} // namespace Ops
