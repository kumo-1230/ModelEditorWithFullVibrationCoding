#include "Dialogs.h"
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <vector>

namespace {
HWND g_owner = nullptr;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// 二重 NUL 終端のフィルタ文字列をワイド文字に変換
std::wstring WidenFilter(const char* f) {
    std::wstring out;
    if (!f) return out;
    const char* p = f;
    while (*p) {
        size_t len = strlen(p);
        out += Widen(std::string(p, len));
        out.push_back(L'\0');
        p += len + 1;
    }
    out.push_back(L'\0');
    return out;
}
} // namespace

namespace Dialogs {
void SetOwner(void* hwnd) { g_owner = (HWND)hwnd; }

std::string OpenFile(const char* filter, const char* title) {
    wchar_t buf[MAX_PATH * 4] = {};
    std::wstring wf = WidenFilter(filter);
    std::wstring wt = title ? Widen(title) : L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_owner;
    ofn.lpstrFilter = wf.empty() ? nullptr : wf.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle = title ? wt.c_str() : nullptr;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) return Narrow(buf);
    return {};
}

std::string SaveFile(const char* filter, const std::string& defaultName, const char* defExt, const char* title) {
    wchar_t buf[MAX_PATH * 4] = {};
    std::wstring wn = Widen(defaultName);
    wcsncpy(buf, wn.c_str(), MAX_PATH * 4 - 1);
    std::wstring wf = WidenFilter(filter);
    std::wstring we = defExt ? Widen(defExt[0] == '.' ? defExt + 1 : defExt) : L"";
    std::wstring wt = title ? Widen(title) : L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_owner;
    ofn.lpstrFilter = wf.empty() ? nullptr : wf.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrDefExt = defExt ? we.c_str() : nullptr;
    ofn.lpstrTitle = title ? wt.c_str() : nullptr;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetSaveFileNameW(&ofn)) return Narrow(buf);
    return {};
}

std::string PickFolder(const char* title) {
    std::string result;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, (void**)&dlg))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (title) dlg->SetTitle(Widen(title).c_str());
        if (SUCCEEDED(dlg->Show(g_owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = Narrow(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return result;
}

bool Confirm(const char* title, const char* message) {
    return MessageBoxW(g_owner, Widen(message).c_str(), Widen(title).c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void OpenInExplorer(const std::string& path) {
    std::wstring arg = L"/select,\"" + Widen(path) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}
} // namespace Dialogs

#else
namespace Dialogs {
void SetOwner(void*) {}
std::string OpenFile(const char*, const char*) { return {}; }
std::string SaveFile(const char*, const std::string&, const char*, const char*) { return {}; }
std::string PickFolder(const char*) { return {}; }
bool Confirm(const char*, const char*) { return true; }
void OpenInExplorer(const std::string&) {}
} // namespace Dialogs
#endif
