#include "nbody.hpp"

#include <algorithm>
#include <cmath>

namespace sc {

namespace {

// Узлы Гаусса–Радау на [0, 1] (Rein & Spiegel 2015, табл. 1)
constexpr double kH[8] = {0.0,
                          0.0562625605369221464656521910318,
                          0.180240691736892364987579942780,
                          0.352624717113169637373907769648,
                          0.547153626330555383001448554766,
                          0.734210177215410531523210605558,
                          0.885320946839095768090359771030,
                          0.977520613561287501891174488626};

// C[j][k]: коэффициент τ^(k+1) в базисном многочлене Ньютона N_j(τ) = τ·∏_{l=1..j}(τ − h_l), j, k = 0..6.
// b_k = Σ_{j ≥ k} C[j][k]·g_j.
struct NewtonTable {
    double c[7][7] = {};
    NewtonTable() {
        for (int j = 0; j < 7; ++j) {
            long double p[8] = {0};  // p[m] — коэффициент τ^m
            p[1] = 1.0L;              // τ
            for (int l = 1; l <= j; ++l) {
                long double q[8] = {0};
                for (int m = 0; m < 7; ++m) {
                    q[m + 1] += p[m];
                    q[m] -= p[m] * static_cast<long double>(kH[l]);
                }
                for (int m = 0; m < 8; ++m) p[m] = q[m];
            }
            for (int k = 0; k < 7; ++k) c[j][k] = static_cast<double>(p[k + 1]);
        }
    }
};
const NewtonTable kNT;

// коэффициенты в x(τ) и v(τ): ∫∫ τ^(k+1) → τ^(k+3)/((k+2)(k+3)), ∫ τ^(k+1) → τ^(k+2)/(k+2)
constexpr double kX[7] = {1.0 / 6, 1.0 / 12, 1.0 / 20, 1.0 / 30, 1.0 / 42, 1.0 / 56, 1.0 / 72};
constexpr double kV[7] = {1.0 / 2, 1.0 / 3, 1.0 / 4, 1.0 / 5, 1.0 / 6, 1.0 / 7, 1.0 / 8};

void b_from_g(const std::vector<V3> *g, std::vector<V3> *b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < 7; ++k) {
            V3 s;
            for (int j = 6; j >= k; --j) s += g[j][i] * kNT.c[j][k];
            b[k][i] = s;
        }
}

void g_from_b(const std::vector<V3> *b, std::vector<V3> *g, size_t n) {
    // треугольная система: b_k = g_k + Σ_{j>k} C[j][k]·g_j (C[k][k] = 1)
    for (size_t i = 0; i < n; ++i)
        for (int k = 6; k >= 0; --k) {
            V3 s = b[k][i];
            for (int j = k + 1; j < 7; ++j) s -= g[j][i] * kNT.c[j][k];
            g[k][i] = s;
        }
}

inline void kahan_add(double &sum, double &comp, double add) {
    const double y = add - comp;
    const double t = sum + y;
    comp = (t - sum) - y;
    sum = t;
}

double max_abs(const V3 &v) { return std::max({std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}); }

}  // namespace

void Simulation::reset() {
    have_state_ = false;
    have_b_ = false;
    have_a0_ = false;
    t_end_dt_ = 0.0;
}

void Simulation::accelerations(const std::vector<V3> &x, const std::vector<V3> &v, std::vector<V3> &a) const {
    std::vector<V3> local(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i)
        if (bodies[i].parent >= 0) local[i] = x[i] - x[bodies[i].parent];
    forces(x, v, local, t, a);
}

void Simulation::accel_internal(const std::vector<V3> &q, const std::vector<V3> &u, double et,
                                std::vector<V3> &a) const {
    const size_t n = bodies.size();
    std::vector<V3> X(n), V(n), local(n), A;
    for (size_t i = 0; i < n; ++i) {
        const int p = bodies[i].parent;
        X[i] = p >= 0 ? q[i] + q[p] : q[i];
        V[i] = p >= 0 ? u[i] + u[p] : u[i];
        if (p >= 0) local[i] = q[i];
    }
    forces(X, V, local, et, A);
    a.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const int p = bodies[i].parent;
        a[i] = p >= 0 ? A[i] - A[p] : A[i];
    }
}

void Simulation::forces(const std::vector<V3> &X, const std::vector<V3> &V, const std::vector<V3> &local,
                        double et, std::vector<V3> &a) const {
    const size_t n = bodies.size();
    // вектор от i к j: внутри одной планетной системы — из относительных координат (точнее)
    auto sep = [&](size_t i, size_t j) { return top(i) == top(j) ? local[j] - local[i] : X[j] - X[i]; };
    a.assign(n, V3{});
    // ньютоновская часть
    for (size_t i = 0; i < n; ++i) {
        const double gmi = bodies[i].gm;
        for (size_t j = i + 1; j < n; ++j) {
            const double gmj = bodies[j].gm;
            if (gmi == 0.0 && gmj == 0.0) continue;
            const V3 d = sep(i, j);
            const double r2 = dot(d, d);
            const double inv3 = 1.0 / (r2 * std::sqrt(r2));
            a[i] += d * (gmj * inv3);
            a[j] -= d * (gmi * inv3);
        }
    }
    if (opt.gr) {
        // EIH, β = γ = 1. an — ньютоновские ускорения, phi — потенциалы GM/r от остальных тел.
        constexpr double c2 = kC * kC;
        const std::vector<V3> an = a;
        std::vector<double> phi(n, 0.0);
        for (size_t i = 0; i < n; ++i)
            for (size_t k = 0; k < n; ++k)
                if (k != i && bodies[k].gm != 0.0) phi[i] += bodies[k].gm / norm(sep(i, k));
        for (size_t i = 0; i < n; ++i) {
            const double vi2 = dot(V[i], V[i]);
            V3 corr;
            for (size_t j = 0; j < n; ++j) {
                const double gmj = bodies[j].gm;
                if (j == i || gmj == 0.0) continue;
                const V3 rji = sep(i, j);  // от i к j
                const double r = norm(rji);
                const double inv3 = 1.0 / (r * r * r);
                const double rv = -dot(rji, V[j]) / r;
                const double f = -4.0 * phi[i] / c2 - phi[j] / c2 + vi2 / c2 + 2.0 * dot(V[j], V[j]) / c2 -
                                 4.0 * dot(V[i], V[j]) / c2 - 1.5 * rv * rv / c2 + 0.5 * dot(rji, an[j]) / c2;
                corr += rji * (gmj * inv3 * f);
                corr += (V[i] - V[j]) * (gmj * inv3 * -dot(rji, V[i] * 4.0 - V[j] * 3.0) / c2);
                corr += an[j] * (3.5 * gmj / (r * c2));
            }
            a[i] += corr;
        }
    }
    if (opt.j2) {
        // Φ = Σₙ GM·Jₙ·Rⁿ·r^−(n+1)·Pₙ(s), s = z/r; a = −∇Φ = GM Σₙ Jₙ Rⁿ / r^(n+2) · [((n+1)Pₙ + s·Pₙ′)·r̂ − Pₙ′·k]
        for (size_t j = 0; j < n; ++j) {
            const Body &bj = bodies[j];
            if (bj.field && bj.gm != 0.0) {
                // полное поле во вращающейся системе тела
                const M3 rot = bj.field->to_body(et);
                const M3 inv = rot.transposed();
                for (size_t i = 0; i < n; ++i) {
                    if (i == j) continue;
                    const V3 acc = inv * bj.field->accel_body(bj.gm, rot * sep(j, i));
                    a[i] += acc;
                    if (bodies[i].gm != 0.0) a[j] -= acc * (bodies[i].gm / bj.gm);
                }
                continue;
            }
            if (bj.zonal.size() < 3 || bj.gm == 0.0) continue;
            const int nmax = static_cast<int>(bj.zonal.size()) - 1;
            for (size_t i = 0; i < n; ++i) {
                if (i == j) continue;
                const V3 r = sep(j, i);  // от сплюснутого тела к i
                const double rr = norm(r);
                const V3 rh = r / rr;
                const double s = dot(rh, bj.pole);
                double p0 = 1.0, p1 = s, dp0 = 0.0, dp1 = 1.0;  // Pₙ и Pₙ′
                double radial = 0.0, axial = 0.0, ratio = bj.r_ref / rr, rn = ratio;
                for (int k = 2; k <= nmax; ++k) {
                    const double p2 = ((2 * k - 1) * s * p1 - (k - 1) * p0) / k;
                    const double dp2 = dp0 + (2 * k - 1) * p1;
                    rn *= ratio;  // (R/r)^k
                    const double jk = bj.zonal[k];
                    if (jk != 0.0) {
                        radial += jk * rn * ((k + 1) * p2 + s * dp2);
                        axial += jk * rn * dp2;
                    }
                    p0 = p1; p1 = p2; dp0 = dp1; dp1 = dp2;
                }
                const double f = bj.gm / (rr * rr);
                const V3 acc = (rh * radial - bj.pole * axial) * f;
                a[i] += acc;
                if (bodies[i].gm != 0.0) a[j] -= acc * (bodies[i].gm / bj.gm);  // реакция на сплюснутое тело
            }
        }
    }
}

void Simulation::to_internal() {
    const size_t n = bodies.size();
    q_.resize(n);
    u_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        int p = bodies[i].parent;
        if (p >= static_cast<int>(n) || p == static_cast<int>(i) || (p >= 0 && bodies[p].parent >= 0)) {
            bodies[i].parent = -1;  // допускается один уровень: спутник → планета
            p = -1;
        }
        q_[i] = p >= 0 ? bodies[i].x - bodies[p].x : bodies[i].x;
        u_[i] = p >= 0 ? bodies[i].v - bodies[p].v : bodies[i].v;
    }
    csx_.assign(n, V3{});
    csv_.assign(n, V3{});
    have_state_ = true;
}

void Simulation::to_bodies() {
    for (size_t i = 0; i < bodies.size(); ++i) {
        const int p = bodies[i].parent;
        bodies[i].x = p >= 0 ? q_[i] + q_[p] : q_[i];
        bodies[i].v = p >= 0 ? u_[i] + u_[p] : u_[i];
    }
}

bool Simulation::try_step(double h) {
    const size_t n = bodies.size();
    for (int k = 0; k < 7; ++k) {
        b_[k].resize(n);
        g_[k].resize(n);
    }
    if (!have_b_)
        for (int k = 0; k < 7; ++k) std::fill(b_[k].begin(), b_[k].end(), V3{});
    g_from_b(b_, g_, n);

    xs_.resize(n);
    vs_.resize(n);
    std::vector<V3> b6_prev(n);
    double a_max = 0.0;
    int it = 0;
    double pc_err = HUGE_VAL, pc_err_prev = HUGE_VAL;
    for (; it < opt.max_iterations; ++it) {
        for (size_t i = 0; i < n; ++i) b6_prev[i] = b_[6][i];
        for (int s = 1; s <= 7; ++s) {
            const double tau = kH[s];
            for (size_t i = 0; i < n; ++i) {
                V3 px = a0_[i] * 0.5, pv = a0_[i];
                double tk = tau;
                for (int k = 0; k < 7; ++k) {
                    px += b_[k][i] * (kX[k] * tk);
                    pv += b_[k][i] * (kV[k] * tk);
                    tk *= tau;
                }
                xs_[i] = q_[i] + u_[i] * (h * tau) + px * (h * h * tau * tau);
                vs_[i] = u_[i] + pv * (h * tau);
            }
            accel_internal(xs_, vs_, t + h * tau, as_);
            // разделённые разности Ньютона по узлам 0, h1..hs; g1..g(s−1) уже обновлены в этом проходе
            for (size_t i = 0; i < n; ++i) {
                V3 tmp = (as_[i] - a0_[i]) / kH[s];
                for (int m = 1; m < s; ++m) tmp = (tmp - g_[m - 1][i]) / (kH[s] - kH[m]);
                g_[s - 1][i] = tmp;
            }
            b_from_g(g_, b_, n);
        }
        double db = 0.0;
        a_max = 0.0;
        for (size_t i = 0; i < n; ++i) {
            db = std::max(db, max_abs(b_[6][i] - b6_prev[i]));
            a_max = std::max(a_max, max_abs(as_[i]));
        }
        pc_err_prev = pc_err;
        pc_err = a_max > 0.0 ? db / a_max : 0.0;
        if (!std::isfinite(pc_err)) {
            error_ = "NaN в предикторе-корректоре";
            return false;
        }
        if (pc_err < 1e-16) break;
        if (it > 2 && pc_err >= pc_err_prev) break;  // дальше не сходится: округление
    }
    iterations += it + 1;
    if (pc_err >= 1e-13) ++not_converged;

    // оценка ошибки шага: по каждому телу max|b6| / max|a|, берётся наибольшее
    double err = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double am = max_abs(a0_[i]);
        if (am > 0.0) err = std::max(err, max_abs(b_[6][i]) / am);
    }
    double h_new;
    if (err > 0.0 && std::isfinite(err))
        h_new = h * std::pow(opt.epsilon / err, 1.0 / 7.0);
    else
        h_new = h / opt.safety;
    if (std::fabs(h_new / h) < opt.safety) {
        // шаг отвергнут: тот же многочлен в новом масштабе времени как начальное приближение
        const double q = h_new / h;
        double qk = q;
        for (int k = 0; k < 7; ++k, qk *= q)
            for (size_t i = 0; i < n; ++i) b_[k][i] *= qk;
        have_b_ = true;
        dt = h_new;
        ++rejected;
        return false;
    }
    if (std::fabs(h_new / h) > 1.0 / opt.safety) h_new = h / opt.safety;

    // принять шаг: τ = 1, компенсированное суммирование
    for (size_t i = 0; i < n; ++i) {
        V3 px = a0_[i] * 0.5, pv = a0_[i];
        for (int k = 0; k < 7; ++k) {
            px += b_[k][i] * kX[k];
            pv += b_[k][i] * kV[k];
        }
        const V3 dx = u_[i] * h + px * (h * h);
        const V3 dv = pv * h;
        for (int c = 0; c < 3; ++c) {
            kahan_add(q_[i][c], csx_[i][c], dx[c]);
            kahan_add(u_[i][c], csv_[i][c], dv[c]);
        }
    }
    to_bodies();
    t += h;
    ++steps;
    dt = h_new;
    predict_next(h_new / h);
    return true;
}

void Simulation::predict_next(double q) {
    // многочлен прошлого шага в новом времени: s = 1 + q·τ', a(s) − a(1) = Σ_k b_k·((1 + qτ')^(k+1) − 1)
    // → b'_(m−1) = q^m · Σ_{k ≥ m−1} C(k+1, m)·b_k
    const size_t n = bodies.size();
    static const double binom[9][9] = {
        {1}, {1, 1}, {1, 2, 1}, {1, 3, 3, 1}, {1, 4, 6, 4, 1}, {1, 5, 10, 10, 5, 1},
        {1, 6, 15, 20, 15, 6, 1}, {1, 7, 21, 35, 35, 21, 7, 1}, {1, 8, 28, 56, 70, 56, 28, 8, 1}};
    std::vector<V3> nb[7];
    for (int m = 1; m <= 7; ++m) {
        nb[m - 1].assign(n, V3{});
        const double qm = std::pow(q, m);
        for (size_t i = 0; i < n; ++i) {
            V3 s;
            for (int k = m - 1; k < 7; ++k) s += b_[k][i] * binom[k + 1][m];
            nb[m - 1][i] = s * qm;
        }
    }
    for (int k = 0; k < 7; ++k) b_[k].swap(nb[k]);
    have_b_ = true;
    have_a0_ = false;
}

bool Simulation::step() {
    const size_t n = bodies.size();
    if (n == 0) return true;
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (!have_state_) to_internal();
        if (!have_a0_) {
            accel_internal(q_, u_, t, a0_);
            have_a0_ = true;
        }
        if (std::fabs(dt) < opt.min_dt && t_end_dt_ == 0.0) {  // укороченный финишный шаг может быть сколь угодно мал
            error_ = "шаг меньше минимального";
            return false;
        }
        if (try_step(dt)) return true;
        if (*error_) return false;
    }
    error_ = "шаг отвергнут 50 раз подряд";
    return false;
}

bool Simulation::integrate(double t_end, int64_t max_steps) {
    error_ = "";
    if (t_end == t) return true;
    const double dir = t_end > t ? 1.0 : -1.0;
    if (dt * dir <= 0.0) {
        dt = std::fabs(dt) * dir;
        reset();
    }
    if (t_end_dt_ != 0.0) {
        // прошлый вызов укоротил шаг до финиша: вернуть обычный шаг и перемасштабировать предсказание
        const double q = t_end_dt_ / dt;
        double qk = q;
        for (int k = 0; k < 7; ++k, qk *= q)
            for (auto &b : b_[k]) b *= qk;
        dt = t_end_dt_;
        t_end_dt_ = 0.0;
    }
    int64_t done = 0;
    while ((t_end - t) * dir > 0.0) {
        if (max_steps >= 0 && done >= max_steps) return true;
        const double remaining = t_end - t;
        if (std::fabs(dt) > std::fabs(remaining)) {
            // последний шаг: укоротить, многочлен-приближение перемасштабировать
            const double q = remaining / dt;
            double qk = q;
            for (int k = 0; k < 7; ++k, qk *= q)
                for (auto &b : b_[k]) b *= qk;
            t_end_dt_ = dt;
            dt = remaining;
        }
        if (!step()) return false;
        ++done;
    }
    t = t_end;  // убрать накопленное округление времени
    return true;
}

double Simulation::energy() const {
    const size_t n = bodies.size();
    double e = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const Body &bi = bodies[i];
        e += 0.5 * bi.gm * dot(bi.v, bi.v);
        for (size_t j = i + 1; j < n; ++j) {
            const Body &bj = bodies[j];
            e -= bi.gm * bj.gm / norm(bj.x - bi.x);
        }
    }
    if (opt.j2)
        for (size_t j = 0; j < n; ++j) {
            const Body &bj = bodies[j];
            if (bj.field && bj.gm != 0.0) {
                const M3 rot = bj.field->to_body(t);
                for (size_t i = 0; i < n; ++i)
                    if (i != j && bodies[i].gm != 0.0)
                        e -= bodies[i].gm * bj.field->potential_body(bj.gm, rot * (bodies[i].x - bj.x));
                continue;
            }
            if (bj.zonal.size() < 3 || bj.gm == 0.0) continue;
            for (size_t i = 0; i < n; ++i) {
                if (i == j || bodies[i].gm == 0.0) continue;
                const V3 r = bodies[i].x - bj.x;
                const double rr = norm(r), s = dot(r, bj.pole) / rr;
                double p0 = 1.0, p1 = s, phi = 0.0, ratio = bj.r_ref / rr, rn = ratio;
                for (size_t k = 2; k < bj.zonal.size(); ++k) {
                    const double p2 = ((2.0 * k - 1) * s * p1 - (k - 1.0) * p0) / k;
                    rn *= ratio;
                    phi += bj.zonal[k] * rn * p2;
                    p0 = p1; p1 = p2;
                }
                e += bodies[i].gm * bj.gm / rr * phi;
            }
        }
    return e;
}

V3 Simulation::angular_momentum() const {
    V3 l;
    for (const Body &b : bodies) l += cross(b.x, b.v) * b.gm;
    return l;
}

V3 Simulation::momentum() const {
    V3 p;
    for (const Body &b : bodies) p += b.v * b.gm;
    return p;
}

V3 Simulation::center_of_mass() const {
    V3 c;
    double m = 0.0;
    for (const Body &b : bodies) {
        c += b.x * b.gm;
        m += b.gm;
    }
    return m > 0.0 ? c / m : c;
}

}  // namespace sc
