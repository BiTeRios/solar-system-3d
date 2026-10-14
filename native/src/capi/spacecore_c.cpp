// C-API ядра для тестов из Python (ctypes). В игру не входит: там работает привязка Godot (src/godot).
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "core/catalog.hpp"
#include "core/ephemeris.hpp"
#include "core/nbody.hpp"
#include "core/orientation.hpp"
#include "core/sim_runner.hpp"
#include "core/timeconv.hpp"

#define SC_API extern "C" __declspec(dllexport)

using namespace sc;

// ---------- N-body ----------

SC_API void *sc_sim_new() { return new Simulation(); }
SC_API void sc_sim_free(void *s) { delete static_cast<Simulation *>(s); }

SC_API int sc_sim_add(void *s, double gm, const double *xv, double j2, double r_ref, const double *pole, int parent) {
    auto *sim = static_cast<Simulation *>(s);
    Body b;
    b.gm = gm;
    b.x = {xv[0], xv[1], xv[2]};
    b.v = {xv[3], xv[4], xv[5]};
    if (j2 != 0.0) b.zonal = {0.0, 0.0, j2};
    b.r_ref = r_ref;
    if (pole) b.pole = {pole[0], pole[1], pole[2]};
    b.parent = parent;
    sim->bodies.push_back(b);
    sim->reset();
    return static_cast<int>(sim->bodies.size()) - 1;
}

// Полное поле тяготения тела: c, s — массивы по индексу n·(n+1)/2 + m; ориентация — полюс и вращение.
SC_API void sc_sim_set_field(void *s, int index, int nmax, const double *c, const double *sn, double r_ref,
                             double ra0, double dec0, double ra_rate, double dec_rate, double w0, double wdot,
                             double epoch_et) {
    auto *sim = static_cast<Simulation *>(s);
    auto f = std::make_shared<HarmonicField>();
    f->resize(nmax);
    std::memcpy(f->c.data(), c, sizeof(double) * f->c.size());
    std::memcpy(f->s.data(), sn, sizeof(double) * f->s.size());
    f->r_ref = r_ref;
    f->ra0 = ra0;
    f->dec0 = dec0;
    f->ra_rate = ra_rate;
    f->dec_rate = dec_rate;
    f->w0 = w0;
    f->wdot = wdot;
    f->epoch_et = epoch_et;
    sim->bodies[static_cast<size_t>(index)].field = f;
    sim->reset();
}

// Зональные гармоники тела: jn[0..n_max], используются jn[2..n_max].
SC_API void sc_sim_set_zonal(void *s, int index, const double *jn, int n_max) {
    auto *sim = static_cast<Simulation *>(s);
    sim->bodies[static_cast<size_t>(index)].zonal.assign(jn, jn + n_max + 1);
    sim->reset();
}

SC_API void sc_sim_configure(void *s, int gr, int j2, double epsilon, double t0, double dt) {
    auto *sim = static_cast<Simulation *>(s);
    sim->opt.gr = gr != 0;
    sim->opt.j2 = j2 != 0;
    sim->opt.epsilon = epsilon;
    sim->t = t0;
    sim->dt = dt;
    sim->reset();
}

SC_API int sc_sim_integrate(void *s, double t_end) { return static_cast<Simulation *>(s)->integrate(t_end) ? 1 : 0; }
SC_API int sc_sim_count(void *s) { return static_cast<int>(static_cast<Simulation *>(s)->bodies.size()); }
SC_API double sc_sim_time(void *s) { return static_cast<Simulation *>(s)->t; }
SC_API const char *sc_sim_error(void *s) { return static_cast<Simulation *>(s)->error(); }

SC_API void sc_sim_states(void *s, double *out) {
    for (const Body &b : static_cast<Simulation *>(s)->bodies) {
        for (int c = 0; c < 3; ++c) out[c] = b.x[c];
        for (int c = 0; c < 3; ++c) out[3 + c] = b.v[c];
        out += 6;
    }
}

SC_API void sc_sim_stats(void *s, long long *out) {
    auto *sim = static_cast<Simulation *>(s);
    out[0] = sim->steps;
    out[1] = sim->rejected;
    out[2] = sim->iterations;
    out[3] = sim->not_converged;
}

SC_API double sc_sim_energy(void *s) { return static_cast<Simulation *>(s)->energy(); }

SC_API void sc_sim_angmom(void *s, double *out) {
    const V3 l = static_cast<Simulation *>(s)->angular_momentum();
    out[0] = l.x;
    out[1] = l.y;
    out[2] = l.z;
}

SC_API void sc_sim_momentum(void *s, double *out) {
    const V3 p = static_cast<Simulation *>(s)->momentum();
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

// ---------- эфемериды ----------

SC_API void *sc_eph_open(const char *path) {
    auto e = std::make_unique<Ephemeris>();
    if (!e->open(std::string(path))) return nullptr;
    return e.release();
}
SC_API void sc_eph_close(void *e) { delete static_cast<Ephemeris *>(e); }

SC_API int sc_eph_state(void *e, int target, int ssb, double et, double *out) {
    auto *eph = static_cast<Ephemeris *>(e);
    V3 p, v;
    const bool ok = ssb ? eph->state_ssb(target, et, p, v) : eph->state_rel(target, et, p, v);
    if (!ok) return 0;
    for (int c = 0; c < 3; ++c) {
        out[c] = p[c];
        out[3 + c] = v[c];
    }
    return 1;
}

// ---------- время ----------

SC_API void *sc_time_new(double dta, double k, double eb, double m0, double m1, int n, const double *epochs,
                         const double *values) {
    auto *t = new TimeModel();
    t->delta_t_a = dta;
    t->k = k;
    t->eb = eb;
    t->m0 = m0;
    t->m1 = m1;
    t->leap_epochs.assign(epochs, epochs + n);
    t->leap_values.assign(values, values + n);
    return t;
}
SC_API void sc_time_free(void *t) { delete static_cast<TimeModel *>(t); }

SC_API double sc_time_utc_to_tdb(void *t, long long y, int mo, int d, int h, int mi, double s) {
    UtcDate u;
    u.year = y;
    u.month = mo;
    u.day = d;
    u.hour = h;
    u.minute = mi;
    u.second = s;
    return static_cast<TimeModel *>(t)->utc_to_tdb(u);
}

SC_API void sc_time_tdb_to_utc(void *t, double et, long long *ymdhm, double *sec) {
    const UtcDate u = static_cast<TimeModel *>(t)->tdb_to_utc(et);
    ymdhm[0] = u.year;
    ymdhm[1] = u.month;
    ymdhm[2] = u.day;
    ymdhm[3] = u.hour;
    ymdhm[4] = u.minute;
    *sec = u.second;
}

SC_API double sc_time_delta_et_utc(void *t, double et) { return static_cast<TimeModel *>(t)->delta_et_utc(et); }

// ---------- ориентация ----------

SC_API void sc_orient_matrix(const double *ra, int nra, const double *dec, int ndec, const double *pm, int npm,
                             const double *nut_ra, int nnra, const double *nut_dec, int nndec, const double *nut_pm,
                             int nnpm, const double *angles, int nangles, int max_phase_degree, double et,
                             double *out9) {
    RotationModel r;
    r.pole_ra.assign(ra, ra + nra);
    r.pole_dec.assign(dec, dec + ndec);
    r.pm.assign(pm, pm + npm);
    r.nut_ra.assign(nut_ra, nut_ra + nnra);
    r.nut_dec.assign(nut_dec, nut_dec + nndec);
    r.nut_pm.assign(nut_pm, nut_pm + nnpm);
    r.system_angles.assign(angles, angles + nangles);
    r.max_phase_degree = max_phase_degree;
    const M3 m = r.j2000_to_body(et);
    std::memcpy(out9, m.m, sizeof(double) * 9);
}

// ---------- каталог и «Симуляция» в потоке (этап 4) ----------

namespace {

std::string read_text(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

struct World {
    Ephemeris eph;
    Catalog cat;
    SimRunner runner;
    std::string error;
};

}  // namespace

// Открыть данные игры из каталога (ephemeris.eph, bodies.json, spice_constants.json, dynamics.json).
SC_API void *sc_world_new(const char *data_dir) {
    auto w = std::make_unique<World>();
    const std::string d = std::string(data_dir) + "/";
    if (!w->eph.open(d + "ephemeris.eph")) return nullptr;
    std::string err;
    if (!w->cat.load(read_text(d + "bodies.json"), read_text(d + "spice_constants.json"), read_text(d + "dynamics.json"),
                     err))
        return nullptr;
    w->runner.attach(&w->cat, &w->eph);
    return w.release();
}
SC_API void sc_world_free(void *w) { delete static_cast<World *>(w); }
SC_API int sc_world_count(void *w) { return static_cast<int>(static_cast<World *>(w)->cat.bodies.size()); }
SC_API int sc_world_naif(void *w, int i) { return static_cast<World *>(w)->cat.bodies[static_cast<size_t>(i)].naif; }
SC_API const char *sc_world_error(void *w) { return static_cast<World *>(w)->error.c_str(); }

// Начальное состояние N-body: на тело — x, v (6), gm, r_ref, pole (3), parent; include — маска по каталогу или NULL.
// Возвращает число тел или −1.
SC_API int sc_world_initial_state(void *wp, double et, const int *include, double *xv, double *gm, double *r_ref,
                                  double *pole, int *parent, int *naif, int *nzonal, double *zonal, int zonal_stride) {
    auto *w = static_cast<World *>(wp);
    std::vector<bool> inc;
    if (include)
        for (size_t i = 0; i < w->cat.bodies.size(); ++i) inc.push_back(include[i] != 0);
    std::vector<SimBody> out;
    if (!w->cat.initial_state(w->eph, et, include ? &inc : nullptr, out, w->error)) return -1;
    for (size_t i = 0; i < out.size(); ++i) {
        const Body &b = out[i].body;
        for (int c = 0; c < 3; ++c) {
            xv[i * 6 + c] = b.x[c];
            xv[i * 6 + 3 + c] = b.v[c];
            pole[i * 3 + c] = b.pole[c];
        }
        gm[i] = b.gm;
        r_ref[i] = b.r_ref;
        parent[i] = b.parent;
        naif[i] = out[i].naif;
        nzonal[i] = b.field ? -1 : static_cast<int>(b.zonal.size());
        for (size_t k = 0; k < b.zonal.size() && static_cast<int>(k) < zonal_stride; ++k)
            zonal[i * static_cast<size_t>(zonal_stride) + k] = b.zonal[k];
    }
    return static_cast<int>(out.size());
}

SC_API int sc_runner_seed(void *wp, double et, int min_level, int max_level, int gr) {
    auto *w = static_cast<World *>(wp);
    w->runner.min_level = min_level;
    w->runner.max_level = max_level;
    w->runner.options.gr = gr != 0;
    return w->runner.seed(et, w->error) ? 1 : 0;
}
SC_API void sc_runner_request(void *wp, double t, double rate) { static_cast<World *>(wp)->runner.request(t, rate); }
SC_API int sc_runner_range(void *wp, double *lo, double *hi) {
    return static_cast<World *>(wp)->runner.range(*lo, *hi) ? 1 : 0;
}
// Состояния всех тел каталога на момент t: n × 6.
SC_API int sc_runner_state(void *wp, double t, double *xv) {
    auto *w = static_cast<World *>(wp);
    std::vector<V3> x, v;
    if (!w->runner.state_at(t, x, v)) return 0;
    for (size_t i = 0; i < x.size(); ++i)
        for (int c = 0; c < 3; ++c) {
            xv[i * 6 + c] = x[i][c];
            xv[i * 6 + 3 + c] = v[i][c];
        }
    return 1;
}
SC_API int sc_runner_level(void *wp) { return static_cast<World *>(wp)->runner.level(); }
// Моменты смены уровня: не больше max штук в out, возвращает число.
SC_API int sc_runner_switches(void *wp, double *out, int max) {
    const std::vector<double> s = static_cast<World *>(wp)->runner.level_switches();
    const int n = std::min(max, static_cast<int>(s.size()));
    for (int i = 0; i < n; ++i) out[i] = s[static_cast<size_t>(i)];
    return n;
}
SC_API double sc_runner_throughput(void *wp, int level) { return static_cast<World *>(wp)->runner.throughput(level); }
SC_API long long sc_runner_steps(void *wp) { return static_cast<World *>(wp)->runner.steps(); }
SC_API int sc_runner_running(void *wp) { return static_cast<World *>(wp)->runner.running() ? 1 : 0; }
SC_API void sc_runner_stop(void *wp) { static_cast<World *>(wp)->runner.stop(); }
// Ошибка потока расчёта (пустая строка — нет). Буфер живёт до следующего вызова.
SC_API const char *sc_runner_error(void *wp) {
    auto *w = static_cast<World *>(wp);
    w->error = w->runner.error();
    return w->error.c_str();
}
