// コマンドライン版エントリーポイント (ModelEditorCli.exe)
#include "app/Cli.h"
#include "core/Log.h"
#include "util/CrashHandler.h"
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    CrashHandler::Install(false);
    std::vector<std::string> args;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        args.push_back(s);
    }
    LocalFree(w);
    if (args.size() <= 1) args.push_back("--help");
    int code = 0;
    try {
        if (!RunCli(args, code)) {
            args = { args[0], "--help" };
            RunCli(args, code);
            code = 2;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "例外: %s\n", e.what());
        code = 1;
    }
    return code;
}
