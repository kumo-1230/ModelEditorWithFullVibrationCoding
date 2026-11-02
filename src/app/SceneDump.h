#pragma once
#include "core/Scene.h"
#include <string>

// デバッグ用: シーン構造をテキスト / JSON に書き出す
std::string DumpSceneText(const Scene& scene);
std::string DumpSceneJson(const Scene& scene);
