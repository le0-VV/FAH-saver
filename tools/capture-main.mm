#import <Foundation/Foundation.h>
#include "fah/Source.hpp"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void saveNewFile(const std::string& path, const std::string& data) {
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) throw std::runtime_error("Cannot create capture file: " + std::string(std::strerror(errno)));
    std::size_t written = 0;
    while (written < data.size()) {
        const auto count = write(file, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(file); unlink(path.c_str()); throw std::runtime_error("Could not write capture file"); }
        written += static_cast<std::size_t>(count);
    }
    if (close(file)) { unlink(path.c_str()); throw std::runtime_error("Could not close capture file"); }
}
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            fah::SourceOptions options;
            std::string output;
            double seconds = 15;
            std::size_t maximumFrames = 8;
            for (int i = 1; i < argc; ++i) {
                const std::string argument = argv[i];
                if (argument == "--help") {
                    std::cout << "fah-capture --output NEW_FILE [--url ws://host:7396/api/websocket] [--unit ID] [--seconds 15] [--frames 8]\n";
                    return 0;
                }
                if (++i == argc) throw std::runtime_error("Missing option value");
                const std::string value = argv[i];
                if (argument == "--output") output = value;
                else if (argument == "--url") options.url = value;
                else if (argument == "--unit") options.unit = value;
                else if (argument == "--seconds" || argument == "--frames") {
                    std::size_t used = 0;
                    const double number = std::stod(value, &used);
                    if (used != value.size() || !std::isfinite(number)) throw std::runtime_error("Invalid numeric option");
                    if (argument == "--seconds") {
                        if (number < 1 || number > 3600) throw std::runtime_error("Seconds must be between 1 and 3600");
                        seconds = number;
                    } else {
                        if (number < 1 || number > 10000 || std::floor(number) != number)
                            throw std::runtime_error("Frames must be an integer between 1 and 10000");
                        maximumFrames = static_cast<std::size_t>(number);
                    }
                } else throw std::runtime_error("Unknown option: " + argument);
            }
            if (output.empty()) throw std::runtime_error("--output is required; existing files are never overwritten");
            bool done = false;
            std::string failure;
            auto capture = std::make_shared<fah::Capture>();
            std::unique_ptr<fah::Source> source = fah::makeSource(options, [capture, maximumFrames, &done, &failure](fah::SourceEvent event) {
                switch (event.kind) {
                case fah::SourceEvent::Kind::Reset: *capture = {}; break;
                case fah::SourceEvent::Kind::Topology: capture->topology = *event.topology; break;
                case fah::SourceEvent::Kind::Frame:
                    if (capture->frames.size() < maximumFrames) capture->frames.push_back(*event.frame);
                    if (capture->frames.size() == maximumFrames) done = true;
                    break;
                case fah::SourceEvent::Kind::Status: std::cerr << event.text << '\n'; break;
                case fah::SourceEvent::Kind::Error: failure = event.text; done = true; break;
                }
            });
            source->start();
            NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
            while (!done && deadline.timeIntervalSinceNow > 0)
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            source->stop();
            if (!failure.empty()) throw std::runtime_error(failure);
            if (capture->frames.empty()) throw std::runtime_error("No visualisation frames received before timeout");
            saveNewFile(output, fah::writeCapture(*capture));
            std::cout << "Captured " << capture->topology.atoms.size() << " atoms, " << capture->topology.bonds.size()
                      << " bonds and " << capture->frames.size() << " frames\n";
            return 0;
        } catch (const std::exception& exception) {
            std::cerr << "fah-capture: " << exception.what() << '\n';
            return 1;
        }
    }
}
