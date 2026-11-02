#pragma once
#include <string>
#include <vector>

// コマンドライン (バッチ変換) モード。
// 例: ModelEditor.exe --convert in.fbx out.glb --scale 0.01 --freeze-all
// args に --convert / --validate / --dump / --help が含まれていれば処理して true を返す。
bool RunCli(const std::vector<std::string>& args, int& exitCode);
