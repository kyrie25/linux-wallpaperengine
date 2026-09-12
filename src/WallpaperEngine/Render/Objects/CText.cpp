#include "CText.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

using namespace WallpaperEngine::Render::Objects;

namespace {
constexpr float kPointSizeScale = 4.0f;

// TODO: Phase 2 – load font from wallpaper's materials/fonts/ using AssetLocator
// Phase 1 uses a system font instead of the font shipped by the wallpaper.
// Wallpaper Engine bundles .ttf files in `materials/fonts/`; wiring those in
// is deferred to Phase 2 along with dynamic/scripted text.
const std::vector<std::string> kFontCandidates = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

const std::vector<std::string> kFallbackFontCandidates = {
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/local/share/fonts/WindowsFonts/msgothic.ttc",
};

std::vector<uint32_t> decodeUtf8 (std::string_view input) {
    std::vector<uint32_t> result;
    result.reserve (input.size ());

    for (size_t index = 0; index < input.size ();) {
	const auto first = static_cast<uint8_t> (input[index]);
	if (first < 0x80) {
	    result.push_back (first);
	    index++;
	    continue;
	}

	size_t length = 0;
	uint32_t codepoint = 0;
	if ((first & 0xE0) == 0xC0) {
	    length = 2;
	    codepoint = first & 0x1F;
	} else if ((first & 0xF0) == 0xE0) {
	    length = 3;
	    codepoint = first & 0x0F;
	} else if ((first & 0xF8) == 0xF0) {
	    length = 4;
	    codepoint = first & 0x07;
	}

	if (length == 0 || index + length > input.size ()) {
	    result.push_back (0xFFFD);
	    index++;
	    continue;
	}

	bool valid = true;
	for (size_t offset = 1; offset < length; offset++) {
	    const auto continuation = static_cast<uint8_t> (input[index + offset]);
	    if ((continuation & 0xC0) != 0x80) {
		valid = false;
		break;
	    }
	    codepoint = (codepoint << 6) | (continuation & 0x3F);
	}

	const bool overlong = (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800)
	    || (length == 4 && codepoint < 0x10000);
	if (!valid || overlong || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
	    result.push_back (0xFFFD);
	    index++;
	    continue;
	}

	result.push_back (codepoint);
	index += length;
    }

    return result;
}

const char* kVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(
#version 330 core
in vec2 vUV;
uniform sampler2D uTexture;
uniform vec4 uColor;
out vec4 FragColor;
void main() {
    float coverage = texture(uTexture, vUV).r;
    FragColor = vec4(uColor.rgb, uColor.a * coverage);
}
)glsl";

GLuint compileShader (GLenum type, const char* source) {
    GLuint shader = glCreateShader (type);
    glShaderSource (shader, 1, &source, nullptr);
    glCompileShader (shader);

    GLint status = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
	char log[1024];
	glGetShaderInfoLog (shader, sizeof (log), nullptr, log);
	sLog.error ("CText shader compile failed: ", log);
	glDeleteShader (shader);
	return 0;
    }
    return shader;
}
} // namespace

CText::CText (Wallpapers::CScene& scene, const Text& text) :
    CObject (scene, text), ScriptableObject (scene, text), m_text (text) {
    this->registerProperty ("color", *text.color->value);
    this->registerProperty ("alpha", *text.alpha->value);
    this->registerProperty ("origin", *text.origin->value);
    this->registerProperty ("scale", *text.scale->value);
    this->registerProperty ("visible", *text.visible->value);
    this->registerProperty ("pointSize", *text.pointSize->value);
    this->registerProperty ("text", *text.text->value);
    this->registerProperty ("pointSize", *text.pointSize->value);
}

CText::~CText () {
    if (m_layerHandle != Scripting::kInvalidLayerHandle) {
	this->getScene ().getScriptEngine ().destroyLayer (m_layerHandle);
	m_layerHandle = Scripting::kInvalidLayerHandle;
    }
    if (m_vbo != 0) {
	glDeleteBuffers (1, &m_vbo);
    }
    if (m_vao != 0) {
	glDeleteVertexArrays (1, &m_vao);
    }
    if (m_program != 0) {
	glDeleteProgram (m_program);
    }
    if (m_texture != 0) {
	glDeleteTextures (1, &m_texture);
    }
    if (m_ftFace != nullptr) {
	FT_Done_Face (m_ftFace);
    }
    if (m_fallbackFace != nullptr) {
	FT_Done_Face (m_fallbackFace);
    }
    if (m_ftLibrary != nullptr) {
	FT_Done_FreeType (m_ftLibrary);
    }
}

void CText::setup () {
    const bool scripted = m_text.text->value->getScriptSource ().has_value ();
    const auto& text = m_text.text->value->getString ();

    // Nothing to render and no script to produce text later → bail.
    if (text.empty () && !scripted) {
	return;
    }

    if (!initFreeType ()) {
	return;
    }

    if (!loadEmbeddedFont () && !loadSystemFont ()) {
	return;
    }
    loadFallbackFont ();

    m_lastPixelSize = computeEffectivePixelSize ();
    FT_Set_Pixel_Sizes (m_ftFace, 0, static_cast<FT_UInt> (m_lastPixelSize));
    if (m_fallbackFace != nullptr) {
	FT_Set_Pixel_Sizes (m_fallbackFace, 0, static_cast<FT_UInt> (m_lastPixelSize));
    }

    buildShader ();
    // Scripted text may have an empty placeholder; use a single space so the
    // glyph texture has non-zero dimensions until the script produces a value.
    rebuildTextureFrom (text.empty () ? std::string (" ") : text);

    if (scripted) {
	initScriptLayer ();
    }

    m_valid = m_texture != 0 && m_program != 0 && m_vao != 0;
}

bool CText::initFreeType () {
    if (FT_Init_FreeType (&m_ftLibrary) == 0) {
	return true;
    }
    sLog.error ("CText: FT_Init_FreeType failed for object ", m_text.name);
    return false;
}

bool CText::loadEmbeddedFont () {
    // Wallpapers packed in .pkg don't expose physical paths, so we read the font
    // into memory and use FT_New_Memory_Face. m_fontData must outlive the face.
    // `systemfont_*` references signal "use a system font"; let the fallback handle them.
    if (m_text.font.empty () || m_text.font.rfind ("systemfont_", 0) == 0) {
	return false;
    }

    try {
	auto stream = getAssetLocator ().read (m_text.font);
	stream->seekg (0, std::ios::end);
	const auto size = stream->tellg ();
	stream->seekg (0, std::ios::beg);
	m_fontData.resize (static_cast<size_t> (size));
	stream->read (reinterpret_cast<char*> (m_fontData.data ()), size);

	if (FT_New_Memory_Face (
		m_ftLibrary, m_fontData.data (), static_cast<FT_Long> (m_fontData.size ()), 0, &m_ftFace
	    )
	    == 0) {
	    return true;
	}

	sLog.error ("CText: FT_New_Memory_Face failed for '", m_text.font, "', falling back to system font");
    } catch (const std::exception& e) {
	sLog.error ("CText: cannot read font '", m_text.font, "': ", e.what (), ", falling back to system font");
    }

    m_fontData.clear ();
    return false;
}

bool CText::loadSystemFont () {
    std::string fontPath;
    for (const auto& candidate : kFontCandidates) {
	if (std::filesystem::exists (candidate)) {
	    fontPath = candidate;
	    break;
	}
    }
    if (fontPath.empty ()) {
	sLog.error ("CText: no usable system font found");
	return false;
    }
    if (FT_New_Face (m_ftLibrary, fontPath.c_str (), 0, &m_ftFace) != 0) {
	sLog.error ("CText: FT_New_Face failed for ", fontPath);
	return false;
    }
    return true;
}

bool CText::loadFallbackFont () {
    for (const auto& candidate : kFallbackFontCandidates) {
	if (std::filesystem::exists (candidate) && FT_New_Face (m_ftLibrary, candidate.c_str (), 0, &m_fallbackFace) == 0) {
	    return true;
	}
    }
    return false;
}

FT_Face CText::glyphFace (uint32_t codepoint) const {
    if (FT_Get_Char_Index (m_ftFace, codepoint) != 0 || m_fallbackFace == nullptr) {
	return m_ftFace;
    }
    return m_fallbackFace;
}

float CText::maxTextureWidth () const {
    if (!m_text.parent.has_value ()) {
	return 0.0f;
    }

    const auto* parent = dynamic_cast<const CImage*> (this->getScene ().getObject (*m_text.parent));
    if (parent == nullptr) {
	return 0.0f;
    }

    const float authoredParentWidth = parent->getImage ().size.x;
    const float parentLocalWidth = authoredParentWidth > 0.0f ? authoredParentWidth : parent->getSize ().x;
    if (parentLocalWidth < m_text.size.x) {
	return 0.0f;
    }

    const auto textTransform = this->resolveTransform (m_text);
    const auto parentTransform = this->resolveTransform (parent->getImage ());
    if (std::abs (textTransform.angle - parentTransform.angle) > 0.0001f || textTransform.scale.x == 0.0f) {
	return 0.0f;
    }

    const float parentWidth = parentLocalWidth * std::abs (parentTransform.scale.x);
    float parentLeft = parentTransform.origin.x - parentWidth * 0.5f;
    float parentRight = parentTransform.origin.x + parentWidth * 0.5f;
    if (parent->getImage ().alignment.find ("left") != std::string::npos) {
	parentLeft = parentTransform.origin.x;
	parentRight = parentLeft + parentWidth;
    } else if (parent->getImage ().alignment.find ("right") != std::string::npos) {
	parentRight = parentTransform.origin.x;
	parentLeft = parentRight - parentWidth;
    }

    const float textScale = std::abs (textTransform.scale.x);
    const float horizontalPadding = m_text.padding.x * textScale;
    if (m_text.alignment == "left") {
	const float textLeft = textTransform.origin.x;
	return std::max (0.0f, (parentRight - horizontalPadding - textLeft) / textScale);
    }
    if (m_text.alignment == "right") {
	const float textRight = textTransform.origin.x;
	return std::max (0.0f, (textRight - parentLeft - horizontalPadding) / textScale);
    }

    const float leftSpace = textTransform.origin.x - parentLeft - horizontalPadding;
    const float rightSpace = parentRight - textTransform.origin.x - horizontalPadding;
    return std::max (0.0f, 2.0f * std::min (leftSpace, rightSpace) / textScale);
}

unsigned int CText::computeEffectivePixelSize () const {
    // WE text objects often come with scale ~0.09 that, combined with a modest
    // pointsize, would rasterize glyphs to ~2px on screen (invisible). Rasterize
    // at higher resolution so that after the model scale is applied in render()
    // the on-screen size matches the intended pointsize.
    const glm::vec3 initialScale = this->resolveTransform (m_text).scale;
    const float avgScale = (initialScale.x + initialScale.y) * 0.5f;
    const float compensate = (avgScale > 0.0f && avgScale < 1.0f) ? std::min (1.0f / avgScale, 32.0f) : 1.0f;
    return std::max<unsigned int> (
	1u, static_cast<unsigned int> (m_text.pointSize->value->getFloat () * kPointSizeScale * compensate)
    );
}

void CText::initScriptLayer () {
    const auto& script = m_text.text->value->getScriptSource ();

    if (!script.has_value ()) {
	return;
    }

    m_layerHandle = this->getScene ().getScriptEngine ().createLayerScript (
	*script, m_text.text->value->getProperties (), m_text.text->value->getString ()
    );

    if (m_layerHandle == Scripting::kInvalidLayerHandle) {
	sLog.error ("CText: createLayerScript failed for '", m_text.name, "'");
    }
}

void CText::rebuildTextureFrom (const std::string& text) {
    // Two-pass rasterization: first measure the bounding box, then rasterize
    // every glyph into a single grayscale bitmap. Phase 1 renders one line —
    // multi-line wrapping, alignment, and padding come with Phase 2.
    //
    // Safe to call repeatedly: GL handles (texture, VAO, VBO) are reused when
    // already allocated, so dynamic/scripted text can regenerate the glyph
    // bitmap every time the rendered string changes without leaking.
    const auto codepoints = decodeUtf8 (text);

    int penX = 0;
    int maxAscent = 0;
    int maxDescent = 0;

    for (uint32_t codepoint : codepoints) {
	FT_Face face = glyphFace (codepoint);
	if (FT_Load_Char (face, codepoint, FT_LOAD_RENDER) != 0) {
	    continue;
	}
	FT_GlyphSlot slot = face->glyph;
	penX += slot->advance.x >> 6;
	maxAscent = std::max (maxAscent, slot->bitmap_top);
	maxDescent = std::max (maxDescent, static_cast<int> (slot->bitmap.rows) - slot->bitmap_top);
    }

    const float widthLimit = maxTextureWidth ();
    const int width = std::max (1, widthLimit > 0.0f ? std::min (penX, static_cast<int> (widthLimit)) : penX);
    const int height = std::max (1, maxAscent + maxDescent);
    std::vector<uint8_t> pixels (static_cast<size_t> (width) * height, 0);

    penX = 0;
    for (uint32_t codepoint : codepoints) {
	FT_Face face = glyphFace (codepoint);
	if (FT_Load_Char (face, codepoint, FT_LOAD_RENDER) != 0) {
	    continue;
	}

	FT_GlyphSlot slot = face->glyph;
	const auto& bmp = slot->bitmap;
	const int originX = penX + slot->bitmap_left;
	const int originY = maxAscent - slot->bitmap_top;

	for (unsigned int row = 0; row < bmp.rows; ++row) {
	    for (unsigned int col = 0; col < bmp.width; ++col) {
		const int dstX = originX + static_cast<int> (col);
		const int dstY = originY + static_cast<int> (row);
		if (dstX < 0 || dstX >= width || dstY < 0 || dstY >= height) {
		    continue;
		}
		pixels[static_cast<size_t> (dstY) * width + dstX] = bmp.buffer[row * bmp.pitch + col];
	    }
	}

	penX += slot->advance.x >> 6;
    }

    const bool firstUpload = (m_texture == 0);
    if (firstUpload) {
	glGenTextures (1, &m_texture);
    }
    glBindTexture (GL_TEXTURE_2D, m_texture);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels.data ());
    if (firstUpload) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    m_textureSize = { width, height };
    m_quadSize = { static_cast<float> (width), static_cast<float> (height) };
    m_lastRenderedText = text;

    uploadQuadVertices ();
}

void CText::buildShader () {
    GLuint vs = compileShader (GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader (GL_FRAGMENT_SHADER, kFragmentShader);
    if (vs == 0 || fs == 0) {
	if (vs) {
	    glDeleteShader (vs);
	}
	if (fs) {
	    glDeleteShader (fs);
	}
	return;
    }

    m_program = glCreateProgram ();
    glAttachShader (m_program, vs);
    glAttachShader (m_program, fs);
    glLinkProgram (m_program);
    glDeleteShader (vs);
    glDeleteShader (fs);

    GLint status = GL_FALSE;
    glGetProgramiv (m_program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
	char log[1024];
	glGetProgramInfoLog (m_program, sizeof (log), nullptr, log);
	sLog.error ("CText program link failed: ", log);
	glDeleteProgram (m_program);
	m_program = 0;
	return;
    }

    m_uMVP = glGetUniformLocation (m_program, "uMVP");
    m_uColor = glGetUniformLocation (m_program, "uColor");
    m_uTexture = glGetUniformLocation (m_program, "uTexture");
}

void CText::uploadQuadVertices () {
    // The authored origin already includes horizontal padding. Anchor the
    // tightly packed glyph texture directly to that origin.
    float left = m_quadSize.x * -0.5f;
    if (m_text.alignment == "left") {
	left = 0.0f;
    } else if (m_text.alignment == "right") {
	left = -m_quadSize.x;
    }
    const float right = left + m_quadSize.x;
    const float top = m_text.verticalalign == "center" ? -m_text.padding.y : 0.0f;
    const float bottom = top + m_quadSize.y;
    // With vflip=true (Wayland/GLFW), GL y- = screen top. So the quad bottom (y=0,
    // lower GL y) appears at screen top. UV.v=0 here = FT glyph top → shows at screen top ✓
    const float verts[] = {
	// pos        // uv
	left, top, 0.0f, 0.0f, right, top, 1.0f, 0.0f, right, bottom, 1.0f, 1.0f,
	left, top, 0.0f, 0.0f, right, bottom, 1.0f, 1.0f, left, bottom, 0.0f, 1.0f,
    };

    const bool firstUpload = (m_vao == 0);
    if (firstUpload) {
	glGenVertexArrays (1, &m_vao);
	glGenBuffers (1, &m_vbo);
    }
    glBindVertexArray (m_vao);
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (GL_ARRAY_BUFFER, sizeof (verts), verts, GL_DYNAMIC_DRAW);
    if (firstUpload) {
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), reinterpret_cast<void*> (0));
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (
	    1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), reinterpret_cast<void*> (2 * sizeof (float))
	);
    }
    glBindVertexArray (0);
}

void CText::render () {
    if (!m_valid) {
	return;
    }
    if (!m_text.visible->value->getBool ()) {
	return;
    }

#if !NDEBUG
    std::string str = "Text " + this->getObject ().name + " (" + std::to_string (this->getObject ().id) + ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */
    std::string renderedText = m_lastRenderedText;
    if (m_layerHandle != Scripting::kInvalidLayerHandle) {
	auto& se = this->getScene ().getScriptEngine ();
	se.tickLayer (
	    m_layerHandle, static_cast<double> (getScene ().getTime ()),
	    static_cast<double> (getScene ().getDeltaTime ()), static_cast<double> (getScene ().getFps ())
	);
	const std::string current = se.layerText (m_layerHandle);
	renderedText = current.empty () ? std::string (" ") : current;
    }

    const unsigned int pixelSize = computeEffectivePixelSize ();
    if (pixelSize != m_lastPixelSize) {
	m_lastPixelSize = pixelSize;
	FT_Set_Pixel_Sizes (m_ftFace, 0, static_cast<FT_UInt> (m_lastPixelSize));
	if (m_fallbackFace != nullptr) {
	    FT_Set_Pixel_Sizes (m_fallbackFace, 0, static_cast<FT_UInt> (m_lastPixelSize));
	}
	rebuildTextureFrom (renderedText);
    } else if (renderedText != m_lastRenderedText) {
	rebuildTextureFrom (renderedText);
    }

    const glm::vec4 color = m_text.color->value->getVec4 ();
    const float alpha = m_text.alpha->value->getFloat ();

    // Text objects can be parented to other objects (e.g. clock widgets composed of
    // several text layers); resolve origin/scale/angle through the parent chain.
    const auto transform = this->resolveTransform (m_text);

    // WE uses a Y-down coordinate system (origin at top-left, y increases downward).
    // The final FBO is presented to screen with vflip=true on Wayland/GLFW, which maps
    // GL y- to screen top and GL y+ to screen bottom. This matches CImage's mapping:
    // gl_y = scene_h/2 - origin.y, corrected back by the vflip on presentation.
    const float scene_w = getScene ().getCamera ().getWidth ();
    const float scene_h = getScene ().getCamera ().getHeight ();
    const glm::vec3 gl_origin = {
	transform.origin.x - scene_w * 0.5f,
	scene_h * 0.5f - transform.origin.y,
	transform.origin.z,
    };

    glm::mat4 model = glm::translate (glm::mat4 (1.0f), gl_origin);
    model = glm::rotate (model, transform.angle, glm::vec3 (0.0f, 0.0f, 1.0f));
    model = glm::scale (model, transform.scale);

    const glm::mat4 mvp = getScene ().getCamera ().getProjection () * getScene ().getCamera ().getLookAt () * model;

    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram (m_program);
    glUniformMatrix4fv (m_uMVP, 1, GL_FALSE, glm::value_ptr (mvp));
    glUniform4f (m_uColor, color.r, color.g, color.b, color.a * alpha);

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, m_texture);
    glUniform1i (m_uTexture, 0);

    glBindVertexArray (m_vao);
    glDrawArrays (GL_TRIANGLES, 0, 6);
    glBindVertexArray (0);
#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}
