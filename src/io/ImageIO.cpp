#include "ImageIO.h"
#include "core/Log.h"
#include "util/FileUtil.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace ImageIO {

std::string MimeFromExtension(const std::string& path) {
    std::string ext = FileUtil::LowerExt(path);
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".tga") return "image/x-tga";
    if (ext == ".gif") return "image/gif";
    if (ext == ".psd") return "image/vnd.adobe.photoshop";
    if (ext == ".hdr") return "image/vnd.radiance";
    if (ext == ".webp") return "image/webp";
    if (ext == ".dds") return "image/vnd-ms.dds";
    return "application/octet-stream";
}

std::string ExtensionFromMime(const std::string& mime) {
    if (mime == "image/png") return ".png";
    if (mime == "image/jpeg") return ".jpg";
    if (mime == "image/bmp") return ".bmp";
    if (mime == "image/x-tga") return ".tga";
    return ".png";
}

std::string GuessMimeFromBytes(const uint8_t* d, size_t n) {
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return "image/bmp";
    if (n >= 4 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return "image/gif";
    if (n >= 4 && d[0] == '8' && d[1] == 'B' && d[2] == 'P' && d[3] == 'S') return "image/vnd.adobe.photoshop";
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) return "image/webp";
    if (n >= 4 && memcmp(d, "DDS ", 4) == 0) return "image/vnd-ms.dds";
    return "image/x-tga"; // TGA にはマジックが無い
}

bool LoadMemory(const uint8_t* data, size_t size, Texture& tex, std::string* err) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4);
    tex.encoded = std::make_shared<std::vector<uint8_t>>(data, data + size);
    tex.mimeType = GuessMimeFromBytes(data, size);
    if (!px) {
        if (err) *err = std::string("画像のデコードに失敗: ") + (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        tex.image.reset();
        return false;
    }
    auto img = std::make_shared<ImageData>();
    img->width = w;
    img->height = h;
    img->rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    tex.image = img;
    return true;
}

bool LoadFile(const std::string& path, Texture& tex, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!FileUtil::ReadBinary(path, bytes)) {
        if (err) *err = "ファイルを開けません: " + path;
        return false;
    }
    bool ok = LoadMemory(bytes.data(), bytes.size(), tex, err);
    tex.sourcePath = path;
    if (tex.name.empty()) tex.name = FileUtil::Stem(path);
    return ok;
}

static void WriteToVector(void* ctx, void* data, int size) {
    auto* v = (std::vector<uint8_t>*)ctx;
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

std::vector<uint8_t> EncodePNG(const ImageData& img) {
    std::vector<uint8_t> out;
    if (img.width <= 0 || img.height <= 0) return out;
    stbi_write_png_to_func(WriteToVector, &out, img.width, img.height, 4, img.rgba.data(), img.width * 4);
    return out;
}

bool SavePNG(const std::string& path, const ImageData& img) {
    auto bytes = EncodePNG(img);
    return !bytes.empty() && FileUtil::WriteBinary(path, bytes.data(), bytes.size());
}

std::vector<uint8_t> GetEncodedBytes(const Texture& tex, std::string& mimeOut) {
    // glTF は PNG/JPEG のみ正式サポート。それ以外は PNG に再エンコードする
    if (tex.encoded && !tex.encoded->empty() && (tex.mimeType == "image/png" || tex.mimeType == "image/jpeg")) {
        mimeOut = tex.mimeType;
        return *tex.encoded;
    }
    mimeOut = "image/png";
    if (tex.image) return EncodePNG(*tex.image);
    return {};
}

} // namespace ImageIO
