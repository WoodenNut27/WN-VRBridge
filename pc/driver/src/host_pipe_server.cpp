#include "host_pipe_server.h"

#include "driver_log.h"
#include "stats_collector.h"

#include <wnvr/host_ipc/pipe_io.h>
#include <wnvr/version.h>

#include <chrono>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace wnvr::driver {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kStatusInterval = std::chrono::seconds(1);
constexpr DWORD kCreateRetryMs = 5000;
constexpr DWORD kErrorRetryMs = 1000;
// ホストがつながっていないあいだに届いた通知を、ためておける数。
constexpr size_t kMaxQueuedMessages = 16;

}

struct HostPipeServer::Impl {
    HostPipeListener& m_listener;
    const StatsCollector& m_stats;
    ipc::Handle m_stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    ipc::Handle m_wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex m_mutex;
    std::deque<ipc::Message> m_queue;
    bool m_connected = false;
    std::jthread m_thread;

    Impl(HostPipeListener& listener, const StatsCollector& stats) : m_listener(listener), m_stats(stats) {}

    void SetConnected(bool connected) {
        std::lock_guard lock(m_mutex);
        m_connected = connected;
        if (!connected) {
            m_queue.clear();
        }
    }

    // ホストがつないでくるのを待つ。止められたか失敗したら false。
    bool WaitForHost(HANDLE pipe) {
        ipc::Operation connect(pipe);
        if (ConnectNamedPipe(pipe, &connect.m_overlapped)) {
            return true;
        }
        const DWORD error = GetLastError();
        // パイプを作ってから待ち始めるまでのあいだに、すでにつながっていることがある。
        if (error == ERROR_PIPE_CONNECTED) {
            return true;
        }
        connect.m_pending = error == ERROR_IO_PENDING;
        if (!connect.m_pending || !connect.Wait(m_stop.m_value, INFINITE)) {
            return false;
        }
        DWORD bytes = 0;
        return GetOverlappedResult(pipe, &connect.m_overlapped, &bytes, FALSE) != FALSE;
    }

    // つながっているあいだ、1 秒ごとに状態を送り、ホストからの設定を受け取る。
    void Serve(HANDLE pipe) {
        if (!ipc::WriteMessage(pipe, m_stop.m_value, ipc::HelloMessage{ipc::kProtocolVersion, kVersion})) {
            return;
        }
        auto nextStatus = Clock::now();
        const auto sendPending = [&] {
            // 承認の要求など、ほかのスレッドから頼まれたメッセージを先に送る。
            std::deque<ipc::Message> queue;
            {
                std::lock_guard lock(m_mutex);
                queue.swap(m_queue);
            }
            for (const auto& message : queue) {
                if (!ipc::WriteMessage(pipe, m_stop.m_value, message)) {
                    return false;
                }
            }
            if (Clock::now() < nextStatus) {
                return true;
            }
            nextStatus = Clock::now() + kStatusInterval;
            if (!ipc::WriteMessage(pipe, m_stop.m_value, ipc::StatusMessage{m_stats.GetSnapshot()})) {
                Log("ホストへの状態の送信に失敗しました: %lu", GetLastError());
                return false;
            }
            return true;
        };
        const auto receive = [&](const ipc::Message& message) {
            if (const auto* settings = std::get_if<ipc::SettingsMessage>(&message)) {
                m_listener.OnHostSettings(*settings);
            }
            return true;
        };
        const auto result = ipc::ReadMessages(pipe, m_stop.m_value, m_wake.m_value, sendPending, receive);
        // 切断の原因を後から追えるよう、自分で止めたとき以外は理由を残す。
        if (result.end != ipc::ReadEnd::Stopped) {
            Log("ホストとのパイプを閉じます: %s", result.reason.c_str());
        }
    }

    // パイプを 1 つ作り、ホストが 1 回つないで切るまでを受け持つ。
    void RunOnce() {
        // 同じ名前のパイプを先に作られていたら失敗させ、別のプロセスになりすまされないようにする。
        ipc::Handle pipe(CreateNamedPipeW(ipc::kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, static_cast<DWORD>(ipc::kMaxMessageBytes), static_cast<DWORD>(ipc::kMaxMessageBytes), 0, nullptr));
        if (pipe.m_value == INVALID_HANDLE_VALUE) {
            Log("ホストパイプを作成できません: %lu（5 秒後に再試行）", GetLastError());
            WaitForSingleObject(m_stop.m_value, kCreateRetryMs);
            return;
        }
        if (!WaitForHost(pipe.m_value)) {
            return;
        }
        SetConnected(true);
        Log("ホストがパイプに接続しました");
        // 例外で抜けるときも、切断の後始末を必ず行う。
        struct Disconnect {
            Impl& impl;
            HANDLE pipe;
            ~Disconnect() {
                impl.SetConnected(false);
                DisconnectNamedPipe(pipe);
                Log("ホストがパイプから切断しました");
            }
        } disconnect{*this, pipe.m_value};
        Serve(pipe.m_value);
    }

    void Run() {
        while (WaitForSingleObject(m_stop.m_value, 0) == WAIT_TIMEOUT) {
            try {
                RunOnce();
            } catch (const std::exception& error) {
                Log("ホストパイプで例外: %s", error.what());
                WaitForSingleObject(m_stop.m_value, kErrorRetryMs);
            } catch (...) {
                Log("ホストパイプで予期しない例外");
                WaitForSingleObject(m_stop.m_value, kErrorRetryMs);
            }
        }
    }
};

HostPipeServer::HostPipeServer(HostPipeListener& listener, const StatsCollector& stats)
    : m_impl(std::make_unique<Impl>(listener, stats)) {}

HostPipeServer::~HostPipeServer() {
    Stop();
}

void HostPipeServer::Start() {
    if (m_impl->m_thread.joinable()) {
        return;
    }
    if (!m_impl->m_stop.m_value || !m_impl->m_wake.m_value) {
        Log("ホストパイプの停止イベントを作成できません");
        return;
    }
    ResetEvent(m_impl->m_stop.m_value);
    try {
        m_impl->m_thread = std::jthread([this] {
            m_impl->Run();
        });
    } catch (...) {
        Log("ホストパイプのスレッドを開始できません");
    }
}

void HostPipeServer::Send(const ipc::Message& message) {
    std::lock_guard lock(m_impl->m_mutex);
    if (!m_impl->m_connected) {
        return;
    }
    // あふれたら古いものから捨てる（承認の要求は、Hello が続くかぎり送り直される）。
    if (m_impl->m_queue.size() >= kMaxQueuedMessages) {
        m_impl->m_queue.pop_front();
    }
    m_impl->m_queue.push_back(message);
    SetEvent(m_impl->m_wake.m_value);
}

void HostPipeServer::Stop() {
    SetEvent(m_impl->m_stop.m_value);
    if (m_impl->m_thread.joinable()) {
        m_impl->m_thread.join();
    }
}

}
