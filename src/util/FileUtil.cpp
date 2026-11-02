#include "FileUtil.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace FileUtil {

fs::path ToPath(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string FromPath(const fs::path& p) {
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

bool Exists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(ToPath(path), ec);
}

bool ReadBinary(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(ToPath(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize((size_t)size);
    if (size > 0) f.read((char*)out.data(), size);
    return (bool)f;
}

bool WriteBinary(const std::string& path, const void* data, size_t size) {
    std::ofstream f(ToPath(path), std::ios::binary);
    if (!f) return false;
    f.write((const char*)data, size);
    return (bool)f;
}

bool WriteText(const std::string& path, const std::string& text) { return WriteBinary(path, text.data(), text.size()); }

std::string Filename(const std::string& path) { return FromPath(ToPath(path).filename()); }
std::string Stem(const std::string& path) { return FromPath(ToPath(path).stem()); }
std::string Directory(const std::string& path) { return FromPath(ToPath(path).parent_path()); }

std::string LowerExt(const std::string& path) {
    std::string e = FromPath(ToPath(path).extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

std::string ReplaceExt(const std::string& path, const std::string& ext) {
    fs::path p = ToPath(path);
    p.replace_extension(ToPath(ext));
    return FromPath(p);
}

std::string Join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return FromPath(ToPath(a) / ToPath(b));
}

std::string Normalize(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(ToPath(path), ec);
    if (ec) p = ToPath(path).lexically_normal();
    return FromPath(p);
}

std::string Relative(const std::string& path, const std::string& base) {
    std::error_code ec;
    fs::path r = fs::relative(ToPath(path), ToPath(base), ec);
    if (ec || r.empty()) return Filename(path);
    std::string s = FromPath(r);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string SanitizeFilename(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' || (unsigned char)c < 32)
            out += '_';
        else
            out += c;
    }
    if (out.empty()) out = "unnamed";
    return out;
}

bool CreateDirs(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(ToPath(dir), ec);
    return !ec;
}

std::string ResolveTexturePath(const std::string& uriIn, const std::string& modelDir,
                               const std::vector<std::string>& searchDirs) {
    if (uriIn.empty()) return "";
    std::string uri = uriIn;
    // URL エンコードされた空白など最低限デコード
    {
        std::string dec;
        for (size_t i = 0; i < uri.size(); ++i) {
            if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit((unsigned char)uri[i + 1]) && std::isxdigit((unsigned char)uri[i + 2])) {
                dec += (char)std::stoi(uri.substr(i + 1, 2), nullptr, 16);
                i += 2;
            } else dec += uri[i];
        }
        uri = dec;
    }
    if (uri.rfind("file://", 0) == 0) uri = uri.substr(7);
    std::replace(uri.begin(), uri.end(), '\\', '/');

    std::vector<std::string> candidates;
    candidates.push_back(uri);
    if (!modelDir.empty()) candidates.push_back(Join(modelDir, uri));
    std::string fname = Filename(uri);
    std::vector<std::string> dirs;
    if (!modelDir.empty()) {
        dirs.push_back(modelDir);
        dirs.push_back(Join(modelDir, "textures"));
        dirs.push_back(Join(modelDir, "Textures"));
        dirs.push_back(Join(modelDir, "tex"));
        dirs.push_back(Join(modelDir, "images"));
        dirs.push_back(Join(Directory(modelDir), "textures"));
        dirs.push_back(Join(Directory(modelDir), "Textures"));
        dirs.push_back(Join(modelDir, Stem(uri) + ".fbm"));
    }
    for (auto& d : searchDirs) dirs.push_back(d);
    for (auto& d : dirs) candidates.push_back(Join(d, fname));
    for (auto& c : candidates)
        if (Exists(c)) return Normalize(c);

    // 拡張子違い (.tga -> .png 等) も探す
    static const char* exts[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd" };
    std::string stem = Stem(uri);
    for (auto& d : dirs)
        for (auto* e : exts) {
            std::string c = Join(d, stem + e);
            if (Exists(c)) return Normalize(c);
        }
    return "";
}

std::string ExecutableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return FromPath(fs::path(buf).parent_path());
#else
    return FromPath(fs::current_path());
#endif
}

static std::string KnownFolder(const GUID& id) {
#ifdef _WIN32
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = FromPath(fs::path(p));
    if (p) CoTaskMemFree(p);
    return out;
#else
    (void)id;
    return {};
#endif
}

std::string AppDataDir() {
    static std::string dir = [] {
        std::string base = KnownFolder(FOLDERID_RoamingAppData);
        if (!base.empty()) {
            std::string d = Join(base, "ModelEditor");
            if (CreateDirs(d)) return d;
        }
        return ExecutableDir();
    }();
    return dir;
}

std::string DocumentsDir() {
    std::string d = KnownFolder(FOLDERID_Documents);
    return d.empty() ? ExecutableDir() : d;
}

} // namespace FileUtil
