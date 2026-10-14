// Тесты ядра без Godot и без внешних данных. Сверка со SPICE, Horizons и REBOUND — в pipeline/tests (через C-API).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "core/ephemeris.hpp"
#include "core/gravity_field.hpp"
#include "core/nbody.hpp"
#include "core/timeconv.hpp"

using namespace sc;

namespace {

constexpr double kGMSun = 1.3271244004127942e11;  // gm_de440, км³/с²
constexpr double kAU = 149597870.7;               // км

// Решение задачи двух тел: положение и скорость из элементов (относительное движение, μ = GM₁ + GM₂).
void kepler_state(double mu, double a, double e, double inc, double node, double peri, double m, V3 &r, V3 &v) {
    double ea = m;
    for (int i = 0; i < 100; ++i) {
        const double d = (ea - e * std::sin(ea) - m) / (1.0 - e * std::cos(ea));
        ea -= d;
        if (std::fabs(d) < 1e-16) break;
    }
    const double cE = std::cos(ea), sE = std::sin(ea), q = std::sqrt(1.0 - e * e);
    const double n = std::sqrt(mu / (a * a * a));
    const double xo = a * (cE - e), yo = a * q * sE;
    const double vxo = -a * n * sE / (1.0 - e * cE), vyo = a * n * q * cE / (1.0 - e * cE);
    const double cO = std::cos(node), sO = std::sin(node), cw = std::cos(peri), sw = std::sin(peri);
    const double ci = std::cos(inc), si = std::sin(inc);
    const double p11 = cO * cw - sO * sw * ci, p12 = -cO * sw - sO * cw * ci;
    const double p21 = sO * cw + cO * sw * ci, p22 = -sO * sw + cO * cw * ci;
    const double p31 = sw * si, p32 = cw * si;
    r = {p11 * xo + p12 * yo, p21 * xo + p22 * yo, p31 * xo + p32 * yo};
    v = {p11 * vxo + p12 * vyo, p21 * vxo + p22 * vyo, p31 * vxo + p32 * vyo};
}

// Два тела в системе центра масс.
Simulation two_body(double gm1, double gm2, const V3 &r, const V3 &v) {
    Simulation s;
    const double f1 = gm2 / (gm1 + gm2), f2 = gm1 / (gm1 + gm2);
    Body a, b;
    a.gm = gm1;
    a.x = r * -f1;
    a.v = v * -f1;
    b.gm = gm2;
    b.x = r * f2;
    b.v = v * f2;
    s.bodies = {a, b};
    s.opt.gr = false;
    s.opt.j2 = false;
    return s;
}

}  // namespace

TEST_CASE("Кленшоу: многочлен Чебышёва и его производная") {
    // p(s) = 1 + 2·T1 + 3·T2 + 4·T3 = 1 + 2s + 3(2s²−1) + 4(4s³−3s)
    const double c[4] = {1, 2, 3, 4};
    for (double s = -1.0; s <= 1.0; s += 0.125) {
        double p, dp;
        cheb_eval(c, 4, s, p, dp);
        CHECK(p == doctest::Approx(1 + 2 * s + 3 * (2 * s * s - 1) + 4 * (4 * s * s * s - 3 * s)).epsilon(1e-15));
        CHECK(dp == doctest::Approx(2 + 12 * s + 4 * (12 * s * s - 3)).epsilon(1e-14));
    }
}

TEST_CASE("Календарь: 400 лет — 146097 суток, в том числе до нашей эры") {
    CHECK(days_from_civil(2000, 1, 1) == 10957);
    for (int64_t y = -5000; y < 5000; y += 37) {
        CHECK(days_from_civil(y + 400, 3, 1) - days_from_civil(y, 3, 1) == 146097);
        int64_t yy;
        int m, d;
        civil_from_days(days_from_civil(y, 7, 15), yy, m, d);
        CHECK(yy == y);
        CHECK(m == 7);
        CHECK(d == 15);
    }
}

TEST_CASE("Два тела: 1000 оборотов против точного решения, e = 0.5") {
    // Порог задан до запуска: |Δr| ≤ 1e-10·a, |ΔE/E| ≤ 1e-12.
    const double gm2 = 3.986004355e5, a = kAU, e = 0.5;
    const double mu = kGMSun + gm2;
    V3 r0, v0;
    kepler_state(mu, a, e, 0.3, 1.1, 2.2, 0.7, r0, v0);
    Simulation s = two_body(kGMSun, gm2, r0, v0);
    s.dt = 86400.0;
    const double period = 2 * kPi * std::sqrt(a * a * a / mu);
    const double e0 = s.energy();
    REQUIRE(s.integrate(1000.0 * period));
    V3 r1, v1;
    kepler_state(mu, a, e, 0.3, 1.1, 2.2, 0.7 + 2 * kPi * 1000.0, r1, v1);
    const V3 rel = s.bodies[1].x - s.bodies[0].x;
    const double dr = norm(rel - r1) / a;
    const double de = std::fabs((s.energy() - e0) / e0);
    std::printf("два тела: шагов %lld, отказов %lld, |Δr|/a = %.3e, |ΔE/E| = %.3e\n",
                static_cast<long long>(s.steps), static_cast<long long>(s.rejected), dr, de);
    CHECK(dr <= 1e-10);
    CHECK(de <= 1e-12);
}

TEST_CASE("Интегрирование назад возвращает в начальное состояние") {
    V3 r0, v0;
    kepler_state(kGMSun, 0.387 * kAU, 0.2056, 0.12, 0.84, 0.51, 3.0, r0, v0);
    Simulation s = two_body(kGMSun, 2.2e4, r0, v0);
    const V3 x0 = s.bodies[1].x;
    REQUIRE(s.integrate(50.0 * 365.25 * kDay));
    REQUIRE(s.integrate(0.0));
    CHECK(norm(s.bodies[1].x - x0) / norm(x0) <= 1e-11);
    CHECK(s.t == 0.0);
}

TEST_CASE("J2: прецессия узла спутника против аналитики") {
    // dΩ/dt = −1.5·n·J2·(R/a)²·cos i / (1 − e²)²; Земля, спутник на 7000 км, i = 50°
    const double gm = 3.986004355e5, R = 6378.137, j2 = 1082.63e-6, a = 7000.0, inc = 50.0 * kDeg;
    V3 r, v;
    kepler_state(gm, a, 0.001, inc, 0.0, 0.0, 0.0, r, v);
    Simulation s;
    Body planet, sat;
    planet.gm = gm;
    planet.zonal = {0.0, 0.0, j2};
    planet.r_ref = R;
    planet.pole = {0, 0, 1};
    sat.gm = 0.0;
    sat.x = r;
    sat.v = v;
    s.bodies = {planet, sat};
    s.opt.gr = false;
    s.dt = 60.0;
    const double n = std::sqrt(gm / (a * a * a));
    const double rate = -1.5 * n * j2 * (R / a) * (R / a) * std::cos(inc) / std::pow(1.0 - 0.001 * 0.001, 2);
    const double t_end = 10.0 * kDay;
    REQUIRE(s.integrate(t_end));
    const V3 h = cross(s.bodies[1].x - s.bodies[0].x, s.bodies[1].v - s.bodies[0].v);
    const double node = std::atan2(h.x, -h.y);
    std::printf("J2: узел %.6f°, аналитика %.6f°\n", node / kDeg, rate * t_end / kDeg);
    // за 10 суток короткопериодические колебания малы по сравнению с вековым дрейфом (~−49°)
    CHECK(node == doctest::Approx(rate * t_end).epsilon(0.01));
}

TEST_CASE("ОТО: перигелий Меркурия, 42,98″ за век ± 1 %") {
    const double gm_merc = 2.2031868551400003e4, a = 0.387098 * kAU, e = 0.205630;
    V3 r0, v0;
    kepler_state(kGMSun + gm_merc, a, e, 0.0, 0.0, 0.0, 0.0, r0, v0);
    Simulation s = two_body(kGMSun, gm_merc, r0, v0);
    s.opt.gr = true;
    s.dt = kDay;
    auto lrl = [&](const Simulation &sim) {  // вектор Лапласа–Рунге–Ленца указывает на перигелий
        const V3 r = sim.bodies[1].x - sim.bodies[0].x, v = sim.bodies[1].v - sim.bodies[0].v;
        return cross(v, cross(r, v)) - r * ((kGMSun + gm_merc) / norm(r));
    };
    const V3 p0 = lrl(s);
    // целое число оборотов около 100 лет: направление LRL в одной фазе орбиты
    const double period = 2 * kPi * std::sqrt(a * a * a / (kGMSun + gm_merc));
    const double n_orbits = std::round(36525.0 * kDay / period);
    REQUIRE(s.integrate(n_orbits * period));
    const V3 p1 = lrl(s);
    const double dw = std::atan2(cross(p0, p1).z, dot(p0, p1)) / kDeg * 3600.0;  // угловые секунды
    const double per_century = dw * (36525.0 * kDay) / (n_orbits * period);
    std::printf("ОТО: перигелий %.3f″ за век\n", per_century);
    CHECK(per_century == doctest::Approx(42.98).epsilon(0.01));
}

TEST_CASE("Поле в сферических гармониках: ускорение = градиент потенциала") {
    HarmonicField f;
    f.resize(6);
    f.r_ref = 3396.0;
    unsigned seed = 12345;
    auto rnd = [&]() {
        seed = seed * 1103515245u + 12345u;
        return ((seed >> 8) & 0xFFFF) / 65535.0 - 0.5;
    };
    for (int n = 2; n <= 6; ++n)
        for (int m = 0; m <= n; ++m) {
            f.c[HarmonicField::idx(n, m)] = 1e-3 * rnd();
            if (m > 0) f.s[HarmonicField::idx(n, m)] = 1e-3 * rnd();
        }
    const double gm = 42828.0;
    double worst = 0.0;
    for (int k = 0; k < 50; ++k) {
        const V3 r{9000.0 * (1 + rnd()), 9000.0 * rnd(), 9000.0 * rnd()};
        const V3 a = f.accel_body(gm, r);
        const double h = 1e-2;
        V3 g;
        for (int c = 0; c < 3; ++c) {
            V3 rp = r, rm = r;
            rp[c] += h;
            rm[c] -= h;
            g[c] = (f.potential_body(gm, rp) - f.potential_body(gm, rm)) / (2 * h);
        }
        worst = std::max(worst, norm(a - g) / norm(a));
    }
    std::printf("поле: max |a − ∇U| / |a| = %.2e\n", worst);
    CHECK(worst < 1e-6);
}

TEST_CASE("Осесимметричное поле совпадает с формулой Jₙ") {
    const double j[11] = {0, 0, 1.4697e-2, -4.5e-8, -5.87e-4, -7.2e-8, 3.42e-5, 1.2e-7, -2.42e-6, -1.1e-7, 1.8e-7};
    auto f = std::make_shared<HarmonicField>();
    f->resize(10);
    f->r_ref = 71492.0;
    f->ra0 = 0.0;
    f->dec0 = 90.0;  // полюс по оси z
    for (int n = 2; n <= 10; ++n) f->c[HarmonicField::idx(n, 0)] = -j[n];
    Simulation a, b;
    Body planet, moon;
    planet.gm = 1.2668e8;
    moon.gm = 0.0;
    moon.x = {300000.0, 200000.0, 150000.0};
    planet.zonal.assign(j, j + 11);
    planet.r_ref = 71492.0;
    a.bodies = {planet, moon};
    planet.zonal.clear();
    planet.field = f;
    b.bodies = {planet, moon};
    a.opt.gr = b.opt.gr = false;
    std::vector<V3> x = {a.bodies[0].x, a.bodies[1].x}, v(2), aa, ab;
    a.accelerations(x, v, aa);
    b.accelerations(x, v, ab);
    std::printf("Jₙ против поля: |Δa|/|a| = %.2e\n", norm(aa[1] - ab[1]) / norm(aa[1]));
    CHECK(norm(aa[1] - ab[1]) / norm(aa[1]) < 1e-14);
}
