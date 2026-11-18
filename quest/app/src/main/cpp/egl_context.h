#pragma once

#include <EGL/egl.h>

namespace wnvr::quest {

class EglContext {
public:
    EglContext() = default;
    ~EglContext() { Destroy(); }
    EglContext(const EglContext&) = delete;
    EglContext& operator=(const EglContext&) = delete;

    bool Create();
    void Destroy();
    EGLDisplay Display() const { return m_display; }
    EGLConfig Config() const { return m_config; }
    EGLContext Context() const { return m_context; }

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLConfig m_config = nullptr;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
};

}
