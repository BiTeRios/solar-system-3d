// Тесты этапа 4 без данных и без Godot: JSON, коники, спутник на элементах, камера, кубосфера.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "core/camera_rig.hpp"
#include "core/cubesphere.hpp"
#include "core/frames.hpp"
#include "core/json.hpp"
#include "core/kepler.hpp"

using namespace sc;

TEST_CASE("JSON: числа до бита, UTF-8 и \\u, порядок ключей") {
    const Json j = Json::parse(
        R"({"b": 0.1, "a": [1.3271244004127942e11, -2.5e-300, 6.67430e-20], "name": "Земля", "esc": "Зе", )"
        R"("nested": {"x": {"y": [true, false, null]}}, "s": "кавычка \" и \\"})");
    REQUIRE(j.is_object());
    CHECK(j["b"].num() == 0.1);
    CHECK(j["a"][0].num() == 1.3271244004127942e11);
    CHECK(j["a"][1].num() == -2.5e-300);
    CHECK(j["a"][2].num() == 6.67430e-20);
    CHECK(j["name"].str() == "Земля");
    CHECK(j["esc"].str() == "Зе");
    CHECK(j["nested"]["x"]["y"][0].boolean() == true);
    CHECK(j["nested"]["x"]["y"][2].is_null());
    CHECK(j["нет"].is_null());
    CHECK(j["s"].str() == "кавычка \" и \\");
    CHECK(j.keys() == std::vector<std::string>{"b", "a", "name", "esc", "nested", "s"});
    std::string err;
    Json::parse("{\"a\": [1, 2}", &err);
    CHECK(!err.empty());
}

TEST_CASE("Коника проходит через тело; спутник на элементах повторяет состояние") {
    const double mu = 398600.4418;
    const V3 r{7000, -1200, 3000}, v{1.1, 6.9, -2.0};
    const Conic c = conic_from_state(r, v, mu);
    REQUIRE(c.valid);
    CHECK(norm(conic_point(c, c.nu) - r) < 1e-8);
    CHECK(c.e < 1);
    // без J2: в момент построения — то же состояние, через период — снова оно
    const KinematicOrbit k = kinematic_from_state(r, v, mu, V3{0.1, 0.2, 0.97}, 0.0, 0.0, 100.0);
    REQUIRE(k.valid);
    V3 r1, v1;
    k.state(100.0, r1, v1);
    CHECK(norm(r1 - r) < 1e-8);
    CHECK(norm(v1 - v) < 1e-11);
    const double period = 2 * kPi / k.m_rate;
    k.state(100.0 + period, r1, v1);
    CHECK(norm(r1 - r) < 1e-6);
    // J2: узел прямой орбиты уходит назад, как у спутников Земли
    const KinematicOrbit kj = kinematic_from_state(r, v, mu, V3{0, 0, 1}, 1.08263e-3, 6378.137, 0.0);
    CHECK(kj.node_rate < 0);
    CHECK(solve_kepler(1.0, 0.9) - 0.9 * std::sin(solve_kepler(1.0, 0.9)) == doctest::Approx(1.0).epsilon(1e-14));
}

TEST_CASE("Сцена: ICRF → эклиптика → оси Godot — правый поворот, полюс эклиптики вверх") {
    const M3 s = icrf_to_scene();
    const double ce = std::cos(kObliquityJ2000), se = std::sin(kObliquityJ2000);
    const V3 north = s * V3{0, -se, ce};  // полюс эклиптики в ICRF
    CHECK(north.y == doctest::Approx(1.0).epsilon(1e-15));
    const V3 x = s * V3{1, 0, 0};
    CHECK(x.x == doctest::Approx(1.0));
    // определитель = +1
    const double det = s.m[0][0] * (s.m[1][1] * s.m[2][2] - s.m[1][2] * s.m[2][1]) -
                       s.m[0][1] * (s.m[1][0] * s.m[2][2] - s.m[1][2] * s.m[2][0]) +
                       s.m[0][2] * (s.m[1][0] * s.m[2][1] - s.m[1][1] * s.m[2][0]);
    CHECK(det == doctest::Approx(1.0).epsilon(1e-15));
}

TEST_CASE("Камера: шкала скорости от 1 м/с до 1000 c, установившаяся скорость полёта") {
    CHECK(CameraRig::speed_from_slider(0.0) == 1e-3);
    CHECK(CameraRig::speed_from_slider(1.0) == 1000.0 * 299792.458);
    for (double u = 0.05; u < 1; u += 0.1)
        CHECK(CameraRig::slider_from_speed(CameraRig::speed_from_slider(u)) == doctest::Approx(u).epsilon(1e-12));
    for (double sp : {1e-3, 1000.0 * 299792.458}) {
        CameraRig rig;
        rig.speed = sp;
        CamFrame f;
        const double dt = 1.0 / 60;
        for (int i = 0; i < 180; ++i) rig.update_free({0, 0, -1}, {}, dt, f);
        const V3 p0 = rig.pos;
        for (int i = 0; i < 60; ++i) rig.update_free({0, 0, -1}, {}, dt, f);
        CHECK(norm(rig.pos - p0) == doctest::Approx(sp).epsilon(1e-5));  // за 1 с
    }
}

TEST_CASE("Камера: перелёт доходит до цели за 2–5 с и смотрит на неё") {
    CameraRig rig;
    rig.pos = {1.2e8, -3e7, 4e6};
    rig.orient = Quat::axis_angle({0, 1, 0}, 0.7);
    CamFrame f;
    f.body_radius = 71492;
    f.surface_r = 71492;
    f.sun_dir = {-1, 0, 0};
    const double d1 = 4 * f.body_radius;
    const double dur = rig.start_flyto(f, d1);
    CHECK(dur >= 2.0);
    CHECK(dur <= 5.0);
    double prev = norm(rig.pos);
    const double dt = 1.0 / 60;
    for (int i = 0; i < 400 && rig.mode == CamMode::FlyTo; ++i) {
        rig.update_flyto(dt, f);
        CHECK(norm(rig.pos) <= prev * (1 + 1e-12));  // расстояние только убывает
        prev = norm(rig.pos);
    }
    CHECK(rig.mode == CamMode::Orbit);
    CHECK(norm(rig.pos) == doctest::Approx(d1).epsilon(1e-12));
    const V3 fwd = rig.orient.matrix() * V3{0, 0, -1};
    CHECK(dot(fwd, rig.pos * (-1.0 / norm(rig.pos))) == doctest::Approx(1.0).epsilon(1e-12));
    // зум внутрь не опускает камеру ниже 10 см над поверхностью
    rig.update_orbit(0, 0, -500, f);
    CHECK(norm(rig.pos) >= f.surface_r + CameraRig::kMinAltitude - 1e-12);
}

TEST_CASE("Кубосфера: вершины на эллипсоиде, края соседей совпадают, у поверхности — самый мелкий уровень") {
    CubeSphere cs;
    cs.set_body({6378.1366, 6378.1366, 6356.7519});
    CHECK(cs.max_level >= 20);
    // точки на эллипсоиде
    for (int f = 0; f < 6; ++f)
        for (double s = 0; s <= 1; s += 0.125)
            for (double t = 0; t <= 1; t += 0.125) {
                const V3 p = cs.surface(f, s, t);
                const double q = std::pow(p.x / cs.radii.x, 2) + std::pow(p.y / cs.radii.y, 2) + std::pow(p.z / cs.radii.z, 2);
                CHECK(q == doctest::Approx(1.0).epsilon(1e-14));
            }
    // соседние чанки одного уровня: общий край до бита
    ChunkMesh a, b;
    cs.build(ChunkId{0, 3, 2, 5}, a);
    cs.build(ChunkId{0, 3, 3, 5}, b);
    const int row = cs.grid + 1;
    for (int j = 0; j <= cs.grid; ++j) {
        const size_t ia = static_cast<size_t>(j * row + cs.grid), ib = static_cast<size_t>(j * row);
        for (int c = 0; c < 3; ++c) {
            const double pa = a.center[c] + a.pos[ia * 3 + c] / 1000.0, pb = b.center[c] + b.pos[ib * 3 + c] / 1000.0;
            // точки края одинаковы в double (это видно по dir); в чанках они хранятся во float относительно своих
            // центров, поэтому расходятся только на округление float: ~1e-7 от размера чанка
            CHECK(std::fabs(pa - pb) < 4 * 1.2e-7 * std::max(a.radius, b.radius));
            CHECK(a.dir[ia * 3 + c] == b.dir[ib * 3 + c]);
        }
    }
    CHECK(a.index.size() % 3 == 0);
    // камера в 1 м над поверхностью: под ней — самый мелкий уровень, за горизонтом ничего
    const V3 u{0.6, -0.3, 0.74};
    const V3 un = u / norm(u);
    const V3 cam = un * (cs.radius_along(un) + 1e-3);
    std::vector<ChunkId> sel;
    cs.select(cam, 1.5, sel);
    int deepest = 0;
    for (const ChunkId &id : sel) {
        deepest = std::max(deepest, id.level);
        V3 c;
        double r;
        cs.bounds(id, c, r);
        CHECK(dot(c, un) > -r);  // обратная сторона отброшена
    }
    CHECK(deepest == cs.max_level);
    CHECK(sel.size() < 800);
    double k = 0;
    CHECK(cs.ray_hit(cam, un * -1.0, k));
    CHECK(k == doctest::Approx(1e-3).epsilon(1e-6));
}
