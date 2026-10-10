#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Parsers/TextureParser.h"

#include <sstream>
#include <string>
#include <bit>
#include <limits>

using WallpaperEngine::Data::Parsers::TextureParser;
using WallpaperEngine::Data::Utils::BinaryReader;

namespace {
void append32 (std::string& bytes, uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8)
        bytes.push_back (static_cast<char> (value >> shift));
}

std::string texFile (uint32_t compression, int declaredSize, int payloadSize,
                     std::string payload, uint32_t imageCount = 1, uint32_t mipmapCount = 1,
                     uint32_t flags = 0) {
    std::string bytes = "TEXV0005";
    bytes.push_back ('\0');
    bytes += "TEXI0001";
    bytes.push_back ('\0');
    append32 (bytes, 0); // ARGB8888
    append32 (bytes, flags);
    for (int i = 0; i < 4; ++i) append32 (bytes, 1); // texture/real dimensions
    append32 (bytes, 0); // ignored header field
    bytes += "TEXB0002";
    bytes.push_back ('\0');
    append32 (bytes, imageCount);
    if (imageCount == 0) return bytes;
    append32 (bytes, mipmapCount);
    if (mipmapCount == 0) return bytes;
    append32 (bytes, 1); // mip width
    append32 (bytes, 1); // mip height
    append32 (bytes, compression);
    append32 (bytes, static_cast<uint32_t> (declaredSize));
    append32 (bytes, static_cast<uint32_t> (payloadSize));
    bytes += payload;
    return bytes;
}

void appendFloat (std::string& bytes, float value) {
    append32 (bytes, std::bit_cast<uint32_t> (value));
}

std::string animatedFile (uint32_t page, float duration, uint32_t frameCount = 1) {
    std::string bytes = texFile (0, 4, 4, "ABCD", 1, 1, 4);
    bytes += "TEXS0002";
    bytes.push_back ('\0');
    append32 (bytes, frameCount);
    if (frameCount == 0) return bytes;
    append32 (bytes, page);
    appendFloat (bytes, duration);
    for (float value : {0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f})
        appendFloat (bytes, value);
    return bytes;
}

auto parse (const std::string& bytes) {
    auto stream = std::make_shared<std::istringstream> (bytes, std::ios::in | std::ios::binary);
    return TextureParser::parse (BinaryReader (stream));
}
} // namespace

TEST_CASE ("Texture parser retains native TEXI flag metadata with supported payloads", "[data][texture]") {
    // The installed 3115768633 lighting mask uses 0x800002. The native
    // header reader preserves the high authoring bit alongside clamp (2),
    // without changing the decoded image's content or dimensions.
    for (const uint32_t flags : {0x800002u, 0x80000003u}) {
        const auto texture = parse (texFile (0, 4, 4, "ABCD", 1, 1, flags));
        REQUIRE (texture->flags == flags);
        REQUIRE ((texture->flags & 3u) == (flags & 3u)); // sampler bits unchanged
        REQUIRE (texture->images.at (0).front ()->width == 1);
        REQUIRE (texture->images.at (0).front ()->height == 1);
        REQUIRE (std::string (texture->images.at (0).front ()->uncompressedData.get (), 4) == "ABCD");
    }
}
