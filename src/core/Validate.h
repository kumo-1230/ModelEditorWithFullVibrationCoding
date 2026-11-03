#pragma once
// シーンの問題点を検出するバリデーター (デバッグ用)
#include "Scene.h"
#include <string>
#include <vector>

enum class Severity { Info = 0, Warning = 1, Error = 2 };

enum class FixAction {
    None,
    RecalcNormals,      // mesh
    GenerateTangents,   // mesh
    NormalizeWeights,   // mesh
    RemoveDegenerate,   // mesh
    UniquifyNames,
    RemoveUnused,
    ClampJointIndices,  // mesh
    FlipInsideOut,      // mesh: 裏返ったメッシュの面と法線を反転
    FixQuaternions,     // animation: 回転キーの符号と長さを揃える
};

struct Issue {
    Severity severity = Severity::Info;
    std::string category;
    std::string message;
    int node = -1;
    int mesh = -1;
    int material = -1;
    int texture = -1;
    int animation = -1;
    int prim = -1;
    FixAction fix = FixAction::None;
};

std::vector<Issue> ValidateScene(const Scene& scene);
// FixAction を適用。true なら変更あり
bool ApplyFix(Scene& scene, const Issue& issue);
