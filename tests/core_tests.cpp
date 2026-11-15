#include "fah/Model.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void close(float actual, float expected) {
    check(std::abs(actual - expected) < .0001f, "Unexpected floating-point result");
}
void rejects(const std::function<void()>& action) {
    try { action(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Invalid input was accepted");
}

void checkMesh(const fah::Mesh& mesh) {
    check(!mesh.vertices.empty() && !mesh.indices.empty(), "Mesh is empty");
    check(mesh.indices.size() % 3 == 0, "Mesh has incomplete triangles");
    for (const auto& vertex : mesh.vertices) {
        float length = 0;
        for (auto component : vertex.normal) length += component * component;
        close(length, 1);
    }
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        for (auto offset : {0, 1, 2})
            check(mesh.indices[i + offset] < mesh.vertices.size(), "Mesh index is out of range");
        const auto& a = mesh.vertices[mesh.indices[i]];
        const auto& b = mesh.vertices[mesh.indices[i + 1]];
        const auto& c = mesh.vertices[mesh.indices[i + 2]];
        const float u[] = {b.position[0] - a.position[0], b.position[1] - a.position[1],
                           b.position[2] - a.position[2]};
        const float v[] = {c.position[0] - a.position[0], c.position[1] - a.position[1],
                           c.position[2] - a.position[2]};
        const float cross[] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                               u[0] * v[1] - u[1] * v[0]};
        float outward = 0;
        for (auto axis : {0, 1, 2})
            outward += cross[axis] * (a.normal[axis] + b.normal[axis] + c.normal[axis]);
        check(outward > 0, "Triangle winding does not face outward");
    }
}

void checkProjection(float angle, float aspect) {
    const auto matrix = fah::viewProjection(angle, aspect, 1);
    for (auto point : {fah::Vec3{0, 0, 0}, fah::Vec3{1, 0, 0}, fah::Vec3{-1, 0, 0},
                       fah::Vec3{0, 1, 0}, fah::Vec3{0, -1, 0},
                       fah::Vec3{0, 0, 1}, fah::Vec3{0, 0, -1}}) {
        const float vector[] = {point.x, point.y, point.z, 1};
        float clip[4]{};
        for (auto row : {0, 1, 2, 3})
            for (auto column : {0, 1, 2, 3}) clip[row] += matrix[column * 4 + row] * vector[column];
        check(clip[3] > 0 && std::abs(clip[0]) <= clip[3] && std::abs(clip[1]) <= clip[3],
              "Camera does not fit model bounds");
        check(clip[2] >= 0 && clip[2] <= clip[3], "Depth is not in the 0..1 clip range");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        static_assert(sizeof(fah::Vertex) == 24, "GPU vertex stride changed");
        static_assert(sizeof(fah::Instance) == 80, "GPU instance stride changed");
        const auto capture = fah::readCapture(argc > 1 ? argv[1] : "tests/fixtures/synthetic-small.json");
        check(capture.topology.atoms.size() == 6 && capture.topology.bonds.size() == 5 &&
              capture.frames.size() == 2, "Synthetic fixture dimensions changed");
        close(capture.topology.atoms[0].radius, .17f);
        close(capture.topology.atoms[1].radius, .109f);
        close(capture.topology.atoms[2].radius, .155f);
        close(capture.topology.atoms[3].colour.x, .8f);
        close(capture.topology.atoms[4].colour.x, .6f);
        close(capture.topology.atoms[5].colour.x, 128.f / 255);

        const auto named = fah::parseTopology(R"({"atoms":[["1HG"],["CA"],["",0,0,0,3],["",0,0,0,10]],"bonds":[]})");
        close(named.atoms[0].radius, .109f);
        close(named.atoms[1].radius, .17f);
        close(named.atoms[2].radius, .109f);
        close(named.atoms[3].radius, .155f);

        const auto serialised = fah::writeCapture(capture);
        const auto restored = fah::parseCapture(serialised);
        check(restored.frames.size() == capture.frames.size(), "Roundtrip lost frames");
        for (std::size_t frame = 0; frame < capture.frames.size(); ++frame)
            for (std::size_t atom = 0; atom < capture.topology.atoms.size(); ++atom) {
                close(restored.frames[frame][atom].x, capture.frames[frame][atom].x);
                close(restored.frames[frame][atom].y, capture.frames[frame][atom].y);
                close(restored.frames[frame][atom].z, capture.frames[frame][atom].z);
                close(restored.topology.atoms[atom].radius, capture.topology.atoms[atom].radius);
            }
        check(serialised.find("account") == std::string::npos, "Capture contains client state");

        for (const auto* input : {"", "{", "[]", R"({"atoms":[],"bonds":[]})",
            R"({"atoms":[["C"]],"bonds":[[0,1]]})",
            R"({"atoms":[["C"]],"bonds":[[-1,0]]})",
            R"({"atoms":[["C"]],"bonds":[[0.5,0]]})",
            R"({"atoms":[["C"]],"bonds":[[18446744073709551615,0]]})",
            R"({"atoms":[["C"]],"bonds":[[0,true]]})",
            R"({"atoms":[["C",0,0,0,"6"]],"bonds":[]})",
            R"({"atoms":[["C",0,0,0,-1]],"bonds":[]})",
            R"({"atoms":[["C",0,0,0,1001]],"bonds":[]})"})
            rejects([&] { fah::parseTopology(input); });
        for (const auto* input : {"[]", "[[0,0]]", "[[0,0,0],[0,0,0]]",
            "[[null,0,0]]", "[[true,0,0]]", "[[1000001,0,0]]", "[[1e999,0,0]]"})
            rejects([&] { fah::parseFrame(input, 1); });
        rejects([] { fah::parseFrame("[[0,0,0]]", 0); });
        rejects([] { fah::parseCapture(R"({"topology":{"atoms":[["C"]],"bonds":[]},"frames":[]})"); });
        rejects([] { fah::parseCapture(R"({"topology":{"atoms":[["C"]],"bonds":[]},"frames":[[]]})"); });
        rejects([] { fah::parseTopology(std::string(32 * 1024 * 1024 + 1, ' ')); });
        rejects([] { fah::parseTopology(std::string(20, '[') + "0" + std::string(20, ']')); });
        rejects([] { fah::readCapture("/this/path/does/not/exist/fah.json"); });

        const auto pair = fah::parseTopology(R"({"atoms":[["C"],["O"]],"bonds":[[0,1]]})");
        const fah::Frame line{{0, 0, 0}, {0, 2, 0}};
        const auto spheres = fah::makeScene(pair, line, fah::Style::SpaceFilling);
        const auto ballStick = fah::makeScene(pair, line, fah::Style::BallAndStick);
        const auto sticks = fah::makeScene(pair, line, fah::Style::Sticks);
        check(spheres.atoms.size() == 2 && spheres.bonds.empty(), "Space-filling contains bonds");
        check(ballStick.bonds.size() == 1 && sticks.bonds.size() == 1, "Bond styles omit bonds");
        close(spheres.atoms[0].transform[13], -1);
        close(spheres.atoms[1].transform[13], 1);
        close(spheres.atoms[0].transform[0], .17f);
        close(ballStick.atoms[0].transform[0], .17f / 3);
        close(sticks.atoms[0].transform[0], .025f);
        close(ballStick.bonds[0].transform[5], 2);
        close(ballStick.bonds[0].transform[13], -1);
        close(spheres.radius, 1.17f);
        close(spheres.radius, sticks.radius);
        auto withOutlier = pair;
        withOutlier.atoms.push_back(pair.atoms[0]);
        const fah::Frame outlierFrame{{0, 0, 0}, {0, 2, 0}, {10000, -4000, 6000}};
        for (auto style : {fah::Style::SpaceFilling, fah::Style::BallAndStick, fah::Style::Sticks}) {
            const auto framed = fah::makeScene(withOutlier, outlierFrame, style);
            check(framed.atoms.size() == 3, "Framing discarded an isolated atom");
            close(framed.radius, spheres.radius);
            close(framed.atoms[0].transform[13], -1);
            close(framed.atoms[1].transform[13], 1);
            close(framed.atoms[2].transform[12], 10000);
            close(framed.atoms[2].transform[13], -4001);
            close(framed.atoms[2].transform[14], 6000);
            check(framed.bonds.size() == (style == fah::Style::SpaceFilling ? 0u : 1u),
                  "Framing discarded a visible bond");
        }
        const auto components = fah::parseTopology(
            R"({"atoms":[["C"],["O"],["C"],["O"],["C"]],"bonds":[[0,1],[2,3]]})");
        const auto framedComponents = fah::makeScene(components,
            {{-10, 0, 0}, {-8, 0, 0}, {8, 0, 0}, {10, 0, 0}, {5000, 5000, 5000}},
            fah::Style::BallAndStick);
        check(framedComponents.atoms.size() == 5 && framedComponents.bonds.size() == 2,
              "Framing changed topology instance counts");
        close(framedComponents.radius, 10.17f);
        close(framedComponents.atoms[0].transform[12], -10);
        close(framedComponents.atoms[3].transform[12], 10);
        close(framedComponents.atoms[4].transform[12], 5000);
        auto unbonded = pair;
        unbonded.bonds.clear();
        const auto unbondedScene = fah::makeScene(unbonded, {{0, 0, 0}, {20, 0, 0}},
                                                 fah::Style::SpaceFilling);
        close(unbondedScene.radius, 10.17f);
        close(unbondedScene.atoms[0].transform[12], -10);
        close(unbondedScene.atoms[1].transform[12], 10);
        check(fah::makeScene(pair, {{1, 1, 1}, {1, 1, 1}}, fah::Style::Sticks).bonds.empty(),
              "Coincident endpoints created a degenerate cylinder");
        auto invalidBond = pair;
        invalidBond.bonds[0].b = 2;
        rejects([&] { fah::makeScene(invalidBond, line, fah::Style::SpaceFilling); });
        rejects([&] { fah::makeScene(pair, {{0, 0, 0}}, fah::Style::Sticks); });
        rejects([&] { fah::makeScene(pair, line, static_cast<fah::Style>(99)); });
        auto badCapture = capture;
        badCapture.frames[0][0].x = std::numeric_limits<float>::infinity();
        rejects([&] { fah::writeCapture(badCapture); });
        rejects([&] { fah::makeScene(badCapture.topology, badCapture.frames[0], fah::Style::Sticks); });
        badCapture = capture;
        badCapture.topology.atoms[0].colour.x = .4f;
        rejects([&] { fah::writeCapture(badCapture); });

        checkMesh(fah::sphereMesh());
        checkMesh(fah::cylinderMesh());
        for (float aspect : {.2f, 1.f, 2.f})
            for (float angle : {0.f, 1.f, 3.f}) checkProjection(angle, aspect);
        rejects([] { fah::viewProjection(0, 0, 1); });
        rejects([] { fah::viewProjection(0, 1, 0); });
        rejects([] { fah::viewProjection(std::numeric_limits<float>::quiet_NaN(), 1, 1); });
        std::cout << "Core parsing, validation, scene, mesh and projection checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
