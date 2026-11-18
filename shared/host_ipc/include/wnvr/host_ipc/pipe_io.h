#pragma once

#include <wnvr/host_ipc/host_ipc.h>

#include <windows.h>

#include <functional>

namespace wnvr::ipc {

// Windows 専用。未完了 I/O の回収までハンドルと OVERLAPPED を保持する。
struct Handle {
    HANDLE m_value;

    explicit Handle(HANDLE handle);
    ~Handle();
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Operation {
    HANDLE m_pipe;
    Handle m_event;
    OVERLAPPED m_overlapped{};
    bool m_pending = false;

    explicit Operation(HANDLE pipe);
    ~Operation();
    bool Wait(HANDLE stop, DWORD timeout);
};

enum class ReadEnd {
    Stopped,
    PeerClosed,
    IoError,
    TickStopped,
    ReceiveStopped,
    MessageTooLarge,
    InvalidJson,
    ProtocolMismatch
};

struct ReadResult {
    ReadEnd end;
    std::string reason;
};

bool WriteMessage(HANDLE pipe, HANDLE stop, const Message& message);
ReadResult ReadMessages(HANDLE pipe, HANDLE stop, HANDLE wake,
    const std::function<bool()>& tick, const std::function<bool(const Message&)>& receive);

}
