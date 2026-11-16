#pragma once

#include "fah/Model.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fah {

struct SourceOptions {
    std::string url = "ws://127.0.0.1:7396/api/websocket";
    std::string unit;
    // Deliver only coordinates at or beyond the selected WU's published tail.
    bool latestOnly = false;
};

struct SourceEvent {
    // Reset invalidates live topology/history. A screensaver may retain its last
    // complete cached model until valid live data replaces it.
    enum class Kind { Reset, Topology, Frame, Status, Error };
    Kind kind = Kind::Status;
    std::string unit;
    std::string text;
    std::shared_ptr<const fah::Topology> topology;
    std::shared_ptr<const fah::Frame> frame;
    std::size_t index = 0;
};

using SourceCallback = std::function<void(SourceEvent)>;

// Process a connection's messages on one serial thread. Malformed data throws.
// Retains only WU identifiers, states and topology, never donor/account state.
class SourceSession {
public:
    SourceSession(SourceOptions options, SourceCallback callback);
    ~SourceSession();
    SourceSession(const SourceSession&) = delete;
    SourceSession& operator=(const SourceSession&) = delete;
    void connected();
    std::vector<std::string> receive(std::string_view message);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Source {
public:
    virtual ~Source() = default;
    virtual void start() = 0;
    // No callback occurs after stop returns. Do not call stop from a callback.
    virtual void stop() = 0;
};

// macOS delivers callbacks on the main queue. Windows uses its receiver thread;
// a window host must marshal events to its UI thread before changing a model.
std::unique_ptr<Source> makeSource(SourceOptions options, SourceCallback callback);

} // namespace fah
