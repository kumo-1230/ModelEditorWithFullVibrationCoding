#pragma once
// UTF-8 パスを扱うファイルユーティリティ (Windows の日本語パス対応)
#include <filesystem>
#include <string>
#include <vector>

namespace FileUtil {
std::filesystem::path ToPath(const std::string& utf8);
std::string FromPath(const std::filesystem::path& p);

bool Exists(const std::string& path);
bool ReadBinary(const std::string& path, std::vector<uint8_t>& out);
bool WriteBinary(const std::string& path, const void* data, size_t size);
bool WriteText(const std::string& path, const std::string& text);

std::string Filename(const std::string& path);   // "a/b/c.png" -> "c.png"
std::string Stem(const std::string& path);       // -> "c"
std::string Directory(const std::string& path);  // -> "a/b"
std::string LowerExt(const std::string& path);   // -> ".png"
std::string ReplaceExt(const std::string& path, const std::string& ext);
std::string Join(const std::string& a, const std::string& b);
std::string Normalize(const std::string& path);
std::string Relative(const std::string& path, const std::string& base);
std::string SanitizeFilename(const std::string& name);
bool CreateDirs(const std::string& dir);

// テクスチャパスを解決する: 絶対パス/モデル相対/ファイル名のみ/検索ディレクトリ
std::string ResolveTexturePath(const std::string& uri, const std::string& modelDir,
                               const std::vector<std::string>& searchDirs);
std::string ExecutableDir();
// %APPDATA%\ModelEditor (作成できなければ exe のフォルダ)。設定・ログ・クラッシュダンプの保存先
std::string AppDataDir();
std::string DocumentsDir();
} // namespace FileUtil
