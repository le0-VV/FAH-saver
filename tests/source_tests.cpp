#include "fah/Source.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <limits>
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

Json countedUnit(const char* id, Json count, const char* state = "RUN") {
    Json value = unit(id, state); value["frames"] = std::move(count); return value;
}
std::vector<std::size_t> indices(const std::vector<fah::SourceEvent>& events) {
    std::vector<std::size_t> result;
    for (const auto& event : events) if (event.kind == Kind::Frame) result.push_back(event.index);
    return result;
}
void coordinates(fah::SourceSession& session, const char* id, std::size_t index, Json positions = frame) {
    session.receive(Json{"viz", id, "frames", index, std::move(positions)}.dump());
}

void latestOnlyChecks() {
    fah::SourceOptions options; options.latestOnly = true;
    std::vector<fah::SourceEvent> events;
    fah::SourceSession latest(options, [&](fah::SourceEvent event) { events.push_back(std::move(event)); });
    latest.connected();
    auto commands = latest.receive(Json{{"units", {countedUnit("running", 3)}}}.dump());
    check(commands.size() == 1 && Json::parse(commands[0]).at("frame") == 0,
          "Latest startup still needs topology and a validated sequential stream");
    latest.receive(Json{"viz", "running", "topology", topology}.dump());
    coordinates(latest,"running",0); coordinates(latest,"running",1);
    check(indices(events).empty(),"Latest startup displayed historical snapshots");
    coordinates(latest,"running",2);
    check(indices(events) == std::vector<std::size_t>{2},"Latest startup did not deliver the published tail");
    latest.receive(Json{"units",0,"frames",4}.dump());
    coordinates(latest,"running",3);
    check(indices(events) == std::vector<std::size_t>({2,3}),"Latest source lost a subsequent live snapshot");
    latest.receive(Json{"units",0,"frames",6}.dump());
    coordinates(latest,"running",4);
    check(indices(events) == std::vector<std::size_t>({2,3}),"Latest source displayed an already superseded live snapshot");
    coordinates(latest,"running",5);
    check(indices(events) == std::vector<std::size_t>({2,3,5}),"Latest source did not follow the next published tail");

    events.clear();
    latest.connected();
    latest.receive(Json{{"units", {countedUnit("catch-up", 2)}}}.dump());
    latest.receive(Json{"viz", "catch-up", "topology", topology}.dump());
    coordinates(latest,"catch-up",0);
    latest.receive(Json{"units",0,countedUnit("catch-up",4)}.dump());
    coordinates(latest,"catch-up",1); coordinates(latest,"catch-up",2);
    latest.receive(Json{"units",{countedUnit("catch-up",5)}}.dump());
    coordinates(latest,"catch-up",3);
    check(indices(events).empty(),"Catch-up ignored an advancing published watermark");
    coordinates(latest,"catch-up",4);
    check(indices(events) == std::vector<std::size_t>{4},"Catch-up did not reach the updated tail");
    latest.receive(Json{"viz","catch-up","topology",topology}.dump());
    for (std::size_t index = 0; index < 4; ++index) coordinates(latest,"catch-up",index);
    check(indices(events).size() == 1,"Topology reset discarded the unit's published watermark");
    coordinates(latest,"catch-up",4);
    check(indices(events) == std::vector<std::size_t>({4,4}),"Topology reset did not validate a new tail");

    events.clear(); latest.connected();
    latest.receive(Json{{"units",{countedUnit("new",0)}}}.dump());
    latest.receive(Json{"viz","new","topology",topology}.dump());
    check(indices(events).empty(),"An unpublished WU displayed a frame");
    latest.receive(Json{"units",0,"frames",1}.dump());
    coordinates(latest,"new",0);
    check(indices(events) == std::vector<std::size_t>{0},"First published frame was not delivered");
    latest.receive(Json{"units",-1,countedUnit("next",2)}.dump());
    check(latest.receive(Json{"units",0,nullptr}.dump()).size() == 1,"Latest WU switch did not subscribe");
    coordinates(latest,"new",999,nullptr);
    latest.receive(Json{"viz","next","topology",topology}.dump());
    coordinates(latest,"next",0);
    check(indices(events).size() == 1,"WU switch reused the previous unit's watermark");
    coordinates(latest,"next",1);
    check(indices(events) == std::vector<std::size_t>({0,1}),"WU switch did not deliver its own tail");

    // The watermark is mandatory in latest mode; never silently fall back to replay.
    for (const Json& value : {unit("missing"), countedUnit("missing",0)}) {
        fah::SourceSession missing(options,[](fah::SourceEvent) {});
        missing.connected(); missing.receive(Json{{"units",{value}}}.dump());
        missing.receive(Json{"viz","missing","topology",topology}.dump());
        rejects([&] { coordinates(missing,"missing",0); });
    }
    latest.connected();
    latest.receive(Json{{"units",{unit("reconnected")}}}.dump());
    latest.receive(Json{"viz","reconnected","topology",topology}.dump());
    rejects([&] { coordinates(latest,"reconnected",0); });

    for (const Json& count : {Json(nullptr),Json(true),Json("3"),Json(-1),Json(1.5),Json(10001),
                             Json(std::numeric_limits<std::uint64_t>::max()),
                             Json(std::numeric_limits<double>::infinity()),Json::array(),Json::object()}) {
        fah::SourceSession bad(options,[](fah::SourceEvent) {});
        bad.connected();
        rejects([&] { bad.receive(Json{{"units",{countedUnit("bad",count)}}}.dump()); });
        fah::SourceSession update(options,[](fah::SourceEvent) {});
        update.connected(); update.receive(Json{{"units",{countedUnit("bad",1)}}}.dump());
        rejects([&] { update.receive(Json{"units",0,"frames",count}.dump()); });
    }
    fah::SourceSession malformed(options,[](fah::SourceEvent) {});
    malformed.connected(); malformed.receive(Json{{"units",{countedUnit("bad",3)}}}.dump());
    malformed.receive(Json{"viz","bad","topology",topology}.dump());
    rejects([&] { coordinates(malformed,"bad",0,Json{{0,0,0}}); });
    rejects([&] { coordinates(malformed,"bad",1); });

    fah::SourceSession renamed(options,[](fah::SourceEvent) {});
    renamed.connected(); renamed.receive(Json{{"units",{countedUnit("old-name",5)}}}.dump());
    renamed.receive(Json{"units",0,"id","new-name"}.dump());
    renamed.receive(Json{"viz","new-name","topology",topology}.dump());
    rejects([&] { coordinates(renamed,"new-name",0); });

    std::vector<std::size_t> history;
    fah::SourceSession all({},[&](fah::SourceEvent event) { if (event.kind == Kind::Frame) history.push_back(event.index); });
    all.connected(); all.receive(Json{{"units",{countedUnit("history",nullptr)}}}.dump());
    all.receive(Json{"viz","history","topology",topology}.dump());
    for (std::size_t index = 0; index < 3; ++index) coordinates(all,"history",index);
    check(history == std::vector<std::size_t>({0,1,2}),"Default/history mode changed its full replay contract");
}
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
        latestOnlyChecks();
        std::cout << "Source session checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "source_tests: " << error.what() << '\n';
        return 1;
    }
}
