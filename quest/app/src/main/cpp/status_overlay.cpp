#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "status_overlay.h"

#include <wnvr/version.h>
#include "log.h"
#include <openxr/openxr_platform.h>
#include <android/bitmap.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wnvr::quest {
namespace {

constexpr int32_t kWidth = 1024;
constexpr int32_t kHeight = 512;

template<typename T>
T XrInfo(XrStructureType type) {
    T value{};
    value.type = type;
    return value;
}

bool CheckJni(JNIEnv* env, const char* call) {
    if (!env->ExceptionCheck()) {
        return true;
    }
    env->ExceptionClear();
    WNVR_LOGW("Status overlay JNI exception: %s", call);
    return false;
}

bool CheckXr(XrResult result, const char* call) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    WNVR_LOGW("Status overlay %s failed: %d", call, static_cast<int>(result));
    return false;
}

// 失敗経路でもローカル参照が残らないよう、各呼び出しの寿命に結び付ける。
class LocalFrame {
public:
    explicit LocalFrame(JNIEnv* env) : m_env(env) {
        const auto result = env->PushLocalFrame(32);
        m_valid = CheckJni(env, "PushLocalFrame") && result == JNI_OK;
    }
    ~LocalFrame() {
        if (m_valid) {
            m_env->PopLocalFrame(nullptr);
            CheckJni(m_env, "PopLocalFrame");
        }
    }
    bool Valid() const { return m_valid; }

private:
    JNIEnv* m_env;
    bool m_valid = false;
};

}

struct StatusOverlay::Impl {
    JavaVM* m_vm = nullptr;
    XrSwapchain m_swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> m_images;
    XrCompositionLayerQuad m_layer = XrInfo<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
    std::vector<jobject> m_globals;
    jclass m_bitmapClass = nullptr;
    jclass m_canvasClass = nullptr;
    jclass m_paintClass = nullptr;
    jclass m_stringClass = nullptr;
    jobject m_bitmap = nullptr;
    jobject m_canvas = nullptr;
    jobject m_paint = nullptr;
    jobject m_utf8 = nullptr;
    jmethodID m_eraseColor = nullptr;
    jmethodID m_setColor = nullptr;
    jmethodID m_setTextSize = nullptr;
    jmethodID m_drawRoundRect = nullptr;
    jmethodID m_drawText = nullptr;
    jmethodID m_stringConstructor = nullptr;
    bool m_written = false;
    std::string m_title;
    std::string m_detail;
    std::vector<uint8_t> m_pixels;

    JNIEnv* Environment() const {
        JNIEnv* env = nullptr;
        if (!m_vm || m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            return nullptr;
        }
        return env;
    }

    jobject Global(JNIEnv* env, jobject local) {
        if (!local) {
            return nullptr;
        }
        auto global = env->NewGlobalRef(local);
        if (!CheckJni(env, "NewGlobalRef") || !global) {
            return nullptr;
        }
        m_globals.push_back(global);
        return global;
    }

    jclass Class(JNIEnv* env, const char* name) {
        const auto local = env->FindClass(name);
        if (!CheckJni(env, name) || !local) {
            return nullptr;
        }
        return static_cast<jclass>(Global(env, local));
    }

    jmethodID Method(JNIEnv* env, jclass type, const char* name, const char* signature, bool isStatic = false) {
        const auto method = isStatic ? env->GetStaticMethodID(type, name, signature)
            : env->GetMethodID(type, name, signature);
        return CheckJni(env, name) ? method : nullptr;
    }

    jobject Field(JNIEnv* env, jclass type, const char* name, const char* signature) {
        const auto field = env->GetStaticFieldID(type, name, signature);
        if (!CheckJni(env, name) || !field) {
            return nullptr;
        }
        const auto value = env->GetStaticObjectField(type, field);
        return CheckJni(env, name) ? value : nullptr;
    }

    bool InitializeJni(JNIEnv* env) {
        LocalFrame frame(env);
        if (!frame.Valid()) {
            return false;
        }
        m_bitmapClass = Class(env, "android/graphics/Bitmap");
        m_canvasClass = Class(env, "android/graphics/Canvas");
        m_paintClass = Class(env, "android/graphics/Paint");
        m_stringClass = Class(env, "java/lang/String");
        const auto configClass = Class(env, "android/graphics/Bitmap$Config");
        const auto alignClass = Class(env, "android/graphics/Paint$Align");
        if (!m_bitmapClass || !m_canvasClass || !m_paintClass || !m_stringClass || !configClass || !alignClass) {
            return false;
        }
        const auto createBitmap = Method(env, m_bitmapClass, "createBitmap",
            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;", true);
        const auto canvasConstructor = Method(env, m_canvasClass, "<init>", "(Landroid/graphics/Bitmap;)V");
        const auto paintConstructor = Method(env, m_paintClass, "<init>", "(I)V");
        const auto setAlign = Method(env, m_paintClass, "setTextAlign", "(Landroid/graphics/Paint$Align;)V");
        m_eraseColor = Method(env, m_bitmapClass, "eraseColor", "(I)V");
        m_setColor = Method(env, m_paintClass, "setColor", "(I)V");
        m_setTextSize = Method(env, m_paintClass, "setTextSize", "(F)V");
        m_drawRoundRect = Method(env, m_canvasClass, "drawRoundRect", "(FFFFFFLandroid/graphics/Paint;)V");
        m_drawText = Method(env, m_canvasClass, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
        m_stringConstructor = Method(env, m_stringClass, "<init>", "([BLjava/lang/String;)V");
        if (!createBitmap || !canvasConstructor || !paintConstructor || !setAlign || !m_eraseColor
            || !m_setColor || !m_setTextSize || !m_drawRoundRect || !m_drawText || !m_stringConstructor) {
            return false;
        }
        const auto config = Field(env, configClass, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
        const auto align = Field(env, alignClass, "CENTER", "Landroid/graphics/Paint$Align;");
        if (!config || !align) {
            return false;
        }
        auto object = env->CallStaticObjectMethod(m_bitmapClass, createBitmap, kWidth, kHeight, config);
        if (!CheckJni(env, "Bitmap.createBitmap") || !(m_bitmap = Global(env, object))) {
            return false;
        }
        object = env->NewObject(m_canvasClass, canvasConstructor, m_bitmap);
        if (!CheckJni(env, "Canvas constructor") || !(m_canvas = Global(env, object))) {
            return false;
        }
        object = env->NewObject(m_paintClass, paintConstructor, jint{1});
        if (!CheckJni(env, "Paint constructor") || !(m_paint = Global(env, object))) {
            return false;
        }
        env->CallVoidMethod(m_paint, setAlign, align);
        if (!CheckJni(env, "Paint.setTextAlign")) {
            return false;
        }
        object = env->NewStringUTF("UTF-8");
        if (!CheckJni(env, "UTF-8 charset") || !(m_utf8 = Global(env, object))) {
            return false;
        }
        return true;
    }

    bool Color(JNIEnv* env, uint32_t color) {
        env->CallVoidMethod(m_paint, m_setColor, std::bit_cast<jint>(color));
        return CheckJni(env, "Paint.setColor");
    }

    bool Text(JNIEnv* env, const std::string& text, float size, float y) {
        // PC 名に補助文字が含まれても壊さないよう、Modified UTF-8 ではなく通常の UTF-8 を渡す。
        const auto bytes = env->NewByteArray(static_cast<jsize>(text.size()));
        if (!CheckJni(env, "NewByteArray") || !bytes) {
            return false;
        }
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jbyte*>(text.data()));
        if (!CheckJni(env, "SetByteArrayRegion")) {
            return false;
        }
        const auto string = env->NewObject(m_stringClass, m_stringConstructor, bytes, m_utf8);
        if (!CheckJni(env, "String constructor") || !string) {
            return false;
        }
        env->CallVoidMethod(m_paint, m_setTextSize, size);
        if (!CheckJni(env, "Paint.setTextSize")) {
            return false;
        }
        env->CallVoidMethod(m_canvas, m_drawText, string, 512.0f, y, m_paint);
        const bool ok = CheckJni(env, "Canvas.drawText");
        env->DeleteLocalRef(string);
        env->DeleteLocalRef(bytes);
        return ok;
    }

    bool Draw(JNIEnv* env, const std::string& title, const std::string& detail) {
        env->CallVoidMethod(m_bitmap, m_eraseColor, jint{0});
        if (!CheckJni(env, "Bitmap.eraseColor") || !Color(env, 0xCC000000)) {
            return false;
        }
        env->CallVoidMethod(m_canvas, m_drawRoundRect, 16.0f, 16.0f, 1008.0f, 496.0f, 32.0f, 32.0f, m_paint);
        if (!CheckJni(env, "Canvas.drawRoundRect") || !Color(env, 0xFFFFFFFF)
            || !Text(env, title, 56.0f, 200.0f) || !Color(env, 0xFFCCCCCC)) {
            return false;
        }
        size_t start = 0;
        for (int line = 0; line < 3 && start < detail.size(); ++line) {
            const auto end = detail.find('\n', start);
            if (!Text(env, detail.substr(start, end == std::string::npos ? end : end - start),
                    40.0f, 260.0f + static_cast<float>(line) * 60.0f)) {
                return false;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return Color(env, 0xFF888888) && Text(env, std::string("WN-VRBridge ") + kVersion, 28.0f, 470.0f);
    }
};

StatusOverlay::StatusOverlay() = default;

StatusOverlay::~StatusOverlay() {
    Shutdown();
}

bool StatusOverlay::Initialize(JavaVM* vm, XrSession session) {
    Shutdown();
    m_impl = std::make_unique<Impl>();
    auto& impl = *m_impl;
    impl.m_vm = vm;
    auto* env = impl.Environment();
    if (!env || !impl.InitializeJni(env)) {
        Shutdown();
        return false;
    }
    uint32_t count = 0;
    if (!CheckXr(xrEnumerateSwapchainFormats(session, 0, &count, nullptr), "xrEnumerateSwapchainFormats")) {
        Shutdown();
        return false;
    }
    std::vector<int64_t> formats(count);
    if (!CheckXr(xrEnumerateSwapchainFormats(session, count, &count, formats.data()), "xrEnumerateSwapchainFormats")) {
        Shutdown();
        return false;
    }
    const auto hasFormat = [&formats](int64_t format) {
        return std::find(formats.begin(), formats.end(), format) != formats.end();
    };
    if (!hasFormat(GL_SRGB8_ALPHA8) && !hasFormat(GL_RGBA8)) {
        WNVR_LOGW("Status overlay RGBA swapchain format unavailable");
        Shutdown();
        return false;
    }
    auto info = XrInfo<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = hasFormat(GL_SRGB8_ALPHA8) ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    info.sampleCount = 1;
    info.width = kWidth;
    info.height = kHeight;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!CheckXr(xrCreateSwapchain(session, &info, &impl.m_swapchain), "xrCreateSwapchain")
        || !CheckXr(xrEnumerateSwapchainImages(impl.m_swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages")
        || count == 0) {
        Shutdown();
        return false;
    }
    impl.m_images.assign(count, XrInfo<XrSwapchainImageOpenGLESKHR>(XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR));
    if (!CheckXr(xrEnumerateSwapchainImages(impl.m_swapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(impl.m_images.data())), "xrEnumerateSwapchainImages")) {
        Shutdown();
        return false;
    }
    impl.m_pixels.resize(static_cast<size_t>(kWidth) * kHeight * 4);
    impl.m_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    impl.m_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    impl.m_layer.subImage.swapchain = impl.m_swapchain;
    impl.m_layer.subImage.imageRect.extent = {kWidth, kHeight};
    impl.m_layer.pose = {{0, 0, 0, 1}, {0, 0, -1.5f}};
    impl.m_layer.size = {1.0f, 0.5f};
    return true;
}

void StatusOverlay::Shutdown() {
    if (!m_impl) {
        return;
    }
    if (m_impl->m_swapchain != XR_NULL_HANDLE) {
        CheckXr(xrDestroySwapchain(m_impl->m_swapchain), "xrDestroySwapchain");
    }
    if (auto* env = m_impl->Environment()) {
        for (auto global : m_impl->m_globals) {
            env->DeleteGlobalRef(global);
        }
    }
    m_impl.reset();
}

void StatusOverlay::SetText(const std::string& title, const std::string& detail) {
    if (!m_impl || m_impl->m_swapchain == XR_NULL_HANDLE) {
        return;
    }
    auto& impl = *m_impl;
    if (impl.m_written && title == impl.m_title && detail == impl.m_detail) {
        return;
    }
    auto* env = impl.Environment();
    if (!env) {
        return;
    }
    LocalFrame frame(env);
    if (!frame.Valid() || !impl.Draw(env, title, detail)) {
        return;
    }
    AndroidBitmapInfo bitmapInfo{};
    const auto infoResult = AndroidBitmap_getInfo(env, impl.m_bitmap, &bitmapInfo);
    if (!CheckJni(env, "AndroidBitmap_getInfo") || infoResult != ANDROID_BITMAP_RESULT_SUCCESS
        || bitmapInfo.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || bitmapInfo.width != kWidth
        || bitmapInfo.height != kHeight || bitmapInfo.stride < static_cast<uint32_t>(kWidth * 4)) {
        WNVR_LOGW("Status overlay bitmap format invalid");
        return;
    }
    void* pixels = nullptr;
    const auto lockResult = AndroidBitmap_lockPixels(env, impl.m_bitmap, &pixels);
    const bool lockOk = CheckJni(env, "AndroidBitmap_lockPixels");
    if (lockResult != ANDROID_BITMAP_RESULT_SUCCESS) {
        WNVR_LOGW("Status overlay bitmap lock failed: %d", lockResult);
        return;
    }
    if (lockOk && pixels) {
        // Canvas の先頭行は上、GLES の先頭行は下なので、行だけを逆転し色と乗算済みアルファは保つ。
        for (int32_t y = 0; y < kHeight; ++y) {
            std::memcpy(impl.m_pixels.data() + static_cast<size_t>(kHeight - 1 - y) * kWidth * 4,
                static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * bitmapInfo.stride, kWidth * 4);
        }
    }
    const auto unlockResult = AndroidBitmap_unlockPixels(env, impl.m_bitmap);
    if (!CheckJni(env, "AndroidBitmap_unlockPixels") || unlockResult != ANDROID_BITMAP_RESULT_SUCCESS || !lockOk || !pixels) {
        return;
    }
    uint32_t index = 0;
    const auto acquireInfo = XrInfo<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    if (!CheckXr(xrAcquireSwapchainImage(impl.m_swapchain, &acquireInfo, &index), "xrAcquireSwapchainImage")) {
        return;
    }
    auto waitInfo = XrInfo<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    waitInfo.timeout = XR_INFINITE_DURATION;
    const auto waitResult = xrWaitSwapchainImage(impl.m_swapchain, &waitInfo);
    if (!CheckXr(waitResult, "xrWaitSwapchainImage") || waitResult == XR_TIMEOUT_EXPIRED) {
        // wait 失敗後の画像は release できないため、この板を破棄して本体の描画を続ける。
        Shutdown();
        return;
    }
    GLint texture = 0;
    GLint rowLength = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    glBindTexture(GL_TEXTURE_2D, impl.m_images[index].image);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, impl.m_pixels.data());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
    glFlush();
    const auto error = glGetError();
    const auto releaseInfo = XrInfo<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    const bool released = CheckXr(xrReleaseSwapchainImage(impl.m_swapchain, &releaseInfo), "xrReleaseSwapchainImage");
    if (error != GL_NO_ERROR || !released) {
        WNVR_LOGW("Status overlay upload failed: GL error 0x%x", error);
        impl.m_written = false;
        return;
    }
    impl.m_title = title;
    impl.m_detail = detail;
    impl.m_written = true;
}

const XrCompositionLayerBaseHeader* StatusOverlay::Layer(XrSpace viewSpace) {
    if (!m_impl || !m_impl->m_written) {
        return nullptr;
    }
    m_impl->m_layer.space = viewSpace;
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_impl->m_layer);
}

}
