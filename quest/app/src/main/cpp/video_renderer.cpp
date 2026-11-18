#include "video_renderer.h"
#include "log.h"

#include <algorithm>

namespace wnvr::quest {
namespace {

constexpr char kVertexShader[] = R"(#version 300 es
precision highp float;
uniform highp int eyeIndex;
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    uv = vec2((p.x + float(eyeIndex)) * 0.5, 1.0 - p.y);
}
)";

constexpr char kFragmentShader[] = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES video;
uniform highp int eyeIndex;
uniform int foveationEnabled;
uniform vec4 foveationCenter;
uniform vec2 foveationRatio;
float CompressAxis(float u, float c0, float c1, float r) {
    float wn = c0 / r + (c1 - c0) + (1.0 - c1) / r;
    float s = u < c0 ? u / r
        : (u < c1 ? c0 / r + (u - c0) : c0 / r + (c1 - c0) + (u - c1) / r);
    return s / wn;
}
in vec2 uv;
out vec4 color;
void main() {
    vec2 sampleUv = uv;
    if (foveationEnabled != 0) {
        vec2 u = vec2(uv.x * 2.0 - float(eyeIndex), uv.y);
        vec2 compressed = vec2(
            CompressAxis(u.x, foveationCenter.x, foveationCenter.y, foveationRatio.x),
            CompressAxis(u.y, foveationCenter.z, foveationCenter.w, foveationRatio.y));
        sampleUv = vec2((float(eyeIndex) + compressed.x) * 0.5, compressed.y);
    }
    vec3 srgb = texture(video, sampleUv).rgb;
    vec3 linear = mix(pow((srgb + 0.055) / 1.055, vec3(2.4)),
        srgb / 12.92, lessThanEqual(srgb, vec3(0.04045)));
    color = vec4(linear, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char log[1024]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        WNVR_LOGE("Video shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}

bool VideoRenderer::Initialize(EGLDisplay display) {
    Shutdown();
    m_display = display;
    m_getClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    m_createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    m_destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    m_targetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (display == EGL_NO_DISPLAY || !m_getClientBuffer || !m_createImage || !m_destroyImage || !m_targetTexture) {
        WNVR_LOGE("Video renderer EGL extensions unavailable");
        Shutdown();
        return false;
    }
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vertex && fragment) {
        m_program = glCreateProgram();
        if (m_program) {
            glAttachShader(m_program, vertex);
            glAttachShader(m_program, fragment);
            glLinkProgram(m_program);
        }
    }
    if (vertex) {
        glDeleteShader(vertex);
    }
    if (fragment) {
        glDeleteShader(fragment);
    }
    GLint linked = GL_FALSE;
    if (m_program) {
        glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    }
    if (linked != GL_TRUE) {
        char log[1024]{};
        if (m_program) {
            glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        }
        WNVR_LOGE("Video program link failed: %s", log);
        Shutdown();
        return false;
    }
    m_eyeUniform = glGetUniformLocation(m_program, "eyeIndex");
    m_foveationEnabledUniform = glGetUniformLocation(m_program, "foveationEnabled");
    m_foveationCenterUniform = glGetUniformLocation(m_program, "foveationCenter");
    m_foveationRatioUniform = glGetUniformLocation(m_program, "foveationRatio");
    glUseProgram(m_program);
    glUniform1i(glGetUniformLocation(m_program, "video"), 0);
    glUseProgram(0);
    glGenVertexArrays(1, &m_vertexArray);
    if (glGetError() != GL_NO_ERROR || !m_vertexArray || m_eyeUniform < 0
        || m_foveationEnabledUniform < 0 || m_foveationCenterUniform < 0 || m_foveationRatioUniform < 0) {
        Shutdown();
        return false;
    }
    return true;
}

void VideoRenderer::DestroyImage(CachedImage& image) {
    if (image.texture) {
        glDeleteTextures(1, &image.texture);
    }
    if (image.image != EGL_NO_IMAGE_KHR) {
        m_destroyImage(m_display, image.image);
    }
    if (image.buffer) {
        AHardwareBuffer_release(image.buffer);
    }
    image = {};
}

void VideoRenderer::ClearCache() {
    for (auto& image : m_cache) {
        DestroyImage(image);
    }
    m_cache.clear();
    m_texture = 0;
}

void VideoRenderer::Shutdown() {
    ClearCache();
    if (m_program) {
        glDeleteProgram(m_program);
        m_program = 0;
    }
    if (m_vertexArray) {
        glDeleteVertexArrays(1, &m_vertexArray);
        m_vertexArray = 0;
    }
    m_display = EGL_NO_DISPLAY;
    m_eyeUniform = -1;
    m_foveationEnabledUniform = -1;
    m_foveationCenterUniform = -1;
    m_foveationRatioUniform = -1;
    m_foveation = {};
    m_generation = 0;
}

bool VideoRenderer::BindImage(AHardwareBuffer* buffer, uint64_t generation) {
    m_texture = 0;
    if (!m_program || !buffer) {
        return false;
    }
    if (m_generation != generation) {
        ClearCache();
        m_generation = generation;
    }
    const auto found = std::find_if(m_cache.begin(), m_cache.end(), [buffer](const auto& image) {
        return image.buffer == buffer;
    });
    glActiveTexture(GL_TEXTURE0);
    if (found != m_cache.end()) {
        m_texture = found->texture;
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
        return true;
    }
    CachedImage image;
    const EGLClientBuffer clientBuffer = m_getClientBuffer(buffer);
    if (!clientBuffer) {
        WNVR_LOGW("eglGetNativeClientBufferANDROID failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    image.image = m_createImage(m_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, attributes);
    if (image.image == EGL_NO_IMAGE_KHR) {
        WNVR_LOGW("Video EGLImage creation failed: 0x%x", eglGetError());
        return false;
    }
    // reader 再生成後もキャッシュのキーのアドレスが別のバッファに再利用されないよう保持する。
    AHardwareBuffer_acquire(buffer);
    image.buffer = buffer;
    glGenTextures(1, &image.texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, image.texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_targetTexture(GL_TEXTURE_EXTERNAL_OES, image.image);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        WNVR_LOGW("Video texture import failed: 0x%x", error);
        DestroyImage(image);
        return false;
    }
    if (m_cache.size() == 8) {
        DestroyImage(m_cache.front());
        m_cache.erase(m_cache.begin());
    }
    m_texture = image.texture;
    m_cache.push_back(image);
    return true;
}

void VideoRenderer::DrawEye(int eyeIndex) {
    if (!m_program || !m_texture) {
        return;
    }
    // AImage は上の行が先頭、GL の FBO は下の行が先頭なので、頂点シェーダーで UV の Y を反転している
    // （反転しないと上下が逆さまに表示されることを実機で確認した）。
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glUseProgram(m_program);
    glUniform1i(m_eyeUniform, eyeIndex);
    glUniform1i(m_foveationEnabledUniform, m_foveation.enabled);
    const auto& center = eyeIndex == 0 ? m_foveation.left : m_foveation.right;
    glUniform4f(m_foveationCenterUniform, center.centerX0, center.centerX1, center.centerY0, center.centerY1);
    glUniform2f(m_foveationRatioUniform, m_foveation.edgeRatioX, m_foveation.edgeRatioY);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
    glBindVertexArray(m_vertexArray);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
}

}
