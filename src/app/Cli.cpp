#include "Cli.h"
#include "SceneDump.h"
#include "core/Log.h"
#include "core/SceneOps.h"
#include "core/Validate.h"
#include "io/Exporter.h"
#include "io/Importer.h"
#include "util/FileUtil.h"
#include <cstdio>

bool RunSelfTest(const std::string& path, int& exitCode);

static void PrintHelp() {
    printf(
        "ModelEditor コマンドラインモード\n"
        "  --convert <入力> <出力>   形式変換 (出力の拡張子で形式を決定: .glb .gltf .fbx .obj .dae .stl .ply .x .3mf .3ds)\n"
        "  --formats                 読み込み / 書き出しできる拡張子の一覧\n"
        "  --validate <入力>         シーンを検証して問題を表示\n"
        "  --dump <入力>             シーン構造をテキストで表示\n"
        "  --selftest <入力>         フリーズ/スケール適用/軸変換/書き出しの整合性を自動テスト (@rig で内蔵のテスト用リグ)\n"
        "  --screenshot <png> [--frames N] [入力]  GUI を起動し N フレーム後に画面を保存して終了\n"
        "オプション:\n"
        "  --scale <f>               書き出し時にシーン全体を f 倍\n"
        "  --import-scale <f>        読み込み時に f 倍\n"
        "  --no-unit-convert         FBX の cm→m 自動変換をしない\n"
        "  --no-axis-convert         FBX の Z-up→Y-up 自動変換をしない\n"
        "  --freeze-all              全ノードのトランスフォームをフリーズ (ジョイント以外)\n"
        "  --apply-root-scale        ルートの均等スケールを階層に適用\n"
        "  --embedded                .gltf をテクスチャ/バッファ埋め込みで出力\n"
        "  --fbx-ascii               FBX を ASCII で出力\n"
        "  --embed-fbx-textures      FBX にテクスチャを埋め込む\n"
        "  --no-anim / --no-skin / --no-morph / --no-tangents\n"
        "  --anim-speed <f>          全アニメーションを f 倍速にする (2 で 2 倍速、長さは半分)\n"
        "  --fix                     検証で見つかった修正可能な問題を自動修正\n"
        "  --strip-namespaces        ノード名の 'xxx:' を除去\n");
}

bool RunCli(const std::vector<std::string>& args, int& exitCode) {
    bool convert = false, validate = false, dump = false, help = false;
    std::string in, out;
    ImportOptions io;
    ExportOptions eo;
    eo.format = ExportFormat::GLB;
    bool freezeAll = false, applyRootScale = false, fix = false, stripNs = false;
    float animSpeed = 1.0f;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--convert") { convert = true; in = next(); out = next(); }
        else if (a == "--validate") { validate = true; in = next(); }
        else if (a == "--dump") { dump = true; in = next(); }
        else if (a == "--help" || a == "-h" || a == "/?") help = true;
        else if (a == "--formats") {
            printf("読み込み (%zu):", SupportedModelExtensions().size());
            for (auto& e : SupportedModelExtensions()) printf(" %s", e.c_str());
            printf("\n書き出し:");
            for (int f = 0; f < (int)ExportFormat::Count; ++f) printf(" %s", ExportFormatExt((ExportFormat)f));
            printf("\n");
            exitCode = 0;
            return true;
        }
        else if (a == "--selftest") { Log::Get().SetEchoStdout(false); return RunSelfTest(next(), exitCode); }
        else if (a == "--scale") eo.scale = std::stof(next());
        else if (a == "--import-scale") io.extraScale = std::stof(next());
        else if (a == "--no-unit-convert") io.convertUnitsToMeters = false;
        else if (a == "--no-axis-convert") io.convertUpAxisToY = false;
        else if (a == "--freeze-all") freezeAll = true;
        else if (a == "--apply-root-scale") applyRootScale = true;
        else if (a == "--embedded") eo.format = ExportFormat::GLTF_Embedded;
        else if (a == "--fbx-ascii") eo.format = ExportFormat::FBX_ASCII;
        else if (a == "--embed-fbx-textures") eo.embedTexturesFBX = true;
        else if (a == "--no-anim") eo.exportAnimations = false;
        else if (a == "--anim-speed") animSpeed = std::stof(next());
        else if (a == "--no-skin") eo.exportSkins = false;
        else if (a == "--no-morph") eo.exportMorphTargets = false;
        else if (a == "--no-tangents") eo.exportTangents = false;
        else if (a == "--fix") fix = true;
        else if (a == "--strip-namespaces") stripNs = true;
    }
    if (help) { PrintHelp(); exitCode = 0; return true; }
    if (!convert && !validate && !dump) return false;

    if (in.empty()) { PrintHelp(); exitCode = 2; return true; }
    Scene scene;
    std::string err;
    if (!ImportModel(in, io, scene, err)) {
        fprintf(stderr, "読み込み失敗: %s\n", err.c_str());
        exitCode = 1;
        return true;
    }
    if (stripNs) LOG_INFO("名前空間を除去: %d", Ops::StripNamespaces(scene));
    if (applyRootScale)
        for (int r : std::vector<int>(scene.roots)) {
            std::string e;
            if (!Ops::ApplyUniformScaleToHierarchy(scene, r, &e) && !e.empty()) LOG_WARN("%s", e.c_str());
        }
    if (freezeAll)
        for (int r : std::vector<int>(scene.roots)) Ops::FreezeHierarchy(scene, r, true, true, true, nullptr);
    if (animSpeed > 0.0f && std::abs(animSpeed - 1.0f) > 1e-6f) {
        for (auto& a : scene.animations) Ops::ScaleAnimationSpeed(a, animSpeed);
        LOG_INFO("アニメーション %zu 個の速度を x%g にしました", scene.animations.size(), animSpeed);
    }

    auto issues = ValidateScene(scene);
    if (fix) {
        int fixedCount = 0;
        for (auto& is : issues) if (is.fix != FixAction::None && ApplyFix(scene, is)) ++fixedCount;
        LOG_INFO("自動修正: %d 件", fixedCount);
        issues = ValidateScene(scene);
    }
    if (validate || convert) {
        int errors = 0, warnings = 0;
        for (auto& is : issues) {
            const char* sev = is.severity == Severity::Error ? "ERROR" : is.severity == Severity::Warning ? "WARN " : "INFO ";
            if (validate || is.severity != Severity::Info) printf("[%s] %s: %s\n", sev, is.category.c_str(), is.message.c_str());
            if (is.severity == Severity::Error) ++errors;
            if (is.severity == Severity::Warning) ++warnings;
        }
        printf("検証結果: エラー %d / 警告 %d / 全 %zu 件\n", errors, warnings, issues.size());
        exitCode = errors ? 3 : 0;
    }
    if (dump) {
        printf("%s\n", DumpSceneText(scene).c_str());
        exitCode = 0;
    }
    if (convert) {
        if (out.empty()) { PrintHelp(); exitCode = 2; return true; }
        if (!ExportScene(scene, out, eo, err)) {
            fprintf(stderr, "書き出し失敗: %s\n", err.c_str());
            exitCode = 1;
            return true;
        }
        exitCode = 0;
    }
    return true;
}
