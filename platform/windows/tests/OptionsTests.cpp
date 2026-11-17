#include "../Options.hpp"
#include "../DecimalEdit.hpp"

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
    check(options.snapshotInterval == 1 && !options.darkSky);
    const auto dark = parseOptions({"--background", "dark", "--snapshot-interval", "0.75"});
    check(dark.sky && dark.darkSky && dark.snapshotInterval == .75);
    const auto light = parseOptions({"--background", "sky"},dark);
    check(light.sky && !light.darkSky && light.snapshotInterval == .75);
    const auto solid = parseOptions({"--background", "solid"},dark);
    check(!solid.sky && !solid.darkSky);
    auto loop = parseOptions({"--playback", "loop"});
    check(snapshotIndex(0,3,loop) == 0 && snapshotIndex(.999,3,loop) == 0);
    check(snapshotIndex(1,3,loop) == 1 && snapshotIndex(2.999,3,loop) == 2);
    check(snapshotIndex(3,3,loop) == 0);
    loop.fps = 240;
    check(snapshotIndex(1,3,loop) == 1);
    loop.loop = false;
    check(snapshotIndex(0,3,loop) == 2 && snapshotIndex(3,3,loop) == 2);
    const auto fast = parseOptions({"--playback", "loop", "--snapshot-interval", "0.05"});
    const auto slow = parseOptions({"--playback", "loop", "--snapshot-interval", "60"});
    check(snapshotIndex(.049,3,fast) == 0 && snapshotIndex(.05,3,fast) == 1);
    check(snapshotIndex(59.999,3,slow) == 0 && snapshotIndex(60,3,slow) == 1);
    for (const auto* interval : {"60.001", "3600", "1e100"})
        check(parseOptions({"--snapshot-interval",interval}).snapshotInterval == std::stod(interval));
    const auto explicitInterval = parseOptions({"--snapshot-interval","0.5"});
    check(parseOptions({},explicitInterval).snapshotInterval == .5 &&
        liveScreensaverOptions(explicitInterval).snapshotInterval == .5);
    check(decimalReplacement(L"1.2",1,2,L".") && decimalReplacement(L"1.2",0,3,L".5"));
    check(decimalReplacement(L"1.2",0,3,L"") && decimalReplacement(L"",0,0,L"."));
    check(decimalReplacement(L"1.",2,2,L"5") && decimalReplacement(L"1.2",1,3,L"0"));
    check(!decimalReplacement(L"1.2",3,3,L".") && !decimalReplacement(L"1.2",0,3,L"abc3.4"));
    check(!decimalReplacement(L"1.2",0,3,L"3.4\n") && !decimalReplacement(L"",0,0,L"1e100"));
    check(!decimalReplacement(L"1",0,1,L"\uff11") && !decimalReplacement(L"1",2,2,L"2"));
    const auto largeText = fah::decimalText(1e100);
    check(decimalReplacement(L"",0,0,std::wstring(largeText.begin(),largeText.end())));
    rejects([&] { snapshotIndex(0,0,loop); });
    check(!options.fullscreen);
    check(parseOptions({"--fullscreen", "true", "--seconds", "3600"}).fullscreen);
    check(!parseOptions({"--fullscreen", "false"}).fullscreen);
    const auto size = parseOptions({"--width","1920","--height","1080"});
    check(size.width == 1920 && size.height == 1080);
    check(parseOptions({"--source", "ws://localhost:7396/api/websocket"}).fps == 24);
    Options saved; saved.source = "ws://localhost:7396/api/websocket";
    check(parseOptions({"--capture", "file.json"},saved).source.empty());
    saved.source.clear(); saved.capture = "file.json";
    check(parseOptions({"--source", "ws://localhost:7396/api/websocket"},saved).capture.empty());
    Options legacy; legacy.capture = "missing-legacy-capture.json"; legacy.fps = 30;
    const auto migrated = liveScreensaverOptions(legacy);
    check(migrated.capture.empty() && migrated.source == "ws://127.0.0.1:7396/api/websocket" && migrated.fps == 30);
    legacy.source = "ws://configured-host:7396/api/websocket";
    check(liveScreensaverOptions(legacy).source == legacy.source);
    const auto developerCapture = parseOptions({"--capture", "fixture.json"},migrated);
    check(developerCapture.source.empty() && developerCapture.capture == "fixture.json");
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
    rejects([] { parseOptions({"--snapshot-interval", "-1"}); });
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
