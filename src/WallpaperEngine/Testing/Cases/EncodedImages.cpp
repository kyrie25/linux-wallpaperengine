#include <catch2/catch_test_macros.hpp>
#include "WallpaperEngine/Render/GifAnimation.h"
#include "WallpaperEngine/Render/ImageDecoder.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace WallpaperEngine::Render;
namespace {
std::string fixture (const std::string& name) {
    const auto path = std::filesystem::path (__FILE__).parent_path ().parent_path () / "Fixtures" / name;
    std::ifstream input (path, std::ios::binary);
    return {std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char> ()};
}
using Pixels = std::unique_ptr<stbi_uc, decltype (&stbi_image_free)>;
}

TEST_CASE ("Plain GIFs retain authored delays and loop through all frames", "[image][gif]") {
    const auto bytes = fixture ("timing.gif");
    auto gif = GifAnimation::open (bytes.data (), bytes.size ());
    REQUIRE (gif);
    REQUIRE (gif->advance (0));
    REQUIRE (gif->width () == 64);
    REQUIRE (gif->height () == 32);
    REQUIRE (gif->pixels ()[0] == 255);
    REQUIRE_FALSE (gif->advance (.15f));
    REQUIRE (gif->advance (.06f));
    REQUIRE (gif->pixels ()[1] == 128);
    REQUIRE_FALSE (gif->advance (.7f));
    REQUIRE (gif->advance (.1f));
    REQUIRE (gif->pixels ()[2] == 255);
    REQUIRE (gif->advance (.41f));
    REQUIRE (gif->pixels ()[0] == 255);
}

TEST_CASE ("GIF restore-previous disposal preserves earlier transparent regions", "[image][gif]") {
    const auto bytes = fixture ("restore.gif");
    auto gif = GifAnimation::open (bytes.data (), bytes.size ());
    REQUIRE (gif);
    for (int frame = 0; frame < 3; frame++) {
        REQUIRE (gif->advance (frame == 0 ? 0 : .101f));
        const auto reference = fixture ("restore-" + std::to_string (frame) + ".png");
        int width = 0, height = 0;
        Pixels pixels (decodeImageRGBA (reference.data (), reference.size (), width, height), stbi_image_free);
        REQUIRE (pixels);
        REQUIRE (width == gif->width ());
        REQUIRE (height == gif->height ());
        REQUIRE (std::memcmp (pixels.get (), gif->pixels (), width * height * 4) == 0);
    }
}

TEST_CASE ("Malformed GIFs cannot allocate overflowed canvases or decode missing data", "[image][gif]") {
    for (const std::string bytes : {std::string ("GIF89a"), std::string ("GIF89a\xff\xff\xff\xff", 10),
                                  std::string ("GIF89a\0\0\0\0", 10)}) {
        auto gif = GifAnimation::open (bytes.data (), bytes.size ());
        REQUIRE (gif);
        REQUIRE_FALSE (gif->advance (0));
    }
}

TEST_CASE ("GIF background and disposal colors use the same RGBA order as raster pixels", "[image][gif]") {
    const auto bytes = fixture ("background.gif");
    auto gif = GifAnimation::open (bytes.data (), bytes.size ());
    REQUIRE (gif);
    for (int frame = 0; frame < 2; frame++) {
        REQUIRE (gif->advance (frame == 0 ? 0 : .101f));
        const auto* background = gif->pixels () + (7 * 8 + 7) * 4;
        REQUIRE (background[0] == 255);
        REQUIRE (background[1] == 0);
        REQUIRE (background[2] == 0);
        REQUIRE (background[3] == 255);
        if (frame == 1) {
            REQUIRE (gif->pixels ()[0] == 255);
            REQUIRE (gif->pixels ()[2] == 0);
        }
    }
}

TEST_CASE ("JPEG decoding and size probing agree for all eight EXIF orientations", "[image][jpeg]") {
    const auto bytes = fixture ("orientation.jpg");
    int sourceWidth = 0, sourceHeight = 0;
    Pixels source (decodeImageRGBA (bytes.data (), bytes.size (), sourceWidth, sourceHeight), stbi_image_free);
    REQUIRE (source);
    for (int orientation = 1; orientation <= 8; orientation++) {
        // APP1 with little-endian TIFF, a single SHORT orientation tag.
        std::string metadata ("Exif\0\0II\x2a\0\x08\0\0\0\x01\0\x12\x01\x03\0\x01\0\0\0", 24);
        metadata += static_cast<char> (orientation);
        metadata.append (7, '\0');
        std::string tagged = bytes.substr (0, 2) + std::string ("\xff\xe1", 2);
        tagged += static_cast<char> ((metadata.size () + 2) >> 8);
        tagged += static_cast<char> (metadata.size () + 2);
        tagged += metadata + bytes.substr (2);
        int width = 0, height = 0, probeWidth = 0, probeHeight = 0;
        Pixels decoded (decodeImageRGBA (tagged.data (), tagged.size (), width, height), stbi_image_free);
        REQUIRE (decoded);
        REQUIRE (decodedImageSize (tagged.data (), tagged.size (), probeWidth, probeHeight));
        REQUIRE (width == probeWidth);
        REQUIRE (height == probeHeight);
        REQUIRE (width == (orientation >= 5 ? sourceHeight : sourceWidth));
        REQUIRE (height == (orientation >= 5 ? sourceWidth : sourceHeight));
        const std::array<std::array<int, 6>, 8> expected {{
            {{0, 1, 2, 3, 4, 5}}, {{2, 1, 0, 5, 4, 3}},
            {{5, 4, 3, 2, 1, 0}}, {{3, 4, 5, 0, 1, 2}},
            {{0, 3, 1, 4, 2, 5}}, {{3, 0, 4, 1, 5, 2}},
            {{5, 2, 4, 1, 3, 0}}, {{2, 5, 1, 4, 0, 3}},
        }};
        for (int pixel = 0; pixel < 6; pixel++) {
            REQUIRE (std::memcmp (decoded.get () + pixel * 4,
                source.get () + expected[orientation - 1][pixel] * 4, 4) == 0);
        }
    }
}
