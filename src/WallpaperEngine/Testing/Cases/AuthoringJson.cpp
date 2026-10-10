#include <catch2/catch_test_macros.hpp>
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
using namespace WallpaperEngine::Data::JSON;
TEST_CASE ("Authored JSON accepts comments and trailing commas without changing strings", "[json]") {
    const auto value = parseAuthoringJson (R"({
      // top-level comment
      "url": "https://example.test/a//b/*c*/",
      "escaped": "quote: \" // still a string",
      "slashes": "\\/* not a comment */",
      "items": [1, /* between elements */ 2,],
    })", "fixture.json");

    REQUIRE (value["url"] == "https://example.test/a//b/*c*/");
    REQUIRE (value["escaped"] == "quote: \" // still a string");
    REQUIRE (value["slashes"] == "\\/* not a comment */");
    REQUIRE (value["items"].size () == 2);
    REQUIRE (value["items"][1] == 2);
}

TEST_CASE ("Authored JSON reports original source position for malformed input", "[json]") {
    try {
        (void)parseAuthoringJson ("{\n  \"items\": [1,,]\n}", "broken.json");
        FAIL ("Expected syntax error");
    } catch (const JsonSyntaxError& e) {
        REQUIRE (std::string (e.what ()).find ("broken.json:2:") != std::string::npos);
    }
    try {
        (void)parseAuthoringJson ("{\n  /* unterminated", "comment.json");
        FAIL ("Expected unterminated comment error");
    } catch (const JsonSyntaxError& e) {
        REQUIRE (std::string (e.what ()).find ("comment.json:2:3: unterminated block comment")
                 != std::string::npos);
    }
    try {
        (void)parseAuthoringJson ("{\"value\":1e10000}", "overflow.json");
        FAIL ("Expected numeric overflow error");
    } catch (const JsonSyntaxError& e) {
        REQUIRE (std::string (e.what ()).find ("overflow.json:") != std::string::npos);
        REQUIRE (std::string (e.what ()).find ("406") != std::string::npos);
    }
}

TEST_CASE ("Material loader accepts authoring JSON and retains source diagnostics", "[json][material]") {
    auto container = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    container->getVFS ().add ("material.json", R"({
        // Native authoring material
        "passes": [{"shader": "genericimage2", "combos": {"TEST": 2,},},],
    })");
    container->getVFS ().add ("broken-material.json", "{\n  \"passes\": [1,,]\n}");
    WallpaperEngine::Data::Model::Project project {};
    project.assetLocator = std::make_unique<WallpaperEngine::Assets::AssetLocator> (std::move (container));
    const auto material = WallpaperEngine::Data::Parsers::MaterialParser::load (project, "material.json");
    REQUIRE (material->passes.size () == 1);
    REQUIRE (material->passes[0]->shader == "genericimage2");
    REQUIRE (material->passes[0]->combos.at ("TEST") == 2);
    try {
        (void)WallpaperEngine::Data::Parsers::MaterialParser::load (project, "broken-material.json");
        FAIL ("Expected malformed material error");
    } catch (const JsonSyntaxError& e) {
        REQUIRE (std::string (e.what ()).find ("broken-material.json:2:") != std::string::npos);
    }
}
