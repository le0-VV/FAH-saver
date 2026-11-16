#include "../Options.hpp"

#include <cstdlib>
#include <iostream>

void check(bool condition) { if (!condition) std::exit(1); }
template<class F> void rejects(F function) {
    try { function(); } catch (const std::exception&) { return; }
    std::cerr << "Expected invalid WebView2 options to fail\n"; std::exit(1);
}
int main() {
    using namespace fah::web_benchmark;
    const auto value = parse({"--capture","protein.json","--html","index.html","--width","1920",
        "--height","1080","--fps","60","--seconds","2.5","--warmup","0","--style","cycle",
        "--playback","loop","--background","solid","--colour","123abc"});
    check(value.width == 1920 && value.height == 1080 && value.fps == 60 && value.seconds == 2.5);
    check(value.style == "cycle" && value.playback == "loop" && value.colour == "123abc");
    rejects([] { parse({}); });
    rejects([] { parse({"--capture","p","--html","h","--fps","nan"}); });
    rejects([] { parse({"--capture","p","--html","h","--fps","0"}); });
    rejects([] { parse({"--capture","p","--html","h","--fps","60.5"}); });
    rejects([] { parse({"--capture","p","--html","h","--width","8193"}); });
    rejects([] { parse({"--capture","p","--html","h","--seconds","3601"}); });
    rejects([] { parse({"--capture","p","--html","h","--warmup","-1"}); });
    rejects([] { parse({"--capture","p","--html","h","--style","other"}); });
    rejects([] { parse({"--capture","p","--html","h","--playback","other"}); });
    rejects([] { parse({"--capture","p","--html","h","--background","other"}); });
    rejects([] { parse({"--capture","p","--html","h","--colour","#123456"}); });
    rejects([] { parse({"--capture","p","--html","h","--colour","12345g"}); });
    rejects([] { parse({"--capture","p","--html","h","--unknown","1"}); });
    rejects([] { parse({"--capture","p","--html"}); });
    std::cout << "WebView2 benchmark option tests passed\n";
}
