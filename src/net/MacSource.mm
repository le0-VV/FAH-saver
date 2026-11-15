#import <Foundation/Foundation.h>
#include "fah/Source.hpp"
#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace fah {
namespace {
struct MacState : std::enable_shared_from_this<MacState> {
    SourceOptions options;
    SourceCallback callback;
    std::unique_ptr<SourceSession> protocol;
    NSURL* url;
    NSURLSession* session;
    NSURLSessionWebSocketTask* socket;
    bool active = false;
    unsigned generation = 0;
    unsigned retry = 0;
    bool opened = false;

    void emit(SourceEvent::Kind kind, std::string text) {
        if (!active) return;
        SourceEvent event;
        event.kind = kind;
        event.text = std::move(text);
        callback(std::move(event));
    }

    void releaseConnection() {
        ++generation;
        [socket cancelWithCloseCode:NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
        [session invalidateAndCancel];
        socket = nil;
        session = nil;
        protocol.reset();
    }

    void fail(std::string message, bool reconnect) {
        if (!active) return;
        releaseConnection();
        if (!reconnect) {
            emit(SourceEvent::Kind::Error, std::move(message));
            active = false;
            return;
        }
        const unsigned delay = std::min(30u, 1u << std::min(retry++, 5u));
        emit(SourceEvent::Kind::Status, std::move(message) + "; retrying in " + std::to_string(delay) + " seconds");
        const auto version = generation;
        std::weak_ptr<MacState> weak = shared_from_this();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            if (auto self = weak.lock())
                if (self->active && self->generation == version) self->connect();
        });
    }

    bool retryable(NSError* error) {
        if (![error.domain isEqualToString:NSURLErrorDomain]) return false;
        switch (error.code) {
        case NSURLErrorTimedOut: case NSURLErrorCannotFindHost:
        case NSURLErrorCannotConnectToHost: case NSURLErrorNetworkConnectionLost:
        case NSURLErrorDNSLookupFailed: case NSURLErrorNotConnectedToInternet:
            return true;
        default: return false;
        }
    }

    void receive() {
        if (!active || !socket) return;
        std::weak_ptr<MacState> weak = shared_from_this();
        const auto version = generation;
        [socket receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message, NSError* error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                auto self = weak.lock();
                if (!self || !self->active || self->generation != version) return;
                if (error) {
                    self->fail("FAH connection lost", self->retryable(error));
                    return;
                }
                if (message.type != NSURLSessionWebSocketMessageTypeString) {
                    self->fail("FAH sent a non-text message", false);
                    return;
                }
                try {
                    if (!self->opened) {
                        self->opened = true;
                        self->protocol->connected();
                    }
                    const char* text = message.string.UTF8String;
                    if (!text) throw std::runtime_error("FAH sent invalid text");
                    const auto bytes = [message.string lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
                    auto commands = self->protocol->receive(std::string_view(text, bytes));
                    self->retry = 0;
                    for (const auto& command : commands) {
                        NSString* value = [[NSString alloc] initWithBytes:command.data() length:command.size() encoding:NSUTF8StringEncoding];
                        auto outgoing = [[NSURLSessionWebSocketMessage alloc] initWithString:value];
                        [self->socket sendMessage:outgoing completionHandler:^(NSError* sendError) {
                            if (!sendError) return;
                            dispatch_async(dispatch_get_main_queue(), ^{
                                if (auto locked = weak.lock())
                                    if (locked->active && locked->generation == version)
                                        locked->fail("Could not request FAH visualisation", locked->retryable(sendError));
                            });
                        }];
                    }
                    self->receive();
                } catch (const std::exception& exception) {
                    self->fail(exception.what(), false);
                }
            });
        }];
    }

    void connect() {
        if (!active) return;
        opened = false;
        emit(SourceEvent::Kind::Status, "Connecting to FAH");
        auto configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.timeoutIntervalForRequest = 20;
        configuration.timeoutIntervalForResource = 0;
        configuration.HTTPShouldSetCookies = NO;
        configuration.URLCache = nil;
        session = [NSURLSession sessionWithConfiguration:configuration];
        socket = [session webSocketTaskWithURL:url];
        socket.maximumMessageSize = 26'000'000;
        std::weak_ptr<MacState> weak = shared_from_this();
        protocol = std::make_unique<SourceSession>(options, [weak](SourceEvent event) {
            if (auto self = weak.lock()) if (self->active) self->callback(std::move(event));
        });
        [socket resume];
        receive();
    }
};

class MacSource final : public Source {
    std::shared_ptr<MacState> state_;
public:
    MacSource(SourceOptions options, SourceCallback callback) : state_(std::make_shared<MacState>()) {
        if (options.url.size() > 4096 || options.unit.size() > 1024)
            throw std::runtime_error("FAH source options exceed size limits");
        NSString* text = [[NSString alloc] initWithBytes:options.url.data() length:options.url.size() encoding:NSUTF8StringEncoding];
        NSURLComponents* parts = [NSURLComponents componentsWithString:text];
        if (!parts || (!([parts.scheme isEqualToString:@"ws"] || [parts.scheme isEqualToString:@"wss"])) ||
            !parts.host.length || parts.host.length > 253 || parts.user || parts.password || parts.fragment ||
            (parts.port && (parts.port.integerValue < 1 || parts.port.integerValue > 65535)) || !parts.URL || !callback)
            throw std::runtime_error("FAH source must be a valid ws:// or wss:// URL without embedded credentials");
        state_->url = parts.URL;
        state_->options = std::move(options);
        state_->callback = std::move(callback);
    }
    ~MacSource() override { stop(); }
    void start() override {
        auto state = state_;
        auto action = ^{ if (!state->active) { state->active = true; state->retry = 0; state->connect(); } };
        if ([NSThread isMainThread]) action();
        else dispatch_sync(dispatch_get_main_queue(), action);
    }
    void stop() override {
        auto state = state_;
        auto action = ^{ state->active = false; state->releaseConnection(); };
        if ([NSThread isMainThread]) action();
        else dispatch_sync(dispatch_get_main_queue(), action);
    }
};
}

std::unique_ptr<Source> makeSource(SourceOptions options, SourceCallback callback) {
    return std::make_unique<MacSource>(std::move(options), std::move(callback));
}
} // namespace fah
