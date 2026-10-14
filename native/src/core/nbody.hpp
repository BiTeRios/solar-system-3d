// N-body: интегратор IAS15 (Rein & Spiegel 2015, MNRAS 446, 1424), реализация по статье.
// Силы: ньютоновская гравитация, поправка ОТО 1PN в форме Эйнштейна–Инфельда–Хоффмана (как в DE440),
// зональные гармоники Jₙ сплюснутых тел (с реакцией на само тело, импульс сохраняется).
// Единицы: км, км/с, с (TDB). Координаты — ICRF относительно барицентра.
//
// Спутники интегрируются в координатах относительно своей планеты (Body::parent): в барицентрических
// координатах (~10⁹ км, шаг double ~10⁻⁷ км) расстояние Фобос–Марс несёт шум ~10⁻¹², IAS15 усиливает его
// в старшем коэффициенте выше ε, и шаг схлопывается. Расстояния внутри планетной системы считаются из
// относительных координат.
#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "gravity_field.hpp"
#include "vec3.hpp"

namespace sc {

struct Body {
    double gm = 0.0;     // км³/с²; 0 — безмассовая частица (комета, пробное тело)
    V3 x, v;             // положение и скорость
    std::vector<double> zonal;  // zonal[n] = Jₙ (n ≥ 2, ненормированные); пусто — сфера
    double r_ref = 0.0;         // опорный радиус для Jₙ, км
    V3 pole{0, 0, 1};           // ось симметрии (единичный вектор, ICRF), постоянна в течение интегрирования
    int parent = -1;     // индекс планеты для спутника (у самой планеты parent = −1); один уровень
    // Полное поле (тессеральные гармоники, вращение тела). Если задано — вместо zonal.
    std::shared_ptr<const HarmonicField> field;
};

struct NBodyOptions {
    bool gr = true;          // поправка ОТО (EIH, β = γ = 1)
    bool j2 = true;          // учитывать зональные гармоники Jₙ
    double epsilon = 1e-9;   // точность IAS15 (по старшему коэффициенту)
    double safety = 0.25;    // шаг меньше в 4 раза — шаг отвергается; рост не больше чем в 4 раза
    int max_iterations = 12;
    double min_dt = 1e-3;    // с; меньше — ошибка интегрирования
};

class Simulation {
public:
    std::vector<Body> bodies;
    NBodyOptions opt;
    double t = 0.0;      // с TDB от J2000
    double dt = 3600.0;  // текущий шаг, знак задаёт направление

    int64_t steps = 0, rejected = 0, iterations = 0, not_converged = 0;

    // Интегрирует до t_end, последний шаг укорачивается точно до t_end.
    // max_steps < 0 — без ограничения. Возвращает false при ошибке (шаг меньше min_dt, NaN).
    bool integrate(double t_end, int64_t max_steps = -1);
    // Один шаг IAS15 (с повторами при отказе). false — ошибка.
    bool step();
    // Сбросить внутреннее состояние интегратора (после ручной правки тел).
    void reset();

    // Ускорения в абсолютных (барицентрических) координатах — для внешних проверок.
    void accelerations(const std::vector<V3> &x, const std::vector<V3> &v, std::vector<V3> &a) const;

    // Ньютоновская энергия (в единицах G: Σ ½·GM·v² − Σ GMᵢGMⱼ/r) плюс потенциал зональных гармоник.
    double energy() const;
    V3 angular_momentum() const;  // Σ GM·(x × v)
    V3 momentum() const;          // Σ GM·v
    V3 center_of_mass() const;

    const char *error() const { return error_; }

private:
    bool try_step(double h);
    void predict_next(double q);
    void to_internal();
    void to_bodies();
    // Ускорения во внутренних координатах: у спутника — относительно планеты.
    void accel_internal(const std::vector<V3> &q, const std::vector<V3> &u, double et, std::vector<V3> &a) const;
    // Общий расчёт сил: X, V — абсолютные; local — координаты внутри планетной системы (у планеты — ноль);
    // et — эпоха (для вращающихся полей тяготения).
    void forces(const std::vector<V3> &X, const std::vector<V3> &V, const std::vector<V3> &local, double et,
                std::vector<V3> &a) const;
    int top(size_t i) const { return bodies[i].parent >= 0 ? bodies[i].parent : static_cast<int>(i); }

    // внутреннее состояние — главное: у спутников относительно планеты, у остальных — барицентрическое
    std::vector<V3> q_, u_;
    bool have_state_ = false;

    // коэффициенты многочлена ускорения: a(τ) = a0 + Σ b[k]·τ^(k+1), τ ∈ [0, 1]
    std::vector<V3> b_[7], g_[7];
    std::vector<V3> a0_, csx_, csv_;
    bool have_b_ = false, have_a0_ = false;
    double t_end_dt_ = 0.0;  // шаг до укорачивания на последнем отрезке
    const char *error_ = "";

    // рабочие массивы
    std::vector<V3> xs_, vs_, as_;
};

}  // namespace sc
