#include "../Options.hpp"

#include <cstdlib>
#include <iostream>

static void check(bool condition) {
    if (!condition) { std::cerr << "Option test failed\n"; std::exit(1); }
}

template<class F> static void rejects(F function) {
    try { function(); } catch (const std::exception&) { return; }
    std::cerr << "Expected invalid options to fail\n"; std::exit(1);
}

int main() {
    using namespace fah::windows;
    const auto options = parseOptions({"--capture", "protein.json", "--fps", "60",
        "--seconds", "2.5", "--style", "random", "--playback", "loop",
        "--background", "solid", "--colour", "#ff0080"});
    check(options.fps == 60 && options.seconds == 2.5 && options.style == 5);
    check(options.loop && !options.sky && options.colour[0] == 1 && options.colour[1] == 0);
    check(!options.fullscreen);
    check(parseOptions({"--fullscreen", "true", "--seconds", "3600"}).fullscreen);
    check(!parseOptions({"--fullscreen", "false"}).fullscreen);
    check(parseOptions({"--source", "ws://localhost:7396/api/websocket"}).fps == 60);
    Options saved; saved.source = "ws://localhost:7396/api/websocket";
    check(parseOptions({"--capture", "file.json"},saved).source.empty());
    saved.source.clear(); saved.capture = "file.json";
    check(parseOptions({"--source", "ws://localhost:7396/api/websocket"},saved).capture.empty());
    rejects([] { parseOptions({"--fps", "0"}); });
    rejects([] { parseOptions({"--fps", "60extra"}); });
    rejects([] { parseOptions({"--fps", "241"}); });
    rejects([] { parseOptions({"--seconds", "nan"}); });
    rejects([] { parseOptions({"--seconds", "-1"}); });
    rejects([] { parseOptions({"--seconds", "1x"}); });
    rejects([] { parseOptions({"--seconds", "3600.1"}); });
    rejects([] { parseOptions({"--seconds", "1e20"}); });
    rejects([] { parseOptions({"--fullscreen", "1"}); });
    rejects([] { parseOptions({"--fullscreen", "TRUE"}); });
    rejects([] { parseOptions({"--fullscreen"}); });
    rejects([] { parseOptions({"--style", "0"}); });
    rejects([] { parseOptions({"--colour", "#abcd"}); });
    rejects([] { parseOptions({"--playback", "other"}); });
    rejects([] { parseOptions({"--background", "other"}); });
    rejects([] { parseOptions({"--fps"}); });
    rejects([] { parseOptions({"--unknown", "1"}); });
    rejects([] { parseOptions({"--capture", "a", "--source", "b"}); });
    std::cout << "Windows option tests passed\n";
}
