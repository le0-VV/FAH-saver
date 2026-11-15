#include "fah/Source.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

// Run against a synthetic loopback server, never a folding client.
int main(int argc, char** argv) {
    try {
        fah::SourceOptions options;
        unsigned cycles = 10;
        unsigned delayMs = 100;
        bool requireFrame = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--expect-frame") { requireFrame = true; continue; }
            if (++i == argc) throw std::runtime_error("Missing option value");
            const std::string value = argv[i];
            if (argument == "--url") options.url = value;
            else if (argument == "--cycles" || argument == "--delay-ms") {
                std::size_t used = 0;
                const auto number = std::stoul(value, &used);
                if (used != value.size() || number > 10000) throw std::runtime_error("Invalid numeric option");
                if (argument == "--cycles") cycles = static_cast<unsigned>(number);
                else delayMs = static_cast<unsigned>(number);
            } else throw std::runtime_error("Unknown option");
        }
        if (!cycles || cycles > 100 ||
            (options.url.rfind("ws://127.0.0.1:", 0) != 0 && options.url.rfind("ws://localhost:", 0) != 0))
            throw std::runtime_error("Use a synthetic loopback URL and 1..100 cycles");
        double maximumStopMs = 0;
        unsigned totalFrames = 0;
        for (unsigned i = 0; i < cycles; ++i) {
            std::atomic<bool> stopped{false};
            std::atomic<unsigned> late{0}, frames{0};
            auto source = fah::makeSource(options, [&](fah::SourceEvent event) {
                if (stopped) ++late;
                if (event.kind == fah::SourceEvent::Kind::Frame) ++frames;
            });
            source->start();
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            const auto before = std::chrono::steady_clock::now();
            source->stop();
            const double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
            stopped = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (late) throw std::runtime_error("Callback occurred after stop returned");
            if (stopMs > 1000) throw std::runtime_error("Cancellation exceeded the one-second test budget");
            if (requireFrame && !frames) throw std::runtime_error("Synthetic server frame was not received");
            maximumStopMs = std::max(maximumStopMs, stopMs);
            totalFrames += frames.load();
        }
        std::cout << "{\"cycles\":" << cycles << ",\"maximum_stop_ms\":" << maximumStopMs
                  << ",\"frames\":" << totalFrames << ",\"late_callbacks\":0}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "source cancellation check: " << error.what() << '\n';
        return 1;
    }
}
