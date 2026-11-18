#include <wnvr/host_ipc/pipe_io.h>

#include <array>
#include <stdexcept>

namespace wnvr::ipc {

Handle::Handle(HANDLE handle) : m_value(handle) {}

Handle::~Handle() {
    if (m_value && m_value != INVALID_HANDLE_VALUE) {
        CloseHandle(m_value);
    }
}

Operation::Operation(HANDLE pipe)
    : m_pipe(pipe), m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!m_event.m_value) {
        throw std::runtime_error("パイプイベントを作成できません");
    }
    m_overlapped.hEvent = m_event.m_value;
}

Operation::~Operation() {
    // バッファと OVERLAPPED を破棄する前に、キャンセルの完了まで回収する。
    if (m_pending) {
        CancelIoEx(m_pipe, &m_overlapped);
        DWORD bytes = 0;
        GetOverlappedResult(m_pipe, &m_overlapped, &bytes, TRUE);
    }
}

bool Operation::Wait(HANDLE stop, DWORD timeout) {
    const HANDLE events[] = {stop, m_event.m_value};
    return WaitForMultipleObjects(2, events, FALSE, timeout) == WAIT_OBJECT_0 + 1;
}

bool WriteMessage(HANDLE pipe, HANDLE stop, const Message& message) {
    const auto text = Encode(message);
    Operation operation(pipe);
    DWORD bytes = 0;
    if (!WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), nullptr, &operation.m_overlapped)) {
        operation.m_pending = GetLastError() == ERROR_IO_PENDING;
        if (!operation.m_pending || !operation.Wait(stop, 1000)) {
            return false;
        }
    }
    // 即時完了でも、オーバーラップ I/O の転送量は完了結果から取得する。
    if (!GetOverlappedResult(pipe, &operation.m_overlapped, &bytes, FALSE)) {
        return false;
    }
    operation.m_pending = false;
    return bytes == text.size();
}

ReadResult ReadMessages(HANDLE pipe, HANDLE stop, HANDLE wake,
    const std::function<bool()>& tick, const std::function<bool(const Message&)>& receive) {
    std::array<char, 4096> buffer{};
    Operation operation(pipe);
    std::string text;
    for (;;) {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
            return {ReadEnd::Stopped, "停止されました"};
        }
        if (!tick()) {
            return {ReadEnd::TickStopped, "送信または定期処理を終了しました"};
        }
        DWORD bytes = 0;
        DWORD error = ERROR_SUCCESS;
        if (!operation.m_pending) {
            ResetEvent(operation.m_event.m_value);
            if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &operation.m_overlapped)) {
                error = GetLastError();
                operation.m_pending = error == ERROR_IO_PENDING;
            }
            // ERROR_MORE_DATA で即時完了した断片も、転送量を回収する。
            if (!operation.m_pending && (error == ERROR_SUCCESS || error == ERROR_MORE_DATA)) {
                error = ERROR_SUCCESS;
                if (!GetOverlappedResult(pipe, &operation.m_overlapped, &bytes, FALSE)) {
                    error = GetLastError();
                }
            }
        }
        if (operation.m_pending) {
            const HANDLE events[] = {stop, operation.m_event.m_value, wake};
            const DWORD result = WaitForMultipleObjects(wake ? 3 : 2, events, FALSE, 100);
            if (result == WAIT_OBJECT_0) {
                return {ReadEnd::Stopped, "停止されました"};
            }
            if (result == WAIT_FAILED) {
                return {ReadEnd::IoError, "パイプの待機に失敗しました"};
            }
            if (result != WAIT_OBJECT_0 + 1) {
                continue;
            }
            operation.m_pending = false;
            error = ERROR_SUCCESS;
            if (!GetOverlappedResult(pipe, &operation.m_overlapped, &bytes, FALSE)) {
                error = GetLastError();
            }
        }
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) {
            return {ReadEnd::PeerClosed, "相手がパイプを閉じました"};
        }
        if (error != ERROR_SUCCESS && error != ERROR_MORE_DATA) {
            return {ReadEnd::IoError, "パイプの読み取りに失敗しました: " + std::to_string(error)};
        }
        if (text.size() + bytes > kMaxMessageBytes) {
            return {ReadEnd::MessageTooLarge, "パイプのメッセージが上限を超えています"};
        }
        text.append(buffer.data(), bytes);
        if (error == ERROR_MORE_DATA) {
            continue;
        }
        std::string reason;
        const auto message = Decode(text, &reason);
        text.clear();
        if (!message) {
            return {ReadEnd::InvalidJson, "パイプの JSON が不正です: " + reason};
        }
        if (const auto* hello = std::get_if<HelloMessage>(&*message);
            hello && hello->protocolVersion != kProtocolVersion) {
            return {ReadEnd::ProtocolMismatch, "パイプのプロトコルが異なります: " + std::to_string(hello->protocolVersion)};
        }
        if (!receive(*message)) {
            return {ReadEnd::ReceiveStopped, "受信処理を終了しました"};
        }
    }
}

}
