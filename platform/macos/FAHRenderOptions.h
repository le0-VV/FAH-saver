#pragma once

#include "fah/Model.hpp"

enum class FAHStyleMode { Fixed, Cycle, Random };

struct FAHRenderOptions {
    int fps = 60;
    fah::Style style = fah::Style::SpaceFilling;
    FAHStyleMode styleMode = FAHStyleMode::Fixed;
    bool loop = false;
    bool sky = true;
    bool darkSky = false;
    fah::Vec3 colour = {0.02f, 0.025f, 0.035f};
    double snapshotInterval = 1;
    double styleInterval = 10;
    bool collectMetrics = false;
};

