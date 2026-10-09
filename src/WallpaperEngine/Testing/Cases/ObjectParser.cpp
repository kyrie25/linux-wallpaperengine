#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Model.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/FileSystem/Container.h"

using WallpaperEngine::Data::JSON::JSON;
using namespace WallpaperEngine::Data::Model;
using WallpaperEngine::Data::Parsers::ObjectParser;

TEST_CASE ("Image instance textures replace material defaults without losing other slots") {
    auto container = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    auto& files = container->getVFS ();
    files.add ("models/image.json", JSON { { "material", "materials/image.json" } });
    files.add (
	"materials/image.json",
	JSON { { "passes",
		 JSON::array (
		     { { { "shader", "genericimage4" },
			 { "textures", JSON::array ({ "util/white", "mask" }) },
			 { "usertextures", JSON::array ({ "oldproperty", "maskproperty" }) } } }
		 ) } }
    );
    Project project {};
    project.assetLocator = std::make_unique<WallpaperEngine::Assets::AssetLocator> (std::move (container));

    const JSON imageData = {
	{ "id", 322 },
	{ "name", "Custom Image" },
	{ "image", "models/image.json" },
	{ "size", "612 371" },
	{ "instance",
	  { { "textures", JSON::array ({ "bundled-video" }) }, { "usertextures", JSON::array ({ "customimage" }) } } },
    };
    const auto object = ObjectParser::parse (imageData, project);
    REQUIRE (object->is<Image> ());
    const auto* image = object->as<Image> ();
    const auto& pass = *image->model->material->passes.front ();
    CHECK (pass.textures.at (0) == "bundled-video");
    CHECK (pass.textures.at (1) == "mask");
    CHECK (pass.usertextures.at (0) == "customimage");
    CHECK (pass.usertextures.at (1) == "maskproperty");
    CHECK (image->size == glm::vec2 (612.0f, 371.0f));
}
