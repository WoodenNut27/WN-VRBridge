#pragma once

#include <android/log.h>

#define WNVR_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "WNVRBridge", __VA_ARGS__)
#define WNVR_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "WNVRBridge", __VA_ARGS__)
#define WNVR_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "WNVRBridge", __VA_ARGS__)
