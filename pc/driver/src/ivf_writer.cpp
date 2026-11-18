#include "ivf_writer.h"

#include "driver_log.h"

#include <array>
#include <limits>

namespace wnvr::driver {
namespace {

void WriteLittleEndian(std::ostream& file, uint64_t value, size_t size) {
    std::array<char, 8> bytes{};
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8)) & 0xff);
    }
    file.write(bytes.data(), static_cast<std::streamsize>(size));
}

}

IvfWriter::~IvfWriter() {
    Close();
}

bool IvfWriter::Open(const std::filesystem::path& path, uint32_t width, uint32_t height,
    uint32_t frameRate, uint32_t maxFrames) {
    Close();
    if (width == 0 || height == 0 || width > 65535 || height > 65535 || frameRate == 0 || maxFrames == 0) {
        Log("IVF の設定が不正です。");
        return false;
    }
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error) {
        Log("IVF 保存先を作成できません: %s", error.message().c_str());
        return false;
    }
    m_path = path;
    m_maxFrames = maxFrames;
    m_frameCount = 0;
    m_file.open(path, std::ios::binary | std::ios::trunc);
    if (!m_file) {
        Log("IVF ファイルを開けません。");
        return false;
    }
    m_file.write("DKIF", 4);
    WriteLittleEndian(m_file, 0, 2);
    WriteLittleEndian(m_file, 32, 2);
    m_file.write("AV01", 4);
    WriteLittleEndian(m_file, width, 2);
    WriteLittleEndian(m_file, height, 2);
    WriteLittleEndian(m_file, frameRate, 4);
    WriteLittleEndian(m_file, 1, 4);
    WriteLittleEndian(m_file, 0, 4);
    WriteLittleEndian(m_file, 0, 4);
    if (!m_file) {
        Log("IVF ヘッダの書き込みに失敗しました。");
        Close();
        return false;
    }
    return true;
}

void IvfWriter::OnEncodedFrame(const EncodedFrame& frame) {
    if (!m_file.is_open()) {
        return;
    }
    if (frame.data.size() > std::numeric_limits<uint32_t>::max()) {
        Log("IVF に格納できないフレームサイズです。");
        Close();
        return;
    }
    WriteLittleEndian(m_file, frame.data.size(), 4);
    WriteLittleEndian(m_file, frame.frameIndex, 8);
    m_file.write(reinterpret_cast<const char*>(frame.data.data()), static_cast<std::streamsize>(frame.data.size()));
    if (!m_file) {
        Log("IVF フレームの書き込みに失敗しました。");
        Close();
        return;
    }
    if (++m_frameCount >= m_maxFrames) {
        Close();
    }
}

void IvfWriter::Close() {
    if (!m_file.is_open()) {
        return;
    }
    const bool writeFailed = !m_file;
    m_file.clear();
    m_file.seekp(24);
    WriteLittleEndian(m_file, m_frameCount, 4);
    m_file.close();
    const auto path = m_path.u8string();
    Log("IVF 保存%s: %s, %u フレーム", writeFailed || !m_file ? "失敗" : "完了",
        reinterpret_cast<const char*>(path.c_str()), m_frameCount);
}

}
