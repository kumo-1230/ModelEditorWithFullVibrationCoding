#pragma once
#include "core/Scene.h"
#include <string>

enum class ExportFormat {
    GLB = 0,        // バイナリ glTF (テクスチャ埋め込み)
    GLTF,           // .gltf + .bin + 画像ファイル
    GLTF_Embedded,  // .gltf 単体 (base64 埋め込み)
    FBX,            // バイナリ FBX
    FBX_ASCII,
    OBJ,
    DAE,
    STL,
    PLY,
    X,              // DirectX (.x)
    ThreeMF,        // 3MF (.3mf、3D プリント向け)
    ThreeDS,        // 3ds Max (.3ds)
    Count
};

const char* ExportFormatName(ExportFormat f);
const char* ExportFormatExt(ExportFormat f);
ExportFormat ExportFormatFromPath(const std::string& path, ExportFormat fallback);

struct ExportOptions {
    ExportFormat format = ExportFormat::GLB;
    int content = 0;               // 書き出す内容: 0=全部 1=モデルだけ (アニメーションなし) 2=アニメーションだけ (骨格 + アニメーション)
    bool exportAnimations = true;
    bool exportSkins = true;
    bool exportMorphTargets = true;
    bool exportNormals = true;
    bool exportTangents = true;
    bool exportUV1 = true;
    bool exportColors = true;
    bool onlyVisible = false;
    bool normalizeWeights = true;
    bool uniquifyNames = true;     // ボーン名の重複を自動修正 (FBX では必須)
    bool embedTexturesFBX = false; // FBX にテクスチャを埋め込む
    bool fbxUnitCentimeters = false; // FBX を cm 単位で出力 (Maya 既定)。false ならメートル (UnitScaleFactor=100)
    bool copyTextures = true;      // 外部参照形式でテクスチャファイルを出力先にコピー
    // テクスチャを出力するフォルダ: 0=モデル名 + Texture (命名はモデル名の書き方に合わせる) 1=textureSubdir を使う
    int textureFolderMode = 0;
    std::string textureSubdir = ""; // textureFolderMode=1 のときのサブフォルダ (空なら出力先と同じ場所)
    int objectScaleMode = 0;       // オブジェクトノードのスケール: 0=中身に焼き込む 1=スケール付きの親ノードとして残す
    bool dissolveObjectWrappers = true; // 焼き込み時、自動作成した親ノードを書き出さない
    float scale = 1.0f;            // 書き出し時の追加スケール
    int upAxisConversion = 0;      // 0=なし 1=Y-up→Z-up 2=Z-up→Y-up
    bool prettyPrintJson = true;
    bool verifyAfterExport = true; // 書き出し後に再読み込みして検証
};

// モデル名 + "texture" のフォルダ名。モデル名の命名規則に合わせる:
// my_model → my_model_texture / MY_MODEL → MY_MODEL_TEXTURE / SK_Hero → SK_Hero_Texture / my-model → my-model-texture /
// MyModel → MyModelTexture / myModel → myModelTexture / それ以外 (空白入り・小文字だけ等) はキャメルケース: "my model" → myModelTexture
std::string TextureFolderName(const std::string& modelName);
// 書き出し先のパスと設定から、テクスチャを置くサブフォルダを決める (空なら出力先と同じ場所)
std::string ResolveTextureSubdir(const std::string& exportPath, const ExportOptions& opts);

// 書き出し前の前処理 (コピーしたシーンに対して実行)
void PrepareSceneForExport(Scene& scene, const ExportOptions& opts);
bool ExportScene(const Scene& scene, const std::string& path, const ExportOptions& opts, std::string& err);

// 個別の書き出し実装
bool ExportGltf(const Scene& scene, const std::string& path, const ExportOptions& opts, std::string& err);
bool ExportWithAssimp(const Scene& scene, const std::string& path, const ExportOptions& opts, std::string& err);
// Assimp の FBX 書き出しはスケールのアニメーションカーブを "Lcl Scale" に接続する (正しくは "Lcl Scaling")。
// このままでは Assimp / FBX SDK がスケールキーを無視するので、接続のプロパティ名を直す。
// ASCII はテキスト置換、バイナリはレコードのオフセットを計算し直して書き直す。直した数を返す (読めない形式は -1)
int FixFbxScaleCurveLinks(std::vector<uint8_t>& data);
// glTF を tinygltf で読み込み直して検証する
bool VerifyGltf(const std::string& path, std::string& report);
