#include "TextureCache.h"

#include "AlbumTexture.h"
#include "WallpaperEngine/FileSystem/Container.h"

#include "CTexture.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Parsers/TextureParser.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stb_image.h>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Assets;

namespace {
TextureUniquePtr loadExternalImage (const std::filesystem::path& filename) {
    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info (filename.c_str (), &width, &height, &channels) || width <= 0 || height <= 0) {
	throw AssetLoadException (
	    "Cannot decode external image", filename, std::make_error_code (std::errc::invalid_argument)
	);
    }

    std::error_code error;
    const auto size = std::filesystem::file_size (filename, error);
    if (!error && size > static_cast<uintmax_t> (std::numeric_limits<int>::max ())) {
	error = std::make_error_code (std::errc::file_too_large);
    }
    if (error) {
	throw AssetLoadException ("Cannot read external image", filename, error);
    }

    auto mipmap = std::make_shared<Mipmap> ();
    mipmap->width = static_cast<uint32_t> (width);
    mipmap->height = static_cast<uint32_t> (height);
    mipmap->uncompressedSize = static_cast<int> (size);
    mipmap->compressedSize = mipmap->uncompressedSize;
    mipmap->uncompressedData = std::make_unique<char[]> (size);

    std::ifstream stream (filename, std::ios::binary);
    if (!stream.read (mipmap->uncompressedData.get (), static_cast<std::streamsize> (size))) {
	throw AssetLoadException ("Cannot read external image", filename, std::make_error_code (std::errc::io_error));
    }

    auto texture = std::make_unique<Texture> ();
    texture->containerVersion = ContainerVersion_TEXB0003;
    texture->flags = TextureFlags_ClampUVs;
    texture->width = static_cast<uint32_t> (width);
    texture->height = static_cast<uint32_t> (height);
    texture->textureWidth = texture->width;
    texture->textureHeight = texture->height;
    texture->format = TextureFormat_ARGB8888;
    texture->freeImageFormat = FIF_PNG;
    texture->imageCount = 1;
    texture->images.emplace (0, MipmapList { std::move (mipmap) });
    return texture;
}
}

TextureCache::TextureCache (RenderContext& context) : Helpers::ContextAware (context) {
    // these textures are special cases, so make sure they're created only upon request
    this->m_currentThumbnail = std::make_shared<AlbumTexture> (this->getContext ());

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_currentThumbnail->getTextureID (0), -1, "$mediaThumbnail");
#endif

    this->m_previousThumbnail = std::make_shared<AlbumTexture> (this->getContext ());

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_previousThumbnail->getTextureID (0), -1, "$mediaPreviousThumbnail");
#endif

    // load the latest texture (if available)
    this->m_currentThumbnail->load ();

    // add these to the cache and return the right one
    this->store ("$mediaThumbnail", this->m_currentThumbnail);
    this->store ("$mediaPreviousThumbnail", this->m_previousThumbnail);

    this->m_mediaCallback = this->getContext ().getMediaSource ().addAlbumArtListener (
	[this] (const Media::MediaSource::MediaInfo& data) {
	    if (this->m_currentThumbnail->isReady ()) {
		// copy over pixel data and setup the new texture with the new data
		this->m_previousThumbnail->copyContents (*this->m_currentThumbnail);
	    }

	    // load the next image
	    this->m_currentThumbnail->load ();
	}
    );
}

TextureCache::~TextureCache () { this->m_mediaCallback (); }

std::shared_ptr<const TextureProvider> TextureCache::resolve (const std::string& filename) {
    if (const auto found = this->m_textureCache.find (filename); found != this->m_textureCache.end ()) {
	return found->second;
    }

    std::error_code error;
    if (std::filesystem::is_regular_file (filename, error)) {
	auto texture = std::make_shared<CTexture> (this->getContext (), loadExternalImage (filename));
	this->store (filename, texture);
	return texture;
    }

    // search for the texture in all the different containers just in case
    for (const auto& project : this->getContext ().getApp ().getBackgrounds () | std::views::values) {
	try {
	    const auto contents = project->assetLocator->texture (filename);
	    auto stream = BinaryReader (contents);

	    // Create metadata loader lambda that captures the assetLocator
	    // so we need to construct the full path here
	    auto metadataLoader = [&project] (const std::string& metaFilename) -> std::string {
		std::filesystem::path fullPath = std::filesystem::path ("materials") / metaFilename;
		return project->assetLocator->readString (fullPath);
	    };

	    auto parsedTexture = TextureParser::parse (stream, filename, metadataLoader);
	    auto texture = std::make_shared<CTexture> (this->getContext (), std::move (parsedTexture));

#if !NDEBUG
	    glObjectLabel (GL_TEXTURE, texture->getTextureID (0), -1, filename.c_str ());
#endif

	    this->store (filename, texture);

	    return texture;
	} catch (AssetLoadException&) {
	    // ignored, this happens if we're looking at the wrong background
	}
    }

    // TODO: FILL IN WITH A CHECKERED PATTERN TEXTURE INSTEAD?
    throw AssetLoadException ("Cannot find file", filename, std::error_code ());
}

void TextureCache::store (const std::string& name, std::shared_ptr<const TextureProvider> texture) {
    this->m_textureCache.insert_or_assign (name, texture);
}
