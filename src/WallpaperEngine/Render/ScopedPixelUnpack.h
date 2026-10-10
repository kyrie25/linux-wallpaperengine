#pragma once

#include <GL/glew.h>

namespace WallpaperEngine::Render {
class TightPixelTransfer {
public:
    TightPixelTransfer () {
        glGetIntegerv (GL_TEXTURE_BINDING_2D, &m_texture);
        glGetIntegerv (GL_PIXEL_UNPACK_BUFFER_BINDING, &m_unpackBuffer);
        glGetIntegerv (GL_UNPACK_ALIGNMENT, &m_unpackAlignment);
        glGetIntegerv (GL_UNPACK_ROW_LENGTH, &m_unpackRowLength);
        glGetIntegerv (GL_UNPACK_SKIP_ROWS, &m_unpackSkipRows);
        glGetIntegerv (GL_UNPACK_SKIP_PIXELS, &m_unpackSkipPixels);
        glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
    }

    ~TightPixelTransfer () {
        glPixelStorei (GL_UNPACK_ALIGNMENT, m_unpackAlignment);
        glPixelStorei (GL_UNPACK_ROW_LENGTH, m_unpackRowLength);
        glPixelStorei (GL_UNPACK_SKIP_ROWS, m_unpackSkipRows);
        glPixelStorei (GL_UNPACK_SKIP_PIXELS, m_unpackSkipPixels);
        glBindBuffer (GL_PIXEL_UNPACK_BUFFER, m_unpackBuffer);
        glBindTexture (GL_TEXTURE_2D, m_texture);
    }

private:
    GLint m_texture = 0;
    GLint m_unpackBuffer = 0, m_unpackAlignment = 4, m_unpackRowLength = 0;
    GLint m_unpackSkipRows = 0, m_unpackSkipPixels = 0;
};

}
