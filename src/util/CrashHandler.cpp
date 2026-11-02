#include "CrashHandler.h"
#include "FileUtil.h"
#include "core/Log.h"
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <ctime>
#include <exception>

namespace {
bool g_showMessage = false;

LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    static volatile LONG entered = 0;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_EXECUTE_HANDLER;
    char name[64];
    time_t t = time(nullptr);
    tm lt;
    localtime_s(&lt, &t);
    strftime(name, sizeof(name), "crash_%Y%m%d_%H%M%S.dmp", &lt);
    std::string path = FileUtil::Join(FileUtil::AppDataDir(), name);
    HANDLE f = CreateFileW(FileUtil::ToPath(path).wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool dumped = false;
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(f);
    }
    unsigned code = ep && ep->ExceptionRecord ? (unsigned)ep->ExceptionRecord->ExceptionCode : 0;
    LOG_ERROR("クラッシュしました (例外コード 0x%08x)。ダンプ: %s", code, dumped ? path.c_str() : "(保存失敗)");
    if (g_showMessage) {
        wchar_t msg[1024];
        swprintf(msg, 1024, L"予期しないエラーで終了します (0x%08x)。\n\nクラッシュダンプ:\n%s\n\n不具合報告の際はこのファイルとログ (同じフォルダ) を添付してください。",
                 code, dumped ? FileUtil::ToPath(path).wstring().c_str() : L"(保存できませんでした)");
        MessageBoxW(nullptr, msg, L"ModelEditor", MB_OK | MB_ICONERROR);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void OnTerminate() {
    // 捕まえられなかった C++ 例外
    RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
} // namespace

namespace CrashHandler {
void Install(bool showMessageBox) {
    g_showMessage = showMessageBox;
    SetUnhandledExceptionFilter(OnCrash);
    std::set_terminate(OnTerminate);
    // エラーダイアログ (WER) を出さず自前の処理に任せる
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
}
} // namespace CrashHandler
