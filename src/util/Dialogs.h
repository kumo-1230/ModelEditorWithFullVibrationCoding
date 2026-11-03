#pragma once
#include <string>
#include <vector>

// ネイティブのファイルダイアログ (Windows)。filter は "説明\0*.ext\0...\0" 形式 (二重終端)
namespace Dialogs {
void SetOwner(void* hwnd);
std::string OpenFile(const char* filter, const char* title = nullptr);
// 複数選択できる「開く」ダイアログ。選んだファイルの完全なパスを返す (キャンセルなら空)
std::vector<std::string> OpenFiles(const char* filter, const char* title = nullptr);
// initialDir を指定するとそのフォルダで開く。defExt と同じ拡張子のフィルタを最初に選んでおく
std::string SaveFile(const char* filter, const std::string& defaultName, const char* defExt, const char* title = nullptr, const std::string& initialDir = {});
std::string PickFolder(const char* title = nullptr);
bool Confirm(const char* title, const char* message);
void OpenInExplorer(const std::string& path);
} // namespace Dialogs
