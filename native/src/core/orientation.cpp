#include "orientation.hpp"

#include <cmath>

namespace sc {

namespace {

double horner(const std::vector<double> &c, size_t from, size_t n, double x) {
    double acc = 0.0;
    for (size_t i = n; i-- > 0;) acc = acc * x + c[from + i];
    return acc;
}

M3 rot1(double a) {
    const double c = std::cos(a), s = std::sin(a);
    M3 r;
    r.m[0][0] = 1; r.m[0][1] = 0; r.m[0][2] = 0;
    r.m[1][0] = 0; r.m[1][1] = c; r.m[1][2] = s;
    r.m[2][0] = 0; r.m[2][1] = -s; r.m[2][2] = c;
    return r;
}

M3 rot3(double a) {
    const double c = std::cos(a), s = std::sin(a);
    M3 r;
    r.m[0][0] = c; r.m[0][1] = s; r.m[0][2] = 0;
    r.m[1][0] = -s; r.m[1][1] = c; r.m[1][2] = 0;
    r.m[2][0] = 0; r.m[2][1] = 0; r.m[2][2] = 1;
    return r;
}

}  // namespace

void RotationModel::euler(double et, double &ra, double &dec, double &w) const {
    const double d = et / 86400.0;
    const double t = d / 36525.0;
    double a = horner(pole_ra, 0, pole_ra.size(), t);
    double b = horner(pole_dec, 0, pole_dec.size(), t);
    double c = horner(pm, 0, pm.size(), d);
    const size_t n = static_cast<size_t>(max_phase_degree) + 1;
    if (!system_angles.empty() && !nut_ra.empty()) {
        for (size_t i = 0, k = 0; k + n <= system_angles.size(); ++i, k += n) {
            const double th = horner(system_angles, k, n, t) * kDeg;
            if (i < nut_ra.size()) a += nut_ra[i] * std::sin(th);
            if (i < nut_dec.size()) b += nut_dec[i] * std::cos(th);
            if (i < nut_pm.size()) c += nut_pm[i] * std::sin(th);
        }
    }
    c = std::fmod(c, 360.0);
    ra = a * kDeg;
    dec = b * kDeg;
    w = c * kDeg;
}

M3 RotationModel::j2000_to_body(double et) const {
    double ra, dec, w;
    euler(et, ra, dec, w);
    return rot3(w) * rot1(kPi / 2 - dec) * rot3(kPi / 2 + ra);
}

V3 RotationModel::pole(double et) const {
    double ra, dec, w;
    euler(et, ra, dec, w);
    return {std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec)};
}

}  // namespace sc
