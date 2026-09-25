#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
// World-space direction toward the shared directional light.
class SceneLight {
public:
    static float Azimuth() { return azimuth_; }
    static float Elevation() { return elevation_; }
    static void SetAngles(float azimuth, float elevation) {
        if (!std::isfinite(azimuth) || !std::isfinite(elevation)) return;
        azimuth_ = azimuth;
        elevation_ = elevation;
    }
    static glm::vec3 Direction() {
        const float az = glm::radians(std::remainder(azimuth_, 360.f)), el = glm::radians(std::remainder(elevation_, 360.f));
        return { std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el) };
    }
private:
    inline static float azimuth_ = 21.f, elevation_ = 59.f;
};
