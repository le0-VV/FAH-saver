#include "fah/Model.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace fah {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maxBytes = 32 * 1024 * 1024;
constexpr std::size_t maxAtoms = 200000;
constexpr std::size_t maxBonds = 1000000;
constexpr std::size_t maxFrames = 10000;
constexpr std::size_t maxPositions = 2000000;
constexpr float pi = 3.14159265358979323846f;
constexpr Atom materials[] = {
    {.17f, {.2f, .2f, .2f}},
    {.109f, {.6f, .6f, .6f}},
    {.155f, {32.f / 255, 32.f / 255, .8f}},
    {.152f, {.8f, 38.f / 255, 38.f / 255}},
    {.18f, {.6f, .6f, 38.f / 255}},
    {.2f, {128.f / 255, 0, .6f}}
};
constexpr int elements[] = {6, 1, 7, 8, 16, 999};
constexpr const char* names[] = {"C", "H", "N", "O", "S", "X"};

[[noreturn]] void invalid(const std::string& message) {
    throw std::runtime_error(message);
}

Json parse(std::string_view text) {
    if (text.empty() || text.size() > maxBytes)
        invalid("JSON input must contain 1 to 33554432 bytes");
    std::size_t values = 0;
    try {
        return Json::parse(text.begin(), text.end(), [&values](int depth,
            Json::parse_event_t event, Json&) {
            if (depth > 16) invalid("JSON nesting exceeds 16 levels");
            if (event == Json::parse_event_t::value && ++values > 4000000)
                invalid("JSON contains too many values");
            return true;
        });
    } catch (const Json::exception& error) {
        invalid(std::string("Invalid JSON: ") + error.what());
    }
}

const Json& member(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key))
        invalid(std::string("Missing object member: ") + key);
    return object.at(key);
}

float number(const Json& value, const char* context, float minimum, float maximum) {
    if (!value.is_number()) invalid(std::string(context) + " must be a number");
    const double result = value.get<double>();
    if (!std::isfinite(result) || result < minimum || result > maximum)
        invalid(std::string(context) + " is outside the supported finite range");
    return static_cast<float>(result);
}

std::uint32_t integer(const Json& value, const char* context, std::uint32_t maximum) {
    if (!value.is_number_integer())
        invalid(std::string(context) + " must be a non-negative integer");
    if ((!value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
        value.get<std::uint64_t>() > maximum)
        invalid(std::string(context) + " is outside the supported integer range");
    return value.get<std::uint32_t>();
}

std::size_t materialIndex(int element) {
    switch (element) {
    case 6: return 0;
    case 1: case 3: return 1; // OpenMM 0x22 also reports hydrogen as 3.
    case 7: case 10: return 2; // OpenMM 0x22 also reports nitrogen as 10.
    case 8: return 3;
    case 16: return 4;
    default: return 5;
    }
}

int elementFromName(const std::string& name) {
    for (unsigned char letter : name) {
        switch (std::toupper(letter)) {
        case 'C': return 6;
        case 'H': return 1;
        case 'N': return 7;
        case 'O': return 8;
        case 'S': return 16;
        }
    }
    return 999;
}

Topology topologyFromJson(const Json& value) {
    const auto& atoms = member(value, "atoms");
    const auto& bonds = member(value, "bonds");
    if (!atoms.is_array() || atoms.empty()) invalid("No visualisation atoms available");
    if (atoms.size() > maxAtoms) invalid("Topology exceeds 200000 atoms");
    if (!bonds.is_array() || bonds.size() > maxBonds)
        invalid("Topology bonds must be an array of at most 1000000 pairs");
    Topology result;
    result.atoms.reserve(atoms.size());
    result.bonds.reserve(bonds.size());
    for (const auto& atom : atoms) {
        if (!atom.is_array() || atom.empty() || atom.size() > 16 ||
            !atom[0].is_string() || atom[0].get_ref<const std::string&>().size() > 256)
            invalid("Each atom must be an array with an atom name");
        int element = elementFromName(atom[0].get_ref<const std::string&>());
        if (atom.size() > 4 && !atom[4].is_null()) {
            const auto specified = integer(atom[4], "Atomic number", 1000);
            if (specified) element = static_cast<int>(specified);
        }
        result.atoms.push_back(materials[materialIndex(element)]);
    }
    for (const auto& bond : bonds) {
        if (!bond.is_array() || bond.size() != 2)
            invalid("Each bond must contain exactly two atom indices");
        const auto limit = static_cast<std::uint32_t>(atoms.size() - 1);
        result.bonds.push_back({integer(bond[0], "Bond index", limit),
                                integer(bond[1], "Bond index", limit)});
    }
    return result;
}

Frame frameFromJson(const Json& value, std::size_t atomCount) {
    if (atomCount == 0 || atomCount > maxAtoms)
        invalid("Frame atom count must be between 1 and 200000");
    if (!value.is_array() || value.size() != atomCount)
        invalid("Frame position count does not match topology atom count");
    Frame result;
    result.reserve(atomCount);
    for (const auto& position : value) {
        if (!position.is_array() || position.size() != 3)
            invalid("Each position must contain exactly three coordinates");
        result.push_back({number(position[0], "Coordinate", -1000000, 1000000),
                          number(position[1], "Coordinate", -1000000, 1000000),
                          number(position[2], "Coordinate", -1000000, 1000000)});
    }
    return result;
}

void validateFrame(const Frame& frame, std::size_t atomCount) {
    if (atomCount == 0 || atomCount > maxAtoms || frame.size() != atomCount)
        invalid("Frame position count does not match a supported topology");
    for (auto position : frame)
        for (auto coordinate : {position.x, position.y, position.z})
            if (!std::isfinite(coordinate) || std::abs(coordinate) > 1000000)
                invalid("Coordinate is outside the supported finite range");
}

Vec3 subtract(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(Vec3 value, float amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}
float length(Vec3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
Instance instance(Vec3 x, Vec3 y, Vec3 z, Vec3 translation, Vec3 colour) {
    return {{x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0,
             translation.x, translation.y, translation.z, 1},
            {colour.x, colour.y, colour.z, 1}};
}

} // namespace

Topology parseTopology(std::string_view json) { return topologyFromJson(parse(json)); }
Frame parseFrame(std::string_view json, std::size_t atomCount) {
    return frameFromJson(parse(json), atomCount);
}

Capture parseCapture(std::string_view json) {
    const auto value = parse(json);
    Capture result;
    result.topology = topologyFromJson(member(value, "topology"));
    const auto& frames = member(value, "frames");
    if (!frames.is_array() || frames.empty()) invalid("No visualisation frames available");
    if (frames.size() > maxFrames || frames.size() > maxPositions / result.topology.atoms.size())
        invalid("Capture exceeds the supported frame or total position limit");
    result.frames.reserve(frames.size());
    for (const auto& frame : frames)
        result.frames.push_back(frameFromJson(frame, result.topology.atoms.size()));
    return result;
}

Capture readCapture(const std::string& path) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!input) invalid("Cannot open capture: " + path);
    const auto size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(maxBytes))
        invalid("Capture file must contain 1 to 33554432 bytes");
    std::string json(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(json.data(), size)) invalid("Cannot read complete capture: " + path);
    return parseCapture(json);
}

std::string writeCapture(const Capture& capture) {
    const auto& topology = capture.topology;
    if (topology.atoms.empty() || topology.atoms.size() > maxAtoms ||
        topology.bonds.size() > maxBonds || capture.frames.empty() ||
        capture.frames.size() > maxFrames ||
        capture.frames.size() > maxPositions / topology.atoms.size())
        invalid("Capture dimensions are outside the supported limits");
    Json atoms = Json::array(), bonds = Json::array(), frames = Json::array();
    for (auto atom : topology.atoms) {
        std::size_t type = 0;
        while (type < std::size(materials)) {
            const auto& material = materials[type];
            if (atom.radius == material.radius && atom.colour.x == material.colour.x &&
                atom.colour.y == material.colour.y && atom.colour.z == material.colour.z) break;
            ++type;
        }
        if (type == std::size(materials)) invalid("Cannot serialise a non-FAH atom material");
        atoms.push_back(Json::array({names[type], 0, 0, 0, elements[type]}));
    }
    for (auto bond : topology.bonds) {
        if (bond.a >= topology.atoms.size() || bond.b >= topology.atoms.size())
            invalid("Bond index exceeds topology atom count");
        bonds.push_back(Json::array({bond.a, bond.b}));
    }
    for (const auto& frame : capture.frames) {
        validateFrame(frame, topology.atoms.size());
        Json positions = Json::array();
        for (auto position : frame) positions.push_back({position.x, position.y, position.z});
        frames.push_back(std::move(positions));
    }
    auto result = Json({{"topology", {{"atoms", atoms}, {"bonds", bonds}}},
                        {"frames", frames}}).dump();
    if (result.size() > maxBytes) invalid("Serialised capture exceeds 33554432 bytes");
    return result;
}

Scene makeScene(const Topology& topology, const Frame& frame, Style style) {
    validateFrame(frame, topology.atoms.size());
    if (topology.bonds.size() > maxBonds) invalid("Topology exceeds the supported bond count");
    if (style != Style::SpaceFilling && style != Style::BallAndStick && style != Style::Sticks)
        invalid("Unknown model style");
    double x = 0, y = 0, z = 0;
    for (auto point : frame) { x += point.x; y += point.y; z += point.z; }
    const Vec3 centre{static_cast<float>(x / frame.size()), static_cast<float>(y / frame.size()),
                      static_cast<float>(z / frame.size())};
    Scene result;
    result.radius = 0;
    result.atoms.reserve(frame.size());
    if (style != Style::SpaceFilling) result.bonds.reserve(topology.bonds.size());
    for (std::size_t i = 0; i < frame.size(); ++i) {
        const auto& atom = topology.atoms[i];
        if (!std::isfinite(atom.radius) || atom.radius <= 0 || atom.radius > 1000)
            invalid("Atom radius is outside the supported finite range");
        for (auto channel : {atom.colour.x, atom.colour.y, atom.colour.z})
            if (!std::isfinite(channel) || channel < 0 || channel > 1)
                invalid("Atom colour must contain finite channels between 0 and 1");
        const auto position = subtract(frame[i], centre);
        result.radius = std::max(result.radius, length(position) + atom.radius);
        const float radius = style == Style::Sticks ? .025f :
            style == Style::BallAndStick ? atom.radius / 3 : atom.radius;
        result.atoms.push_back(instance({radius, 0, 0}, {0, radius, 0}, {0, 0, radius},
                                         position, atom.colour));
    }
    for (auto bond : topology.bonds) {
        if (bond.a >= frame.size() || bond.b >= frame.size())
            invalid("Bond index exceeds topology atom count");
        if (style == Style::SpaceFilling) continue;
        const auto direction = subtract(frame[bond.b], frame[bond.a]);
        const auto distance = length(direction);
        if (distance == 0) continue; // Coincident endpoints have no cylinder to draw.
        const auto axis = scale(direction, 1 / distance);
        const Vec3 helper = std::abs(axis.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
        auto right = cross(axis, helper);
        right = scale(right, 1 / length(right));
        const auto front = cross(right, axis);
        result.bonds.push_back(instance(scale(right, .01f), direction, scale(front, .01f),
            subtract(frame[bond.a], centre), {.75f, .75f, .75f}));
    }
    return result;
}

Mesh sphereMesh() {
    constexpr unsigned segments = 16, rings = 12;
    Mesh mesh;
    for (unsigned ring = 0; ring <= rings; ++ring) {
        const float phi = pi * ring / rings;
        for (unsigned segment = 0; segment <= segments; ++segment) {
            const float theta = 2 * pi * segment / segments;
            const Vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi),
                              std::sin(phi) * std::sin(theta)};
            mesh.vertices.push_back({{normal.x, normal.y, normal.z},
                                      {normal.x, normal.y, normal.z}});
        }
    }
    for (unsigned ring = 0; ring < rings; ++ring)
        for (unsigned segment = 0; segment < segments; ++segment) {
            const auto a = ring * (segments + 1) + segment;
            const auto b = a + segments + 1;
            if (ring != 0) mesh.indices.insert(mesh.indices.end(), {a, a + 1, b});
            if (ring != rings - 1) mesh.indices.insert(mesh.indices.end(), {a + 1, b + 1, b});
        }
    return mesh;
}

Mesh cylinderMesh() {
    constexpr unsigned segments = 12;
    Mesh mesh;
    for (unsigned segment = 0; segment <= segments; ++segment) {
        const float theta = 2 * pi * segment / segments;
        const float x = std::cos(theta), z = std::sin(theta);
        mesh.vertices.push_back({{x, 0, z}, {x, 0, z}});
        mesh.vertices.push_back({{x, 1, z}, {x, 0, z}});
        if (segment < segments) {
            const auto a = segment * 2;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
        }
    }
    for (unsigned cap = 0; cap < 2; ++cap) {
        const auto centre = static_cast<std::uint32_t>(mesh.vertices.size());
        const float y = static_cast<float>(cap), normal = cap ? 1.f : -1.f;
        mesh.vertices.push_back({{0, y, 0}, {0, normal, 0}});
        for (unsigned segment = 0; segment <= segments; ++segment) {
            const float theta = 2 * pi * segment / segments;
            mesh.vertices.push_back({{std::cos(theta), y, std::sin(theta)}, {0, normal, 0}});
            if (segment < segments) {
                const auto a = centre + segment + 1;
                if (cap) mesh.indices.insert(mesh.indices.end(), {centre, a + 1, a});
                else mesh.indices.insert(mesh.indices.end(), {centre, a, a + 1});
            }
        }
    }
    return mesh;
}

Matrix viewProjection(float angle, float aspect, float radius) {
    if (!std::isfinite(angle) || !std::isfinite(aspect) || aspect < .0001f || aspect > 10000 ||
        !std::isfinite(radius) || radius <= 0 || radius > 10000000)
        invalid("Camera angle, aspect or radius is outside the supported finite range");
    const float distance = radius * 1.1f * std::max(1.f, 1 / aspect) / std::sin(pi / 8);
    const float nearPlane = std::max(radius * .01f, distance - radius * 1.1f);
    const float farPlane = distance + radius * 1.1f;
    const float f = 1 / std::tan(pi / 8);
    const Matrix projection{f / aspect, 0, 0, 0, 0, f, 0, 0,
        0, 0, farPlane / (nearPlane - farPlane), -1,
        0, 0, farPlane * nearPlane / (nearPlane - farPlane), 0};
    const float cosine = std::cos(angle), sine = std::sin(angle);
    const Matrix view{cosine, 0, -sine, 0, 0, 1, 0, 0,
                      sine, 0, cosine, 0, 0, 0, -distance, 1};
    Matrix result{};
    for (unsigned column = 0; column < 4; ++column)
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned inner = 0; inner < 4; ++inner)
                result[column * 4 + row] += projection[inner * 4 + row] * view[column * 4 + inner];
    return result;
}

} // namespace fah
