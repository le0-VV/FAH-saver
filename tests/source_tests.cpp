#include "fah/Source.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
using Kind = fah::SourceEvent::Kind;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "Expected malformed input to fail");
}

Json unit(const char* id, const char* state = "RUN") { return Json{{"id", id}, {"state", state}}; }
const Json topology{{"atoms", {{"C", "SYN", 0, 0, 6}, {"O", "SYN", 0, 0, 8}}}, {"bonds", {{0, 1}}}};
const Json frame{{0, 0, 0}, {1, 0, 0}};
}

int main() {
    try {
        std::vector<fah::SourceEvent> events;
        fah::SourceSession session({}, [&](fah::SourceEvent event) { events.push_back(std::move(event)); });
        session.connected();
        check(events.front().kind == Kind::Reset, "Connection must reset the model");
        auto commands = session.receive(Json{{"units", {unit("waiting", "WAIT"), unit("running")}},
                                             {"config", {{"user", "PRIVATE_DONOR"}}}}.dump());
        check(commands.size() == 1, "Initial running WU needs one subscription");
        const Json command = Json::parse(commands.front());
        check(command == Json{{"cmd", "viz"}, {"unit", "running"}, {"frame", 0}}, "Only a viz command is allowed");
        session.receive(Json{"viz", "running", "topology", topology}.dump());
        session.receive(Json{"viz", "running", "frames", 0, frame}.dump());
        check(events.back().kind == Kind::Frame && events.back().index == 0 && events.back().frame->size() == 2,
              "Frame payload must be delivered without client state");
        for (const auto& event : events) check(event.text.find("PRIVATE_DONOR") == std::string::npos, "Donor state leaked");
        check(session.receive("\"ping\"").empty(), "Keepalive is not a state update");
        check(session.receive(Json{"units", 1, "frames", 1}.dump()).empty(), "Progress must not resubscribe");
        rejects([&] { session.receive(Json{"viz", "running", "frames", 2, frame}.dump()); });
        rejects([&] { session.receive(Json{"viz", "running", "frames", 1, Json{{0, 0, 0}}}.dump()); });
        rejects([&] { session.receive(Json{"viz", "running", "frames", 1.5, frame}.dump()); });
        rejects([&] { session.receive(Json{"units", -3, nullptr}.dump()); });
        rejects([&] { session.receive("[bad JSON]"); });
        rejects([&] { session.receive(std::string(26'000'001, ' ')); });
        rejects([&] { session.receive(std::string(70, '[') + "0" + std::string(70, ']')); });

        // Removal preserves server indices and causes selection of the next RUN unit.
        check(session.receive(Json{"units", -1, unit("next")}.dump()).empty(), "A running selected WU stays selected");
        commands = session.receive(Json{"units", 1, nullptr}.dump());
        check(commands.size() == 1 && Json::parse(commands[0]).at("unit") == "next", "WU removal must follow the next WU");
        check(session.receive(Json{"viz", "running", "frames", 10, frame}.dump()).empty(), "Old-WU frames must not change the new model");
        rejects([&] { session.receive(Json{"viz", "next", "frames", 0, frame}.dump()); });
        session.receive(Json{"viz", "next", "topology", Json{{"atoms", Json::array()}}}.dump());
        check(events.back().kind == Kind::Status && events.back().text.find("No visualisation") != std::string::npos,
              "Empty topology needs an explicit no-data state");
        session.receive(Json{"viz", "next", "topology", topology}.dump());
        session.receive(Json{"viz", "next", "frames", 0, frame}.dump());

        commands = session.receive(Json{"units", Json::array()}.dump());
        check(commands.empty() && events.back().kind == Kind::Status, "No WUs must clear and report status");
        commands = session.receive(Json{"units", -2, {unit("bulk-a"), unit("bulk-b")}}.dump());
        check(commands.size() == 1 && Json::parse(commands[0]).at("unit") == "bulk-a", "Bulk append must select its first RUN unit");
        commands = session.receive(Json{"units", 0, "state", "UPLOAD"}.dump());
        check(commands.size() == 1 && Json::parse(commands[0]).at("unit") == "bulk-b", "WU state change must follow a running WU");
        session.connected();
        rejects([&] { session.receive(Json{"viz", "bulk-b", "topology", topology}.dump()); });
        check(session.receive(Json{{"units", {unit("fresh")}}}.dump()).size() == 1, "Reconnect must subscribe from frame zero");

        fah::SourceOptions fixed;
        fixed.unit = "chosen";
        fah::SourceSession manual(fixed, [](fah::SourceEvent) {});
        manual.connected();
        check(manual.receive(Json{{"units", {unit("other"), unit("chosen", "PAUSE")}}}.dump()).size() == 1,
              "Explicit WU must be honoured, including paused snapshots");
        check(manual.receive(Json{"units", 1, nullptr}.dump()).empty(), "Explicit WU must not silently switch");
        std::cout << "Source session checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "source_tests: " << error.what() << '\n';
        return 1;
    }
}
