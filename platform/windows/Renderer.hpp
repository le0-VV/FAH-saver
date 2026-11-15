#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fah/Model.hpp>
#include <memory>
#include <string>

namespace fah::windows {

struct DrawTiming { bool rendered = false; double cpuMilliseconds = 0; double gpuMilliseconds = -1; };

class Renderer {
public:
    explicit Renderer(HWND window);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void setScene(const fah::Scene& scene);
    DrawTiming draw(float angle, bool sky, const float colour[3]);
    void resize();
    const std::string& adapter() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace fah::windows
