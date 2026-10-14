#include "kepler.hpp"

#include <cmath>

namespace sc {

namespace {

V3 unit(const V3 &a) {
    const double n = norm(a);
    return n > 0 ? a / n : V3{};
}

double wrap_pi(double x) {
    x = std::fmod(x, 2 * kPi);
    if (x > kPi) x -= 2 * kPi;
    if (x < -kPi) x += 2 * kPi;
    return x;
}

// Оси перицентра и узла по углам Ω, i, ω (стандартная последовательность 3-1-3).
void perifocal(double node, double inc, double argp, V3 &P, V3 &Q) {
    const double cO = std::cos(node), sO = std::sin(node), ci = std::cos(inc), si = std::sin(inc);
    const double cw = std::cos(argp), sw = std::sin(argp);
    P = {cO * cw - sO * sw * ci, sO * cw + cO * sw * ci, sw * si};
    Q = {-cO * sw - sO * cw * ci, -sO * sw + cO * cw * ci, cw * si};
}

}  // namespace

Conic conic_from_state(const V3 &r, const V3 &v, double mu) {
    Conic c;
    c.mu = mu;
    const V3 h = cross(r, v);
    const double hn = norm(h), rn = norm(r);
    if (hn <= 0 || rn <= 0 || mu <= 0) return c;
    c.W = h / hn;
    c.p = hn * hn / mu;
    const V3 ev = cross(v, h) / mu - r / rn;
    c.e = norm(ev);
    const double energy = 0.5 * dot(v, v) - mu / rn;
    c.a = energy != 0 ? -mu / (2 * energy) : INFINITY;
    c.P = c.e > 1e-12 ? ev / c.e : r / rn;
    c.Q = cross(c.W, c.P);
    c.nu = std::atan2(dot(r, c.Q), dot(r, c.P));
    c.valid = true;
    return c;
}

V3 conic_point(const Conic &c, double nu) {
    const double rr = c.p / (1 + c.e * std::cos(nu));
    return c.P * (rr * std::cos(nu)) + c.Q * (rr * std::sin(nu));
}

double true_from_eccentric(double E, double e) {
    return 2 * std::atan2(std::sqrt(1 + e) * std::sin(E / 2), std::sqrt(1 - e) * std::cos(E / 2));
}

double eccentric_from_true(double nu, double e) {
    return 2 * std::atan2(std::sqrt(1 - e) * std::sin(nu / 2), std::sqrt(1 + e) * std::cos(nu / 2));
}

double solve_kepler(double M, double e) {
    M = wrap_pi(M);
    double E = e < 0.8 ? M : (M >= 0 ? kPi : -kPi);
    for (int k = 0; k < 60; ++k) {
        const double f = E - e * std::sin(E) - M;
        const double d = f / (1 - e * std::cos(E));
        E -= d;
        if (std::fabs(d) < 1e-15) break;
    }
    return E;
}

void KinematicOrbit::state(double t, V3 &r, V3 &v) const {
    const double dt = t - t0;
    const double node = node0 + node_rate * dt, argp = argp0 + argp_rate * dt;
    const double E = solve_kepler(m0 + m_rate * dt, e);
    const double nu = true_from_eccentric(E, e);
    const double p = a * (1 - e * e);
    const double rr = p / (1 + e * std::cos(nu));
    V3 P, Q;
    perifocal(node, inc, argp, P, Q);
    const double s = std::sqrt(mu / p);
    const V3 re = P * (rr * std::cos(nu)) + Q * (rr * std::sin(nu));
    const V3 ve = P * (-s * std::sin(nu)) + Q * (s * (e + std::cos(nu)));
    r = eq_to_icrf * re;
    v = eq_to_icrf * ve;
}

KinematicOrbit kinematic_from_state(const V3 &r_icrf, const V3 &v_icrf, double mu, const V3 &pole, double j2,
                                    double r_ref, double t0) {
    KinematicOrbit k;
    const V3 Z = unit(pole);
    V3 X = cross(V3{0, 0, 1}, Z);
    X = norm(X) > 1e-12 ? unit(X) : V3{1, 0, 0};
    const V3 Y = cross(Z, X);
    for (int i = 0; i < 3; ++i) {
        k.eq_to_icrf.m[i][0] = X[i];
        k.eq_to_icrf.m[i][1] = Y[i];
        k.eq_to_icrf.m[i][2] = Z[i];
    }
    const M3 to_eq = k.eq_to_icrf.transposed();
    const V3 r = to_eq * r_icrf, v = to_eq * v_icrf;
    const double rn = norm(r);
    const V3 h = cross(r, v);
    const double hn = norm(h);
    const double energy = 0.5 * dot(v, v) - mu / rn;
    if (hn <= 0 || energy >= 0 || mu <= 0) return k;  // только эллипсы
    k.mu = mu;
    k.t0 = t0;
    k.a = -mu / (2 * energy);
    const V3 ev = cross(v, h) / mu - r / rn;
    k.e = norm(ev);
    k.inc = std::acos(std::fmax(-1.0, std::fmin(1.0, h.z / hn)));
    V3 nv{-h.y, h.x, 0};  // направление на восходящий узел
    const bool equatorial = norm(nv) < 1e-12 * hn;
    nv = equatorial ? V3{1, 0, 0} : unit(nv);
    k.node0 = std::atan2(nv.y, nv.x);
    const V3 w = h / hn;
    const V3 Pdir = k.e > 1e-12 ? ev / k.e : nv;  // у круговой орбиты перицентр условно в узле
    k.argp0 = std::atan2(dot(Pdir, cross(w, nv)), dot(Pdir, nv));
    const double nu = std::atan2(dot(r, cross(w, Pdir)), dot(r, Pdir));
    const double E = eccentric_from_true(nu, k.e);
    k.m0 = E - k.e * std::sin(E);
    const double n = std::sqrt(mu / (k.a * k.a * k.a));
    const double p = k.a * (1 - k.e * k.e);
    const double q = (j2 != 0 && r_ref > 0) ? j2 * (r_ref / p) * (r_ref / p) : 0.0;
    const double ci = std::cos(k.inc);
    k.node_rate = -1.5 * n * q * ci;
    k.argp_rate = 0.75 * n * q * (5 * ci * ci - 1);
    k.m_rate = n * (1 + 0.75 * q * std::sqrt(1 - k.e * k.e) * (3 * ci * ci - 1));
    k.valid = true;
    return k;
}

}  // namespace sc
