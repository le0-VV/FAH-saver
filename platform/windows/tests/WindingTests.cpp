#include <fah/Model.hpp>

#include <iostream>
#include <stdexcept>

static void checkFrontWinding(const fah::Mesh& mesh) {
    const auto matrix = fah::viewProjection(0,1,2);
    unsigned tested = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto& a = mesh.vertices[mesh.indices[i]];
        const auto& b = mesh.vertices[mesh.indices[i+1]];
        const auto& c = mesh.vertices[mesh.indices[i+2]];
        if (a.normal[2] < .8f || b.normal[2] < .8f || c.normal[2] < .8f) continue;
        float x[3],y[3]; unsigned index = 0;
        for (const auto* vertex : {&a,&b,&c}) {
            float clip[4]{};
            for (unsigned row = 0; row < 4; ++row) {
                clip[row] = matrix[12+row];
                for (unsigned column = 0; column < 3; ++column)
                    clip[row] += matrix[column*4+row]*vertex->position[column];
            }
            x[index] = clip[0]/clip[3];
            y[index] = -clip[1]/clip[3]; // The Vulkan shader flips projection Y.
            ++index;
        }
        // Vulkan's framebuffer-area formula has the opposite sign to the xy cross product.
        const float area = -((x[1]-x[0])*(y[2]-y[0])-(y[1]-y[0])*(x[2]-x[0]));
        if (!(area > 0)) throw std::runtime_error("Visible front triangle does not have Vulkan CCW area");
        ++tested;
    }
    if (!tested) throw std::runtime_error("No front-facing test triangles selected");
}

int main() {
    try {
        checkFrontWinding(fah::sphereMesh());
        checkFrontWinding(fah::cylinderMesh());
        std::cout << "Vulkan projection/winding tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
