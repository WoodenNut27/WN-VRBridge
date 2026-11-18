#pragma once

#include <cstdint>
#include <functional>
#include <istream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Quest のアプリのフォルダに置く、調整用の小さな設定ファイルの読み取り。
// Android の API を使わないので、PC の単体テストでも同じコードを確かめられる。
namespace wnvr::quest {

// 「キー=値」の 1 項目。値は minimum〜maximum（両端を含む）の整数だけを受け付ける。
struct IntSetting {
    std::string_view key;
    int minimum = 0;
    int maximum = 0;
    std::function<void(int)> apply;
};

// 1 行に 1 項目の「キー=値」を読み、対応する apply を呼ぶ。空行と # で始まる行は読み飛ばす。
// 読めなかった行は無視して続け、理由を戻り値に入れる（ログに出すのは呼び出し側）。
std::vector<std::string> ReadIntSettings(std::istream& input, std::string_view fileName,
    std::span<const IntSetting> settings);

struct EyeSize {
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const EyeSize&) const = default;
};
std::optional<EyeSize> ReadEyeSize(std::istream& input, std::vector<std::string>* warnings);
std::string FormatEyeSize(EyeSize size);
// ランタイムの上限を超えるときも、映像をゆがめないよう縦横比を保つ。
EyeSize ChooseEyeSize(EyeSize recommended, EyeSize maximum, std::optional<EyeSize> saved);

struct HostAddress {
    std::string host;
    uint16_t port = 0;
};

// host.txt の 1 行目（「IPv4」か「IPv4:ポート」）を分ける。形が合わなければ nullopt。
// host が正しい IPv4 アドレスかどうかは、ソケットを開く側で確かめる。
std::optional<HostAddress> ParseHostLine(std::string_view line, uint16_t defaultPort);

}
