#include "sim_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sc {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// Интерполяция Эрмита 5-й степени по x, v, a на концах отрезка; s ∈ [0, 1], h — длина отрезка.
void hermite5(const V3 &x0, const V3 &v0, const V3 &a0, const V3 &x1, const V3 &v1, const V3 &a1, double h, double s,
              V3 &x, V3 &v) {
    const double s2 = s * s, s3 = s2 * s, s4 = s3 * s, s5 = s4 * s;
    const double h0 = 1 - 10 * s3 + 15 * s4 - 6 * s5;
    const double h1 = s - 6 * s3 + 8 * s4 - 3 * s5;
    const double h2 = 0.5 * (s2 - 3 * s3 + 3 * s4 - s5);
    const double h3 = 0.5 * (s3 - 2 * s4 + s5);
    const double h4 = -4 * s3 + 7 * s4 - 3 * s5;
    const double h5 = 10 * s3 - 15 * s4 + 6 * s5;
    x = x0 * h0 + x1 * h5 + (v0 * h1 + v1 * h4) * h + (a0 * h2 + a1 * h3) * (h * h);
    const double d0 = -30 * s2 + 60 * s3 - 30 * s4;
    const double d1 = 1 - 18 * s2 + 32 * s3 - 15 * s4;
    const double d2 = 0.5 * (2 * s - 9 * s2 + 12 * s3 - 5 * s4);
    const double d3 = 0.5 * (3 * s2 - 8 * s3 + 5 * s4);
    const double d4 = -12 * s2 + 28 * s3 - 15 * s4;
    const double d5 = 30 * s2 - 60 * s3 + 30 * s4;
    v = (x0 * d0 + x1 * d5) / h + (v0 * d1 + v1 * d4) + (a0 * d2 + a1 * d3) * h;
}

constexpr size_t kMaxSnapshots = 20000;

}  // namespace

void SimSetup::expand(double t, const std::vector<V3> &x, const std::vector<V3> &v, std::vector<V3> &cx,
                      std::vector<V3> &cv) const {
    cx.assign(cat_to_sim.size(), V3{});
    cv.assign(cat_to_sim.size(), V3{});
    for (size_t i = 0; i < sim_to_cat.size(); ++i) {
        cx[static_cast<size_t>(sim_to_cat[i])] = x[i];
        cv[static_cast<size_t>(sim_to_cat[i])] = v[i];
    }
    if (kin.empty()) return;
    // центр планеты = барицентр системы − Σ GMₖ·rₖ / GM системы
    std::vector<V3> sx(sim_to_cat.size()), sv(sim_to_cat.size()), rk(kin.size()), vk(kin.size());
    for (size_t k = 0; k < kin.size(); ++k) {
        kin[k].orbit.state(t, rk[k], vk[k]);
        sx[static_cast<size_t>(kin[k].planet_sim)] += rk[k] * kin[k].gm;
        sv[static_cast<size_t>(kin[k].planet_sim)] += vk[k] * kin[k].gm;
    }
    for (size_t i = 0; i < sim_to_cat.size(); ++i) {
        if (merged_gm[i] <= 0) continue;
        cx[static_cast<size_t>(sim_to_cat[i])] = x[i] - sx[i] / merged_gm[i];
        cv[static_cast<size_t>(sim_to_cat[i])] = v[i] - sv[i] / merged_gm[i];
    }
    for (size_t k = 0; k < kin.size(); ++k) {
        const size_t p = static_cast<size_t>(sim_to_cat[static_cast<size_t>(kin[k].planet_sim)]);
        cx[static_cast<size_t>(kin[k].cat)] = cx[p] + rk[k];
        cv[static_cast<size_t>(kin[k].cat)] = cv[p] + vk[k];
    }
}

SimRunner::SimRunner() {
    // начальные оценки скорости счёта, с/с (замер на RTX-машине разработки, этап 4); уточняются во время работы
    tp_[0] = 4.5e5;
    tp_[1] = 2.2e6;
    tp_[2] = 2.0e8;
}

SimRunner::~SimRunner() { stop(); }

void SimRunner::attach(const Catalog *catalog, const Ephemeris *eph) {
    stop();
    cat_ = catalog;
    eph_ = eph;
}

int SimRunner::kinematic_from(int /*level*/, int cat) const {
    const BodyInfo &b = cat_->bodies[static_cast<size_t>(cat)];
    if (b.kind != "moon" || b.parent < 0) return 99;
    const int p = cat_->bodies[static_cast<size_t>(b.parent)].naif;
    if (p == 499) return 1;
    if (p == 399) return 99;  // Луна интегрируется на всех уровнях
    return 2;
}

std::shared_ptr<SimSetup> SimRunner::make_setup(int level, double t, const std::vector<V3> &cx,
                                                const std::vector<V3> &cv, std::vector<V3> &sx,
                                                std::vector<V3> &sv) const {
    auto st = std::make_shared<SimSetup>();
    st->level = level;
    const size_t n = cat_->bodies.size();
    st->cat_to_sim.assign(n, -1);
    std::vector<bool> kin(n, false);
    for (size_t i = 0; i < n; ++i) kin[i] = kinematic_from(level, static_cast<int>(i)) <= level;
    for (size_t i = 0; i < n; ++i) {
        if (kin[i]) continue;
        st->cat_to_sim[i] = static_cast<int>(st->sim_to_cat.size());
        st->sim_to_cat.push_back(static_cast<int>(i));
    }
    st->merged_gm.assign(st->sim_to_cat.size(), 0.0);
    sx.assign(st->sim_to_cat.size(), V3{});
    sv.assign(st->sim_to_cat.size(), V3{});
    for (size_t j = 0; j < st->sim_to_cat.size(); ++j) {
        const size_t c = static_cast<size_t>(st->sim_to_cat[j]);
        sx[j] = cx[c];
        sv[j] = cv[c];
    }
    // объединённые системы: барицентр планеты и её спутников на элементах
    for (size_t j = 0; j < st->sim_to_cat.size(); ++j) {
        const size_t p = static_cast<size_t>(st->sim_to_cat[j]);
        double total = templates_[p].body.gm;
        V3 mx = cx[p] * total, mv = cv[p] * total;
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            if (!kin[i] || cat_->bodies[i].parent != static_cast<int>(p)) continue;
            any = true;
            const double g = templates_[i].body.gm;
            total += g;
            mx += cx[i] * g;
            mv += cv[i] * g;
            SimSetup::Kin k;
            k.cat = static_cast<int>(i);
            k.planet_sim = static_cast<int>(j);
            k.gm = g;
            const Body &pb = templates_[p].body;
            double j2 = pb.zonal.size() > 2 ? pb.zonal[2] : 0.0, rr = pb.r_ref;
            V3 pole = pb.pole;
            if (pb.field) {
                j2 = -pb.field->C(2, 0);
                rr = pb.field->r_ref;
                const M3 m = pb.field->to_body(t);
                pole = {m.m[2][0], m.m[2][1], m.m[2][2]};
            }
            k.orbit = kinematic_from_state(cx[i] - cx[p], cv[i] - cv[p], pb.gm + g, pole, j2, rr, t);
            st->kin.push_back(k);
        }
        if (any) {
            st->merged_gm[j] = total;
            sx[j] = mx / total;
            sv[j] = mv / total;
        }
    }
    return st;
}

void SimRunner::load_sim(const SimSnapshot &s) {
    const SimSetup &st = *s.setup;
    const double dt = sim_.dt;
    sim_.bodies.clear();
    for (size_t j = 0; j < st.sim_to_cat.size(); ++j) {
        const size_t c = static_cast<size_t>(st.sim_to_cat[j]);
        Body b = templates_[c].body;
        if (st.merged_gm[j] > 0) {
            b.gm = st.merged_gm[j];
            b.zonal.clear();
            b.field.reset();
        }
        b.parent = b.parent >= 0 ? st.cat_to_sim[static_cast<size_t>(b.parent)] : -1;
        b.x = s.x[j];
        b.v = s.v[j];
        sim_.bodies.push_back(std::move(b));
    }
    sim_.opt = options;
    sim_.t = s.t;
    sim_.dt = dt != 0 ? dt : 600.0;
    sim_.reset();
    sim_setup_ = s.setup;
}

void SimRunner::fill_accel(SimSnapshot &s) {
    std::vector<V3> a;
    sim_.accelerations(s.x, s.v, a);
    s.a = std::move(a);
}

bool SimRunner::seed(double et, std::string &error) {
    stop();
    if (!cat_ || !eph_) {
        error = "нет каталога или эфемерид";
        return false;
    }
    if (!cat_->initial_state(*eph_, et, nullptr, templates_, error)) return false;
    if (adjust) adjust(templates_);
    std::vector<V3> cx, cv;
    for (const SimBody &b : templates_) {
        cx.push_back(b.body.x);
        cv.push_back(b.body.v);
    }
    const int lvl = std::max(min_level, std::min(max_level, choose_level(req_rate_)));
    SimSnapshot s;
    s.t = et;
    s.setup = make_setup(lvl, et, cx, cv, s.x, s.v);
    sim_.dt = 600.0;
    load_sim(s);
    sim_.t = et;
    fill_accel(s);
    {
        std::lock_guard<std::mutex> lk(mu_);
        snaps_.clear();
        snaps_.push_back(std::move(s));
        req_t_ = et;
        error_.clear();
        level_ = lvl;
    }
    quit_ = false;
    running_ = true;
    thread_ = std::thread(&SimRunner::loop, this);
    return true;
}

void SimRunner::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_ = false;
    std::lock_guard<std::mutex> lk(mu_);
    snaps_.clear();
}

void SimRunner::request(double t, double rate) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        req_t_ = t;
        req_rate_ = rate;
    }
    cv_.notify_all();
}

double SimRunner::throughput(int level) const {
    std::lock_guard<std::mutex> lk(mu_);
    return (level >= 0 && level < kLevels) ? tp_[level] : 0.0;
}

std::string SimRunner::error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

int SimRunner::choose_level(double rate) const {
    const double r = std::fabs(rate);
    const int cur = level_;
    for (int l = min_level; l <= max_level; ++l) {
        // вниз — с запасом, чтобы уровень не прыгал туда-обратно
        const double need = l < cur ? 3.0 * r : 1.3 * r;
        if (tp_[l] >= need) return l;
    }
    return max_level;
}

std::vector<double> SimRunner::level_switches() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<double> out;
    for (size_t i = 0; i + 1 < snaps_.size(); ++i)
        if (snaps_[i].setup != snaps_[i + 1].setup) out.push_back(snaps_[i + 1].t);
    return out;
}

bool SimRunner::range(double &lo, double &hi) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (snaps_.empty()) return false;
    lo = snaps_.front().t;
    hi = snaps_.back().t;
    if (lo > hi) std::swap(lo, hi);
    return true;
}

bool SimRunner::interpolate(const SimSnapshot &a, const SimSnapshot &b, double t, std::vector<V3> &x,
                            std::vector<V3> &v) const {
    const double h = b.t - a.t;
    std::vector<V3> sx(a.x.size()), sv(a.x.size());
    if (h == 0) {
        sx = a.x;
        sv = a.v;
    } else {
        const double s = (t - a.t) / h;
        for (size_t i = 0; i < a.x.size(); ++i) hermite5(a.x[i], a.v[i], a.a[i], b.x[i], b.v[i], b.a[i], h, s, sx[i], sv[i]);
    }
    a.setup->expand(t, sx, sv, x, v);
    return true;
}

bool SimRunner::state_at(double t, std::vector<V3> &x, std::vector<V3> &v) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (snaps_.empty()) return false;
    if (snaps_.size() == 1) {
        if (t != snaps_[0].t) return false;
        snaps_[0].setup->expand(t, snaps_[0].x, snaps_[0].v, x, v);
        return true;
    }
    // снимки упорядочены по времени (по возрастанию)
    if (t < snaps_.front().t || t > snaps_.back().t) return false;
    size_t lo = 0, hi = snaps_.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (snaps_[mid].t <= t) lo = mid;
        else hi = mid;
    }
    // на смене уровня два снимка с одним временем и разным составом: берём пару с общим составом
    for (size_t i = (lo > 0 ? lo - 1 : 0); i + 1 < snaps_.size() && i <= lo + 1; ++i) {
        const SimSnapshot &a = snaps_[i], &b = snaps_[i + 1];
        if (a.setup == b.setup && a.t <= t && t <= b.t) return interpolate(a, b, t, x, v);
    }
    // точное попадание на снимок
    for (size_t i = (lo > 0 ? lo - 1 : 0); i < snaps_.size() && i <= lo + 2; ++i)
        if (snaps_[i].t == t) {
            snaps_[i].setup->expand(t, snaps_[i].x, snaps_[i].v, x, v);
            return true;
        }
    return false;
}

void SimRunner::loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!quit_) {
        const double rate = req_rate_;
        const double dir = rate >= 0 ? 1.0 : -1.0;
        // считаем с запасом вперёд: четверть секунды при текущей скорости, но не меньше суток модельного времени
        const double ahead = std::max(std::fabs(rate) * 0.25, 86400.0);
        const double goal = req_t_ + dir * ahead;
        const int want = std::max(min_level, std::min(max_level, choose_level(rate)));
        // Время замедлили: запас впереди посчитан на более грубом уровне — выбрасываем его от момента рендера,
        // иначе спутники ещё долго шли бы на элементах (при 2 годах/с запас — полгода).
        if (dir > 0 && snaps_.back().setup->level > want) {
            size_t k = 0;
            while (k + 1 < snaps_.size() && snaps_[k].t < req_t_) ++k;
            snaps_.erase(snaps_.begin() + static_cast<std::ptrdiff_t>(k) + 1, snaps_.end());
        } else if (dir < 0 && snaps_.front().setup->level > want) {
            size_t k = snaps_.size() - 1;
            while (k > 0 && snaps_[k].t > req_t_) --k;
            snaps_.erase(snaps_.begin(), snaps_.begin() + static_cast<std::ptrdiff_t>(k));
        }
        const bool need_fwd = dir > 0 && snaps_.back().t < goal;
        const bool need_bwd = dir < 0 && snaps_.front().t > goal;
        if (!need_fwd && !need_bwd) {
            cv_.wait(lk);
            continue;
        }
        const bool fwd = need_fwd;
        // перенести интегратор на нужный конец буфера и при необходимости сменить уровень
        SimSnapshot edge = fwd ? snaps_.back() : snaps_.front();
        lk.unlock();
        // интегратор продолжает с того места, где остановился, только если это и есть нужный край буфера
        if (sim_.t != edge.t || sim_setup_ != edge.setup) load_sim(edge);
        std::vector<SimSnapshot> fresh;
        if (edge.setup->level != want) {
            std::vector<V3> cx, cv;
            edge.setup->expand(edge.t, edge.x, edge.v, cx, cv);
            SimSnapshot s;
            s.t = edge.t;
            s.setup = make_setup(want, edge.t, cx, cv, s.x, s.v);
            load_sim(s);
            fill_accel(s);
            fresh.push_back(std::move(s));
            level_ = want;
        }
        // порция шагов: до цели или ~20 мс
        const auto t0 = Clock::now();
        const double sim_t0 = sim_.t;
        std::string err;
        while (!quit_ && seconds_since(t0) < 0.02 && (goal - sim_.t) * dir > 0) {
            if (!sim_.integrate(goal, 1)) {
                err = sim_.error();
                break;
            }
            ++steps_;
            SimSnapshot s;
            s.t = sim_.t;
            s.setup = sim_setup_;
            for (const Body &b : sim_.bodies) {
                s.x.push_back(b.x);
                s.v.push_back(b.v);
            }
            fill_accel(s);
            fresh.push_back(std::move(s));
        }
        const double wall = seconds_since(t0);
        lk.lock();
        if (wall > 0.005) {
            const double tp = std::fabs(sim_.t - sim_t0) / wall;
            double &e = tp_[level_.load()];
            e = 0.7 * e + 0.3 * tp;
        }
        if (!err.empty()) {
            error_ = "интегратор: " + err;
            quit_ = true;
            break;
        }
        for (SimSnapshot &s : fresh) {
            if (fwd) snaps_.push_back(std::move(s));
            else snaps_.push_front(std::move(s));
        }
        // старые снимки за спиной у рендера выбрасываем
        while (snaps_.size() > kMaxSnapshots) {
            if (fwd) snaps_.pop_front();
            else snaps_.pop_back();
        }
        const double behind = std::max(std::fabs(rate) * 2.0, 4 * 86400.0);
        while (snaps_.size() > 2) {
            if (fwd && snaps_[1].t < req_t_ - behind) snaps_.pop_front();
            else if (!fwd && snaps_[snaps_.size() - 2].t > req_t_ + behind) snaps_.pop_back();
            else break;
        }
    }
    running_ = false;
}

}  // namespace sc
