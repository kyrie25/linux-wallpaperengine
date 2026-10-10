#include "TextureCache.h"

#include "AlbumTexture.h"
#include "WallpaperEngine/FileSystem/Container.h"

#include "CTexture.h"
#include "ImageDecoder.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Parsers/TextureParser.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <cctype>
#include <stb_image.h>
extern "C" {
#include <libavformat/avformat.h>
}

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Assets;

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

void TextureCache::updateArtwork () { this->m_currentThumbnail->refresh (*this->m_previousThumbnail); }

TextureCache::~TextureCache () { this->m_mediaCallback (); }

std::shared_ptr<const TextureProvider> TextureCache::resolve (const std::string& filename) {
    if (const auto found = this->m_textureCache.find (filename); found != this->m_textureCache.end ()) {
	return found->second;
    }

    if (std::filesystem::path (filename).is_absolute ()) {
        std::ifstream file (filename, std::ios::binary);
        const std::string contents ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());
	int width = 0, height = 0;
        std::string extension = std::filesystem::path (filename).extension ().string ();
        std::ranges::transform (extension, extension.begin (), [] (unsigned char c) { return std::tolower (c); });
        const bool video = extension == ".mp4" || extension == ".webm" || extension == ".mkv"
            || extension == ".mov" || extension == ".avi" || extension == ".m4v";
        if (video) {
            AVFormatContext* format = nullptr;
            if (avformat_open_input (&format, filename.c_str (), nullptr, nullptr) >= 0
                && avformat_find_stream_info (format, nullptr) >= 0) {
                for (unsigned int i = 0; i < format->nb_streams; i++) {
                    const auto* parameters = format->streams[i]->codecpar;
                    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
                        width = parameters->width;
                        height = parameters->height;
                        break;
                    }
                }
            }
            avformat_close_input (&format);
        }
        const bool gif = extension == ".gif" && GifAnimation::canvasSize (contents.data (), contents.size (), width, height);
        const std::unique_ptr<stbi_uc, decltype (&stbi_image_free)> pixels (
            gif || video ? nullptr : decodeImageRGBA (contents.data (), contents.size (), width, height), stbi_image_free
        );
	if ((!gif && !video && pixels == nullptr) || width <= 0 || height <= 0
	    || contents.size () > std::numeric_limits<int>::max ()
	    || static_cast<uint64_t> (width) * height * 4 > std::numeric_limits<int>::max ()) {
	    throw AssetLoadException (
		"Cannot decode external image", filename, std::make_error_code (std::errc::invalid_argument)
	    );
	}

	// Reuse the packed-texture uploader with a single decoded RGBA mipmap.
	auto mipmap = std::make_shared<Mipmap> ();
	mipmap->width = width;
	mipmap->height = height;
	mipmap->uncompressedSize = gif || video ? contents.size () : width * height * 4;
	mipmap->uncompressedData = std::make_unique<char[]> (mipmap->uncompressedSize);
	std::memcpy (mipmap->uncompressedData.get (), gif || video ? static_cast<const void*> (contents.data ()) : pixels.get (), mipmap->uncompressedSize);

	auto header = std::make_unique<Texture> ();
	header->width = header->textureWidth = width;
	header->height = header->textureHeight = height;
	header->flags = TextureFlags_ClampUVs | (gif ? TextureFlags_NoInterpolation : 0);
	header->freeImageFormat = video ? FIF_MP4 : gif ? FIF_GIF : FIF_UNKNOWN;
	header->isAnimatedGif = gif;
	header->isVideoMp4 = video;
	header->format = TextureFormat_ARGB8888;
	header->imageCount = 1;
	header->images.emplace (0, MipmapList { mipmap });
	auto texture = std::make_shared<CTexture> (this->getContext (), std::move (header));
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
