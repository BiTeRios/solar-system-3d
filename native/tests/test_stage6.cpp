// Тесты этапа 6: правки параметров, отмена и повтор, сценарий JSON, предупреждения, таблица «было → стало».
// Данные — game/data (тесты запускаются из корня проекта, tools/run_tests.ps1). Допуск сравнения с NASA — 5 %.
#include <doctest.h>

#include <cstdio>
#include <map>
#include <string>

#include "core/catalog.hpp"
#include "core/ephemeris.hpp"
#include "core/scenario.hpp"

using namespace sc;

namespace {

std::string read_file(const std::string &path) {
    std::string s;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

struct Data {
    Ephemeris eph;
    Catalog cat;
    bool ok = false;
    Data() {
        std::string err;
        ok = eph.open("game/data/ephemeris.eph") &&
             cat.load(read_file("game/data/bodies.json"), read_file("game/data/spice_constants.json"),
                      read_file("game/data/dynamics.json"), err);
    }
};

const Data &data() {
    static Data d;
    return d;
}

constexpr double kEt = 820497669.0;  // 2026-01-01 00:00 UTC

// Начальное состояние «Симуляции» с правками: положения и скорости тел каталога.
void state(const Scenario &s, std::vector<V3> &x, std::vector<V3> &v) {
    std::vector<SimBody> b;
    std::string err;
    REQUIRE(data().cat.initial_state(data().eph, kEt, nullptr, b, err));
    s.apply(b);
    x.assign(data().cat.bodies.size(), V3{});
    v = x;
    for (const SimBody &sb : b) {
        x[static_cast<size_t>(sb.catalog)] = sb.body.x;
        v[static_cast<size_t>(sb.catalog)] = sb.body.v;
    }
}

bool same(const std::vector<V3> &a, const std::vector<V3> &b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (!(a[i].x == b[i].x && a[i].y == b[i].y && a[i].z == b[i].z)) return false;
    return a.size() == b.size();
}

int idx(const char *key) { return data().cat.index_of_key(key); }

bool has(const std::vector<Warning> &w, Warning::Kind k, int body) {
    for (const Warning &x : w)
        if (x.kind == k && x.body == body) return true;
    return false;
}

}  // namespace

TEST_CASE("Производные: Земля — g 9,798 м/с², v₂ 11,19 км/с, T_eq 255 K при A = 0,306; T_eq тел против NASA") {
    REQUIRE(data().ok);
    Scenario s;
    s.attach(&data().cat);
    std::vector<V3> x, v;
    state(s, x, v);
    const int e = idx("earth");
    std::string err;
    REQUIRE(s.set(e, kAlbedo, 0.306, err));
    const Derived d = s.derive(e, x, v, false);
    std::printf("Земля: g %.4f м/с², v2 %.4f км/с, T_eq(A=0,306) %.2f K\n", d.gravity, d.escape, d.t_eq);
    CHECK(d.gravity == doctest::Approx(9.798).epsilon(0.05));
    CHECK(d.escape == doctest::Approx(11.19).epsilon(0.05));
    CHECK(d.t_eq == doctest::Approx(255.0).epsilon(0.05));
    CHECK(d.t_surface == doctest::Approx(288.0 * d.t_eq / 254.0).epsilon(1e-9));
    int n = 0;
    for (size_t i = 0; i < data().cat.bodies.size(); ++i) {
        const BodyInfo &b = data().cat.bodies[i];
        if (!(b.blackbody_k > 0) || std::isnan(b.bond_albedo)) continue;
        // У Плутона на странице NASA 37,5 K не сходится с её же альбедо Бонда 0,72 (выходит при A ≈ 0,49)
        if (b.key == "pluto") continue;
        const Derived r = s.derive(static_cast<int>(i), x, v, true);
        INFO(b.name_en, ": T_eq ", r.t_eq, " K, NASA ", b.blackbody_k, " K");
        std::printf("T_eq %s: %.1f K, NASA %.1f K\n", b.name_en.c_str(), r.t_eq, b.blackbody_k);
        CHECK(r.t_eq == doctest::Approx(b.blackbody_k).epsilon(0.05));
        ++n;
    }
    CHECK(n >= 8);
}

TEST_CASE("Отмена и повтор восстанавливают состояние полностью") {
    REQUIRE(data().ok);
    Scenario s;
    s.attach(&data().cat);
    std::vector<std::vector<V3>> xs(1), vs(1);
    state(s, xs[0], vs[0]);
    std::string err;
    const auto step = [&](bool ok) {
        REQUIRE(ok);
        xs.emplace_back();
        vs.emplace_back();
        state(s, xs.back(), vs.back());
    };
    step(s.set(idx("earth"), kMass, 2 * s.real(idx("earth"), kMass), err));
    step(s.set(idx("moon"), kSpeed, 1.1, err));
    step(s.set(idx("jupiter"), kDistance, 1.05, err));
    step(s.reset());
    step(s.set(idx("sun"), kMass, 0.9 * s.real(idx("sun"), kMass), err));
    CHECK_FALSE(s.set(idx("earth"), kAlbedo, 1.5, err));  // вне пределов — не команда
    REQUIRE(s.history().size() == 5);
    CHECK_FALSE(same(vs[0], vs[2]));
    std::vector<V3> x, v;
    for (size_t k = 5; k-- > 0;) {
        REQUIRE(s.undo());
        state(s, x, v);
        CHECK(same(x, xs[k]));
        CHECK(same(v, vs[k]));
    }
    CHECK(s.empty());
    CHECK_FALSE(s.undo());
    for (size_t k = 1; k <= 5; ++k) {
        REQUIRE(s.redo());
        state(s, x, v);
        CHECK(same(x, xs[k]));
        CHECK(same(v, vs[k]));
    }
    CHECK_FALSE(s.redo());
}

TEST_CASE("Сценарий JSON сохраняется и загружается без изменений") {
    REQUIRE(data().ok);
    Scenario a;
    a.attach(&data().cat);
    std::string err;
    REQUIRE(a.set(idx("mars"), kMass, 1.234567890123e24, err));
    REQUIRE(a.set(idx("io"), kDistance, 0.987654321, err));
    REQUIRE(a.set(idx("sun"), kTemperature, 6000.5, err));
    REQUIRE(a.set(idx("earth"), kRotation, 1.0 / 3.0, err));
    a.epoch = kEt + 0.1;
    a.mode = 1;
    a.bookmarks = {{3, 1, 1, 0.1, -2e8, 1.0 / 7, 0, 0, 0, 1, 5e4, 0.5, 0.25, 1e3}};
    const std::string j1 = a.to_json();
    Scenario b;
    b.attach(&data().cat);
    REQUIRE(b.from_json(j1, err));
    CHECK(b.to_json() == j1);
    CHECK(b.epoch == a.epoch);
    CHECK(b.bookmarks == a.bookmarks);
    std::vector<V3> xa, va, xb, vb;
    state(a, xa, va);
    state(b, xb, vb);
    CHECK(same(xa, xb));
    CHECK(same(va, vb));
    // испорченный сценарий не загружается и ничего не меняет
    std::string bad = j1;
    bad.replace(bad.find("0.98765"), 7, "-1.0000");
    CHECK_FALSE(b.from_json(bad, err));
    CHECK(b.to_json() == j1);
}

TEST_CASE("Предупреждения: реальная система — без предупреждений; подготовленные случаи срабатывают") {
    REQUIRE(data().ok);
    std::vector<V3> x, v;
    Scenario real;
    real.attach(&data().cat);
    state(real, x, v);
    const std::vector<Warning> none = real.check(x, v);
    for (const Warning &w : none) MESSAGE(w.text);
    CHECK(none.empty());

    const int earth = idx("earth"), moon = idx("moon"), mars = idx("mars"), sun = idx("sun");
    const auto warn = [&](int body, int p, double val) {
        Scenario s;
        s.attach(&data().cat);
        std::string err;
        REQUIRE(s.set(body, p, val, err));
        std::vector<V3> sx, sv;
        state(s, sx, sv);
        return s.check(sx, sv);
    };
    // Луна в 8800 км от центра Земли: внутри предела Роша (~9500 км), но ещё не касается Земли
    const double dm = norm(x[static_cast<size_t>(moon)] - x[static_cast<size_t>(earth)]);
    std::vector<Warning> w = warn(moon, kDistance, 8800.0 / dm);
    CHECK(has(w, Warning::Roche, moon));
    CHECK_FALSE(has(w, Warning::Collision, earth));
    CHECK(has(warn(earth, kSpeed, 1.5), Warning::Escape, earth));
    w = warn(mars, kSpeed, 0.8);
    bool crossing = false;
    for (const Warning &q : w) crossing |= q.kind == Warning::Crossing && (q.body == earth || q.other == earth) &&
                                          (q.body == mars || q.other == mars);
    CHECK(crossing);
    CHECK(has(warn(earth, kRadius, 0.3 * 6371.0), Warning::Density, earth));
    CHECK(has(warn(earth, kRotation, 1.0), Warning::Spin, earth));
    CHECK(has(warn(sun, kRadius, 1.0), Warning::Horizon, sun));
}

TEST_CASE("Таблица «было → стало»: масса Земли × 2") {
    REQUIRE(data().ok);
    Scenario s;
    s.attach(&data().cat);
    std::vector<V3> x0, v0, x, v;
    state(s, x0, v0);
    const int e = idx("earth");
    std::string err;
    REQUIRE(s.set(e, kMass, 2 * s.real(e, kMass), err));
    state(s, x, v);
    std::map<std::string, TableRow> t;
    for (const TableRow &r : s.table(e, x0, v0, x, v)) t[r.name] = r;
    const auto ratio = [&](const char *name) {
        REQUIRE(t.count(name));
        return t[name].after / t[name].before;
    };
    CHECK(ratio("Масса") == doctest::Approx(2.0).epsilon(1e-12));
    CHECK(ratio("Плотность") == doctest::Approx(2.0).epsilon(1e-12));
    CHECK(ratio("Ускорение свободного падения (экватор)") == doctest::Approx(2.0).epsilon(1e-12));
    CHECK(ratio("Вторая космическая скорость") == doctest::Approx(std::sqrt(2.0)).epsilon(1e-12));
    CHECK(ratio("Радиус Шварцшильда") == doctest::Approx(2.0).epsilon(1e-12));
    CHECK(ratio("Сфера Хилла") == doctest::Approx(std::cbrt(2.0)).epsilon(1e-4));
    CHECK(ratio("Средний радиус") == 1.0);
    CHECK(ratio("Равновесная температура") == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(t["Масса"].param == kMass);
    CHECK(t["Плотность"].param == -1);
    CHECK(t["Масса"].before == doctest::Approx(5.9722e24).epsilon(0.05));  // NASA
}

