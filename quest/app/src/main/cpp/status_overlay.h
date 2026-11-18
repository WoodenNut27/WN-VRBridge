#pragma once

#include <jni.h>
#include <openxr/openxr.h>

#include <memory>
#include <string>

namespace wnvr::quest {

class StatusOverlay {
public:
    StatusOverlay();
    ~StatusOverlay();
    StatusOverlay(const StatusOverlay&) = delete;
    StatusOverlay& operator=(const StatusOverlay&) = delete;

    bool Initialize(JavaVM* vm, XrSession session);
    void Shutdown();
    void SetText(const std::string& title, const std::string& detail);
    const XrCompositionLayerBaseHeader* Layer(XrSpace viewSpace);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
