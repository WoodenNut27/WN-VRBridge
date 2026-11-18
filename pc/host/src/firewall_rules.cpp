#include "firewall_rules.h"

#include <wnvr/protocol/messages.h>
#include <wnvr/win32/platform.h>

#include <netfw.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace wnvr::host {
namespace {

using Microsoft::WRL::ComPtr;
constexpr wchar_t kGroup[] = L"WN-VRBridge";
constexpr const wchar_t* kNames[] = {L"WN-VRBridge ホスト (UDP)", L"WN-VRBridge SteamVR ドライバ (UDP)"};
// 公共のネットワーク（カフェの Wi-Fi など）では受け付けない。
constexpr long kProfiles = NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN;

// 管理者として起動し直すときのコマンド ライン。送る側と受ける側をこのファイルにまとめてある。
//   --firewall add "<vrserver.exe のパス>"
//   --firewall remove
constexpr std::wstring_view kCommandSwitch = L"--firewall";
constexpr std::wstring_view kCommandAdd = L"add";
constexpr std::wstring_view kCommandRemove = L"remove";
constexpr int kInvalidCommandExitCode = 2;

void Check(HRESULT result) {
    if (FAILED(result)) {
        throw result;
    }
}

class ComApartment {
public:
    ComApartment() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
        if (m_result != RPC_E_CHANGED_MODE) {
            Check(m_result);
        }
    }
    ~ComApartment() {
        if (m_result == S_OK || m_result == S_FALSE) {
            CoUninitialize();
        }
    }
private:
    HRESULT m_result;
};

struct Bstr {
    explicit Bstr(const wchar_t* text = nullptr) : m_value(text ? SysAllocString(text) : nullptr) {
        if (text && !m_value) {
            throw std::bad_alloc();
        }
    }
    ~Bstr() { SysFreeString(m_value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR m_value;
};

struct Variant {
    Variant() { VariantInit(&m_value); }
    ~Variant() { VariantClear(&m_value); }
    VARIANT m_value;
};

bool Equal(const wchar_t* left, const wchar_t* right) {
    return left && right && CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

std::wstring NormalizePath(const std::wstring& path) {
    return std::filesystem::path(path).lexically_normal().make_preferred().wstring();
}

std::wstring Ports() {
    return std::to_wstring(protocol::kDiscoveryPort) + L"," + std::to_wstring(protocol::kDefaultPort);
}

ComPtr<INetFwRules> OpenRules() {
    ComPtr<INetFwPolicy2> policy;
    Check(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(policy.GetAddressOf())));
    ComPtr<INetFwRules> rules;
    Check(policy->get_Rules(rules.GetAddressOf()));
    return rules;
}

std::vector<ComPtr<INetFwRule>> GroupRules(INetFwRules* rules) {
    ComPtr<IUnknown> unknown;
    Check(rules->get__NewEnum(unknown.GetAddressOf()));
    ComPtr<IEnumVARIANT> enumerator;
    Check(unknown.As(&enumerator));
    std::vector<ComPtr<INetFwRule>> result;
    for (;;) {
        Variant item;
        ULONG fetched = 0;
        const HRESULT next = enumerator->Next(1, &item.m_value, &fetched);
        Check(next);
        if (next == S_FALSE || fetched == 0) {
            break;
        }
        ComPtr<INetFwRule> rule;
        if (item.m_value.vt == VT_DISPATCH && item.m_value.pdispVal) {
            Check(item.m_value.pdispVal->QueryInterface(IID_PPV_ARGS(rule.GetAddressOf())));
        } else if (item.m_value.vt == VT_UNKNOWN && item.m_value.punkVal) {
            Check(item.m_value.punkVal->QueryInterface(IID_PPV_ARGS(rule.GetAddressOf())));
        } else {
            throw E_UNEXPECTED;
        }
        Bstr group;
        Check(rule->get_Grouping(&group.m_value));
        if (Equal(group.m_value, kGroup)) {
            result.push_back(std::move(rule));
        }
    }
    return result;
}

void RemoveGroup(INetFwRules* rules) {
    // 列挙中の削除で項目を飛ばさないよう、先に対象を収集する。
    for (const auto& rule : GroupRules(rules)) {
        Bstr name;
        Check(rule->get_Name(&name.m_value));
        Check(rules->Remove(name.m_value));
    }
}

void AddRule(INetFwRules* rules, const wchar_t* name, const std::wstring& path) {
    ComPtr<INetFwRule> rule;
    Check(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(rule.GetAddressOf())));
    Bstr ruleName(name);
    Bstr group(kGroup);
    Bstr application(path.c_str());
    Bstr ports(Ports().c_str());
    Check(rule->put_Name(ruleName.m_value));
    Check(rule->put_Grouping(group.m_value));
    Check(rule->put_ApplicationName(application.m_value));
    Check(rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP));
    Check(rule->put_LocalPorts(ports.m_value));
    Check(rule->put_Direction(NET_FW_RULE_DIR_IN));
    Check(rule->put_Action(NET_FW_ACTION_ALLOW));
    Check(rule->put_Profiles(kProfiles));
    Check(rule->put_Enabled(VARIANT_TRUE));
    Check(rules->Add(rule.Get()));
}

int AddFirewallRules(const std::wstring& hostExePath, const std::wstring& vrServerPath) {
    try {
        ComApartment apartment;
        const auto rules = OpenRules();
        try {
            RemoveGroup(rules.Get());
            AddRule(rules.Get(), kNames[0], hostExePath);
            AddRule(rules.Get(), kNames[1], vrServerPath);
        } catch (...) {
            // 後始末の失敗で、登録に失敗した元の理由を上書きしない。
            try {
                RemoveGroup(rules.Get());
            } catch (...) {
            }
            throw;
        }
        return 0;
    } catch (HRESULT result) {
        return static_cast<int>(result);
    } catch (...) {
        return 1;
    }
}

int RemoveFirewallRules() {
    try {
        ComApartment apartment;
        const auto rules = OpenRules();
        RemoveGroup(rules.Get());
        return 0;
    } catch (HRESULT result) {
        return static_cast<int>(result);
    } catch (...) {
        return 1;
    }
}

}

FirewallStatus QueryFirewallRules(const std::wstring& hostExePath, const std::wstring& vrServerPath) {
    try {
        ComApartment apartment;
        const auto rules = OpenRules();
        const auto group = GroupRules(rules.Get());
        if (group.empty()) {
            return {FirewallRuleState::Missing, {}};
        }
        bool found[2] = {};
        bool valid = group.size() == 2;
        const std::wstring paths[] = {NormalizePath(hostExePath), NormalizePath(vrServerPath)};
        for (const auto& rule : group) {
            Bstr name;
            Check(rule->get_Name(&name.m_value));
            bool expected = false;
            for (size_t index = 0; index < 2; ++index) {
                if (!Equal(name.m_value, kNames[index])) {
                    continue;
                }
                expected = true;
                Bstr application;
                Bstr ports;
                VARIANT_BOOL enabled = VARIANT_FALSE;
                long protocol = 0;
                long profiles = 0;
                NET_FW_RULE_DIRECTION direction{};
                NET_FW_ACTION action{};
                Check(rule->get_ApplicationName(&application.m_value));
                Check(rule->get_Enabled(&enabled));
                Check(rule->get_Protocol(&protocol));
                Check(rule->get_Profiles(&profiles));
                Check(rule->get_Direction(&direction));
                Check(rule->get_Action(&action));
                // プロトコルの変更後は LocalPorts の取得自体が失敗する場合がある。
                if (protocol == NET_FW_IP_PROTOCOL_UDP) {
                    Check(rule->get_LocalPorts(&ports.m_value));
                }
                const auto applicationPath = application.m_value ? NormalizePath(application.m_value) : std::wstring{};
                valid = valid && !found[index] && application.m_value && Equal(applicationPath.c_str(), paths[index].c_str())
                    && enabled == VARIANT_TRUE && protocol == NET_FW_IP_PROTOCOL_UDP
                    && profiles == kProfiles && direction == NET_FW_RULE_DIR_IN && action == NET_FW_ACTION_ALLOW
                    && Equal(ports.m_value, Ports().c_str());
                found[index] = true;
            }
            valid = valid && expected;
        }
        if (!found[0] && !found[1]) {
            return {FirewallRuleState::Missing, {}};
        }
        return {valid && found[0] && found[1] ? FirewallRuleState::Registered : FirewallRuleState::Outdated, {}};
    } catch (HRESULT result) {
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
        return {FirewallRuleState::Unknown, std::string("ファイアウォールの読み取りに失敗しました（") + code + "）"};
    } catch (...) {
        return {FirewallRuleState::Unknown, "ファイアウォールの確認で予期しないエラーが発生しました"};
    }
}

bool ChangeFirewallRulesElevated(HWND owner, bool add, const std::wstring& vrServerPath, std::string& error) {
    const auto exe = win32::ExecutablePath().wstring();
    // パスは引用符で囲んで渡すので、引用符を含むパスは受け付けない。
    if (exe.empty() || (add && (vrServerPath.empty() || vrServerPath.find(L'\"') != std::wstring::npos))) {
        error = "ホストまたは SteamVR のパスが不正です";
        return false;
    }
    const auto parameters = std::wstring(kCommandSwitch)
        + (add ? L" " + std::wstring(kCommandAdd) + L" \"" + vrServerPath + L"\"" : L" " + std::wstring(kCommandRemove));
    std::optional<ComApartment> apartment;
    try {
        // ShellExecuteEx は、シェルの拡張が COM を使うことがある。
        apartment.emplace();
    } catch (...) {
        error = "管理者プロセスを起動するための COM 初期化に失敗しました";
        return false;
    }
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.hwnd = owner;
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"runas";
    execute.lpFile = exe.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        const DWORD launchError = GetLastError();
        error = launchError == ERROR_CANCELLED ? "キャンセルされました"
            : "管理者プロセスを起動できません（Windows " + std::to_string(launchError) + "）";
        return false;
    }
    if (!execute.hProcess) {
        error = "管理者プロセスのハンドルを取得できません";
        return false;
    }
    DWORD exitCode = 0;
    const bool finished = WaitForSingleObject(execute.hProcess, INFINITE) == WAIT_OBJECT_0
        && GetExitCodeProcess(execute.hProcess, &exitCode);
    CloseHandle(execute.hProcess);
    if (!finished) {
        error = "管理者プロセスの終了を確認できません";
        return false;
    }
    if (exitCode != 0) {
        // HRESULT らしい値は 16 進で、それ以外は 10 進で見せる。
        char code[16]{};
        if ((exitCode & 0x80000000UL) != 0) {
            std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(exitCode));
        } else {
            std::snprintf(code, sizeof(code), "%lu", static_cast<unsigned long>(exitCode));
        }
        error = std::string("許可ルールの変更に失敗しました（終了コード ") + code + "）";
        return false;
    }
    return true;
}

std::optional<int> RunFirewallCommand(int argumentCount, wchar_t** arguments) {
    if (argumentCount < 2 || std::wstring_view(arguments[1]) != kCommandSwitch) {
        return std::nullopt;
    }
    try {
        if (argumentCount == 3 && std::wstring_view(arguments[2]) == kCommandRemove) {
            return RemoveFirewallRules();
        }
        if (argumentCount == 4 && std::wstring_view(arguments[2]) == kCommandAdd) {
            // 管理者として動くので、引数をそのまま信じない。許可するのは、実在する vrserver.exe と
            // 自分自身（引数では受け取らない）だけにする。
            const auto server = std::filesystem::path(arguments[3]).lexically_normal().make_preferred();
            const auto host = win32::ExecutablePath().lexically_normal().make_preferred();
            if (!host.empty() && server.is_absolute()
                && Equal(server.filename().c_str(), L"vrserver.exe")
                && std::filesystem::is_regular_file(server)) {
                return AddFirewallRules(host.wstring(), server.wstring());
            }
        }
    } catch (...) {
    }
    return kInvalidCommandExitCode;
}

}
