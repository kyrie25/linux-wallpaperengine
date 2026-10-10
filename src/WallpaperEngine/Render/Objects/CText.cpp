#include "CText.h"
#include "WallpaperEngine/Render/ScopedPixelUnpack.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include <iterator>

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
uniform sampler2D uTexture, uColorTexture;
uniform vec4 uColor;
uniform bool uColorGlyph, uMsdf;
uniform float uOutline, uBlur, uShadowSize, uShadowOpacity;
uniform vec3 uOutlineColor, uShadowColor;
uniform vec2 uShadowOffset;
out vec4 FragColor;
float distanceAt(vec2 uv) {
    vec3 sampleValue = texture(uTexture, uv).rgb;
    return max(min(sampleValue.r, sampleValue.g), min(max(sampleValue.r, sampleValue.g), sampleValue.b)) - 0.5;
}
void main() {
    if (uColorGlyph) {
        vec4 value = texture(uColorTexture, vUV);
        if (uMsdf) {
            float range = max(0.5 * dot(vec2(24.0) / vec2(textureSize(uTexture, 0)), 1.0 / fwidth(vUV)), 1.0);
            value.a *= clamp(range * (texture(uTexture, vUV).a - 0.5) + 0.5, 0.0, 1.0);
        }
        FragColor = vec4(value.rgb, value.a * uColor.a);
        return;
    }
    if (!uMsdf) {
        FragColor = vec4(uColor.rgb, uColor.a * texture(uTexture, vUV).r);
        return;
    }
    float range = max(0.5 * dot(vec2(24.0) / vec2(textureSize(uTexture, 0)), 1.0 / fwidth(vUV)), 1.0);
    float distance = range * distanceAt(vUV);
    float blur = max(1.0, uBlur);
    float coverage = clamp(distance / blur + 0.5, 0.0, 1.0);
    float outline = clamp((distance + max(uOutline, 0.0)) / blur + 0.5, 0.0, 1.0);
    float shadow = clamp(range * distanceAt(vUV - uShadowOffset) / max(1.0, uShadowSize) + 0.5, 0.0, 1.0) * uShadowOpacity;
    vec3 premultiplied = mix(uOutlineColor * outline, uColor.rgb, coverage);
    float alpha = max(coverage, outline);
    premultiplied += uShadowColor * shadow * (1.0 - alpha);
    alpha += shadow * (1.0 - alpha);
    FragColor = vec4(alpha > 0.0 ? premultiplied / alpha : vec3(0.0), alpha * uColor.a);
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
    this->registerProperty ("alpha", *text.alpha->value, DynamicValue::Float);
    this->registerProperty ("origin", *text.origin->value);
    this->registerProperty ("scale", *text.scale->value);
    this->registerProperty ("visible", *text.visible->value);
    this->registerProperty ("pointSize", *text.pointSize->value, DynamicValue::Float);
    this->registerProperty ("text", *text.text->value);
    this->registerProperty ("spacing", *text.spacing->value, DynamicValue::Float);
    this->registerProperty ("limitWidth", *text.limitWidth->value);
    this->registerProperty ("maxWidth", *text.maxWidth->value, DynamicValue::Float);
    this->registerProperty ("limitRows", *text.limitRows->value);
    this->registerProperty ("maxRows", *text.maxRows->value);
    this->registerProperty ("limitUseEllipsis", *text.limitUseEllipsis->value);
    this->registerProperty ("blockAlign", *text.blockAlign->value);
    this->registerProperty ("msdf", *text.msdf->value);
    this->registerProperty ("outline", *text.outline->value);
    this->registerProperty ("outlineThickness", *text.outlineThickness->value, DynamicValue::Float);
    this->registerProperty ("outlineColor", *text.outlineColor->value);
    this->registerProperty ("blur", *text.blur->value);
    this->registerProperty ("blurSize", *text.blurSize->value, DynamicValue::Float);
    this->registerProperty ("dropShadow", *text.dropShadow->value);
    this->registerProperty ("dropShadowSize", *text.dropShadowSize->value, DynamicValue::Float);
    this->registerProperty ("dropShadowOpacity", *text.dropShadowOpacity->value, DynamicValue::Float);
    this->registerProperty ("dropShadowOffset", *text.dropShadowOffset->value);
    this->registerProperty ("dropShadowColor", *text.dropShadowColor->value);
}

CText::~CText () {
    if (m_vbo != 0) {
	glDeleteBuffers (1, &m_vbo);
    }
    if (m_vao != 0) {
	glDeleteVertexArrays (1, &m_vao);
    }
    if (m_program != 0) {
	glDeleteProgram (m_program);
    }
    if (m_colorPixelsTexture) glDeleteTextures (1, &m_colorPixelsTexture);
    if (m_colorTexture) glDeleteTextures (1, &m_colorTexture);
    if (m_texture != 0) {
	glDeleteTextures (1, &m_texture);
    }

}

void CText::setup () {
    const bool scripted = m_text.text->value->getScriptSource ().has_value ();
    const auto& text = m_text.text->value->getString ();

    // Nothing to render and no script to produce text later → bail.
    if (text.empty () && !scripted) {
	return;
    }

    if (!loadEmbeddedFont () && !loadSystemFont ()) {
	return;
    }
    std::vector<TextFontSource> fallbacks;
    for (const auto& candidate : kFallbackFontCandidates) {
        if (std::filesystem::exists (candidate)) fallbacks.push_back ({nullptr, candidate});
    }
    m_layout.setFallbackFonts (std::move (fallbacks));

    buildShader ();
    // Scripted text may have an empty placeholder; use a single space so the
    // glyph texture has non-zero dimensions until the script produces a value.
    rebuildTextureFrom (text.empty () ? std::string (" ") : text);

    m_valid = m_texture != 0 && m_program != 0 && m_vao != 0;
}

bool CText::loadEmbeddedFont () {
    if (m_text.font.empty () || m_text.font.starts_with ("systemfont_")) return false;
    try {
        auto stream = getAssetLocator ().read (m_text.font);
        std::vector<uint8_t> bytes {std::istreambuf_iterator<char> (*stream), {}};
        if (m_layout.setPrimaryFont (std::move (bytes), m_text.font)) return true;
    } catch (const std::exception& error) {
        sLog.error ("CText: cannot read font '", m_text.font, "': ", error.what ());
    }
    return false;
}

bool CText::loadSystemFont () {
    for (const auto& candidate : kFontCandidates) {
        if (std::filesystem::exists (candidate) && m_layout.setPrimaryFont ({}, candidate)) return true;
    }
    sLog.error ("CText: no usable system font found");
    return false;
}

TextLayoutParams CText::layoutParams () const {
    return {
        .size = m_text.pointSize->value->getFloat (),
        .spacing = m_text.spacing->value->getVec2 (),
        .msdf = m_text.msdf->value->getBool () || m_text.outline->value->getBool ()
            || m_text.blur->value->getBool () || m_text.dropShadow->value->getBool (),
        .align = m_text.alignment == "left" ? TextAlign::Left
            : m_text.alignment == "right" ? TextAlign::Right : TextAlign::Center,
        .maxWidth = m_text.limitWidth->value->getBool () ? m_text.maxWidth->value->getFloat () : 0.0f,
        .maxRows = m_text.limitRows->value->getBool () ? m_text.maxRows->value->getInt () : 0,
        .ellipsis = m_text.limitUseEllipsis->value->getBool (),
        .blockAlign = m_text.blockAlign->value->getBool (),
    };
}

void CText::rebuildTextureFrom (const std::string& text) {
    m_layoutParams = layoutParams ();
    m_layoutResult = m_layout.layout (text, m_layoutParams);
    TightPixelTransfer transfer;
    if (!m_texture) glGenTextures (1, &m_texture);
    glBindTexture (GL_TEXTURE_2D, m_texture);
    const int channels = m_layout.getAtlasChannels ();
    glTexImage2D (GL_TEXTURE_2D, 0, channels == 4 ? GL_RGBA8 : GL_R8, m_layout.getAtlasSize (),
                 m_layout.getAtlasSize (), 0, channels == 4 ? GL_RGBA : GL_RED, GL_UNSIGNED_BYTE,
                 m_layout.getAtlasPixels ().data ());
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!m_layoutResult.colorQuads.empty ()) {
        if (!m_colorTexture) glGenTextures (1, &m_colorTexture);
        glBindTexture (GL_TEXTURE_2D, m_colorTexture);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, m_layout.getColorAtlasSize (), m_layout.getColorAtlasSize (),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, m_layout.getColorAtlasPixels ().data ());
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (m_layout.getColorScale () > 0) {
            if (!m_colorPixelsTexture) glGenTextures (1, &m_colorPixelsTexture);
            glBindTexture (GL_TEXTURE_2D, m_colorPixelsTexture);
            const int size = m_layout.getColorAtlasSize () * m_layout.getColorScale ();
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                          m_layout.getColorTexturePixels ().data ());
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
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
    const auto& result = m_layoutResult;
    const float offsetX = m_text.alignment == "left" ? -result.minX
        : m_text.alignment == "right" ? -result.maxX : -(result.minX + result.maxX) * 0.5f;
    const float offsetY = result.verticalAnchor (m_text.verticalalign);
    std::vector<float> vertices;
    const auto append = [&] (const std::vector<TextGlyphQuad>& quads) {
        for (const auto& q : quads) {
            const float left = q.rect.x + offsetX, right = q.rect.z + offsetX;
            const float top = -q.rect.w + offsetY, bottom = -q.rect.y + offsetY;
            const float quad[] = {
                left, top, q.uv.x, q.uv.y, right, top, q.uv.z, q.uv.y, right, bottom, q.uv.z, q.uv.w,
                left, top, q.uv.x, q.uv.y, right, bottom, q.uv.z, q.uv.w, left, bottom, q.uv.x, q.uv.w};
            vertices.insert (vertices.end (), std::begin (quad), std::end (quad));
        }
    };
    append (result.quads);
    m_glyphVertices = static_cast<GLsizei> (vertices.size () / 4);
    append (result.colorQuads);
    m_colorVertices = static_cast<GLsizei> (vertices.size () / 4) - m_glyphVertices;
    const bool firstUpload = m_vao == 0;
    if (firstUpload) { glGenVertexArrays (1, &m_vao); glGenBuffers (1, &m_vbo); }
    glBindVertexArray (m_vao);
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (GL_ARRAY_BUFFER, vertices.size () * sizeof (float), vertices.data (), GL_DYNAMIC_DRAW);
    if (firstUpload) {
        glEnableVertexAttribArray (0);
        glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), nullptr);
        glEnableVertexAttribArray (1);
        glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), reinterpret_cast<void*> (2 * sizeof (float)));
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
    std::string renderedText = m_text.text->value->getString ();
    if (renderedText.empty ()) renderedText = " ";

    if (renderedText != m_lastRenderedText || layoutParams () != m_layoutParams)
        rebuildTextureFrom (renderedText);

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
    glm::vec3 gl_origin = {
	transform.origin.x - scene_w * 0.5f,
	scene_h * 0.5f - transform.origin.y,
	transform.origin.z,
    };

    if (this->getScene ().getScene ().camera.parallax.enabled->value->getBool ()
	&& !this->getScene ().getContext ().getApp ().getContext ().settings.mouse.disableparallax
	&& m_text.parent.has_value ()) {
	const auto* parent = dynamic_cast<const CImage*> (this->getScene ().getObject (*m_text.parent));
	if (parent != nullptr) {
	    const glm::vec2 depth = parent->getImage ().parallaxDepth->value->getVec2 ();
	    const glm::vec2* displacement = this->getScene ().getParallaxDisplacement ();
	    const float referenceSize = static_cast<float> (this->getScene ().getWidth ());
	    gl_origin.x += depth.x * displacement->x * referenceSize;
	    gl_origin.y += depth.y * displacement->y * referenceSize;
	}
    }

    glm::mat4 model = glm::translate (glm::mat4 (1.0f), gl_origin);
    model = glm::rotate (model, transform.angle, glm::vec3 (0.0f, 0.0f, 1.0f));
    model = glm::scale (model, transform.scale);

    const glm::mat4 mvp = getScene ().getCamera ().getProjection () * getScene ().getCamera ().getLookAt () * model;

    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Text draws directly into the scene, whose alpha must remain opaque.
    GLboolean previousMask[4];
    glGetBooleanv (GL_COLOR_WRITEMASK, previousMask);
    glColorMask (previousMask[0], previousMask[1], previousMask[2], GL_FALSE);

    glUseProgram (m_program);
    glUniformMatrix4fv (m_uMVP, 1, GL_FALSE, glm::value_ptr (mvp));
    glUniform4f (m_uColor, color.r, color.g, color.b, color.a * alpha);

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, m_texture);
    glUniform1i (m_uTexture, 0);

    glBindVertexArray (m_vao);
    glUniform1i (glGetUniformLocation (m_program, "uColorGlyph"), 0);
    glUniform1i (glGetUniformLocation (m_program, "uMsdf"), m_layoutParams.msdf);
    glUniform1f (glGetUniformLocation (m_program, "uOutline"), m_text.outline->value->getBool ()
        ? m_text.outlineThickness->value->getFloat () : 0.0f);
    const auto outline = m_text.outlineColor->value->getVec3 ();
    glUniform3fv (glGetUniformLocation (m_program, "uOutlineColor"), 1, glm::value_ptr (outline));
    glUniform1f (glGetUniformLocation (m_program, "uBlur"), m_text.blur->value->getBool ()
        ? m_text.blurSize->value->getFloat () : 0.0f);
    glUniform1f (glGetUniformLocation (m_program, "uShadowOpacity"), m_text.dropShadow->value->getBool ()
        ? m_text.dropShadowOpacity->value->getFloat () : 0.0f);
    glUniform1f (glGetUniformLocation (m_program, "uShadowSize"), m_text.dropShadowSize->value->getFloat ());
    const auto shadow = m_text.dropShadowColor->value->getVec3 ();
    glUniform3fv (glGetUniformLocation (m_program, "uShadowColor"), 1, glm::value_ptr (shadow));
    const auto shadowOffset = m_text.dropShadowOffset->value->getVec2 () / static_cast<float> (m_layout.getAtlasSize ());
    glUniform2fv (glGetUniformLocation (m_program, "uShadowOffset"), 1, glm::value_ptr (shadowOffset));
    glDrawArrays (GL_TRIANGLES, 0, m_glyphVertices);
    if (m_colorVertices) {
        glBindTexture (GL_TEXTURE_2D, m_colorTexture);
        glActiveTexture (GL_TEXTURE1);
        glBindTexture (GL_TEXTURE_2D, m_layoutParams.msdf ? m_colorPixelsTexture : m_colorTexture);
        glUniform1i (glGetUniformLocation (m_program, "uColorTexture"), 1);
        glActiveTexture (GL_TEXTURE0);
        glUniform1i (glGetUniformLocation (m_program, "uColorGlyph"), 1);
        glDrawArrays (GL_TRIANGLES, m_glyphVertices, m_colorVertices);
    }
    glBindVertexArray (0);
    glColorMask (previousMask[0], previousMask[1], previousMask[2], previousMask[3]);
#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}


glm::vec2 CText::getLayoutOffset () const {
    const auto size = getRasterSize ();
    return {m_text.alignment == "left" ? size.x * .5f : m_text.alignment == "right" ? -size.x * .5f : 0,
            (m_layoutResult.top + m_layoutResult.bottom) * .5f - m_layoutResult.verticalAnchor (m_text.verticalalign)};
}
