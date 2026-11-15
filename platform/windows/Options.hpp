#pragma once

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fah::windows {

struct Options {
    std::string capture;
    std::string source;
    std::string unit;
    std::string metrics;
    unsigned fps = 60;
    double seconds = 0;
    int style = 1; // 4 cycles styles; 5 selects them at random.
    bool loop = false;
    bool sky = true;
    bool fullscreen = false;
    float colour[3] = {.02f, .025f, .035f};
};

inline unsigned number(std::string_view value, unsigned minimum, unsigned maximum) {
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result < minimum || result > maximum)
        throw std::runtime_error("Invalid integer: " + std::string(value));
    return result;
}

inline void colour(Options& options, std::string_view text) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    unsigned rgb = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), rgb, 16);
    if (text.size() != 6 || parsed.ec != std::errc{} || parsed.ptr != text.data() + 6)
        throw std::runtime_error("Colour must contain six hexadecimal digits");
    options.colour[0] = float((rgb >> 16) & 255) / 255;
    options.colour[1] = float((rgb >> 8) & 255) / 255;
    options.colour[2] = float(rgb & 255) / 255;
}

inline Options parseOptions(const std::vector<std::string>& arguments, Options options = {}) {
    bool selectedCapture = false, selectedSource = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& key = arguments[i];
        if (++i == arguments.size()) throw std::runtime_error("Missing value for " + key);
        const auto& value = arguments[i];
        if (key == "--capture") {
            options.capture = value; selectedCapture = true;
            if (!selectedSource) options.source.clear();
        } else if (key == "--source") {
            options.source = value; selectedSource = true;
            if (!selectedCapture) options.capture.clear();
        }
        else if (key == "--unit") options.unit = value;
        else if (key == "--metrics") options.metrics = value;
        else if (key == "--fps") options.fps = number(value, 1, 240);
        else if (key == "--seconds") {
            std::size_t end = 0;
            options.seconds = std::stod(value, &end);
            if (end != value.size() || !std::isfinite(options.seconds) || options.seconds <= 0 || options.seconds > 3600)
                throw std::runtime_error("Duration must be a positive finite number no greater than 3600 seconds");
        } else if (key == "--style") {
            if (value == "cycle") options.style = 4;
            else if (value == "random") options.style = 5;
            else options.style = int(number(value, 1, 3));
        } else if (key == "--playback") {
            if (value != "latest" && value != "loop")
                throw std::runtime_error("Playback must be latest or loop");
            options.loop = value == "loop";
        } else if (key == "--background") {
            if (value != "sky" && value != "solid")
                throw std::runtime_error("Background must be sky or solid");
            options.sky = value == "sky";
        } else if (key == "--colour") colour(options, value);
        else if (key == "--fullscreen") {
            if (value != "true" && value != "false")
                throw std::runtime_error("Fullscreen must be true or false");
            options.fullscreen = value == "true";
        }
        else throw std::runtime_error("Unknown option: " + key);
    }
    if ((selectedCapture && selectedSource) || (!options.source.empty() && !options.capture.empty()))
        throw std::runtime_error("Select either a live source or a capture file");
    return options;
}

} // namespace fah::windows
