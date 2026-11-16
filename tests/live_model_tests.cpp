#include "fah/LiveModel.hpp"
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F function) {
    try { function(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Invalid model update was accepted");
}
fah::SourceEvent event(fah::SourceEvent::Kind kind, std::string unit = "a") {
    fah::SourceEvent result; result.kind = kind; result.unit = std::move(unit); return result;
}
fah::SourceEvent topology(std::size_t atoms, std::string unit = "a") {
    auto result = event(fah::SourceEvent::Kind::Topology, std::move(unit));
    auto value = std::make_shared<fah::Topology>();
    value->atoms.resize(atoms, {.17f, {.2f,.2f,.2f}});
    result.topology = std::move(value); return result;
}
fah::SourceEvent frame(std::size_t atoms, float x, std::string unit = "a") {
    auto result = event(fah::SourceEvent::Kind::Frame, std::move(unit));
    result.frame = std::make_shared<const fah::Frame>(atoms, fah::Vec3{x,0,0}); return result;
}
}

int main() {
    try {
        using Kind = fah::SourceEvent::Kind;
        fah::LiveModel model;
        check(!model.available() && !model.usingFallback(), "Empty model is displayed");
        model.receive(event(Kind::Reset), true);
        model.receive(topology(1), true);
        check(!model.available(), "Topology alone created a displayable model");
        rejects([&] { model.receive(frame(2,0), true); });
        rejects([&] { model.receive(frame(1,0,"wrong-unit"), true); });
        const auto first = frame(1,1);
        check(model.receive(first, true), "Complete live frame did not request a cache update");
        check(model.available() && !model.usingFallback(), "Live frame did not replace the empty model");
        check(model.latestFrame() == first.frame, "Cache frame is not the immutable source frame");
        model.receive(frame(1,2), true); model.receive(frame(1,3), true);
        check(model.capture()->frames.size() == 3, "Live loop history was not retained");
        std::weak_ptr<const fah::Capture> history = model.capture();
        model.receive(event(Kind::Reset), true);
        check(history.expired(), "Fallback retained the old loop history");
        check(model.usingFallback() && model.capture()->frames.size() == 1 &&
            model.capture()->frames[0][0].x == 3, "Disconnect did not select the latest single frame");
        const auto fallback = model.capture();
        model.receive(event(Kind::Status), true);
        check(model.capture() == fallback, "Connection status changed the fallback model");
        model.receive(topology(2,"b"), true);
        check(model.capture() == fallback && model.capture()->topology.atoms.size() == 1,
            "New topology replaced the last complete model before its first frame");
        model.receive(event(Kind::Error,"b"), true);
        check(model.capture() == fallback, "Connection error removed the fallback model");
        rejects([&] { model.receive(frame(2,4,"b"), true); });
        auto nextTopology = topology(2,"b");
        auto nextFrame = frame(2,4,"b");
        model.receive(nextTopology, true); model.receive(nextFrame, true);
        check(model.capture()->topology.atoms.size() == 2 && model.capture()->frames.size() == 1 &&
            model.capture()->frames[0][0].x == 4, "First complete new-WU frame did not replace fallback");
        check(model.latestTopology() == nextTopology.topology && model.latestFrame() == nextFrame.frame,
            "Cache topology/frame pair does not match the source event pair");
        model.receive(frame(2,5,"b"), false);
        const auto liveCapture = model.capture();
        model.receive(frame(2,6,"b"), false);
        check(model.capture() == liveCapture && model.capture()->frames.size() == 1 &&
            model.capture()->frames[0][0].x == 6, "Latest mode did not replace only its snapshot");
        model.receive(event(Kind::Error), false);
        check(model.usingFallback() && model.capture()->frames[0][0].x == 6,
            "Latest mode failed to retain its last complete snapshot");
        auto cached = std::make_shared<fah::Capture>();
        cached->topology = *nextTopology.topology; cached->frames = {*nextFrame.frame};
        model.clear(); model.seed(cached);
        check(model.capture() == cached && model.usingFallback(), "Cache seed was copied or not displayed immediately");
        model.receive(event(Kind::Reset), true); model.receive(topology(1,"c"), true);
        check(model.capture() == cached, "Connecting removed the startup cache");
        auto invalid = std::make_shared<fah::Capture>();
        rejects([&] { model.seed(invalid); });
        check(model.capture() == cached, "Invalid cache seed destroyed a valid fallback");
        model.clear();
        check(!model.available() && !model.capture() && !model.latestTopology() && !model.latestFrame(),
            "Source selector reset retained a previous source's model");
        std::cout << "Live model fallback tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
