#include "xr_app.h"
#include "log.h"

#include <android_native_app_glue.h>

namespace wnvr::quest {
namespace {

struct AppState {
    bool resumed = false;
};

void HandleAppCommand(android_app* app, int32_t command) {
    auto& state = *static_cast<AppState*>(app->userData);
    switch (command) {
    case APP_CMD_RESUME:
        state.resumed = true;
        WNVR_LOGI("APP_CMD_RESUME");
        break;
    case APP_CMD_PAUSE:
        state.resumed = false;
        WNVR_LOGI("APP_CMD_PAUSE");
        break;
    case APP_CMD_DESTROY:
        WNVR_LOGI("APP_CMD_DESTROY");
        break;
    default:
        break;
    }
}

void ProcessAndroidEvent(android_app* app, int timeout) {
    android_poll_source* source = nullptr;
    const int result = ALooper_pollOnce(timeout, nullptr, nullptr, reinterpret_cast<void**>(&source));
    if (result == ALOOPER_POLL_ERROR) {
        WNVR_LOGE("ALooper_pollOnce failed");
        ANativeActivity_finish(app->activity);
    }
    if (source != nullptr) {
        source->process(app, source);
    }
}

}
}

void android_main(android_app* app) {
    wnvr::quest::AppState state;
    app->userData = &state;
    app->onAppCmd = wnvr::quest::HandleAppCommand;
    wnvr::quest::XrApp xrApp;
    if (!xrApp.Initialize(app)) {
        xrApp.Shutdown();
        ANativeActivity_finish(app->activity);
        while (!app->destroyRequested) {
            wnvr::quest::ProcessAndroidEvent(app, -1);
        }
        app->userData = nullptr;
        app->onAppCmd = nullptr;
        return;
    }

    bool exitRequested = false;
    while (!app->destroyRequested && !exitRequested) {
        const int timeout = xrApp.IsSessionRunning() || state.resumed ? 0 : -1;
        wnvr::quest::ProcessAndroidEvent(app, timeout);
        if (app->destroyRequested) {
            break;
        }
        xrApp.SetResumed(state.resumed);
        xrApp.PollEvents(exitRequested);
        if (!exitRequested && xrApp.IsSessionRunning()) {
            xrApp.RenderFrame();
        }
    }
    if (exitRequested) {
        ANativeActivity_finish(app->activity);
    }
    xrApp.Shutdown();
    app->userData = nullptr;
    app->onAppCmd = nullptr;
}
