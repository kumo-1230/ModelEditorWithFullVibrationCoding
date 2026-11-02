#pragma once
#include "core/Scene.h"
#include <string>
#include <vector>

namespace ImageIO {
// ファイルから読み込んで Texture の encoded / image / mimeType を埋める
bool LoadFile(const std::string& path, Texture& tex, std::string* err = nullptr);
// エンコード済みデータ(png/jpg 等)から読み込む
bool LoadMemory(const uint8_t* data, size_t size, Texture& tex, std::string* err = nullptr);
// 画像を PNG にエンコード
std::vector<uint8_t> EncodePNG(const ImageData& img);
bool SavePNG(const std::string& path, const ImageData& img);
// テクスチャを書き出す用のバイト列 (元データがあればそれを、無ければ PNG)
std::vector<uint8_t> GetEncodedBytes(const Texture& tex, std::string& mimeOut);
std::string MimeFromExtension(const std::string& path);
std::string ExtensionFromMime(const std::string& mime);
std::string GuessMimeFromBytes(const uint8_t* data, size_t size);
} // namespace ImageIO
