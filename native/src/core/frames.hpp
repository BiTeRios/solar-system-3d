// Системы координат сцены. Расчёты — в ICRF (в SPICE это J2000), сцена Godot — в эклиптике J2000.
#pragma once

#include <cmath>

#include "vec3.hpp"

namespace sc {

// Наклон эклиптики J2000 — как у системы ECLIPJ2000 в SPICE (IAU 1976): 84381,448″.
constexpr double kObliquityJ2000 = 84381.448 / 3600.0 * kDeg;

// ICRF → эклиптика J2000: поворот на наклон вокруг оси X.
inline M3 icrf_to_ecliptic() {
    const double c = std::cos(kObliquityJ2000), s = std::sin(kObliquityJ2000);
    M3 r;
    r.m[0][0] = 1; r.m[0][1] = 0; r.m[0][2] = 0;
    r.m[1][0] = 0; r.m[1][1] = c; r.m[1][2] = s;
    r.m[2][0] = 0; r.m[2][1] = -s; r.m[2][2] = c;
    return r;
}

// ICRF → сцена Godot (правая, Y вверх): X сцены — на точку весеннего равноденствия,
// Y — на северный полюс эклиптики, Z — против оси Y эклиптики. Единицы сцены — метры.
inline M3 icrf_to_scene() {
    M3 g;  // эклиптика (x, y, z) → сцена (x, z, −y)
    g.m[0][0] = 1; g.m[0][1] = 0; g.m[0][2] = 0;
    g.m[1][0] = 0; g.m[1][1] = 0; g.m[1][2] = 1;
    g.m[2][0] = 0; g.m[2][1] = -1; g.m[2][2] = 0;
    return g * icrf_to_ecliptic();
}

}  // namespace sc
