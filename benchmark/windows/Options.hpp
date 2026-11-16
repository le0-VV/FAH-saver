#pragma once

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace fah::web_benchmark {
struct Options {
    std::string capture,html,output;
    std::string style = "1",playback = "latest",background = "sky",colour = "050609";
    unsigned width = 1280,height = 720,fps = 60;
    double seconds = 10,warmup = 0;
};
inline double numeric(const std::string& text,double minimum,double maximum,bool integral) {
    std::size_t end = 0;
    const double result = std::stod(text,&end);
    if (end != text.size() || !std::isfinite(result) || result < minimum || result > maximum ||
        (integral && std::floor(result) != result))
        throw std::runtime_error("Invalid benchmark numeric value: "+text);
    return result;
}
inline Options parse(const std::vector<std::string>& arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& key = arguments[i];
        if (++i == arguments.size()) throw std::runtime_error("Missing value for "+key);
        const auto& value = arguments[i];
        if (key == "--capture") options.capture = value;
        else if (key == "--html") options.html = value;
        else if (key == "--output") options.output = value;
        else if (key == "--width") options.width = unsigned(numeric(value,1,8192,true));
        else if (key == "--height") options.height = unsigned(numeric(value,1,8192,true));
        else if (key == "--fps") options.fps = unsigned(numeric(value,1,240,true));
        else if (key == "--seconds") options.seconds = numeric(value,1,3600,false);
        else if (key == "--warmup") options.warmup = numeric(value,0,120,false);
        else if (key == "--style") {
            if (value != "1" && value != "2" && value != "3" && value != "cycle" && value != "random")
                throw std::runtime_error("Style must be 1, 2, 3, cycle or random");
            options.style = value;
        } else if (key == "--playback") {
            if (value != "latest" && value != "loop") throw std::runtime_error("Playback must be latest or loop");
            options.playback = value;
        } else if (key == "--background") {
            if (value != "sky" && value != "solid") throw std::runtime_error("Background must be sky or solid");
            options.background = value;
        } else if (key == "--colour") {
            unsigned rgb = 0;
            const auto result = std::from_chars(value.data(),value.data()+value.size(),rgb,16);
            if (value.size() != 6 || result.ec != std::errc{} || result.ptr != value.data()+6)
                throw std::runtime_error("Colour must contain six hexadecimal digits");
            options.colour = value;
        } else throw std::runtime_error("Unknown option: "+key);
    }
    if (options.capture.empty() || options.html.empty()) throw std::runtime_error("--capture and --html are required");
    return options;
}
} // namespace fah::web_benchmark
