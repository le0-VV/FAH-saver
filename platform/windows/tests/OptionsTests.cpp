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
    check(options.snapshotInterval == .5 && !options.darkSky);
    const auto dark = parseOptions({"--background", "dark", "--snapshot-interval", "0.75"});
    check(dark.sky && dark.darkSky && dark.snapshotInterval == .75);
    const auto light = parseOptions({"--background", "sky"},dark);
    check(light.sky && !light.darkSky && light.snapshotInterval == .75);
    const auto solid = parseOptions({"--background", "solid"},dark);
    check(!solid.sky && !solid.darkSky);
    auto loop = parseOptions({"--playback", "loop"});
    check(snapshotIndex(0,3,loop) == 0 && snapshotIndex(.499,3,loop) == 0);
    check(snapshotIndex(.5,3,loop) == 1 && snapshotIndex(1.499,3,loop) == 2);
    check(snapshotIndex(1.5,3,loop) == 0);
    loop.fps = 240;
    check(snapshotIndex(.5,3,loop) == 1);
    loop.loop = false;
    check(snapshotIndex(0,3,loop) == 2 && snapshotIndex(1.5,3,loop) == 2);
    const auto fast = parseOptions({"--playback", "loop", "--snapshot-interval", "0.05"});
    const auto slow = parseOptions({"--playback", "loop", "--snapshot-interval", "60"});
    check(snapshotIndex(.049,3,fast) == 0 && snapshotIndex(.05,3,fast) == 1);
    check(snapshotIndex(59.999,3,slow) == 0 && snapshotIndex(60,3,slow) == 1);
    rejects([&] { snapshotIndex(0,0,loop); });
    check(!options.fullscreen);
    check(parseOptions({"--fullscreen", "true", "--seconds", "3600"}).fullscreen);
    check(!parseOptions({"--fullscreen", "false"}).fullscreen);
    const auto size = parseOptions({"--width","1920","--height","1080"});
    check(size.width == 1920 && size.height == 1080);
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
    rejects([] { parseOptions({"--snapshot-interval", "0"}); });
    rejects([] { parseOptions({"--snapshot-interval", "0.0499"}); });
    rejects([] { parseOptions({"--snapshot-interval", "60.001"}); });
    rejects([] { parseOptions({"--snapshot-interval", "nan"}); });
    rejects([] { parseOptions({"--snapshot-interval", "inf"}); });
    rejects([] { parseOptions({"--snapshot-interval", "0.5extra"}); });
    rejects([] { parseOptions({"--snapshot-interval"}); });
    rejects([] { parseOptions({"--fullscreen", "1"}); });
    rejects([] { parseOptions({"--fullscreen", "TRUE"}); });
    rejects([] { parseOptions({"--fullscreen"}); });
    rejects([] { parseOptions({"--width","63"}); });
    rejects([] { parseOptions({"--height","4097"}); });
    rejects([] { parseOptions({"--width","1280.5"}); });
    rejects([] { parseOptions({"--height","720extra"}); });
    rejects([] { parseOptions({"--style", "0"}); });
    rejects([] { parseOptions({"--colour", "#abcd"}); });
    rejects([] { parseOptions({"--playback", "other"}); });
    rejects([] { parseOptions({"--background", "other"}); });
    rejects([] { parseOptions({"--fps"}); });
    rejects([] { parseOptions({"--unknown", "1"}); });
    rejects([] { parseOptions({"--capture", "a", "--source", "b"}); });
    std::cout << "Windows option tests passed\n";
}
