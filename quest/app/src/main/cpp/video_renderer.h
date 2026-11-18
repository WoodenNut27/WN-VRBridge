#pragma once

#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <wnvr/protocol/messages.h>

#include <cstdint>
#include <vector>

namespace wnvr::quest {

class VideoRenderer {
public:
    bool Initialize(EGLDisplay display);
    void Shutdown();
    bool BindImage(AHardwareBuffer* buffer, uint64_t generation);
    void DrawEye(int eyeIndex);
    void SetFoveation(const protocol::Foveation& foveation) { m_foveation = foveation; }

private:
    struct CachedImage {
        AHardwareBuffer* buffer = nullptr;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
    };

    void ClearCache();
    void DestroyImage(CachedImage& image);

    EGLDisplay m_display = EGL_NO_DISPLAY;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC m_getClientBuffer = nullptr;
    PFNEGLCREATEIMAGEKHRPROC m_createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_targetTexture = nullptr;
    GLuint m_program = 0;
    GLuint m_vertexArray = 0;
    GLuint m_texture = 0;
    GLint m_eyeUniform = -1;
    GLint m_foveationEnabledUniform = -1;
    GLint m_foveationCenterUniform = -1;
    GLint m_foveationRatioUniform = -1;
    protocol::Foveation m_foveation;
    uint64_t m_generation = 0;
    std::vector<CachedImage> m_cache;
};

}
