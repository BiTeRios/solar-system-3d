// Задача двух тел: коника по состоянию (для линий орбит) и спутник на прецессирующих элементах с J2
// (упрощение «Симуляции» при большой скорости времени). Единицы: км, км/с, с.
#pragma once

#include "vec3.hpp"

namespace sc {

// Коника относительно центра: r(ν) = p / (1 + e·cos ν) · (cos ν·P + sin ν·Q).
struct Conic {
    double mu = 0, p = 0, e = 0;
    double a = 0;   // большая полуось (< 0 у гиперболы)
    V3 P, Q, W;     // перицентр, 90° по движению, нормаль (по моменту импульса)
    double nu = 0;  // истинная аномалия тела в момент построения
    bool valid = false;
};

// По положению и скорости относительно центра. Для почти круговой орбиты P направлен на тело.
Conic conic_from_state(const V3 &r, const V3 &v, double mu);
V3 conic_point(const Conic &c, double nu);
// Истинная аномалия по эксцентрической (эллипс).
double true_from_eccentric(double E, double e);
double eccentric_from_true(double nu, double e);
// Уравнение Кеплера для эллипса: M → E (Ньютон, до 1e-15).
double solve_kepler(double M, double e);

// Спутник на элементах в экваториальной системе планеты с вековыми уходами от J2:
// Ω̇ = −1,5·n·J2·(R/p)²·cos i, ω̇ = 0,75·n·J2·(R/p)²·(5cos²i − 1), Ṁ = n·(1 + 0,75·J2·(R/p)²·√(1−e²)·(3cos²i − 1)).
struct KinematicOrbit {
    M3 eq_to_icrf;  // столбцы — оси экваториальной системы планеты в ICRF
    double mu = 0, a = 0, e = 0, inc = 0, node0 = 0, argp0 = 0, m0 = 0, t0 = 0;
    double node_rate = 0, argp_rate = 0, m_rate = 0;
    bool valid = false;

    // Положение и скорость относительно планеты (ICRF) на момент t.
    void state(double t, V3 &r, V3 &v) const;
};

// pole — северный полюс планеты (ICRF), j2 и r_ref — её J2 и опорный радиус (0 — без прецессии).
KinematicOrbit kinematic_from_state(const V3 &r, const V3 &v, double mu, const V3 &pole, double j2, double r_ref,
                                    double t0);

}  // namespace sc
