#pragma once
#include <string>

// ネイティブのファイルダイアログ (Windows)。filter は "説明\0*.ext\0...\0" 形式 (二重終端)
namespace Dialogs {
void SetOwner(void* hwnd);
std::string OpenFile(const char* filter, const char* title = nullptr);
std::string SaveFile(const char* filter, const std::string& defaultName, const char* defExt, const char* title = nullptr);
std::string PickFolder(const char* title = nullptr);
bool Confirm(const char* title, const char* message);
void OpenInExplorer(const std::string& path);
} // namespace Dialogs
