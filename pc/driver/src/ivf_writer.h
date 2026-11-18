#pragma once

#include "video_encoder.h"

#include <filesystem>
#include <fstream>

namespace wnvr::driver {

// Open と Close はエンコーダーの停止中にだけ呼ぶ。
class IvfWriter : public EncodedFrameSink {
public:
    ~IvfWriter();
    bool Open(const std::filesystem::path& path, uint32_t width, uint32_t height, uint32_t frameRate, uint32_t maxFrames);
    void OnEncodedFrame(const EncodedFrame& frame) override;
    void Close();

private:
    std::ofstream m_file;
    std::filesystem::path m_path;
    uint32_t m_maxFrames = 0;
    uint32_t m_frameCount = 0;
};

}
