#include "egl_context.h"
#include "log.h"

#include <EGL/eglext.h>

namespace wnvr::quest {
namespace {

bool CheckEgl(EGLBoolean result, const char* call) {
    if (result == EGL_TRUE) {
        return true;
    }
    WNVR_LOGE("%s failed: EGL error 0x%x", call, eglGetError());
    return false;
}

}

bool EglContext::Create() {
    Destroy();
    m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_display == EGL_NO_DISPLAY) {
        WNVR_LOGE("eglGetDisplay failed: 0x%x", eglGetError());
        return false;
    }
    if (!CheckEgl(eglInitialize(m_display, nullptr, nullptr), "eglInitialize")
        || !CheckEgl(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI")) {
        return false;
    }
    const EGLint attributes[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE
    };
    EGLint count = 0;
    if (!CheckEgl(eglChooseConfig(m_display, attributes, &m_config, 1, &count), "eglChooseConfig")) {
        return false;
    }
    if (count == 0) {
        WNVR_LOGE("No EGL config supports RGBA8 / OpenGL ES 3 / pbuffer");
        return false;
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttributes);
    if (m_context == EGL_NO_CONTEXT) {
        WNVR_LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    m_surface = eglCreatePbufferSurface(m_display, m_config, surfaceAttributes);
    if (m_surface == EGL_NO_SURFACE) {
        WNVR_LOGE("eglCreatePbufferSurface failed: 0x%x", eglGetError());
        return false;
    }
    return CheckEgl(eglMakeCurrent(m_display, m_surface, m_surface, m_context), "eglMakeCurrent");
}

void EglContext::Destroy() {
    if (m_display != EGL_NO_DISPLAY) {
        CheckEgl(eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "eglMakeCurrent");
        if (m_surface != EGL_NO_SURFACE) {
            CheckEgl(eglDestroySurface(m_display, m_surface), "eglDestroySurface");
        }
        if (m_context != EGL_NO_CONTEXT) {
            CheckEgl(eglDestroyContext(m_display, m_context), "eglDestroyContext");
        }
        CheckEgl(eglTerminate(m_display), "eglTerminate");
    }
    m_display = EGL_NO_DISPLAY;
    m_config = nullptr;
    m_context = EGL_NO_CONTEXT;
    m_surface = EGL_NO_SURFACE;
}

}
