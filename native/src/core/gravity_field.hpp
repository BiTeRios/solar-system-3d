// Поле тяготения в сферических гармониках (ненормированные Cₙₘ, Sₙₘ), рекурсия Каннингема
// (Montenbruck & Gill, Satellite Orbits, 2000, §3.2.4–3.2.5). Считается в системе тела; центральный член (n = 0)
// и n = 1 не входят — их даёт точечная масса.
#pragma once

#include <vector>

#include "vec3.hpp"

namespace sc {

struct HarmonicField {
    int nmax = 0;
    double r_ref = 0.0;            // опорный радиус, км
    std::vector<double> c, s;      // индекс n·(n+1)/2 + m; C(n,0) = −Jₙ

    // ориентация тела: полюс (град, град/век) и угол нулевого меридиана (град, град/сутки) от эпохи epoch_et
    double ra0 = 0, dec0 = 90, ra_rate = 0, dec_rate = 0, w0 = 0, wdot = 0, epoch_et = 0;

    static int idx(int n, int m) { return n * (n + 1) / 2 + m; }
    void resize(int n_max);
    double C(int n, int m) const { return c[idx(n, m)]; }
    double S(int n, int m) const { return s[idx(n, m)]; }

    // Матрица ICRF → система тела на эпоху et.
    M3 to_body(double et) const;
    // Ускорение (км/с²) и потенциал (км²/с², знак «+»: U = GM/r·Σ…) в системе тела, без n = 0 и 1.
    V3 accel_body(double gm, const V3 &r) const;
    double potential_body(double gm, const V3 &r) const;
};

}  // namespace sc
