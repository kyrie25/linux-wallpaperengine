#pragma once

#include "TextLayout.h"
#include <string>
#include <vector>

#include <GL/glew.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * Text renderer.
 *
 * Renders shaped static and scripted text from coverage and MSDF glyph atlases.
 */
class CText final : virtual public CObject, public Scripting::ScriptableObject {
public:
    CText (Wallpapers::CScene& scene, const Text& text);
    ~CText () override;

    void setup () override;
    void render () override;
    glm::vec2 getRasterSize () const { return {m_layoutResult.maxX - m_layoutResult.minX, m_layoutResult.top - m_layoutResult.bottom}; }
    glm::vec2 getLayoutOffset () const;

private:
    // Rebuilds the glyph texture (and matching quad VBO) from the given string.
    // Reuses existing GL handles if already allocated, so this is safe to call
    // every time the rendered text changes.
    void rebuildTextureFrom (const std::string& text);
    void buildShader ();
    void uploadQuadVertices ();
    TextLayoutParams layoutParams () const;

    // setup() helpers (kept small to keep the setup flow linear).
    bool loadEmbeddedFont ();
    bool loadSystemFont ();

    TextLayout m_layout;
    TextLayoutResult m_layoutResult;
    TextLayoutParams m_layoutParams;
    GLsizei m_glyphVertices = 0, m_colorVertices = 0;
    GLuint m_colorTexture = 0, m_colorPixelsTexture = 0;
    const Text& m_text;
    std::string m_lastRenderedText;


    GLuint m_texture = 0;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;

    GLint m_uMVP = -1;
    GLint m_uColor = -1;
    GLint m_uTexture = -1;


    bool m_valid = false;
};
} // namespace WallpaperEngine::Render::Objects
