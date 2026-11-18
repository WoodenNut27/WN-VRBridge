#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace wnvr::protocol {

// どちらも失敗を戻り値で返さず Ok() にためる。呼び出し側は最後に一度だけ確かめればよい。
class ByteWriter {
public:
    explicit ByteWriter(std::span<std::byte> buffer) : m_buffer(buffer) {}

    void WriteU8(uint8_t value) { WriteUnsigned(value, 1); }
    void WriteU16(uint16_t value) { WriteUnsigned(value, 2); }
    void WriteU32(uint32_t value) { WriteUnsigned(value, 4); }
    void WriteU64(uint64_t value) { WriteUnsigned(value, 8); }
    void WriteF32(float value) { WriteU32(std::bit_cast<uint32_t>(value)); }

    // 映像の断片は 1 秒に 1 万個を超えるので、1 バイトずつではなくまとめて写す。
    void WriteBytes(std::span<const uint8_t> bytes) {
        if (!Reserve(bytes.size()) || bytes.empty()) {
            return;
        }
        std::memcpy(m_buffer.data() + m_offset, bytes.data(), bytes.size());
        m_offset += bytes.size();
    }

    void WriteString(std::string_view value, size_t maxLength) {
        if (maxLength > 255 || value.size() > maxLength) {
            m_ok = false;
            return;
        }
        if (!Reserve(1 + value.size())) {
            return;
        }
        WriteU8(static_cast<uint8_t>(value.size()));
        WriteBytes({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
    }

    bool Ok() const { return m_ok; }
    size_t Size() const { return m_offset; }

private:
    bool Reserve(size_t size) {
        // 減算で比較し、サイズ加算のオーバーフローを避ける。
        if (!m_ok || size > m_buffer.size() - m_offset) {
            m_ok = false;
            return false;
        }
        return true;
    }

    void WriteUnsigned(uint64_t value, size_t size) {
        if (!Reserve(size)) {
            return;
        }
        for (size_t index = 0; index < size; ++index) {
            m_buffer[m_offset++] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
        }
    }

    std::span<std::byte> m_buffer;
    size_t m_offset = 0;
    bool m_ok = true;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) : m_data(data) {}

    uint8_t ReadU8() { return static_cast<uint8_t>(ReadUnsigned(1)); }
    uint16_t ReadU16() { return static_cast<uint16_t>(ReadUnsigned(2)); }
    uint32_t ReadU32() { return static_cast<uint32_t>(ReadUnsigned(4)); }
    uint64_t ReadU64() { return ReadUnsigned(8); }
    float ReadF32() { return std::bit_cast<float>(ReadU32()); }

    // 足りないときは何も書かずに失敗にする。
    void ReadBytes(std::span<uint8_t> out) {
        if (!Reserve(out.size()) || out.empty()) {
            return;
        }
        std::memcpy(out.data(), m_data.data() + m_offset, out.size());
        m_offset += out.size();
    }

    std::string ReadString(size_t maxLength) {
        const auto length = ReadU8();
        if (length > maxLength || !Reserve(length)) {
            m_ok = false;
            return {};
        }
        std::string value(length, '\0');
        ReadBytes({reinterpret_cast<uint8_t*>(value.data()), value.size()});
        return value;
    }

    bool Ok() const { return m_ok; }
    size_t Remaining() const { return m_data.size() - m_offset; }

private:
    bool Reserve(size_t size) {
        if (!m_ok || size > Remaining()) {
            m_ok = false;
            return false;
        }
        return true;
    }

    uint64_t ReadUnsigned(size_t size) {
        if (!Reserve(size)) {
            return 0;
        }
        uint64_t value = 0;
        for (size_t index = 0; index < size; ++index) {
            value |= static_cast<uint64_t>(std::to_integer<uint8_t>(m_data[m_offset++])) << (index * 8);
        }
        return value;
    }

    std::span<const std::byte> m_data;
    size_t m_offset = 0;
    bool m_ok = true;
};

}
