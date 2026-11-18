#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace wnvr::host {

enum class FirewallRuleState {
    Registered, // 2 つの許可ルールが、今の場所のホストと SteamVR に対して正しく入っている
    Missing,    // ルールが無い
    Outdated,   // ルールはあるが、場所や内容が今と合っていない（登録し直しが必要）
    Unknown,    // 確認できなかった
};

struct FirewallStatus {
    FirewallRuleState state = FirewallRuleState::Unknown;
    std::string error;
};

// 読むだけなら管理者の権限は要らない。
FirewallStatus QueryFirewallRules(const std::wstring& hostExePath, const std::wstring& vrServerPath);

// 許可ルールの追加と削除には管理者の権限が要る。ホストは普段の権限のまま動かし、
// 変更のときだけ自分自身を「--firewall ...」付きで管理者として起動し直す（UAC の確認が出る）。
// 終わるまで待つので、UI スレッドから呼ばない。
bool ChangeFirewallRulesElevated(HWND owner, bool add, const std::wstring& vrServerPath, std::string& error);

// 上の呼び出しで起動された側の入口。「--firewall」で始まる引数でなければ nullopt を返す。
// 戻り値はプロセスの終了コード（0 は成功、失敗は HRESULT か 1・2）。
std::optional<int> RunFirewallCommand(int argumentCount, wchar_t** arguments);

}
