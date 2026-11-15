#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include "fah/Source.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fah {
namespace {
std::wstring wide(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 in FAH source URL");
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

struct Handle {
    HINTERNET value = nullptr;
    ~Handle() { if (value) WinHttpCloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HINTERNET handle) : value(handle) {}
};

struct AsyncControl {
    std::atomic<bool> active{false};
    std::mutex mutex;
    std::condition_variable wake;
    bool closeFailed = false;
};

struct AsyncState {
    std::shared_ptr<AsyncControl> control;
    std::shared_ptr<AsyncState> lifetime;
    DWORD completion = 0;
    DWORD error = 0;
    DWORD bytes = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    bool closed = false;
    std::array<char, 65536> buffer;
    std::string outgoing;
};

void CALLBACK statusCallback(HINTERNET, DWORD_PTR context, DWORD status, void* information, DWORD length) {
    if (!context) return;
    auto state = reinterpret_cast<AsyncState*>(context)->lifetime;
    std::lock_guard<std::mutex> lock(state->control->mutex);
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        state->closed = true;
        state->lifetime.reset();
    } else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        if (length >= sizeof(WINHTTP_ASYNC_RESULT))
            state->error = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
        else state->error = ERROR_INVALID_DATA;
    } else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
               status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE) {
        state->completion = status;
        if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            if (length < sizeof(WINHTTP_WEB_SOCKET_STATUS)) state->error = ERROR_INVALID_DATA;
            else {
                const auto result = static_cast<WINHTTP_WEB_SOCKET_STATUS*>(information);
                state->bytes = result->dwBytesTransferred;
                state->type = result->eBufferType;
            }
        }
    }
    state->control->wake.notify_all();
}

// Only the receiver thread calls WinHTTP. Async buffers remain alive until the
// final HANDLE_CLOSING callback, including when an operation is cancelled.
class AsyncHandle {
    bool closing_ = false;
public:
    HINTERNET value = nullptr;
    std::shared_ptr<AsyncState> state;
    explicit AsyncHandle(std::shared_ptr<AsyncControl> control) : state(std::make_shared<AsyncState>()) {
        state->control = std::move(control);
    }
    AsyncHandle(const AsyncHandle&) = delete;
    AsyncHandle& operator=(const AsyncHandle&) = delete;
    ~AsyncHandle() { close(); }

    DWORD_PTR context() {
        state->lifetime = state;
        return reinterpret_cast<DWORD_PTR>(state.get());
    }
    void attachRequest(HINTERNET handle) {
        if (!handle) return;
        auto binding = context();
        if (!WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE,
                              &binding, sizeof(binding))) {
            state->lifetime.reset();
            WinHttpCloseHandle(handle);
            throw std::runtime_error("Could not initialise asynchronous FAH request");
        }
        value = handle;
    }
    void close() {
        if (!value) {
            if (!closing_) state->lifetime.reset();
            return;
        }
        const auto handle = value;
        value = nullptr;
        closing_ = true;
        if (!WinHttpCloseHandle(handle)) {
            // Keep the callback context alive if Windows cannot release it.
            std::lock_guard<std::mutex> lock(state->control->mutex);
            state->control->closeFailed = true;
            return;
        }
        std::unique_lock<std::mutex> lock(state->control->mutex);
        state->control->wake.wait(lock, [&] { return state->closed; });
    }
    void prepare() {
        std::lock_guard<std::mutex> lock(state->control->mutex);
        state->completion = state->error = state->bytes = 0;
    }
    bool wait(DWORD completion) {
        std::unique_lock<std::mutex> lock(state->control->mutex);
        state->control->wake.wait(lock, [&] {
            return !state->control->active || state->error || state->completion == completion;
        });
        if (!state->control->active) return false;
        if (state->error == ERROR_WINHTTP_SECURE_FAILURE || state->error == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED)
            throw std::runtime_error("FAH TLS certificate validation failed");
        return state->error == 0;
    }
};

class WindowsSource final : public Source {
    SourceOptions options_;
    SourceCallback callback_;
    std::wstring host_, path_;
    INTERNET_PORT port_ = 0;
    bool secure_ = false;
    std::shared_ptr<AsyncControl> control_ = std::make_shared<AsyncControl>();
    std::thread receiver_;

    void emit(SourceEvent::Kind kind, std::string text) {
        if (!control_->active) return;
        SourceEvent event;
        event.kind = kind;
        event.text = std::move(text);
        callback_(std::move(event));
    }

    bool connectAndReceive() {
        Handle session(WinHttpOpen(L"FAH-Saver/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
        if (!session.value) throw std::runtime_error("Could not create native HTTP session");
        if (WinHttpSetStatusCallback(session.value, statusCallback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0) == WINHTTP_INVALID_STATUS_CALLBACK ||
            !WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 30000))
            throw std::runtime_error("Could not configure native HTTP session");
        Handle connection(WinHttpConnect(session.value, host_.c_str(), port_, 0));
        if (!connection.value) return false;
        AsyncHandle request(control_);
        request.attachRequest(WinHttpOpenRequest(connection.value, L"GET", path_.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure_ ? WINHTTP_FLAG_SECURE : 0));
        if (!request.value || !control_->active) return false;
        if (!WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            throw std::runtime_error("Native WebSocket support is unavailable");
        request.prepare();
        if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, request.context())) {
            const auto error = GetLastError();
            if (error == ERROR_WINHTTP_SECURE_FAILURE || error == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED)
                throw std::runtime_error("FAH TLS certificate validation failed");
            if (error != ERROR_IO_PENDING) return false;
        }
        if (!request.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE)) return false;
        request.prepare();
        if (!WinHttpReceiveResponse(request.value, nullptr) && GetLastError() != ERROR_IO_PENDING) return false;
        if (!request.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)) return false;
        DWORD status = 0, length = sizeof(status);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) || status != 101)
            throw std::runtime_error("FAH endpoint refused the WebSocket connection");
        AsyncHandle socket(control_);
        socket.value = WinHttpWebSocketCompleteUpgrade(request.value, socket.context());
        if (!socket.value || !control_->active) return false;
        request.close();
        if (control_->closeFailed) throw std::runtime_error("WinHTTP could not release its asynchronous request");
        SourceSession protocol(options_, [this](SourceEvent event) {
            if (control_->active) callback_(std::move(event));
        });
        protocol.connected();
        std::string message;
        while (control_->active) {
            socket.prepare();
            const auto error = WinHttpWebSocketReceive(socket.value, socket.state->buffer.data(),
                                                       static_cast<DWORD>(socket.state->buffer.size()), nullptr, nullptr);
            if (error != NO_ERROR && error != ERROR_IO_PENDING) return false;
            if (!socket.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE)) return false;
            const auto bytes = socket.state->bytes;
            const auto type = socket.state->type;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;
            if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                throw std::runtime_error("FAH sent a non-text message");
            if (bytes > 26'000'000 - message.size()) throw std::runtime_error("FAH message exceeds size limit");
            if (bytes > socket.state->buffer.size()) throw std::runtime_error("Invalid native WebSocket read length");
            message.append(socket.state->buffer.data(), bytes);
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
            const auto commands = protocol.receive(message);
            message.clear();
            for (const auto& command : commands) {
                socket.state->outgoing = command;
                socket.prepare();
                const auto send = WinHttpWebSocketSend(socket.value, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                    socket.state->outgoing.data(), static_cast<DWORD>(socket.state->outgoing.size()));
                if (send != NO_ERROR && send != ERROR_IO_PENDING) return false;
                if (!socket.wait(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE)) return false;
            }
        }
        return false;
    }

    void run() {
        unsigned retry = 0;
        while (control_->active) {
            try {
                emit(SourceEvent::Kind::Status, "Connecting to FAH");
                connectAndReceive();
                if (control_->closeFailed) throw std::runtime_error("WinHTTP could not release its asynchronous connection");
                emit(SourceEvent::Kind::Reset, "FAH connection ended");
            } catch (const std::exception& error) {
                emit(SourceEvent::Kind::Reset, "FAH connection ended");
                emit(SourceEvent::Kind::Error, error.what());
                control_->active = false;
                return;
            }
            if (!control_->active) return;
            const auto seconds = std::min(30u, 1u << std::min(retry++, 5u));
            emit(SourceEvent::Kind::Status, "FAH connection lost; retrying in " + std::to_string(seconds) + " seconds");
            std::unique_lock<std::mutex> lock(control_->mutex);
            control_->wake.wait_for(lock, std::chrono::seconds(seconds), [this] { return !control_->active; });
        }
    }

public:
    WindowsSource(SourceOptions options, SourceCallback callback) : options_(std::move(options)), callback_(std::move(callback)) {
        if (!callback_ || options_.url.size() > 4096 || options_.unit.size() > 1024)
            throw std::runtime_error("Invalid FAH source options");
        auto url = options_.url;
        if (url.rfind("wss://", 0) == 0) { url.replace(0, 3, "https"); secure_ = true; }
        else if (url.rfind("ws://", 0) == 0) url.replace(0, 2, "http");
        else throw std::runtime_error("FAH source must use ws:// or wss://");
        const auto text = wide(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = -1;
        if (!WinHttpCrackUrl(text.c_str(), static_cast<DWORD>(text.size()), 0, &parts) ||
            !parts.dwHostNameLength || parts.dwHostNameLength > 253 || parts.dwUserNameLength || parts.dwPasswordLength)
            throw std::runtime_error("Invalid FAH source URL or embedded credentials");
        host_.assign(parts.lpszHostName, parts.dwHostNameLength);
        path_.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (path_.empty()) path_ = L"/";
        if (parts.dwExtraInfoLength) {
            const std::wstring extra(parts.lpszExtraInfo, parts.dwExtraInfoLength);
            if (extra.find(L'#') != std::wstring::npos) throw std::runtime_error("FAH source URL must not contain a fragment");
            path_ += extra;
        }
        port_ = parts.nPort;
        if (!port_) throw std::runtime_error("Invalid FAH source port");
    }
    ~WindowsSource() override { stop(); }
    void start() override {
        if (receiver_.joinable()) return;
        control_->active = true;
        receiver_ = std::thread([this] { run(); });
    }
    void stop() override {
        {
            std::lock_guard<std::mutex> lock(control_->mutex);
            control_->active = false;
        }
        control_->wake.notify_all();
        if (receiver_.joinable()) receiver_.join();
    }
};
}

std::unique_ptr<Source> makeSource(SourceOptions options, SourceCallback callback) {
    return std::make_unique<WindowsSource>(std::move(options), std::move(callback));
}
} // namespace fah
