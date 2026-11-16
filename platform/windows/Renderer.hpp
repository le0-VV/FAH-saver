#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fah/Model.hpp>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace fah::windows {

struct PhaseTiming {
    double wallMilliseconds = 0,threadCpuMilliseconds = 0;
    std::uint64_t calls = 0;
};
inline void addPhase(PhaseTiming& total,const PhaseTiming& value) {
    total.wallMilliseconds += value.wallMilliseconds;
    total.threadCpuMilliseconds += value.threadCpuMilliseconds;
    total.calls += value.calls;
}
inline double threadCpuMilliseconds() {
    FILETIME created,exited,kernel,user;
    if (!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))
        throw std::runtime_error("Cannot read main-thread CPU time");
    const auto ticks = [](FILETIME value) { return (std::uint64_t(value.dwHighDateTime)<<32) | value.dwLowDateTime; };
    return double(ticks(kernel)+ticks(user))/1e4;
}
class PhaseTimer {
    bool enabled;
    std::chrono::steady_clock::time_point wall;
    double cpu = 0;
public:
    explicit PhaseTimer(bool measure) : enabled(measure) {
        if (enabled) { wall = std::chrono::steady_clock::now(); cpu = threadCpuMilliseconds(); }
    }
    PhaseTiming finish() const {
        if (!enabled) return {};
        return {std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wall).count(),
            threadCpuMilliseconds()-cpu,1};
    }
};
struct DrawTiming {
    bool rendered = false;
    double cpuMilliseconds = 0,gpuMilliseconds = -1;
    PhaseTiming fenceQueryWait,acquire,recordSubmit,present;
    std::chrono::steady_clock::time_point submittedAt;
};

class Renderer {
public:
    explicit Renderer(HWND window,bool diagnostics = false);
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
