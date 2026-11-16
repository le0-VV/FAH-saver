#include "fah/Source.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace fah {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maxMessageBytes = 26'000'000;
constexpr std::size_t maxFrameBytes = 128 * 1024 * 1024;
constexpr std::size_t maxUnits = 1024;
constexpr std::size_t maxFrames = 10000;

[[noreturn]] void invalid(const char* detail) {
    throw std::runtime_error(std::string("Invalid FAH message: ") + detail);
}

std::string boundedString(const Json& value, std::size_t maximum) {
    if (!value.is_string() || value.get_ref<const std::string&>().size() > maximum)
        invalid("invalid text field");
    return value.get<std::string>();
}

std::int64_t integer(const Json& value) {
    if (!value.is_number_integer()) invalid("expected integer index");
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > INT64_MAX)
        invalid("index too large");
    return value.get<std::int64_t>();
}

std::size_t publishedFrames(const Json& value) {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
        value.get<std::uint64_t>() > maxFrames)
        invalid("invalid published visualisation frame count");
    return static_cast<std::size_t>(value.get<std::uint64_t>());
}

struct Unit { std::string id; std::string state; std::optional<std::size_t> frames; };
Unit readUnit(const Json& value, bool latestOnly) {
    if (!value.is_object()) invalid("work unit is not an object");
    Unit unit;
    if (value.contains("id")) unit.id = boundedString(value.at("id"), 1024);
    if (value.contains("state")) unit.state = boundedString(value.at("state"), 64);
    if (latestOnly && value.contains("frames")) unit.frames = publishedFrames(value.at("frames"));
    return unit;
}
}

struct SourceSession::Impl {
    SourceOptions options;
    SourceCallback callback;
    std::vector<Unit> units;
    std::string selected;
    std::shared_ptr<const Topology> topology;
    std::size_t nextFrame = 0;
    std::size_t decodedBytes = 0;
    bool initial = true;

    void emit(SourceEvent::Kind kind, std::string text = {}) {
        SourceEvent event;
        event.kind = kind;
        event.unit = selected;
        event.text = std::move(text);
        callback(std::move(event));
    }

    std::vector<std::string> select() {
        std::string candidate;
        if (!options.unit.empty()) {
            for (const auto& unit : units)
                if (unit.id == options.unit) { candidate = unit.id; break; }
        } else {
            for (const auto& unit : units)
                if (unit.id == selected && unit.state == "RUN") candidate = unit.id;
            if (candidate.empty())
                for (const auto& unit : units)
                    if (!unit.id.empty() && unit.state == "RUN") { candidate = unit.id; break; }
            if (candidate.empty())
                for (const auto& unit : units)
                    if (unit.id == selected) { candidate = unit.id; break; }
        }
        if (candidate == selected) return {};
        selected = candidate;
        topology.reset();
        nextFrame = decodedBytes = 0;
        emit(SourceEvent::Kind::Reset);
        if (selected.empty()) {
            emit(SourceEvent::Kind::Status, options.unit.empty() ?
                 "No running work unit" : "Selected work unit is unavailable");
            return {};
        }
        emit(SourceEvent::Kind::Status, "Connected; waiting for visualisation data");
        return {Json{{"cmd", "viz"}, {"unit", selected}, {"frame", 0}}.dump()};
    }

    void replaceUnits(const Json& value) {
        if (!value.is_array() || value.size() > maxUnits) invalid("invalid work-unit list");
        std::vector<Unit> replacement;
        replacement.reserve(value.size());
        for (const auto& unit : value) replacement.push_back(readUnit(unit, options.latestOnly));
        units = std::move(replacement);
    }

    void updateUnits(const Json& value) {
        if (value.size() == 2) { replaceUnits(value.at(1)); return; }
        if (value.size() < 3) invalid("incomplete work-unit update");
        auto index = integer(value.at(1));
        if (value.size() == 3) {
            const auto& item = value.at(2);
            if (index == -1) {
                if (units.size() == maxUnits) invalid("too many work units");
                units.push_back(readUnit(item, options.latestOnly));
            } else if (index == -2) {
                if (!item.is_array() || item.size() > maxUnits - units.size())
                    invalid("invalid work-unit append");
                for (const auto& unit : item) units.push_back(readUnit(unit, options.latestOnly));
            } else {
                if (index < 0 || static_cast<std::size_t>(index) >= units.size())
                    invalid("work-unit index outside list");
                if (item.is_null()) units.erase(units.begin() + index);
                else units.at(index) = readUnit(item, options.latestOnly);
            }
        } else if (value.size() == 4) {
            if (index < 0 || static_cast<std::size_t>(index) >= units.size())
                invalid("work-unit index outside list");
            auto field = boundedString(value.at(2), 128);
            if (field == "id") {
                const auto id = value.at(3).is_null() ? "" : boundedString(value.at(3), 1024);
                if (id != units.at(index).id) units.at(index).frames.reset();
                units.at(index).id = id;
            }
            if (field == "state") units.at(index).state = value.at(3).is_null() ? "" : boundedString(value.at(3), 64);
            if (options.latestOnly && field == "frames") units.at(index).frames = publishedFrames(value.at(3));
        }
        // Nested progress and assignment updates do not affect WU selection.
    }

    void visualisation(const Json& value) {
        if (value.size() < 4) invalid("incomplete visualisation update");
        auto unit = boundedString(value.at(1), 1024);
        auto field = boundedString(value.at(2), 128);
        if (unit != selected || selected.empty()) return;
        if (field == "topology") {
            if (value.size() != 4 || !value.at(3).is_object()) invalid("invalid topology update");
            const auto& data = value.at(3);
            if (!data.contains("atoms") || !data.at("atoms").is_array()) invalid("missing atom list");
            if (topology) emit(SourceEvent::Kind::Reset);
            topology.reset();
            nextFrame = decodedBytes = 0;
            if (data.at("atoms").empty()) {
                emit(SourceEvent::Kind::Status, "No visualisation available for this work unit");
                return;
            }
            topology = std::make_shared<Topology>(parseTopology(data.dump()));
            SourceEvent event;
            event.kind = SourceEvent::Kind::Topology;
            event.unit = selected;
            event.topology = topology;
            callback(std::move(event));
        } else if (field == "frames") {
            if (value.size() != 5 || !topology) invalid("frame arrived before topology");
            const auto index = integer(value.at(3));
            if (index < 0 || static_cast<std::size_t>(index) != nextFrame)
                invalid("non-sequential visualisation frame");
            if (nextFrame == maxFrames) invalid("too many visualisation frames");
            const auto bytes = topology->atoms.size() * sizeof(Vec3);
            if (bytes > maxFrameBytes - decodedBytes) invalid("visualisation history exceeds memory limit");
            std::size_t tail = 0;
            if (options.latestOnly) {
                const auto selectedUnit = std::find_if(units.begin(), units.end(),
                    [&](const Unit& candidate) { return candidate.id == selected; });
                if (selectedUnit == units.end() || !selectedUnit->frames || !*selectedUnit->frames)
                    invalid("latest visualisation frame has no positive published count");
                tail = *selectedUnit->frames - 1;
            }
            auto frame = std::make_shared<Frame>(parseFrame(value.at(4).dump(), topology->atoms.size()));
            SourceEvent event;
            event.kind = SourceEvent::Kind::Frame;
            event.unit = selected;
            event.index = nextFrame++;
            decodedBytes += bytes;
            event.frame = std::move(frame);
            if (options.latestOnly && event.index < tail) return;
            callback(std::move(event));
        } else invalid("unknown visualisation field");
    }
};

SourceSession::SourceSession(SourceOptions options, SourceCallback callback) :
    impl_(std::make_unique<Impl>()) {
    if (!callback || options.unit.size() > 1024) throw std::runtime_error("Invalid FAH source options");
    impl_->options = std::move(options);
    impl_->callback = std::move(callback);
}
SourceSession::~SourceSession() = default;

void SourceSession::connected() {
    impl_->units.clear();
    impl_->selected.clear();
    impl_->topology.reset();
    impl_->initial = true;
    impl_->nextFrame = impl_->decodedBytes = 0;
    impl_->emit(SourceEvent::Kind::Reset);
    impl_->emit(SourceEvent::Kind::Status, "Connected; waiting for work units");
}

std::vector<std::string> SourceSession::receive(std::string_view message) {
    if (message.size() > maxMessageBytes) invalid("message exceeds size limit");
    std::size_t nodes = 0;
    Json value;
    try {
        value = Json::parse(message, [&nodes](int depth, Json::parse_event_t, Json&) {
            if (depth > 64 || ++nodes > 2'000'000) invalid("message exceeds structure limit");
            return true;
        });
    } catch (const Json::exception&) { invalid("malformed JSON"); }
    if (value.is_string() && value == "ping") return {};
    if (impl_->initial) {
        if (!value.is_object() || !value.contains("units")) invalid("initial state has no work-unit list");
        impl_->replaceUnits(value.at("units"));
        impl_->initial = false;
        auto commands = impl_->select();
        if (impl_->selected.empty()) impl_->emit(SourceEvent::Kind::Status, "No running work unit");
        return commands;
    }
    if (!value.is_array() || value.empty() || !value.at(0).is_string())
        invalid("expected a state update");
    const auto& type = value.at(0).get_ref<const std::string&>();
    if (type == "units") { impl_->updateUnits(value); return impl_->select(); }
    if (type == "viz") impl_->visualisation(value);
    return {};
}

} // namespace fah
