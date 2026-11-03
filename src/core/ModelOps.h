#pragma once
// 複数モデル (ファイルごとの最上位グループ) の操作と、アニメーションの編集。
// - モデル: シーンの最上位ノード 1 つ = 1 モデル (Maya の読み込みでファイルごとにできるグループと同じ考え方)
// - 結合: 別のモデルのアニメーションをボーン名で対応付けてコピー / モデルを別のモデルのボーンに付ける / 1 つにまとめる
// - 抜き取り: モデルだけ (アニメーションなし) / アニメーションだけ (骨格 + アニメーション、メッシュなし)
// - アニメーション編集 (Maya のタイムライン / ドープシート相当): 範囲の切り抜き・削除・分割、ノードのキー削除
#include "Scene.h"
#include <string>
#include <vector>

namespace Ops {

// ---- モデル -----------------------------------------------------------------
int ModelRootOf(const Scene& scene, int node);                 // 最上位の祖先 (自分が最上位なら自分)
std::vector<char> SubtreeMask(const Scene& scene, int root);   // root とその子孫が 1
// モデルの中身 (子の移動・移動キー・頂点・逆バインド行列) を f 倍する。モデル自身の位置は変えない
bool ScaleModelContents(Scene& scene, int root, float f, std::string* err = nullptr);
// そのモデルだけのシーンを作る (他のモデル、そのモデルを動かさないアニメーション、使われない素材を除く)
Scene ExtractModel(const Scene& scene, int root);
// ファイルを読み込んだシーン全体を、名前 name の 1 つのグループ (モデル) にまとめる。グループの番号を返す
int GroupRootsAsModel(Scene& scene, const std::string& name, size_t firstRoot = 0);

// スキンメッシュを今のレストポーズの形で頂点に焼き込み、スキンを外す (ボーンを持てない形式への書き出し用)。焼き込んだ数を返す
int BakeSkinsToRestPose(Scene& scene);

// ---- 抜き取り ---------------------------------------------------------------
void StripAnimations(Scene& scene);       // モデルだけ: アニメーションを除く
void StripToAnimationOnly(Scene& scene);  // アニメーションだけ: メッシュ・マテリアル・テクスチャ・スキンを除き、骨格とアニメーションを残す

// ---- 結合 -------------------------------------------------------------------
// ボーン名の照合用に正規化する: 名前空間 (mixamorig: など)・前置き (Armature| / Bip01_ など)・記号・大文字小文字を無視
std::string NormalizeBoneName(const std::string& name);
struct RetargetOptions {
    int mode = 0;                 // 0=回転をそのままコピー (同じリグ) 1=レストポーズとの差分を移す (骨の向きが違うリグ)
    bool rootTranslation = true;  // 一番上の動くボーン (腰) の移動キーをコピー (身長の比で拡大縮小)
    bool otherTranslations = false; // それ以外のボーンの移動キーもコピー (同じ体型のときだけ)
    bool scaleKeys = false;       // スケールキーもコピー
    // 手で決めたボーンの対応 (元のボーン名 → 先のボーン名)。先が空なら、そのボーンの動きは使わない。
    // ここに無いボーンは autoMatch のとき名前で自動的に対応させる
    std::vector<std::pair<std::string, std::string>> boneMap;
    bool autoMatch = true;
};
// 名前での自動対応: 元のモデルのボーン (動くノードとジョイント) ごとに、先のモデルの対応するノード (無ければ -1)
std::vector<std::pair<int, int>> AutoBoneMap(const Scene& scene, int srcModel, int dstModel);
struct RetargetReport {
    int animations = 0;
    int matchedBones = 0;
    std::vector<std::string> unmatched; // 対応が見つからなかった元のボーン名
    float heightRatio = 1.0f;
};
// srcModel のアニメーション (anims が空なら全部) を dstModel のボーンへ名前で対応付けてコピーする。追加したアニメーションの番号を返す
std::vector<int> CopyAnimationsBetweenModels(Scene& scene, int srcModel, int dstModel, const std::vector<int>& anims, const RetargetOptions& opt,
                                             RetargetReport* report = nullptr);
// srcModel を dstNode の子にする (武器を手のボーンに付けるなど)。keepWorld=true で見た目の位置を保つ
bool AttachModel(Scene& scene, int srcModel, int dstNode, bool keepWorld, std::string* err = nullptr);
// srcModel の中身を dstModel に移して 1 つのモデルにする (src のグループは消える)。結果のモデル (dst) の番号を返す
int MergeModels(Scene& scene, int srcModel, int dstModel);

// ---- アニメーション編集 -----------------------------------------------------
glm::vec4 SampleChannel(const AnimChannel& ch, float t);
glm::vec4 ChannelDerivative(const AnimChannel& ch, float t); // 時間微分 (1 秒あたり)
// t にキーを入れる (曲線の形は変えない: 値と傾きをその時刻の曲線から取る)。キーの番号を返す (既にあればその番号)
int InsertKey(AnimChannel& ch, float t);
void EraseKey(AnimChannel& ch, int k);
// 曲線 (CubicSpline) を fps 刻みの直線キーにする (曲線を扱えない処理・形式の前に使う)
void LinearizeChannel(AnimChannel& ch, float fps = 30.0f);
void LinearizeAnimation(Animation& anim, float fps = 30.0f);
// 直線 / ステップのチャンネルを、形を変えずに曲線 (接線付き) にする
void MakeCubic(AnimChannel& ch);
// キーの接線の種類 (Maya と同じ名前)。Step はチャンネル全体に効く
enum class TangentMode { Spline, Clamped, Linear, Flat, Step };
void SetKeyTangents(AnimChannel& ch, const std::vector<int>& keys, TangentMode mode);
// t0〜t1 だけを残し、0 秒始まりにする (範囲の端にはその時刻の値でキーを打つ)
void TrimAnimation(Animation& anim, float t0, float t1);
// t0〜t1 を削除して後ろを詰める
void CutAnimationRange(Animation& anim, float t0, float t1);
// t0〜t1 を新しいクリップとして取り出す (元はそのまま)
Animation ExtractAnimationRange(const Animation& anim, float t0, float t1, const std::string& name);
// nodes のチャンネルを消す (pathMask: 1=移動 2=回転 4=スケール)。消した数を返す
int RemoveChannelsForNodes(Animation& anim, const std::vector<int>& nodes, int pathMask = 7);
// nodes (空なら全部) の t0〜t1 のキーを消す (両端の値は残す)。消したキーの数を返す
int DeleteKeysInRange(Animation& anim, const std::vector<int>& nodes, float t0, float t1);
// 値が変わらないキーを間引く (tolerance 以内)。消したキーの数を返す
int ReduceKeys(Animation& anim, float tolerance);

} // namespace Ops
