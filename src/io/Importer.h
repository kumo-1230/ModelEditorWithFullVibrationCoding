#pragma once
// Assimp を使ったインポーター (FBX / glTF / GLB / VRM / OBJ / Blend / DAE / 3DS / X / 3MF / PLY / STL / BVH ほか Assimp が読める形式)
#include "core/Scene.h"
#include <string>
#include <vector>

struct ImportOptions {
    bool triangulate = true;
    bool generateNormals = true;      // 法線が無い場合に生成
    float smoothingAngle = 80.0f;
    bool calcTangents = true;
    bool joinVertices = true;
    bool flipUV = false;              // 追加で V を反転
    bool limitBoneWeights = true;     // 4 本に制限
    bool preservePivots = false;      // FBX の $AssimpFbx$ ピボットノード
    bool optimizeMeshes = false;
    bool optimizeGraph = false;
    bool removeRedundantMaterials = false;
    bool fixInfacingNormals = false;
    bool validateData = true;
    bool removeDummyRoot = true;      // 単位行列の自動生成ルートを除去
    bool convertUnitsToMeters = true; // FBX UnitScaleFactor を見て m に変換
    bool convertUpAxisToY = true;     // FBX UpAxis が Z なら Y-up に変換
    float extraScale = 1.0f;
    bool importAnimations = true;
    bool importMorphTargets = true;
    std::vector<std::string> textureSearchDirs;
};

bool ImportModel(const std::string& path, const ImportOptions& opts, Scene& out, std::string& err);
const char* ImportFileFilter(); // ファイルダイアログ用
bool IsSupportedModelExtension(const std::string& ext); // ".fbx" の形 (小文字)
const std::vector<std::string>& SupportedModelExtensions(); // 読み込める拡張子 (Assimp のビルド内容から取得)
