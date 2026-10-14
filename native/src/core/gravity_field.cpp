#include "gravity_field.hpp"

#include <cmath>

namespace sc {

namespace {

// Функции V(n,m), W(n,m) до степени nmax+1 (Montenbruck & Gill, ур. 3.29–3.31).
struct VW {
    int nn;
    std::vector<double> v, w;
    explicit VW(int n) : nn(n), v(static_cast<size_t>((n + 1) * (n + 2) / 2), 0.0), w(v.size(), 0.0) {}
    double &V(int n, int m) { return v[static_cast<size_t>(n * (n + 1) / 2 + m)]; }
    double &W(int n, int m) { return w[static_cast<size_t>(n * (n + 1) / 2 + m)]; }
};

void compute_vw(VW &t, double rref, const V3 &r) {
    const int nmax = t.nn;
    const double r2 = dot(r, r);
    const double rho = rref * rref / r2;
    const double x0 = rref * r.x / r2, y0 = rref * r.y / r2, z0 = rref * r.z / r2;
    t.V(0, 0) = rref / std::sqrt(r2);
    t.W(0, 0) = 0.0;
    for (int m = 0; m <= nmax; ++m) {
        if (m > 0) {  // диагональ
            const double vp = t.V(m - 1, m - 1), wp = t.W(m - 1, m - 1);
            t.V(m, m) = (2 * m - 1) * (x0 * vp - y0 * wp);
            t.W(m, m) = (2 * m - 1) * (x0 * wp + y0 * vp);
        }
        if (m + 1 <= nmax) {
            t.V(m + 1, m) = (2 * m + 1) * z0 * t.V(m, m);
            t.W(m + 1, m) = (2 * m + 1) * z0 * t.W(m, m);
        }
        for (int n = m + 2; n <= nmax; ++n) {
            t.V(n, m) = ((2 * n - 1) * z0 * t.V(n - 1, m) - (n + m - 1) * rho * t.V(n - 2, m)) / (n - m);
            t.W(n, m) = ((2 * n - 1) * z0 * t.W(n - 1, m) - (n + m - 1) * rho * t.W(n - 2, m)) / (n - m);
        }
    }
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

void HarmonicField::resize(int n_max) {
    nmax = n_max;
    c.assign(static_cast<size_t>(idx(n_max, n_max) + 1), 0.0);
    s.assign(c.size(), 0.0);
}

M3 HarmonicField::to_body(double et) const {
    const double d = (et - epoch_et) / 86400.0;
    const double t = d / 36525.0;
    const double ra = (ra0 + ra_rate * t) * kDeg;
    const double dec = (dec0 + dec_rate * t) * kDeg;
    const double w = std::fmod(w0 + wdot * d, 360.0) * kDeg;
    return rot3(w) * rot1(kPi / 2 - dec) * rot3(kPi / 2 + ra);
}

V3 HarmonicField::accel_body(double gm, const V3 &r) const {
    VW t(nmax + 1);
    compute_vw(t, r_ref, r);
    double ax = 0, ay = 0, az = 0;
    for (int n = 2; n <= nmax; ++n) {
        // m = 0
        const double cn0 = C(n, 0);
        ax += -cn0 * t.V(n + 1, 1);
        ay += -cn0 * t.W(n + 1, 1);
        az += (n + 1) * (-cn0 * t.V(n + 1, 0));
        for (int m = 1; m <= n; ++m) {
            const double cnm = C(n, m), snm = S(n, m);
            if (cnm == 0.0 && snm == 0.0) continue;
            const double fac = static_cast<double>((n - m + 2) * (n - m + 1));
            ax += 0.5 * ((-cnm * t.V(n + 1, m + 1) - snm * t.W(n + 1, m + 1)) +
                         fac * (cnm * t.V(n + 1, m - 1) + snm * t.W(n + 1, m - 1)));
            ay += 0.5 * ((-cnm * t.W(n + 1, m + 1) + snm * t.V(n + 1, m + 1)) +
                         fac * (-cnm * t.W(n + 1, m - 1) + snm * t.V(n + 1, m - 1)));
            az += (n - m + 1) * (-cnm * t.V(n + 1, m) - snm * t.W(n + 1, m));
        }
    }
    const double k = gm / (r_ref * r_ref);
    return {ax * k, ay * k, az * k};
}

double HarmonicField::potential_body(double gm, const V3 &r) const {
    VW t(nmax);
    compute_vw(t, r_ref, r);
    double u = 0.0;
    for (int n = 2; n <= nmax; ++n)
        for (int m = 0; m <= n; ++m) u += C(n, m) * t.V(n, m) + S(n, m) * t.W(n, m);
    return gm / r_ref * u;
}

}  // namespace sc
