#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include "fah/Source.hpp"
#include <algorithm>
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

class WindowsSource final : public Source {
    SourceOptions options_;
    SourceCallback callback_;
    std::wstring host_, path_;
    INTERNET_PORT port_ = 0;
    bool secure_ = false;
    std::atomic<bool> active_{false};
    std::thread receiver_;
    std::mutex mutex_;
    std::condition_variable wake_;
    HINTERNET socket_ = nullptr;
    HINTERNET request_ = nullptr;

    void emit(SourceEvent::Kind kind, std::string text) {
        if (!active_) return;
        SourceEvent event;
        event.kind = kind;
        event.text = std::move(text);
        callback_(std::move(event));
    }

    void closeConnection() {
        HINTERNET socket, request;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            socket = socket_;
            socket_ = nullptr;
            request = request_;
            request_ = nullptr;
        }
        if (socket) WinHttpCloseHandle(socket);
        if (request) WinHttpCloseHandle(request);
    }

    bool connectAndReceive() {
        Handle session(WinHttpOpen(L"FAH-Saver/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.value) throw std::runtime_error("Could not create native HTTP session");
        WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 30000);
        Handle connection(WinHttpConnect(session.value, host_.c_str(), port_, 0));
        if (!connection.value) return false;
        const auto request = WinHttpOpenRequest(connection.value, L"GET", path_.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure_ ? WINHTTP_FLAG_SECURE : 0);
        if (!request) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_) { WinHttpCloseHandle(request); return false; }
            request_ = request;
        }
        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            throw std::runtime_error("Native WebSocket support is unavailable");
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_WINHTTP_SECURE_FAILURE || error == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED)
                throw std::runtime_error("FAH TLS certificate validation failed");
            return false;
        }
        DWORD status = 0, length = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) || status != 101)
            throw std::runtime_error("FAH endpoint refused the WebSocket connection");
        const auto socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        if (!socket) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_) { WinHttpCloseHandle(socket); return false; }
            socket_ = socket;
        }
        SourceSession protocol(options_, [this](SourceEvent event) {
            if (active_) callback_(std::move(event));
        });
        protocol.connected();
        std::string message;
        char buffer[65536];
        while (active_) {
            DWORD bytes = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            const auto error = WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &bytes, &type);
            if (error || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;
            if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                throw std::runtime_error("FAH sent a non-text message");
            if (bytes > 26'000'000 - message.size()) throw std::runtime_error("FAH message exceeds size limit");
            message.append(buffer, bytes);
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
            const auto commands = protocol.receive(message);
            message.clear();
            for (const auto& command : commands)
                if (WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                        const_cast<char*>(command.data()), static_cast<DWORD>(command.size())))
                    return false;
        }
        return false;
    }

    void run() {
        unsigned retry = 0;
        while (active_) {
            try {
                emit(SourceEvent::Kind::Status, "Connecting to FAH");
                connectAndReceive();
                closeConnection();
                emit(SourceEvent::Kind::Reset, "FAH connection ended");
            } catch (const std::exception& error) {
                closeConnection();
                emit(SourceEvent::Kind::Reset, "FAH connection ended");
                emit(SourceEvent::Kind::Error, error.what());
                active_ = false;
                return;
            }
            if (!active_) return;
            const auto seconds = std::min(30u, 1u << std::min(retry++, 5u));
            emit(SourceEvent::Kind::Status, "FAH connection lost; retrying in " + std::to_string(seconds) + " seconds");
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(seconds), [this] { return !active_; });
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
        active_ = true;
        receiver_ = std::thread([this] { run(); });
    }
    void stop() override {
        active_ = false;
        closeConnection();
        wake_.notify_all();
        if (receiver_.joinable()) receiver_.join();
    }
};
}

std::unique_ptr<Source> makeSource(SourceOptions options, SourceCallback callback) {
    return std::make_unique<WindowsSource>(std::move(options), std::move(callback));
}
} // namespace fah
