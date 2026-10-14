#include "scenario.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>

#include "json.hpp"
#include "kepler.hpp"

namespace sc {

const ParamDef kParams[kParamCount] = {
    {"mass", "Масса", "кг"},
    {"radius", "Средний радиус", "км"},
    {"albedo", "Альбедо Бонда", ""},
    {"rotation", "Период вращения", "ч"},
    {"speed", "Скорость по орбите", "×"},
    {"distance", "Расстояние до центра орбиты", "×"},
    {"temperature", "Эффективная температура", "К"},
};

namespace {

constexpr double kAuKm = 1.495978707e8;

// Число по-русски: значащие цифры, десятичная запятая.
std::string fmt(double x, int digits = 3) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", digits, x);
    std::string s = buf;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

std::string km(double x) {
    if (std::fabs(x) >= 0.1 * kAuKm) return fmt(x / kAuKm) + " а. е.";
    if (std::fabs(x) >= 1e6) return fmt(x / 1e6) + " млн км";
    return fmt(x, 4) + " км";
}

}  // namespace

void Scenario::attach(const Catalog *cat) {
    cat_ = cat;
    star_ = 0;
    for (size_t i = 0; i < cat->bodies.size(); ++i)
        if (cat->bodies[i].kind == "star") star_ = static_cast<int>(i);
    values_.clear();
    history_.clear();
    pos_ = 0;
    ++rev_;
}

double Scenario::real(int body, int p) const {
    const BodyInfo &b = cat_->bodies[static_cast<size_t>(body)];
    switch (p) {
    case kMass: return b.gm > 0 ? b.gm / kGravity : NAN;
    case kRadius: return b.mean_radius;
    case kAlbedo: return b.kind == "star" ? NAN : b.bond_albedo;
    case kRotation:
        // период — по скорости угла W из pck00011, иначе со страницы NASA
        if (b.has_rotation && b.rot.pm.size() >= 2 && b.rot.pm[1] != 0) return std::fabs(360.0 / b.rot.pm[1] * 24.0);
        return std::fabs(b.rotation_h);
    case kSpeed:
    case kDistance: return b.parent >= 0 ? 1.0 : NAN;
    case kTemperature: return b.kind == "star" ? b.mean_temp_k : NAN;
    default: return NAN;
    }
}

double Scenario::param(int body, int p, bool real_values) const {
    if (!real_values) {
        auto it = values_.find({body, p});
        if (it != values_.end()) return it->second;
    }
    return real(body, p);
}

double Scenario::value(int body, int p) const { return param(body, p, false); }

bool Scenario::editable(int body, int p) const {
    const BodyInfo &b = cat_->bodies[static_cast<size_t>(body)];
    const bool star = b.kind == "star";
    switch (p) {
    case kMass:
    case kRadius:
    case kRotation: return true;
    case kAlbedo: return !star;
    case kSpeed:
    case kDistance: return b.parent >= 0;
    case kTemperature: return star;
    default: return false;
    }
}

Scenario::Values Scenario::dynamic_edits() const {
    Values out;
    for (const auto &kv : values_)
        if (kv.first.second == kMass || kv.first.second == kSpeed || kv.first.second == kDistance) out.insert(kv);
    return out;
}

bool Scenario::dynamic() const { return !dynamic_edits().empty(); }

bool Scenario::valid(int body, int p, double v, std::string &error) const {
    if (body < 0 || body >= static_cast<int>(cat_->bodies.size()) || p < 0 || p >= kParamCount ||
        !editable(body, p)) {
        error = "этот параметр у тела не правится";
        return false;
    }
    if (!std::isfinite(v)) {
        error = "нужно число";
        return false;
    }
    const bool ok = p == kAlbedo ? v >= 0 && v <= 1 : (p == kSpeed ? v >= 0 : v > 0);
    if (!ok) error = p == kAlbedo ? "альбедо — от 0 до 1" : (p == kSpeed ? "скорость не может быть отрицательной"
                                                                          : std::string(kParams[p].name) + " — больше нуля");
    return ok;
}

bool Scenario::set(int body, int p, double v, std::string &error) {
    if (!valid(body, p, v, error)) return false;
    const double was = value(body, p);
    if (v == was) return true;
    Values after = values_;
    if (v == real(body, p))
        after.erase({body, p});
    else
        after[{body, p}] = v;
    const std::string unit = kParams[p].unit;
    const std::string u = unit.empty() ? "" : (unit == "×" ? "×" : " " + unit);
    const auto show = [&](double x) { return std::isnan(x) ? std::string("нет данных") : (unit == "×" ? "×" + fmt(x, 4) : fmt(x, 5) + u); };
    commit(cat_->bodies[static_cast<size_t>(body)].name + ": " + kParams[p].name + " " + show(was) + " → " + show(v),
           std::move(after));
    return true;
}

bool Scenario::reset() {
    if (values_.empty()) return false;
    commit("Сброс: реальные значения", {});
    return true;
}

void Scenario::commit(const std::string &label, Values after) {
    history_.resize(pos_);
    history_.push_back({label, values_, std::move(after)});
    values_ = history_.back().after;
    pos_ = history_.size();
    ++rev_;
}

bool Scenario::undo() {
    if (!can_undo()) return false;
    values_ = history_[--pos_].before;
    ++rev_;
    return true;
}

bool Scenario::redo() {
    if (!can_redo()) return false;
    values_ = history_[pos_++].after;
    ++rev_;
    return true;
}

std::vector<std::string> Scenario::history() const {
    std::vector<std::string> out;
    for (const Command &c : history_) out.push_back(c.label);
    return out;
}

void Scenario::apply(std::vector<SimBody> &bodies) const {
    const size_t n = cat_->bodies.size();
    std::vector<int> at(n, -1);
    for (size_t k = 0; k < bodies.size(); ++k)
        if (bodies[k].catalog >= 0) at[static_cast<size_t>(bodies[k].catalog)] = static_cast<int>(k);
    for (const auto &kv : values_)
        if (kv.first.second == kMass && at[static_cast<size_t>(kv.first.first)] >= 0)
            bodies[static_cast<size_t>(at[static_cast<size_t>(kv.first.first)])].body.gm = kv.second * kGravity;
    // Скорость и расстояние — относительно родителя; спутники сдвигаются вместе с планетой. Сдвиги считаются
    // по исходным состояниям, потом прибавляются.
    std::vector<V3> dx(n), dv(n);
    std::vector<char> done(n, 0);
    std::function<void(size_t)> solve = [&](size_t i) {
        if (done[i]) return;
        done[i] = 1;
        const int p = cat_->bodies[i].parent;
        if (p < 0 || at[i] < 0 || at[static_cast<size_t>(p)] < 0) return;
        solve(static_cast<size_t>(p));
        const Body &b = bodies[static_cast<size_t>(at[i])].body, &pb = bodies[static_cast<size_t>(at[static_cast<size_t>(p)])].body;
        dx[i] = dx[static_cast<size_t>(p)] + (b.x - pb.x) * (value(static_cast<int>(i), kDistance) - 1.0);
        dv[i] = dv[static_cast<size_t>(p)] + (b.v - pb.v) * (value(static_cast<int>(i), kSpeed) - 1.0);
    };
    for (size_t i = 0; i < n; ++i) solve(i);
    for (size_t i = 0; i < n; ++i)
        if (at[i] >= 0) {
            bodies[static_cast<size_t>(at[i])].body.x += dx[i];
            bodies[static_cast<size_t>(at[i])].body.v += dv[i];
        }
}

V3 Scenario::radii(int body) const {
    const BodyInfo &b = cat_->bodies[static_cast<size_t>(body)];
    return b.radii * (value(body, kRadius) / b.mean_radius);
}

RotationModel Scenario::rotation(int body) const {
    RotationModel r = cat_->bodies[static_cast<size_t>(body)].rot;
    auto it = values_.find({body, kRotation});
    if (it == values_.end() || r.pm.size() < 2 || r.pm[1] == 0) return r;
    const double rate = std::copysign(360.0 * 24.0 / it->second, r.pm[1]);  // град/сутки
    r.pm[0] += (r.pm[1] - rate) * (epoch / 86400.0);
    r.pm[1] = rate;
    return r;
}

Derived Scenario::derive(int i, const std::vector<V3> &x, const std::vector<V3> &v, bool rv) const {
    const BodyInfo &b = cat_->bodies[static_cast<size_t>(i)];
    Derived d;
    d.mass = param(i, kMass, rv);
    d.radius = param(i, kRadius, rv);
    d.radius_eq = b.radii.x * d.radius / b.mean_radius;
    d.albedo = param(i, kAlbedo, rv);
    d.rotation_h = param(i, kRotation, rv);
    d.temperature = param(i, kTemperature, rv);
    const double gm = d.mass * kGravity;
    d.density = d.mass / (4.0 / 3.0 * kPi * d.radius * d.radius * d.radius) * 1e-12;
    d.gravity = gm / (d.radius_eq * d.radius_eq) * 1000.0;
    d.escape = std::sqrt(2.0 * gm / d.radius);
    d.critical_h = 2.0 * kPi * std::sqrt(d.radius * d.radius * d.radius / gm) / 3600.0;
    d.schwarzschild = 2.0 * gm / (kLightKms * kLightKms);
    const int p = b.parent;
    if (p >= 0) {
        const double pm = param(p, kMass, rv), pgm = std::isnan(pm) ? 0.0 : pm * kGravity;
        const V3 r = x[static_cast<size_t>(i)] - x[static_cast<size_t>(p)], u = v[static_cast<size_t>(i)] - v[static_cast<size_t>(p)];
        d.distance = norm(r);
        d.speed = norm(u);
        const Conic c = conic_from_state(r, u, pgm + (std::isnan(gm) ? 0.0 : gm));
        d.a = c.a;
        d.e = c.e;
        if (c.e < 1 && pm > 0) d.hill = c.a * (1 - c.e) * std::cbrt(d.mass / (3.0 * pm));
        const double pr = param(p, kRadius, rv);
        const double prho = pm / (4.0 / 3.0 * kPi * pr * pr * pr) * 1e-12;
        d.roche = pr * std::cbrt(2.0 * prho / d.density);
    }
    if (i == star_) return d;
    // T_eq по орбите вокруг звезды: у спутника — по орбите его планеты
    int top = i;
    while (cat_->bodies[static_cast<size_t>(top)].parent >= 0 && cat_->bodies[static_cast<size_t>(top)].parent != star_)
        top = cat_->bodies[static_cast<size_t>(top)].parent;
    if (cat_->bodies[static_cast<size_t>(top)].parent == star_) {
        const double sgm = param(star_, kMass, rv) * kGravity, tgm = param(top, kMass, rv) * kGravity;
        const V3 r = x[static_cast<size_t>(top)] - x[static_cast<size_t>(star_)];
        const Conic c = conic_from_state(r, v[static_cast<size_t>(top)] - v[static_cast<size_t>(star_)],
                                         sgm + (std::isnan(tgm) ? 0.0 : tgm));
        const double dist = c.e < 1 ? c.a * std::pow(1 - c.e * c.e, 0.25) : norm(r);
        const double ts = param(star_, kTemperature, rv), rs = param(star_, kRadius, rv);
        d.t_eq = ts * std::sqrt(rs / (2.0 * dist)) * std::pow(1.0 - d.albedo, 0.25);
        if (b.mean_temp_k > 0 && b.blackbody_k > 0) d.t_surface = d.t_eq * b.mean_temp_k / b.blackbody_k;
    }
    return d;
}

std::vector<TableRow> Scenario::table(int body, const std::vector<V3> &x0, const std::vector<V3> &v0,
                                      const std::vector<V3> &x, const std::vector<V3> &v) const {
    const Derived a = derive(body, x0, v0, true), b = derive(body, x, v, false);
    std::vector<TableRow> t;
    const auto row = [&](const char *name, const char *unit, double Derived::*f, int prm = -1) {
        if (prm >= 0 && !editable(body, prm)) return;
        if (prm < 0 && std::isnan(a.*f) && std::isnan(b.*f)) return;
        t.push_back({name, unit, a.*f, b.*f, prm});
    };
    row(kParams[kMass].name, "кг", &Derived::mass, kMass);
    row(kParams[kRadius].name, "км", &Derived::radius, kRadius);
    row(kParams[kAlbedo].name, "", &Derived::albedo, kAlbedo);
    row(kParams[kRotation].name, "ч", &Derived::rotation_h, kRotation);
    row(kParams[kTemperature].name, "К", &Derived::temperature, kTemperature);
    if (editable(body, kSpeed)) {
        t.push_back({kParams[kSpeed].name, "×", 1.0, value(body, kSpeed), kSpeed});
        t.push_back({kParams[kDistance].name, "×", 1.0, value(body, kDistance), kDistance});
    }
    row("Плотность", "г/см³", &Derived::density);
    row("Ускорение свободного падения (экватор)", "м/с²", &Derived::gravity);
    row("Вторая космическая скорость", "км/с", &Derived::escape);
    row("Равновесная температура", "К", &Derived::t_eq);
    row("Температура поверхности (оценка)", "К", &Derived::t_surface);
    row("Критический период вращения", "ч", &Derived::critical_h);
    row("Радиус Шварцшильда", "км", &Derived::schwarzschild);
    row("Расстояние до центра орбиты", "км", &Derived::distance);
    row("Скорость относительно центра", "км/с", &Derived::speed);
    row("Большая полуось", "км", &Derived::a);
    row("Эксцентриситет", "", &Derived::e);
    row("Сфера Хилла", "км", &Derived::hill);
    row("Предел Роша", "км", &Derived::roche);
    return t;
}

double orbit_distance(const V3 &r1, const V3 &v1, double mu1, const V3 &r2, const V3 &v2, double mu2) {
    const Conic c1 = conic_from_state(r1, v1, mu1), c2 = conic_from_state(r2, v2, mu2);
    const auto at = [](const Conic &c, double E) {
        return c.P * (c.a * (std::cos(E) - c.e)) + c.Q * (c.a * std::sqrt(1 - c.e * c.e) * std::sin(E));
    };
    constexpr int kN = 128;
    double best = INFINITY, e1 = 0, e2 = 0, step = 2 * kPi / kN;
    for (int i = 0; i < kN; ++i)
        for (int j = 0; j < kN; ++j) {
            const double d = norm(at(c1, i * step) - at(c2, j * step));
            if (d < best) best = d, e1 = i * step, e2 = j * step;
        }
    for (int round = 0; round < 6; ++round) {
        step /= 4;
        const double c1e = e1, c2e = e2;
        for (int i = -4; i <= 4; ++i)
            for (int j = -4; j <= 4; ++j) {
                const double d = norm(at(c1, c1e + i * step) - at(c2, c2e + j * step));
                if (d < best) best = d, e1 = c1e + i * step, e2 = c2e + j * step;
            }
    }
    return best;
}

std::vector<Warning> Scenario::check(const std::vector<V3> &x, const std::vector<V3> &v) const {
    std::vector<Warning> out;
    const size_t n = cat_->bodies.size();
    std::vector<Derived> d(n);
    std::vector<int> ids;
    for (size_t i = 0; i < n; ++i)
        if (cat_->bodies[i].visible) {
            d[i] = derive(static_cast<int>(i), x, v, false);
            ids.push_back(static_cast<int>(i));
        }
    const auto name = [&](int i) { return cat_->bodies[static_cast<size_t>(i)].name; };
    const auto add = [&](Warning::Kind k, int a, int b, const std::string &t) { out.push_back({k, a, b, name(a) + ": " + t}); };
    for (int i : ids) {
        const Derived &q = d[static_cast<size_t>(i)];
        if (q.density < 0.01 || q.density > 25)
            add(Warning::Density, i, -1, "плотность " + fmt(q.density) + " г/см³ — вне 0,01–25 г/см³ (у вещества тел Солнечной системы)");
        if (q.rotation_h < q.critical_h)
            add(Warning::Spin, i, -1, "период вращения " + fmt(q.rotation_h) + " ч короче критического " + fmt(q.critical_h) +
                                          " ч — экватор отрывается");
        if (q.radius < q.schwarzschild)
            add(Warning::Horizon, i, -1, "радиус меньше радиуса Шварцшильда " + km(q.schwarzschild) + " — это чёрная дыра");
        const int p = cat_->bodies[static_cast<size_t>(i)].parent;
        if (p < 0) continue;
        if (q.distance < q.roche)
            add(Warning::Roche, i, p, "ближе предела Роша " + name(p) + " (" + km(q.distance) + " < " + km(q.roche) +
                                          ") — приливы разрушат тело");
        if (q.e >= 1)
            add(Warning::Escape, i, p, "уходит от " + name(p) + ": эксцентриситет " + fmt(q.e) + " ≥ 1");
        else if (p != star_ && q.a * (1 + q.e) > 0.5 * d[static_cast<size_t>(p)].hill)
            add(Warning::Hill, i, p, "апоцентр " + km(q.a * (1 + q.e)) + " дальше половины сферы Хилла " + name(p) + " (" +
                                         km(0.5 * d[static_cast<size_t>(p)].hill) + ") — орбита неустойчива");
    }
    for (size_t k = 0; k < ids.size(); ++k)
        for (size_t l = k + 1; l < ids.size(); ++l) {
            const int i = ids[k], j = ids[l];
            const Derived &a = d[static_cast<size_t>(i)], &b = d[static_cast<size_t>(j)];
            const double sep = norm(x[static_cast<size_t>(i)] - x[static_cast<size_t>(j)]);
            if (sep < a.radius + b.radius) {
                add(Warning::Collision, i, j, "столкновение с " + name(j) + " (между центрами " + km(sep) + ")");
                continue;
            }
            const int p = cat_->bodies[static_cast<size_t>(i)].parent;
            if (p < 0 || p != cat_->bodies[static_cast<size_t>(j)].parent || !(a.e < 1) || !(b.e < 1)) continue;
            const double pm = value(p, kMass);
            if (a.mass > 0 && b.mass > 0 && pm > 0) {
                const double rh = std::cbrt((a.mass + b.mass) / (3 * pm)) * (a.a + b.a) / 2;
                const double delta = std::fabs(b.a - a.a) / rh;
                if (delta < 2 * std::sqrt(3.0))
                    add(Warning::Spacing, i, j, "орбиты с " + name(j) + " разнесены на " + fmt(delta) +
                                                    " взаимных радиуса Хилла (< 2√3) — неустойчиво");
            }
            // тела без известной массы (комета Галлея) не проверяются: сближения с планетами для них обычны
            if (!(a.hill > 0) || !(b.hill > 0)) continue;
            const double hill = std::max(a.hill, b.hill);
            const double pgm = std::isnan(pm) ? 0.0 : pm * kGravity;
            const double moid = orbit_distance(x[static_cast<size_t>(i)] - x[static_cast<size_t>(p)], v[static_cast<size_t>(i)] - v[static_cast<size_t>(p)],
                                               pgm + (a.mass > 0 ? a.mass * kGravity : 0.0),
                                               x[static_cast<size_t>(j)] - x[static_cast<size_t>(p)], v[static_cast<size_t>(j)] - v[static_cast<size_t>(p)],
                                               pgm + (b.mass > 0 ? b.mass * kGravity : 0.0));
            // тесное сближение — в пределах трёх радиусов Хилла
            if (moid < 3 * hill)
                add(Warning::Crossing, i, j, "орбита сближается с орбитой " + name(j) + " до " + km(moid) +
                                                 " — меньше трёх радиусов Хилла (" + km(3 * hill) + "): возможны тесные сближения");
        }
    return out;
}

std::string Scenario::to_json() const {
    const auto num = [](double x) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.17g", x);
        return std::string(buf);
    };
    std::string s = "{\n  \"format\": \"solar-system-scenario\",\n  \"schema\": 1,\n  \"epoch_tdb_s\": " + num(epoch) +
                    ",\n  \"mode\": " + std::to_string(mode) + ",\n  \"edits\": [";
    bool first = true;
    for (const auto &kv : values_) {
        s += first ? "\n" : ",\n";
        first = false;
        s += "    {\"body\": \"" + cat_->bodies[static_cast<size_t>(kv.first.first)].key + "\", \"param\": \"" +
             kParams[kv.first.second].id + "\", \"value\": " + num(kv.second) + "}";
    }
    s += first ? "],\n  \"bookmarks\": [" : "\n  ],\n  \"bookmarks\": [";
    first = true;
    for (const auto &b : bookmarks) {
        s += first ? "\n    [" : ",\n    [";
        first = false;
        for (size_t k = 0; k < b.size(); ++k) s += (k ? ", " : "") + num(b[k]);
        s += "]";
    }
    s += first ? "]\n}\n" : "\n  ]\n}\n";
    return s;
}

bool Scenario::from_json(const std::string &text, std::string &error) {
    const Json j = Json::parse(text, &error);
    if (!error.empty()) return false;
    if (j["format"].str() != "solar-system-scenario" || j["schema"].integer() != 1) {
        error = "это не сценарий (нужен format \"solar-system-scenario\", schema 1)";
        return false;
    }
    Values vals;
    for (const Json &e : j["edits"].elements()) {
        const int body = cat_->index_of_key(e["body"].str());
        int p = -1;
        for (int k = 0; k < kParamCount; ++k)
            if (e["param"].str() == kParams[k].id) p = k;
        if (body < 0 || p < 0) {
            error = "неизвестное тело или параметр: " + e["body"].str() + " / " + e["param"].str();
            return false;
        }
        const double val = e["value"].num(NAN);
        if (!valid(body, p, val, error)) {
            error = e["body"].str() + ": " + error;
            return false;
        }
        vals[{body, p}] = val;
    }
    std::vector<std::vector<double>> bm;
    for (const Json &b : j["bookmarks"].elements()) bm.push_back(b.numbers());
    epoch = j["epoch_tdb_s"].num(0.0);
    mode = j["mode"].integer();
    bookmarks = std::move(bm);
    values_ = std::move(vals);
    history_.clear();
    pos_ = 0;
    ++rev_;
    return true;
}

}  // namespace sc
