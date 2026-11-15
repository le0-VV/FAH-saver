#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fah {

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Atom { float radius; Vec3 colour; };
struct Bond { std::uint32_t a, b; };
struct Topology { std::vector<Atom> atoms; std::vector<Bond> bonds; };
using Frame = std::vector<Vec3>;
struct Capture { Topology topology; std::vector<Frame> frames; };

enum class Style { SpaceFilling = 1, BallAndStick = 2, Sticks = 3 };

struct Vertex { float position[3]; float normal[3]; };
struct Mesh { std::vector<Vertex> vertices; std::vector<std::uint32_t> indices; };

// Matrices use column-major order on all rendering backends.
struct Instance { float transform[16]; float colour[4]; };
struct Scene {
    std::vector<Instance> atoms;
    std::vector<Instance> bonds;
    float radius = 1;
};

// Capture JSON contains {"topology": <FAH topology>, "frames": [<FAH frame>, ...]}.
// Parsers throw std::runtime_error on invalid or oversized data.
Topology parseTopology(std::string_view json);
Frame parseFrame(std::string_view json, std::size_t atomCount);
Capture parseCapture(std::string_view json);
Capture readCapture(const std::string& path);
std::string writeCapture(const Capture& capture);

Scene makeScene(const Topology& topology, const Frame& frame, Style style);
Mesh sphereMesh();
Mesh cylinderMesh();

using Matrix = std::array<float, 16>;
Matrix viewProjection(float angle, float aspect, float radius);

} // namespace fah
