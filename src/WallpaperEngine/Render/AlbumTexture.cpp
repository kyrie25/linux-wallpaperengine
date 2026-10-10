#include "AlbumTexture.h"
#include "ScopedPixelUnpack.h"

#include "RenderContext.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Media/MediaSource.h"
#include "WallpaperEngine/Media/MediaArtwork.h"
#include <iterator>

using namespace WallpaperEngine::Render;


void WallpaperEngine::Render::uploadAlbumArtworkTexture (
    GLuint texture, const WallpaperEngine::Media::MediaArtwork* artwork
) {
    TightPixelTransfer transfer;
    glBindTexture (GL_TEXTURE_2D, texture);
    if (!artwork) {
        constexpr std::uint32_t transparent = 0;
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, &transparent);
        return;
    }
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, artwork->width, artwork->height,
                  0, GL_RGBA, GL_UNSIGNED_BYTE, artwork->rgba.data ());
}

AlbumTexture::AlbumTexture (RenderContext& context) : Helpers::ContextAware (context) {
    // setup a basic texture with clamping and no mipmaps
    this->m_resolution = glm::vec4 (1.0f, 1.0f, 1.0f, 1.0f);

    glGenTextures (1, &this->m_textureID);
    glBindTexture (GL_TEXTURE_2D, this->m_textureID);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
}

AlbumTexture::~AlbumTexture () { glDeleteTextures (1, &this->m_textureID); }

GLuint AlbumTexture::getTextureID (uint32_t imageIndex) const { return this->m_textureID; }
uint32_t AlbumTexture::getTextureWidth (uint32_t imageIndex) const { return this->m_width; }
uint32_t AlbumTexture::getTextureHeight (uint32_t imageIndex) const { return this->m_height; }
uint32_t AlbumTexture::getRealWidth () const { return this->m_width; }
uint32_t AlbumTexture::getRealHeight () const { return this->m_height; }
TextureFormat AlbumTexture::getFormat () const { return TextureFormat_ARGB8888; }
uint32_t AlbumTexture::getFlags () const { return TextureFlags_NoFlags; }
const std::vector<FrameSharedPtr>& AlbumTexture::getFrames () const { return this->m_frames; }
const glm::vec4* AlbumTexture::getResolution () const { return &this->m_resolution; }
bool AlbumTexture::isAnimated () const { return false; }
uint32_t AlbumTexture::getSpritesheetCols () const { return 1; }
uint32_t AlbumTexture::getSpritesheetRows () const { return 1; }
uint32_t AlbumTexture::getSpritesheetFrames () const { return 1; }
float AlbumTexture::getSpritesheetDuration () const { return 0.0f; }

void AlbumTexture::incrementUsageCount () const { }
void AlbumTexture::decrementUsageCount () const { }
void AlbumTexture::update () const { }
void AlbumTexture::refresh (const AlbumTexture& previous) const {
    const auto now = std::chrono::steady_clock::now ();
    if (now < m_nextProbe) return;
    m_nextProbe = now + std::chrono::seconds (1);
    const auto& url = getContext ().getMediaSource ().getMediaInfo ().url;
    // Players sometimes publish a file URL before creating its image, or overwrite
    // the same path for the next cover without a Metadata signal.
    if (url && url->starts_with ("file://") && m_artworkCache.load (*url) != m_loadedArtwork) {
        if (isReady ()) previous.copyContents (*this);
        load ();
    }
}

void AlbumTexture::copyContents (const AlbumTexture& other) const noexcept {
    // Immutable decoded snapshots also preserve the previous cover without a GPU readback.
    m_loadedArtwork = other.m_loadedArtwork;
    m_width = other.m_width;
    m_height = other.m_height;
    m_resolution = other.m_resolution;
    uploadAlbumArtworkTexture (m_textureID, m_loadedArtwork.get ());
}

void AlbumTexture::load () const {
    const auto& url = this->getContext ().getMediaSource ().getMediaInfo ().url;
    std::shared_ptr<const Media::MediaArtwork> artwork;
    if (url && url->starts_with ("file://")) {
        artwork = this->m_artworkCache.load (*url);
    } else if (url) {
        for (const auto& project : this->getContext ().getApp ().getBackgrounds () | std::views::values) {
            try {
                auto contents = project->assetLocator->read ("$mediaThumbnail");
                std::vector<std::uint8_t> bytes (std::istreambuf_iterator<char> (*contents), {});
                artwork = Media::decodeArtworkBytes (bytes);
                if (artwork) break;
            } catch (AssetLoadException&) { }
        }
    }
    this->m_width = artwork ? artwork->width : 0;
    this->m_height = artwork ? artwork->height : 0;
    if (artwork == m_loadedArtwork) return;
    m_loadedArtwork = artwork;
    if (!artwork) {
        m_resolution = glm::vec4 (1);
        uploadAlbumArtworkTexture (m_textureID, nullptr);
        return;
    }
    this->m_resolution = glm::vec4 (artwork->width, artwork->height, artwork->width, artwork->height);
    uploadAlbumArtworkTexture (this->m_textureID, artwork.get ());
}

bool AlbumTexture::isReady () const {
    // these are only ready to be rendered if their content's are present
    return this->m_width > 0 && this->m_height > 0;
}
