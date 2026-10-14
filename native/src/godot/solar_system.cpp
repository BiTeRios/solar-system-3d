#include "solar_system.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "core/frames.hpp"
#include "core/json.hpp"
#include "file_source.hpp"
#include "gd_util.hpp"

namespace godot {

using sc::M3;
using sc::V3;

using namespace gdu;

namespace {

constexpr int kOrbitSegments = 512;
constexpr int kChunkBuildsPerFrame = 48;
constexpr size_t kChunkCacheMax = 1500;  // на тело; чанк ≈ 60 КБ видеопамяти
constexpr double kCompressStart = 1.0e4;  // D0 / near

String fmt_calendar(int64_t y, int mo, int d, int h, int mi, double s) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%04lld-%02d-%02d %02d:%02d:%06.3f", static_cast<long long>(y), mo, d, h, mi,
                  std::floor(s * 1000.0) / 1000.0);
    return String(buf);
}

}  // namespace

SolarSystem::SolarSystem() = default;

SolarSystem::~SolarSystem() {
    runner_.stop();
    free_render();
}

// ---------------------------------------------------------------- данные

bool SolarSystem::load_data(const String &dir) {
    runner_.stop();
    free_render();
    loaded_ = false;
    const String d = dir.ends_with("/") ? dir : dir + String("/");
    Ref<FileAccess> f = FileAccess::open(d + String("ephemeris.eph"), FileAccess::READ);
    if (f.is_null()) {
        error_ = String::utf8("не открывается ") + d + String("ephemeris.eph");
        return false;
    }
    if (!eph_.open(std::make_unique<GodotFileSource>(f))) {
        error_ = u8(eph_.error());
        return false;
    }
    bool ok1, ok2, ok3, ok4;
    const std::string bodies = read_all(d + String("bodies.json"), ok1), consts = read_all(d + String("spice_constants.json"), ok2),
                      dyn = read_all(d + String("dynamics.json"), ok3), tj = read_all(d + String("time.json"), ok4);
    if (!(ok1 && ok2 && ok3 && ok4)) {
        error_ = String::utf8("нет файлов данных в ") + d;
        return false;
    }
    if (!cat_.load(bodies, consts, dyn, error_s_) || !time_.load_json(tj, error_s_)) {
        error_ = u8(error_s_);
        return false;
    }
    // общее покрытие эфемерид: все тела каталога вместе с цепочками центров
    eph_lo_ = -1e300;
    eph_hi_ = 1e300;
    for (const sc::BodyInfo &b : cat_.bodies) {
        int t = b.naif;
        for (int guard = 0; guard < 8 && t != 0; ++guard) {
            double lo, hi;
            if (!eph_.coverage(t, lo, hi)) break;
            eph_lo_ = std::max(eph_lo_, lo);
            eph_hi_ = std::min(eph_hi_, hi);
            t = eph_.center_of(t);
        }
    }
    runner_.attach(&cat_, &eph_);
    real_cat_ = cat_;
    scen_.attach(&real_cat_);
    runner_.adjust = [this](std::vector<sc::SimBody> &b) { scen_.apply(b); };
    catch_up_ = NAN;
    edit_sim_ = false;
    last_dyn_.clear();

    const size_t n = cat_.bodies.size();
    views_.assign(n, BodyView{});
    orbits_.assign(n, OrbitView{});
    x_.assign(n, V3{});
    v_.assign(n, V3{});
    rot_.assign(n, M3{});
    render_pos_.assign(n, V3{});
    for (size_t i = 0; i < n; ++i) {
        const sc::BodyInfo &b = cat_.bodies[i];
        BodyView &bv = views_[i];
        bv.sphere.set_body(b.radii);
        const bool star = b.kind == "star";
        bv.material = make_material(star ? "res://shaders/sun.gdshader" : "res://shaders/body.gdshader");
        bv.material->set_shader_parameter("radii", to_vec3(b.radii));
        // без appearance.json — серый по альбедо (как на этапе 4)
        const double alb = std::isnan(b.geometric_albedo) ? 0.5 : std::clamp(b.geometric_albedo, 0.0, 1.0);
        const float g = static_cast<float>(star ? 1.0 : 0.35 + 0.5 * alb);
        bv.material->set_shader_parameter("base_color", Color(g, g, g));
        // орбиты: родитель по каталогу; у планет со спутниками — орбита барицентра системы
        OrbitView &ov = orbits_[i];
        ov.parent = b.parent;
        for (const sc::BodyInfo &c : cat_.bodies)
            if (c.parent == static_cast<int>(i) && c.kind == "moon") ov.barycentric = true;
    }
    ring_material_ = make_material("res://shaders/ring.gdshader");
    orbit_material_ = make_material("res://shaders/orbit.gdshader");
    for (size_t i = 0; i < n; ++i)
        if (!cat_.bodies[i].rings.empty()) build_rings(static_cast<int>(i), views_[i]);
    load_appearance(d);
    load_sky(d);

    // старт: сейчас, если попадает в эфемериды; камера — у Земли
    mode_ = 0;
    auto_sim_ = false;
    et_ = std::clamp(now_et(), eph_lo_, eph_hi_);
    compute_states();
    const int earth = cat_.index_of_naif(399);
    if (earth >= 0) {
        rig_.anchor = earth;
        rig_.body_fixed = false;
        rig_.pos = unit(x_[0] - x_[static_cast<size_t>(earth)]) * 30000.0 + V3{0, 0, 15000};
        cam_frame_ = M3{};
        rig_.start_orbit(cam_frame());
    }
    loaded_ = true;
    error_ = "";
    return true;
}

Dictionary SolarSystem::body_info(int i) const {
    Dictionary d;
    if (i < 0 || i >= body_count()) return d;
    const sc::BodyInfo &b = cat_.bodies[static_cast<size_t>(i)];
    d["key"] = u8(b.key);
    d["name"] = u8(b.name);
    d["name_en"] = u8(b.name_en);
    d["kind"] = u8(b.kind);
    d["naif"] = b.naif;
    d["parent"] = b.parent;
    d["visible"] = b.visible;
    d["gm"] = b.gm;
    d["radius_km"] = b.mean_radius;
    PackedFloat64Array r;
    r.push_back(b.radii.x);
    r.push_back(b.radii.y);
    r.push_back(b.radii.z);
    d["radii_km"] = r;
    d["geometric_albedo"] = b.geometric_albedo;
    d["has_rotation"] = b.has_rotation;
    d["rings"] = static_cast<int>(b.rings.size());
    return d;
}

int SolarSystem::body_index(const String &key) const { return cat_.index_of_key(key.utf8().get_data()); }

// ---------------------------------------------------------------- время

double SolarSystem::now_et() const {
    using namespace std::chrono;
    const double unix_s = duration<double>(system_clock::now().time_since_epoch()).count();
    const double days = std::floor(unix_s / 86400.0);
    const double sod = unix_s - days * 86400.0;
    sc::UtcDate d;
    int m, dd;
    int64_t y;
    sc::civil_from_days(static_cast<int64_t>(days), y, m, dd);
    d.year = y;
    d.month = m;
    d.day = dd;
    d.hour = static_cast<int>(sod / 3600.0);
    d.minute = static_cast<int>((sod - d.hour * 3600.0) / 60.0);
    d.second = sod - d.hour * 3600.0 - d.minute * 60.0;
    return time_.utc_to_tdb(d);
}

String SolarSystem::et_to_utc(double et) const {
    const sc::UtcDate u = time_.tdb_to_utc(et);
    return fmt_calendar(u.year, u.month, u.day, u.hour, u.minute, u.second);
}

String SolarSystem::et_to_tdb(double et) const {
    const double days = std::floor((et + 43200.0) / 86400.0);
    const double sod = et + 43200.0 - days * 86400.0;
    int64_t y;
    int m, d;
    sc::civil_from_days(static_cast<int64_t>(days) + 10957, y, m, d);
    const int h = static_cast<int>(sod / 3600.0), mi = static_cast<int>((sod - h * 3600.0) / 60.0);
    return fmt_calendar(y, m, d, h, mi, sod - h * 3600.0 - mi * 60.0);
}

double SolarSystem::utc_to_et(const String &s) const {
    const std::string t = s.strip_edges().utf8().get_data();
    long long y = 0;
    int mo = 1, d = 1, h = 0, mi = 0;
    double sec = 0;
    char sep = ' ';
    const int n = std::sscanf(t.c_str(), "%lld-%d-%d%c%d:%d:%lf", &y, &mo, &d, &sep, &h, &mi, &sec);
    if (n < 3 || (n > 3 && n < 6) || (n >= 4 && sep != ' ' && sep != 'T')) return NAN;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec >= 61)
        return NAN;
    sc::UtcDate u;
    u.year = y;
    u.month = mo;
    u.day = d;
    u.hour = h;
    u.minute = mi;
    u.second = sec;
    return time_.utc_to_tdb(u);
}

bool SolarSystem::eph_covers(double et) const { return et >= eph_lo_ && et <= eph_hi_; }

PackedFloat64Array SolarSystem::ephemeris_range() const {
    PackedFloat64Array r;
    r.push_back(eph_lo_);
    r.push_back(eph_hi_);
    return r;
}

void SolarSystem::enter_sim(double et, bool automatic) {
    scen_.epoch = et;  // правки относятся к моменту старта «Симуляции»
    catch_up_ = NAN;
    if (!runner_.seed(et, error_s_)) {
        error_ = u8(error_s_);
        return;
    }
    mode_ = 1;
    auto_sim_ = automatic;
    et_ = et;
}

void SolarSystem::set_mode(int m) {
    if (!loaded_ || m == mode_) return;
    if (m == 1) {
        enter_sim(et_, false);
    } else {
        runner_.stop();
        mode_ = 0;
        auto_sim_ = false;
        edit_sim_ = false;
        catch_up_ = NAN;
        et_ = std::clamp(et_, eph_lo_, eph_hi_);
    }
    compute_states();
}

void SolarSystem::set_et(double et) {
    if (!loaded_ || std::isnan(et)) return;
    if (mode_ == 0) {
        if (eph_covers(et)) {
            et_ = et;
        } else {
            // вне эфемерид спутников — «Симуляция» от ближайшего края
            enter_sim(std::clamp(et, eph_lo_, eph_hi_), true);
            runner_.request(et, et > et_ ? 1e9 : -1e9);
        }
    } else if (eph_covers(et)) {
        enter_sim(et, auto_sim_);  // заново из эфемерид на новый момент, правки — к нему
    } else {
        runner_.request(et, et > et_ ? 1e9 : -1e9);
    }
    compute_states();
}

void SolarSystem::compute_states() {
    const size_t n = cat_.bodies.size();
    if (mode_ == 1) {
        std::vector<V3> x, v;
        if (runner_.state_at(et_, x, v) && x.size() == n) {
            x_ = std::move(x);
            v_ = std::move(v);
        }
    } else {
        for (size_t i = 0; i < n; ++i) eph_.state_ssb(cat_.bodies[i].naif, et_, x_[i], v_[i]);
    }
    for (size_t i = 0; i < n; ++i) rot_[i] = cat_.bodies[i].has_rotation ? cat_.bodies[i].rot.j2000_to_body(et_) : M3{};
}

String SolarSystem::status() const {
    String s;
    if (mode_ == 1) {
        const char *lv[] = {"полная модель", "спутники Марса на элементах", "спутники планет на элементах"};
        const int l = runner_.level();
        s = String::utf8(l >= 0 && l < 3 ? lv[l] : "");
        if (auto_sim_) s += String::utf8("; вне эфемерид спутников (") + et_to_utc(eph_lo_).substr(0, 4) + String::utf8("–") +
                            et_to_utc(eph_hi_).substr(0, 4) + String(")");
        if (!std::isnan(catch_up_)) {
            double lo, hi;
            const double done = runner_.range(lo, hi) ? hi : scen_.epoch;
            const double span = std::max(1.0, std::fabs(catch_up_ - scen_.epoch));
            s += String::utf8("; пересчёт от момента правок: ") +
                 String::num_int64(static_cast<int64_t>(std::clamp(std::fabs(done - scen_.epoch) / span, 0.0, 1.0) * 100)) + String("%");
        }
        const std::string e = runner_.error();
        if (!e.empty())
            s += String::utf8("; расчёт остановлен (") + u8(e) +
                 String::utf8("): тела, вероятно, столкнулись — слияния тел нет, отмените правку (Ctrl+Z)");
    } else if (scen_.dynamic()) {
        s = String::utf8("правки массы, скорости и расстояния действуют только в «Симуляции»");
    }
    if (!error_.is_empty()) s += String::utf8(s.is_empty() ? "" : "; ") + error_;
    return s;
}

// ---------------------------------------------------------------- камера

M3 SolarSystem::frame_to_icrf(int anchor, bool body_fixed) const {
    if (body_fixed && anchor >= 0) return rot_[static_cast<size_t>(anchor)].transposed();
    return M3{};
}

sc::CamFrame SolarSystem::cam_frame() const {
    sc::CamFrame f;
    const int a = rig_.anchor;
    if (a < 0) return f;
    const size_t ai = static_cast<size_t>(a);
    const M3 F = frame_to_icrf(a, rig_.body_fixed);
    const V3 pb = rig_.body_fixed ? rig_.pos : rot_[ai] * rig_.pos;  // положение камеры в осях тела
    f.surface_r = sc::norm(pb) > 0 ? views_[ai].sphere.radius_along(unit(pb)) : cat_.bodies[ai].mean_radius;
    f.body_radius = cat_.bodies[ai].mean_radius;
    f.sun_dir = F.transposed() * unit(x_[0] - x_[ai]);
    if (rig_.body_fixed) {
        f.up = {0, 0, 1};
    } else {
        f.up = {0, -std::sin(sc::kObliquityJ2000), std::cos(sc::kObliquityJ2000)};  // полюс эклиптики в ICRF
    }
    return f;
}

void SolarSystem::rebase(int anchor, bool body_fixed) {
    const M3 F_old = frame_to_icrf(rig_.anchor, rig_.body_fixed);
    const M3 F_new = frame_to_icrf(anchor, body_fixed);
    const V3 xo = rig_.anchor >= 0 ? x_[static_cast<size_t>(rig_.anchor)] : V3{};
    const V3 xn = anchor >= 0 ? x_[static_cast<size_t>(anchor)] : V3{};
    const V3 rel_icrf = (xo - xn) + F_old * rig_.pos;  // камера относительно нового центра
    const M3 T = F_new.transposed() * F_old;
    rig_.pos = F_new.transposed() * rel_icrf;
    rig_.orient = sc::Quat::from_matrix(T * rig_.orient.matrix());
    rig_.vel = T * rig_.vel;
    rig_.anchor = anchor;
    rig_.body_fixed = body_fixed;
    prev_anchor_ = -2;
}

void SolarSystem::auto_anchor() {
    if (rig_.mode != sc::CamMode::Free || rig_.body_fixed) return;
    // самое «глубокое» тело, в сфере Хилла которого камера
    int best = 0;
    double best_r = INFINITY;
    for (size_t i = 1; i < cat_.bodies.size(); ++i) {
        const sc::BodyInfo &b = cat_.bodies[i];
        if (!b.visible || b.parent < 0 || b.gm <= 0) continue;
        const sc::BodyInfo &p = cat_.bodies[static_cast<size_t>(b.parent)];
        if (p.gm <= 0) continue;
        const double rh = sc::norm(x_[i] - x_[static_cast<size_t>(b.parent)]) * std::cbrt(b.gm / (3 * p.gm));
        if (sc::norm(cam_abs_ - x_[i]) < rh && rh < best_r) {
            best = static_cast<int>(i);
            best_r = rh;
        }
    }
    if (best != rig_.anchor) rebase(best, false);
}

V3 SolarSystem::rel_to_camera(int i) const {
    const V3 off = cam_frame_ * rig_.pos;
    if (i == rig_.anchor) return off * -1.0;
    if (rig_.anchor >= 0) return (x_[static_cast<size_t>(i)] - x_[static_cast<size_t>(rig_.anchor)]) - off;
    return x_[static_cast<size_t>(i)] - cam_abs_;
}

void SolarSystem::camera_free_input(const Vector3 &move, const Vector3 &rot) {
    in_move_ = {move.x, move.y, move.z};
    in_rot_ = {rot.x, rot.y, rot.z};
    in_free_ = true;
}

void SolarSystem::camera_orbit_input(double dyaw, double dpitch, double zoom_steps) {
    in_dyaw_ += dyaw;
    in_dpitch_ += dpitch;
    in_zoom_ += zoom_steps;
    in_orbit_ = true;
}

void SolarSystem::camera_fly_to(int body) {
    if (!loaded_ || body < 0 || body >= body_count()) return;
    rebase(body, false);
    cam_frame_ = frame_to_icrf(rig_.anchor, rig_.body_fixed);
    const double r = cat_.bodies[static_cast<size_t>(body)].mean_radius;
    rig_.start_flyto(cam_frame(), std::max(4.0 * r, r + 1.0));
}

void SolarSystem::camera_orbit(int body) {
    if (!loaded_ || body < 0 || body >= body_count()) return;
    rebase(body, rig_.body_fixed && body == rig_.anchor);
    cam_frame_ = frame_to_icrf(rig_.anchor, rig_.body_fixed);
    rig_.start_orbit(cam_frame());
}

void SolarSystem::camera_set_body_fixed(bool on) {
    if (!loaded_ || rig_.anchor < 0 || on == rig_.body_fixed) return;
    const sc::CamMode m = rig_.mode;
    rebase(rig_.anchor, on);
    cam_frame_ = frame_to_icrf(rig_.anchor, rig_.body_fixed);
    if (m == sc::CamMode::Orbit) rig_.start_orbit(cam_frame());
}

void SolarSystem::camera_place(int body, double lat, double lon, double alt_m, double azimuth, double elevation,
                               bool body_fixed) {
    if (!loaded_ || body < 0 || body >= body_count()) return;
    const size_t bi = static_cast<size_t>(body);
    const double la = lat * sc::kDeg, lo = lon * sc::kDeg;
    const V3 u{std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)};
    const sc::CubeSphere &cs = views_[bi].sphere;
    const V3 p = u * cs.radius_along(u);
    const V3 up = unit(V3{p.x / (cs.radii.x * cs.radii.x), p.y / (cs.radii.y * cs.radii.y), p.z / (cs.radii.z * cs.radii.z)});
    V3 east = sc::cross(V3{0, 0, 1}, up);
    east = sc::norm(east) > 1e-9 ? unit(east) : V3{0, 1, 0};
    const V3 north = sc::cross(up, east);
    const double az = azimuth * sc::kDeg, el = elevation * sc::kDeg;
    const V3 fwd = (north * std::cos(az) + east * std::sin(az)) * std::cos(el) + up * std::sin(el);
    V3 right = sc::cross(fwd, up);
    right = sc::norm(right) > 1e-9 ? unit(right) : east;
    const V3 cup = sc::cross(right, fwd);
    M3 m;
    for (int i = 0; i < 3; ++i) {
        m.m[i][0] = right[i];
        m.m[i][1] = cup[i];
        m.m[i][2] = -fwd[i];
    }
    const V3 pos_body = p + up * (alt_m / kKm);
    rig_.anchor = body;
    rig_.body_fixed = body_fixed;
    rig_.vel = V3{};
    rig_.mode = sc::CamMode::Free;
    if (body_fixed) {
        rig_.pos = pos_body;
        rig_.orient = sc::Quat::from_matrix(m);
    } else {
        const M3 rt = rot_[bi].transposed();
        rig_.pos = rt * pos_body;
        rig_.orient = sc::Quat::from_matrix(rt * m);
    }
    prev_anchor_ = -2;
    cam_frame_ = frame_to_icrf(rig_.anchor, rig_.body_fixed);
}

double SolarSystem::camera_altitude() const {
    if (rig_.anchor < 0) return sc::norm(rig_.pos);
    return sc::norm(rig_.pos) - cam_frame().surface_r;
}

double SolarSystem::camera_flyto_progress() const {
    if (rig_.mode != sc::CamMode::FlyTo) return 1.0;
    return std::min(1.0, rig_.flyto_elapsed() / rig_.flyto_duration());
}

PackedFloat64Array SolarSystem::camera_position_icrf() const {
    PackedFloat64Array a;
    for (int c = 0; c < 3; ++c) a.push_back(cam_abs_[c]);
    return a;
}

PackedFloat64Array SolarSystem::body_position_icrf(int i) const {
    PackedFloat64Array a;
    if (i < 0 || i >= body_count()) return a;
    for (int c = 0; c < 3; ++c) a.push_back(x_[static_cast<size_t>(i)][c]);
    return a;
}

double SolarSystem::distance_to_body(int i) const {
    if (i < 0 || i >= body_count()) return NAN;
    return sc::norm(render_pos_[static_cast<size_t>(i)]);
}

PackedFloat64Array SolarSystem::body_state_icrf(int i) const {
    PackedFloat64Array a;
    if (i < 0 || i >= body_count()) return a;
    for (int c = 0; c < 3; ++c) a.push_back(x_[static_cast<size_t>(i)][c]);
    for (int c = 0; c < 3; ++c) a.push_back(v_[static_cast<size_t>(i)][c]);
    return a;
}

double SolarSystem::body_ephemeris_deviation(int i) const {
    if (mode_ != 1 || i < 0 || i >= body_count() || !eph_covers(et_)) return NAN;
    V3 x, v;
    if (!eph_.state_ssb(cat_.bodies[static_cast<size_t>(i)].naif, et_, x, v)) return NAN;
    return sc::norm(x_[static_cast<size_t>(i)] - x);
}

PackedFloat64Array SolarSystem::camera_get_state() const {
    PackedFloat64Array a;
    const double v[] = {static_cast<double>(rig_.anchor), rig_.body_fixed ? 1.0 : 0.0, static_cast<double>(rig_.mode),
                        rig_.pos.x, rig_.pos.y, rig_.pos.z, rig_.orient.w, rig_.orient.x, rig_.orient.y, rig_.orient.z,
                        rig_.orbit_dist, rig_.yaw, rig_.pitch, rig_.speed};
    for (double x : v) a.push_back(x);
    return a;
}

void SolarSystem::camera_set_state(const PackedFloat64Array &s) {
    if (s.size() < 14 || !loaded_) return;
    const int anchor = static_cast<int>(s[0]);
    if (anchor < -1 || anchor >= body_count()) return;
    rig_.anchor = anchor;
    rig_.body_fixed = s[1] != 0.0;
    const int m = static_cast<int>(s[2]);
    rig_.mode = m == 1 ? sc::CamMode::Orbit : sc::CamMode::Free;
    rig_.pos = {s[3], s[4], s[5]};
    rig_.orient = sc::Quat{s[6], s[7], s[8], s[9]}.normalized();
    rig_.orbit_dist = s[10];
    rig_.yaw = s[11];
    rig_.pitch = s[12];
    rig_.speed = s[13];
    rig_.vel = V3{};
    prev_anchor_ = -2;
    cam_frame_ = frame_to_icrf(rig_.anchor, rig_.body_fixed);
}

void SolarSystem::camera_look_from(int body, const Vector3 &dir, double dist_km, bool body_frame) {
    if (!loaded_ || body < 0 || body >= body_count()) return;
    V3 d = unit(V3{dir.x, dir.y, dir.z});
    if (!body_frame) d = rot_[static_cast<size_t>(body)] * d;  // направление задано в ICRF
    rig_.anchor = body;
    rig_.body_fixed = false;
    rig_.vel = V3{};
    rig_.pos = rot_[static_cast<size_t>(body)].transposed() * d * dist_km;
    prev_anchor_ = -2;
    cam_frame_ = M3{};
    rig_.start_orbit(cam_frame());
}

Vector3 SolarSystem::marker_render_position(int id) const {
    if (id < 0 || id >= static_cast<int>(markers_.size())) return Vector3();
    const Marker &m = markers_[static_cast<size_t>(id)];
    const size_t b = static_cast<size_t>(m.body);
    return to_vec3(view_ * (render_pos_[b] + rot_[b].transposed() * m.pos) * kKm);
}

PackedByteArray SolarSystem::reference_classes(int w, int h) const {
    PackedByteArray out;
    if (!loaded_ || w <= 0 || h <= 0) return out;
    out.resize(static_cast<int64_t>(w) * h);
    uint8_t *px = out.ptrw();
    const double ty = std::tan(fov_deg_ * sc::kDeg / 2) / zoom_, tx = ty * w / h;
    const M3 cb = cam_frame_ * rig_.orient.matrix();  // оси камеры → ICRF
    struct Cand {
        size_t i;
        V3 dir;        // на центр, единичный
        double cos_r;  // косинус углового радиуса (с кольцами)
        double rings_in = 0, rings_mid0 = 0, rings_mid1 = 0, rings_out = 0;
    };
    std::vector<Cand> cands;
    for (size_t i = 0; i < cat_.bodies.size(); ++i) {
        const sc::BodyInfo &b = cat_.bodies[i];
        if (!b.visible) continue;
        double rmax = std::max(b.radii.x, std::max(b.radii.y, b.radii.z));
        Cand c;
        c.i = i;
        for (const sc::RingBand &rb : b.rings) {
            if (rb.name == "C") c.rings_in = rb.inner_km;
            if (rb.name == "B") c.rings_mid0 = rb.outer_km;
            if (rb.name == "A") {
                c.rings_mid1 = rb.inner_km;
                c.rings_out = rb.outer_km;
            }
        }
        rmax = std::max(rmax, c.rings_out);
        const double d = sc::norm(render_pos_[i]);
        c.dir = render_pos_[i] / d;
        c.cos_r = d > rmax ? std::cos(std::asin(rmax / d)) : -1.0;
        cands.push_back(c);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double nx = (2.0 * (x + 0.5) / w - 1.0) * tx, ny = (1.0 - 2.0 * (y + 0.5) / h) * ty;
            const V3 d = unit(cb * V3{nx, ny, -1.0});
            double best = INFINITY;
            uint8_t cls = 0;
            for (const Cand &c : cands) {
                if (sc::dot(d, c.dir) < c.cos_r) continue;
                const M3 &R = rot_[c.i];
                const V3 o = R * (render_pos_[c.i] * -1.0), db = R * d;  // камера и луч в осях тела
                double k;
                if (views_[c.i].sphere.ray_hit(o, db, k) && k < best) {
                    best = k;
                    cls = static_cast<uint8_t>(c.i + 1);
                }
                if (c.rings_out > 0 && db.z != 0) {
                    const double t = -o.z / db.z;
                    if (t > 0 && t < best) {
                        const V3 p = o + db * t;
                        const double r = std::hypot(p.x, p.y);
                        if ((r >= c.rings_in && r <= c.rings_mid0) || (r >= c.rings_mid1 && r <= c.rings_out)) {
                            best = t;
                            cls = 255;
                        }
                    }
                }
            }
            px[static_cast<size_t>(y) * w + x] = cls;
        }
    return out;
}

// ---------------------------------------------------------------- кадр

void SolarSystem::update_frame(double delta) {
    if (!loaded_) return;
    ++frame_;
    frame_dt_ = delta;
    // 1. время
    const double prev = et_;
    const double desired = paused_ ? et_ : et_ + rate_ * delta;
    if (mode_ == 0) {
        if (eph_covers(desired)) {
            et_ = desired;
        } else {
            const double edge = std::clamp(desired, eph_lo_, eph_hi_);
            enter_sim(edge, true);
            runner_.request(desired, rate_);
        }
    } else if (!std::isnan(catch_up_)) {
        // после правки: пересчёт от epoch правок до текущего момента, время на экране стоит на нём
        if (!paused_) catch_up_ += rate_ * delta;
        runner_.request(catch_up_, catch_up_ >= scen_.epoch ? 1e9 : -1e9);
        double lo, hi;
        if (runner_.range(lo, hi) && catch_up_ >= lo && catch_up_ <= hi) {
            et_ = catch_up_;
            catch_up_ = NAN;
        }
    } else {
        runner_.request(desired, paused_ ? 0.0 : rate_);
        double lo, hi;
        if (runner_.range(lo, hi)) et_ = std::clamp(desired, lo, hi);
        // автоматическая «Симуляция» заканчивается, когда время вернулось в эфемериды (с запасом от края)
        const double margin = std::max(86400.0, std::fabs(rate_) * 0.5);
        if (auto_sim_ && !scen_.dynamic() && et_ > eph_lo_ + margin && et_ < eph_hi_ - margin) {
            runner_.stop();
            mode_ = 0;
            auto_sim_ = false;
        }
    }
    if (delta > 0) actual_rate_ = 0.85 * actual_rate_ + 0.15 * (et_ - prev) / delta;
    compute_states();

    // 2. камера
    cam_frame_ = frame_to_icrf(rig_.anchor, rig_.body_fixed);
    const V3 pos_before = rig_.pos;
    if (rig_.mode == sc::CamMode::FlyTo) {
        rig_.update_flyto(delta, cam_frame());
    } else if (in_free_ || rig_.mode == sc::CamMode::Free) {
        const bool any = sc::norm(in_move_) > 0 || sc::norm(in_rot_) > 0;
        if (rig_.mode == sc::CamMode::Free || any) rig_.update_free(in_move_, in_rot_, delta, cam_frame());
    }
    if (rig_.mode == sc::CamMode::Orbit && in_orbit_) rig_.update_orbit(in_dyaw_, in_dpitch_, in_zoom_, cam_frame());
    in_move_ = in_rot_ = V3{};
    in_dyaw_ = in_dpitch_ = in_zoom_ = 0;
    in_free_ = in_orbit_ = false;
    const V3 xa = rig_.anchor >= 0 ? x_[static_cast<size_t>(rig_.anchor)] : V3{};
    cam_abs_ = xa + cam_frame_ * rig_.pos;
    auto_anchor();
    cam_frame_ = frame_to_icrf(rig_.anchor, rig_.body_fixed);
    cam_abs_ = (rig_.anchor >= 0 ? x_[static_cast<size_t>(rig_.anchor)] : V3{}) + cam_frame_ * rig_.pos;
    // скорость камеры относительно опорного тела (в его системе отсчёта)
    if (prev_anchor_ == rig_.anchor && delta > 0) {
        const double sp = sc::norm(rig_.pos - pos_before) / delta;
        cam_speed_ = sp;
    }
    prev_anchor_ = rig_.anchor;

    for (size_t i = 0; i < render_pos_.size(); ++i) render_pos_[i] = rel_to_camera(static_cast<int>(i));

    // Оси камеры: ICRF → камера. Сама Camera3D стоит в нуле без поворота, а поворот камеры входит в преобразование
    // каждого объекта, посчитанное в double. Тогда у объекта возле оси взгляда координаты x, y малы и точны во float —
    // это важно при большом увеличении: на 10¹³ м шаг float ≈ 1000 км, а смещение вдоль оси на картинку почти не влияет.
    view_ = (cam_frame_ * rig_.orient.matrix()).transposed();
    if (camera_) {
        camera_->set_global_transform(Transform3D());
        // ближняя плоскость — по ближайшей поверхности; буфер глубины — float с обратным Z, дальнюю можно далеко
        double nearest = INFINITY;  // км до ближайшей поверхности (эллипсоид в направлении на камеру)
        for (size_t i = 0; i < render_pos_.size(); ++i) {
            if (!cat_.bodies[i].visible) continue;
            const double d = sc::norm(render_pos_[i]);
            if (d <= 0) continue;
            const V3 u = rot_[i] * (render_pos_[i] * (-1.0 / d));
            nearest = std::min(nearest, d - views_[i].sphere.radius_along(u));
        }
        if (rig_.anchor >= 0) nearest = std::min(nearest, camera_altitude());
        // Ближняя плоскость — половина расстояния до ближайшей поверхности. Дальше D0 = 10⁴·near расстояния
        // сжимаются логарифмически в шейдерах (shaders/compress.gdshaderinc), и вся сцена до 10¹⁸ м укладывается
        // в far/near < 4·10⁵: при far/near > ~10⁷ Godot (float32) вырождает плоскость отсечения и не рисует ничего.
        const double near_m = std::clamp(0.5 * nearest * kKm, 0.005, 1.0e17);
        log_d0_ = kCompressStart * near_m;
        const double d_far = 1.0e18;  // дальше ничего не рисуем
        const double far_m = (d_far <= log_d0_ ? d_far : log_d0_ * (1.0 + std::log(d_far / log_d0_))) * 1.05;
        RenderingServer::get_singleton()->global_shader_parameter_set("log_d0", log_d0_);
        const double t_half = std::tan(fov_deg_ * sc::kDeg / 2) / zoom_;  // тангенс половины вертикального поля
        if (zoom_ <= 1.0) {
            camera_->set_perspective(static_cast<real_t>(fov_deg_), static_cast<real_t>(near_m), static_cast<real_t>(far_m));
        } else {  // телескоп: усечённая пирамида с тем же центром и полем в zoom раз уже
            camera_->set_frustum(static_cast<real_t>(2 * near_m * t_half), Vector2(), static_cast<real_t>(near_m),
                                 static_cast<real_t>(far_m));
        }
        const Vector2 vp = get_viewport()->get_visible_rect().size;
        px_per_rad_ = 0.5 * vp.y / t_half;
        const double aspect = vp.y > 0 ? vp.x / vp.y : 1.0;
        view_half_ = std::atan(t_half * std::sqrt(1 + aspect * aspect));
        if (orbit_material_.is_valid()) orbit_material_->set_shader_parameter("near_clip", near_m * 1.01);
    }

    // 3. отрисовка
    update_bodies();
    update_orbits();
    update_markers();
    clock_ += delta;
    update_textures(clock_);
}

Vector3 SolarSystem::body_render_position(int i) const {
    if (i < 0 || i >= body_count()) return Vector3();
    return to_vec3(view_ * render_pos_[static_cast<size_t>(i)] * kKm);
}

double SolarSystem::body_angular_radius(int i) const {
    if (i < 0 || i >= body_count()) return 0;
    const double d = sc::norm(render_pos_[static_cast<size_t>(i)]);
    const double r = cat_.bodies[static_cast<size_t>(i)].mean_radius;
    return d > r ? std::asin(r / d) : sc::kPi / 2;
}

bool SolarSystem::body_hidden(int i) const {
    if (i < 0 || i >= body_count() || !camera_) return true;
    const V3 ri = render_pos_[static_cast<size_t>(i)];
    const double di = sc::norm(ri);
    const M3 cb = cam_frame_ * rig_.orient.matrix();
    const V3 fwd{-cb.m[0][2], -cb.m[1][2], -cb.m[2][2]};
    if (sc::dot(ri, fwd) <= 0) return true;
    for (size_t j = 0; j < render_pos_.size(); ++j) {
        if (static_cast<int>(j) == i || !cat_.bodies[j].visible) continue;
        const double dj = sc::norm(render_pos_[j]);
        const double rj = cat_.bodies[j].mean_radius;
        if (dj >= di || dj <= rj) continue;
        const double sep = std::acos(std::clamp(sc::dot(ri, render_pos_[j]) / (di * dj), -1.0, 1.0));
        if (sep < std::asin(rj / dj)) return true;
    }
    return false;
}

Transform3D SolarSystem::scene_transform(const M3 &basis_icrf, const V3 &origin_icrf_km) const {
    return Transform3D(to_basis(view_ * basis_icrf), to_vec3(view_ * origin_icrf_km * kKm));
}

// ---------------------------------------------------------------- тела

M3 SolarSystem::body_rotation(int i) const { return rot_[static_cast<size_t>(i)]; }

void SolarSystem::update_bodies() {
    visible_chunks_ = 0;
    int budget = kChunkBuildsPerFrame;
    const double px_per_rad = px_per_rad_;
    // направление взгляда (ICRF) — для отсечения чанков вне поля зрения
    const M3 cb = cam_frame_ * rig_.orient.matrix();
    const V3 fwd{-cb.m[0][2], -cb.m[1][2], -cb.m[2][2]};
    // порог деления: размер чанка/расстояние; при увеличении — во столько же раз меньше (чанки крупнее на экране)
    const double split = lod_split_ * 860.0 / px_per_rad;
    for (size_t i = 0; i < views_.size(); ++i) {
        BodyView &bv = views_[i];
        const sc::BodyInfo &b = cat_.bodies[i];
        if (!b.visible) continue;
        const V3 rel = render_pos_[i];
        const double d = sc::norm(rel);
        const double rmax = std::max(b.radii.x, std::max(b.radii.y, b.radii.z));
        bv.px = d > rmax ? 2.0 * std::asin(rmax / d) * px_per_rad : 1e9;  // диаметр на экране (для карт 8K)
        const bool tiny = d > rmax && std::asin(rmax / d) * px_per_rad < 0.5;
        auto hide_all = [&]() {
            for (uint64_t k : bv.shown) {
                auto it = bv.chunks.find(k);
                if (it != bv.chunks.end() && it->second.visible) {
                    RenderingServer::get_singleton()->instance_set_visible(it->second.instance, false);
                    it->second.visible = false;
                }
            }
            bv.shown.clear();
            for (const RID &r : bv.ring_instances) RenderingServer::get_singleton()->instance_set_visible(r, false);
            if (bv.sky_instance.is_valid()) RenderingServer::get_singleton()->instance_set_visible(bv.sky_instance, false);
            bv.drawn = false;
        };
        if (tiny) {
            if (bv.drawn) hide_all();
            continue;
        }
        update_lighting(static_cast<int>(i), bv);
        // LOD: камера в осях тела
        const V3 cam_body = rot_[i] * (rel * -1.0);
        std::vector<sc::ChunkId> want;
        bv.sphere.select(cam_body, split, want, rot_[i] * fwd, std::min(4.0, view_half_ * 1.3 + 0.05));
        RenderingServer *rs = RenderingServer::get_singleton();
        const RID scenario = get_world_3d()->get_scenario();
        bool complete = true;
        for (const sc::ChunkId &id : want) {
            const uint64_t key = id.key();
            if (bv.chunks.count(key)) continue;
            if (budget <= 0) {
                complete = false;
                continue;
            }
            --budget;
            sc::ChunkMesh cm;
            bv.sphere.build(id, cm);
            const size_t nv = cm.pos.size() / 3;
            PackedVector3Array verts, normals;
            PackedFloat32Array dirs;
            PackedInt32Array idx;
            verts.resize(static_cast<int64_t>(nv));
            normals.resize(static_cast<int64_t>(nv));
            dirs.resize(static_cast<int64_t>(nv * 3));
            for (size_t k = 0; k < nv; ++k) {
                verts.set(static_cast<int64_t>(k), Vector3(cm.pos[k * 3], cm.pos[k * 3 + 1], cm.pos[k * 3 + 2]));
                normals.set(static_cast<int64_t>(k), Vector3(cm.normal[k * 3], cm.normal[k * 3 + 1], cm.normal[k * 3 + 2]));
            }
            std::copy(cm.dir.begin(), cm.dir.end(), dirs.ptrw());
            idx.resize(static_cast<int64_t>(cm.index.size()));
            std::copy(cm.index.begin(), cm.index.end(), idx.ptrw());
            Array arrays;
            arrays.resize(RenderingServer::ARRAY_MAX);
            arrays[RenderingServer::ARRAY_VERTEX] = verts;
            arrays[RenderingServer::ARRAY_NORMAL] = normals;
            arrays[RenderingServer::ARRAY_CUSTOM0] = dirs;
            arrays[RenderingServer::ARRAY_INDEX] = idx;
            Chunk ch;
            ch.mesh = rs->mesh_create();
            rs->mesh_add_surface_from_arrays(
                ch.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(),
                static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGB_FLOAT) << RenderingServer::ARRAY_FORMAT_CUSTOM0_SHIFT);
            ch.instance = rs->instance_create2(ch.mesh, scenario);
            rs->instance_geometry_set_material_override(ch.instance, bv.material->get_rid());
            rs->instance_geometry_set_cast_shadows_setting(ch.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            rs->instance_set_custom_aabb(ch.instance, kHugeAabb);
            rs->instance_set_visible(ch.instance, false);
            ch.center = cm.center;
            ch.radius = cm.radius;
            bv.chunks.emplace(key, ch);
        }
        // новый набор показываем, только когда построены все его чанки: без дыр при быстром полёте
        if (complete || bv.shown.empty()) {
            std::vector<uint64_t> next;
            for (const sc::ChunkId &id : want)
                if (bv.chunks.count(id.key())) next.push_back(id.key());
            for (uint64_t k : bv.shown) {
                if (std::find(next.begin(), next.end(), k) != next.end()) continue;
                auto it = bv.chunks.find(k);
                if (it != bv.chunks.end() && it->second.visible) {
                    rs->instance_set_visible(it->second.instance, false);
                    it->second.visible = false;
                }
            }
            bv.shown = std::move(next);
            bv.max_level_shown = 0;
            for (const sc::ChunkId &id : want) bv.max_level_shown = std::max(bv.max_level_shown, id.level);
        }
        // floating origin: центр чанка относительно камеры = (центр тела − камера) + R^T·центр чанка
        const M3 rt = rot_[i].transposed();
        const M3 vr = view_ * rt;
        const Basis basis = to_basis(vr);
        for (uint64_t k : bv.shown) {
            Chunk &ch = bv.chunks[k];
            ch.used = frame_;
            const V3 o = rel + rt * ch.center;
            rs->instance_set_transform(ch.instance, Transform3D(basis, to_vec3(view_ * o * kKm)));
            if (!ch.visible) {
                rs->instance_set_visible(ch.instance, true);
                ch.visible = true;
            }
        }
        visible_chunks_ += static_cast<int>(bv.shown.size());
        for (size_t r = 0; r < bv.ring_instances.size(); ++r) {
            const V3 o = rel + rt * bv.ring_centers[r];
            rs->instance_set_transform(bv.ring_instances[r], Transform3D(basis, to_vec3(view_ * o * kKm)));
            rs->instance_set_visible(bv.ring_instances[r], true);
        }
        if (bv.sky_instance.is_valid()) {
            const bool sky = fx_atmosphere_ && !flat_ && debug_view_ == 0;
            rs->instance_set_transform(bv.sky_instance, Transform3D(basis, to_vec3(view_ * rel * kKm)));
            rs->instance_set_visible(bv.sky_instance, sky);
        }
        bv.drawn = true;
        // вытеснение давно не нужных чанков
        if (bv.chunks.size() > kChunkCacheMax) {
            std::vector<std::pair<uint64_t, uint64_t>> age;
            for (auto &kv : bv.chunks)
                if (!kv.second.visible) age.emplace_back(kv.second.used, kv.first);
            std::sort(age.begin(), age.end());
            for (size_t k = 0; k < age.size() && bv.chunks.size() > kChunkCacheMax / 2; ++k) {
                Chunk &ch = bv.chunks[age[k].second];
                rs->free_rid(ch.instance);
                rs->free_rid(ch.mesh);
                bv.chunks.erase(age[k].second);
            }
        }
    }
    // корона: квадрат вокруг Солнца строит шейдер по положению Солнца в осях камеры
    if (corona_instance_.is_valid()) {
        corona_material_->set_shader_parameter("sun_view", to_vec3(view_ * render_pos_[0]));
        RenderingServer::get_singleton()->instance_set_visible(corona_instance_, !flat_ && debug_view_ == 0);
    }
    update_sky();
}

int SolarSystem::chunk_level_max(int body) const {
    if (body < 0 || body >= body_count()) return -1;
    return views_[static_cast<size_t>(body)].max_level_shown;
}

void SolarSystem::build_rings(int i, BodyView &bv) {
    // Кольца C, B, A (основные видимые), между B и A — деление Кассини. Один меш на все кольца: у отдельных мешей
    // общие края во float расходятся, и между ними просвечивают щели. Вершины — в осях планеты относительно её центра
    // (шаг float у края колец ≈ 8 м: заметно только ближе ~10 км к плоскости колец). Цвет вершины: rgb — альбедо,
    // a — τ/4 по средней оптической толщине кольца со страницы NASA (свет и прозрачность — ring.gdshader).
    RenderingServer *rs = RenderingServer::get_singleton();
    const int n_ang = 720;
    PackedVector3Array verts, normals;
    PackedColorArray colors;
    PackedInt32Array idx;
    for (const sc::RingBand &rb : cat_.bodies[static_cast<size_t>(i)].rings) {
        if (rb.name != "C" && rb.name != "B" && rb.name != "A") continue;
        const double tau = 0.5 * (rb.tau_lo + rb.tau_hi), alb = 0.5 * (rb.albedo_lo + rb.albedo_hi);
        const Color col(static_cast<float>(alb), static_cast<float>(alb), static_cast<float>(alb),
                        static_cast<float>(std::clamp(tau / 4.0, 0.0, 1.0)));
        const int base = static_cast<int>(verts.size());
        for (int k = 0; k <= n_ang; ++k) {
            const double ang = 2 * sc::kPi * (k % n_ang) / n_ang;  // последний столбец совпадает с первым до бита
            for (double rr : {rb.inner_km, rb.outer_km}) {
                verts.push_back(to_vec3(V3{rr * std::cos(ang), rr * std::sin(ang), 0} * kKm));
                normals.push_back(Vector3(0, 0, 1));
                colors.push_back(col);
            }
        }
        for (int k = 0; k < n_ang; ++k) {
            const int b0 = base + 2 * k;
            const int tri[6] = {b0, b0 + 1, b0 + 2, b0 + 1, b0 + 3, b0 + 2};
            for (int t : tri) idx.push_back(t);
        }
    }
    if (verts.is_empty()) return;
    Array arrays;
    arrays.resize(RenderingServer::ARRAY_MAX);
    arrays[RenderingServer::ARRAY_VERTEX] = verts;
    arrays[RenderingServer::ARRAY_NORMAL] = normals;
    arrays[RenderingServer::ARRAY_COLOR] = colors;
    arrays[RenderingServer::ARRAY_INDEX] = idx;
    const RID mesh = rs->mesh_create();
    rs->mesh_add_surface_from_arrays(mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
    const RID inst = rs->instance_create2(mesh, get_world_3d()->get_scenario());
    rs->instance_geometry_set_material_override(inst, ring_material_->get_rid());
    rs->instance_geometry_set_cast_shadows_setting(inst, RenderingServer::SHADOW_CASTING_SETTING_OFF);
    rs->instance_set_custom_aabb(inst, kHugeAabb);
    rs->instance_set_visible(inst, false);
    bv.ring_meshes.push_back(mesh);
    bv.ring_instances.push_back(inst);
    bv.ring_centers.push_back(V3{});
}

// ---------------------------------------------------------------- орбиты

void SolarSystem::build_orbit(int i) {
    OrbitView &ov = orbits_[static_cast<size_t>(i)];
    const sc::BodyInfo &b = cat_.bodies[static_cast<size_t>(i)];
    if (ov.parent < 0) return;
    const size_t p = static_cast<size_t>(ov.parent);
    // состояние относительно родителя; у планет со спутниками — барицентр системы
    V3 x = x_[static_cast<size_t>(i)], v = v_[static_cast<size_t>(i)];
    double gm = b.gm;
    if (ov.barycentric) {
        V3 sx = x * b.gm, sv = v * b.gm;
        for (size_t k = 0; k < cat_.bodies.size(); ++k)
            if (cat_.bodies[k].parent == i && cat_.bodies[k].kind == "moon") {
                sx += x_[k] * cat_.bodies[k].gm;
                sv += v_[k] * cat_.bodies[k].gm;
                gm += cat_.bodies[k].gm;
            }
        if (gm > 0) {
            x = sx / gm;
            v = sv / gm;
        }
    }
    const sc::Conic c = sc::conic_from_state(x - x_[p], v - v_[p], cat_.bodies[p].gm + gm);
    if (!c.valid || c.e >= 1.0) return;
    // эксцентрическая аномалия от тела по движению; у тела точки гуще (шаг в 10 раз меньше)
    const double E0 = sc::eccentric_from_true(c.nu, c.e);
    std::vector<V3> pts(kOrbitSegments + 1);
    std::vector<float> fade(kOrbitSegments + 1);
    for (int k = 0; k <= kOrbitSegments; ++k) {
        const double u = static_cast<double>(k) / kOrbitSegments;
        const double g = u - 0.9 * std::sin(2 * sc::kPi * u) / (2 * sc::kPi);
        const double E = E0 + 2 * sc::kPi * g;
        pts[static_cast<size_t>(k)] = sc::conic_point(c, sc::true_from_eccentric(E, c.e)) * kKm;
        fade[static_cast<size_t>(k)] = static_cast<float>(0.3 + 0.7 * u * u);  // след: ярче всего позади тела
    }
    pts[0] = (x - x_[p]) * kKm;  // начало и конец — точно в теле
    pts[kOrbitSegments] = pts[0];
    PackedVector3Array verts;
    PackedFloat32Array c0, c1, c2;
    PackedInt32Array idx;
    verts.resize(kOrbitSegments * 4);
    c0.resize(kOrbitSegments * 16);
    c1.resize(kOrbitSegments * 16);
    c2.resize(kOrbitSegments * 16);
    idx.resize(kOrbitSegments * 6);
    float *p0 = c0.ptrw(), *p1 = c1.ptrw(), *p2 = c2.ptrw();
    int32_t *pi = idx.ptrw();
    for (int k = 0; k < kOrbitSegments; ++k) {
        Vector3 ah, al, bh, bl;
        split(pts[static_cast<size_t>(k)], ah, al);
        split(pts[static_cast<size_t>(k + 1)], bh, bl);
        const float fa = fade[static_cast<size_t>(k)], fb = fade[static_cast<size_t>(k + 1)];
        for (int q = 0; q < 4; ++q) {
            const bool end = q >= 2;
            const float side = (q % 2 == 0) ? -1.0f : 1.0f;
            const Vector3 &th = end ? bh : ah, &tl = end ? bl : al, &oh = end ? ah : bh, &ol = end ? al : bl;
            const int vi = k * 4 + q;
            verts.set(vi, th);
            const float a0[4] = {tl.x, tl.y, tl.z, side}, a1[4] = {oh.x, oh.y, oh.z, end ? 1.0f : 0.0f},
                        a2[4] = {ol.x, ol.y, ol.z, end ? fb : fa};
            std::copy(a0, a0 + 4, p0 + vi * 4);
            std::copy(a1, a1 + 4, p1 + vi * 4);
            std::copy(a2, a2 + 4, p2 + vi * 4);
        }
        const int b0 = k * 4;
        const int tri[6] = {b0, b0 + 1, b0 + 2, b0 + 1, b0 + 3, b0 + 2};
        std::copy(tri, tri + 6, pi + k * 6);
    }
    Array arrays;
    arrays.resize(RenderingServer::ARRAY_MAX);
    arrays[RenderingServer::ARRAY_VERTEX] = verts;
    arrays[RenderingServer::ARRAY_CUSTOM0] = c0;
    arrays[RenderingServer::ARRAY_CUSTOM1] = c1;
    arrays[RenderingServer::ARRAY_CUSTOM2] = c2;
    arrays[RenderingServer::ARRAY_INDEX] = idx;
    RenderingServer *rs = RenderingServer::get_singleton();
    const int64_t fmt =
        (static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA_FLOAT) << RenderingServer::ARRAY_FORMAT_CUSTOM0_SHIFT) |
        (static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA_FLOAT) << RenderingServer::ARRAY_FORMAT_CUSTOM1_SHIFT) |
        (static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA_FLOAT) << RenderingServer::ARRAY_FORMAT_CUSTOM2_SHIFT);
    if (!ov.mesh.is_valid()) {
        ov.mesh = rs->mesh_create();
        ov.instance = rs->instance_create2(ov.mesh, get_world_3d()->get_scenario());
        rs->instance_geometry_set_material_override(ov.instance, orbit_material_->get_rid());
        rs->instance_geometry_set_cast_shadows_setting(ov.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
        rs->instance_set_custom_aabb(ov.instance, kHugeAabb);
        Color col(0.45f, 0.62f, 1.0f, 0.55f);  // планеты
        if (b.kind == "moon") col = Color(0.75f, 0.75f, 0.8f, 0.45f);
        else if (b.kind == "dwarf") col = Color(0.55f, 0.85f, 0.6f, 0.5f);
        else if (b.kind == "comet") col = Color(0.4f, 0.9f, 0.95f, 0.5f);
        rs->instance_geometry_set_shader_parameter(ov.instance, "line_color", col);
    } else {
        rs->mesh_clear(ov.mesh);
    }
    rs->mesh_add_surface_from_arrays(ov.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), fmt);
    ov.built = true;
    ov.gen_et = et_;
}

void SolarSystem::update_orbits() {
    RenderingServer *rs = RenderingServer::get_singleton();
    const double px_per_rad = px_per_rad_;
    const size_t n = orbits_.size();
    if (orbit_material_.is_valid()) orbit_material_->set_shader_parameter("view_rot", to_basis(view_));
    // каждый кадр — орбита опорного тела и его спутников, остальные — по 4 за кадр по кругу
    std::vector<bool> rebuild(n, false);
    if (rig_.anchor >= 0) {
        rebuild[static_cast<size_t>(rig_.anchor)] = true;
        for (size_t k = 0; k < n; ++k)
            if (cat_.bodies[k].parent == rig_.anchor) rebuild[k] = true;
    }
    for (int q = 0; q < 4 && n > 0; ++q) rebuild[orbit_rr_++ % n] = true;
    for (size_t i = 0; i < n; ++i) {
        OrbitView &ov = orbits_[i];
        const sc::BodyInfo &b = cat_.bodies[i];
        if (ov.parent < 0 || !b.visible) continue;
        // размер орбиты на экране: у спутников вдали — меньше пары пикселей, не рисуем
        const double a = sc::norm(x_[i] - x_[static_cast<size_t>(ov.parent)]);
        const double dp = sc::norm(render_pos_[static_cast<size_t>(ov.parent)]);
        const bool small = dp > a && a / dp * px_per_rad < 3.0;
        const bool show = orbits_visible_ && !small;
        if (show && (!ov.built || rebuild[i] || std::fabs(et_ - ov.gen_et) > 3.0e7)) build_orbit(static_cast<int>(i));
        if (!ov.instance.is_valid()) continue;
        rs->instance_set_visible(ov.instance, show);
        if (!show) continue;
        Vector3 hi, lo;
        split(render_pos_[static_cast<size_t>(ov.parent)] * kKm, hi, lo);
        rs->instance_geometry_set_shader_parameter(ov.instance, "origin_hi", hi);
        rs->instance_geometry_set_shader_parameter(ov.instance, "origin_lo", lo);
    }
}

void SolarSystem::set_orbits_visible(bool v) { orbits_visible_ = v; }

void SolarSystem::set_grid_visible(bool v) {
    grid_visible_ = v;
    for (BodyView &bv : views_)
        if (bv.material.is_valid()) bv.material->set_shader_parameter("grid", v);
}

void SolarSystem::set_flat_mode(bool on) {
    flat_ = on;
    for (BodyView &bv : views_) {
        if (bv.material.is_valid()) bv.material->set_shader_parameter("flat_mode", on);
        if (bv.sky_instance.is_valid() && on) RenderingServer::get_singleton()->instance_set_visible(bv.sky_instance, false);
    }
    if (ring_material_.is_valid()) ring_material_->set_shader_parameter("flat_mode", on);
}

void SolarSystem::set_body_color(int i, const Color &c) {
    if (i < 0 || i >= body_count()) return;
    views_[static_cast<size_t>(i)].material->set_shader_parameter("base_color", c);
    if (!cat_.bodies[static_cast<size_t>(i)].rings.empty() && ring_material_.is_valid())
        ring_material_->set_shader_parameter("flat_color", Color(1.0f - c.r, 1.0f - c.g, 1.0f - c.b));
}

// ---------------------------------------------------------------- метки

int SolarSystem::add_surface_marker(int body, double lat, double lon, double alt_m, double size_m, const Color &c) {
    if (!loaded_ || body < 0 || body >= body_count()) return -1;
    const double la = lat * sc::kDeg, lo = lon * sc::kDeg;
    const V3 u{std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)};
    const sc::CubeSphere &cs = views_[static_cast<size_t>(body)].sphere;
    const V3 p = u * cs.radius_along(u);
    const V3 up = unit(V3{p.x / (cs.radii.x * cs.radii.x), p.y / (cs.radii.y * cs.radii.y), p.z / (cs.radii.z * cs.radii.z)});
    Marker m;
    m.body = body;
    m.pos = p + up * (alt_m / kKm);
    Ref<SphereMesh> sm;
    sm.instantiate();
    sm->set_radius(static_cast<float>(size_m / 2));
    sm->set_height(static_cast<float>(size_m));
    sm->set_radial_segments(16);
    sm->set_rings(8);
    Ref<ShaderMaterial> mat = make_material("res://shaders/body.gdshader");
    mat->set_shader_parameter("flat_mode", true);
    mat->set_shader_parameter("base_color", c);
    marker_material_ = mat;
    sm->surface_set_material(0, mat);
    RenderingServer *rs = RenderingServer::get_singleton();
    m.mesh = sm->get_rid();
    m.instance = rs->instance_create2(m.mesh, get_world_3d()->get_scenario());
    rs->instance_geometry_set_cast_shadows_setting(m.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
    rs->instance_set_custom_aabb(m.instance, kHugeAabb);
    markers_.push_back(m);
    marker_refs_.push_back(sm);
    return static_cast<int>(markers_.size()) - 1;
}

void SolarSystem::update_markers() {
    RenderingServer *rs = RenderingServer::get_singleton();
    for (const Marker &m : markers_) {
        const size_t b = static_cast<size_t>(m.body);
        const V3 o = render_pos_[b] + rot_[b].transposed() * m.pos;
        rs->instance_set_transform(m.instance, Transform3D(Basis(), to_vec3(view_ * o * kKm)));
    }
}

void SolarSystem::free_render() {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (!rs) return;
    free_sky();
    for (BodyView &bv : views_) {
        for (auto &kv : bv.chunks) {
            rs->free_rid(kv.second.instance);
            rs->free_rid(kv.second.mesh);
        }
        for (const RID &r : bv.ring_instances) rs->free_rid(r);
        for (const RID &r : bv.ring_meshes) rs->free_rid(r);
        if (bv.sky_instance.is_valid()) rs->free_rid(bv.sky_instance);
    }
    views_.clear();
    if (corona_instance_.is_valid()) rs->free_rid(corona_instance_);
    corona_instance_ = RID();
    corona_ref_.unref();
    for (OrbitView &ov : orbits_) {
        if (ov.instance.is_valid()) rs->free_rid(ov.instance);
        if (ov.mesh.is_valid()) rs->free_rid(ov.mesh);
    }
    orbits_.clear();
    for (Marker &m : markers_) rs->free_rid(m.instance);
    markers_.clear();
    marker_refs_.clear();
}

// ---------------------------------------------------------------- привязка

void SolarSystem::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_data", "dir"), &SolarSystem::load_data);
    ClassDB::bind_method(D_METHOD("last_error"), &SolarSystem::last_error);
    ClassDB::bind_method(D_METHOD("body_count"), &SolarSystem::body_count);
    ClassDB::bind_method(D_METHOD("body_info", "i"), &SolarSystem::body_info);
    ClassDB::bind_method(D_METHOD("body_index", "key"), &SolarSystem::body_index);

    ClassDB::bind_method(D_METHOD("get_et"), &SolarSystem::get_et);
    ClassDB::bind_method(D_METHOD("set_et", "et"), &SolarSystem::set_et);
    ClassDB::bind_method(D_METHOD("set_rate", "rate"), &SolarSystem::set_rate);
    ClassDB::bind_method(D_METHOD("get_rate"), &SolarSystem::get_rate);
    ClassDB::bind_method(D_METHOD("set_paused", "paused"), &SolarSystem::set_paused);
    ClassDB::bind_method(D_METHOD("is_paused"), &SolarSystem::is_paused);
    ClassDB::bind_method(D_METHOD("actual_rate"), &SolarSystem::actual_rate);
    ClassDB::bind_method(D_METHOD("now_et"), &SolarSystem::now_et);
    ClassDB::bind_method(D_METHOD("et_to_utc", "et"), &SolarSystem::et_to_utc);
    ClassDB::bind_method(D_METHOD("et_to_tdb", "et"), &SolarSystem::et_to_tdb);
    ClassDB::bind_method(D_METHOD("utc_to_et", "text"), &SolarSystem::utc_to_et);
    ClassDB::bind_method(D_METHOD("get_mode"), &SolarSystem::get_mode);
    ClassDB::bind_method(D_METHOD("set_mode", "mode"), &SolarSystem::set_mode);
    ClassDB::bind_method(D_METHOD("mode_is_auto"), &SolarSystem::mode_is_auto);
    ClassDB::bind_method(D_METHOD("sim_level"), &SolarSystem::sim_level);
    ClassDB::bind_method(D_METHOD("ephemeris_range"), &SolarSystem::ephemeris_range);
    ClassDB::bind_method(D_METHOD("status"), &SolarSystem::status);
    bind_edit_methods();
    ClassDB::bind_method(D_METHOD("star_direction", "hip"), &SolarSystem::star_direction);
    ClassDB::bind_method(D_METHOD("star_count"), &SolarSystem::star_count);
    ClassDB::bind_method(D_METHOD("set_sky_param", "name", "value"), &SolarSystem::set_sky_param);

    ClassDB::bind_method(D_METHOD("set_camera", "camera"), &SolarSystem::set_camera);
    ClassDB::bind_method(D_METHOD("get_camera"), &SolarSystem::get_camera);
    ClassDB::bind_method(D_METHOD("camera_free_input", "move", "rot"), &SolarSystem::camera_free_input);
    ClassDB::bind_method(D_METHOD("camera_orbit_input", "dyaw", "dpitch", "zoom_steps"), &SolarSystem::camera_orbit_input);
    ClassDB::bind_method(D_METHOD("camera_set_speed", "kms"), &SolarSystem::camera_set_speed);
    ClassDB::bind_method(D_METHOD("camera_speed"), &SolarSystem::camera_speed);
    ClassDB::bind_method(D_METHOD("camera_actual_speed"), &SolarSystem::camera_actual_speed);
    ClassDB::bind_method(D_METHOD("camera_fly_to", "body"), &SolarSystem::camera_fly_to);
    ClassDB::bind_method(D_METHOD("camera_orbit", "body"), &SolarSystem::camera_orbit);
    ClassDB::bind_method(D_METHOD("camera_set_body_fixed", "on"), &SolarSystem::camera_set_body_fixed);
    ClassDB::bind_method(D_METHOD("camera_body_fixed"), &SolarSystem::camera_body_fixed);
    ClassDB::bind_method(D_METHOD("camera_place", "body", "lat", "lon", "alt_m", "azimuth", "elevation", "body_fixed"),
                         &SolarSystem::camera_place);
    ClassDB::bind_method(D_METHOD("camera_anchor"), &SolarSystem::camera_anchor);
    ClassDB::bind_method(D_METHOD("camera_mode"), &SolarSystem::camera_mode);
    ClassDB::bind_method(D_METHOD("camera_altitude"), &SolarSystem::camera_altitude);
    ClassDB::bind_method(D_METHOD("camera_flyto_progress"), &SolarSystem::camera_flyto_progress);
    ClassDB::bind_method(D_METHOD("camera_position_icrf"), &SolarSystem::camera_position_icrf);
    ClassDB::bind_method(D_METHOD("body_position_icrf", "i"), &SolarSystem::body_position_icrf);
    ClassDB::bind_method(D_METHOD("distance_to_body", "i"), &SolarSystem::distance_to_body);
    ClassDB::bind_method(D_METHOD("body_state_icrf", "i"), &SolarSystem::body_state_icrf);
    ClassDB::bind_method(D_METHOD("body_ephemeris_deviation", "i"), &SolarSystem::body_ephemeris_deviation);
    ClassDB::bind_method(D_METHOD("camera_get_state"), &SolarSystem::camera_get_state);
    ClassDB::bind_method(D_METHOD("camera_set_state", "state"), &SolarSystem::camera_set_state);

    ClassDB::bind_method(D_METHOD("update_frame", "delta"), &SolarSystem::update_frame);
    ClassDB::bind_method(D_METHOD("body_render_position", "i"), &SolarSystem::body_render_position);
    ClassDB::bind_method(D_METHOD("body_angular_radius", "i"), &SolarSystem::body_angular_radius);
    ClassDB::bind_method(D_METHOD("body_hidden", "i"), &SolarSystem::body_hidden);

    ClassDB::bind_method(D_METHOD("set_orbits_visible", "visible"), &SolarSystem::set_orbits_visible);
    ClassDB::bind_method(D_METHOD("get_orbits_visible"), &SolarSystem::get_orbits_visible);
    ClassDB::bind_method(D_METHOD("set_grid_visible", "visible"), &SolarSystem::set_grid_visible);
    ClassDB::bind_method(D_METHOD("get_grid_visible"), &SolarSystem::get_grid_visible);
    ClassDB::bind_method(D_METHOD("set_lod_split", "split"), &SolarSystem::set_lod_split);
    ClassDB::bind_method(D_METHOD("get_lod_split"), &SolarSystem::get_lod_split);
    ClassDB::bind_method(D_METHOD("set_flat_mode", "on"), &SolarSystem::set_flat_mode);
    ClassDB::bind_method(D_METHOD("set_effect", "name", "on"), &SolarSystem::set_effect);
    ClassDB::bind_method(D_METHOD("get_effect", "name"), &SolarSystem::get_effect);
    ClassDB::bind_method(D_METHOD("set_debug_view", "mode"), &SolarSystem::set_debug_view);
    ClassDB::bind_method(D_METHOD("appearance_info", "i"), &SolarSystem::appearance_info);
    ClassDB::bind_method(D_METHOD("body_texture_width", "i"), &SolarSystem::body_texture_width);
    ClassDB::bind_method(D_METHOD("hi_texture_bytes"), &SolarSystem::hi_texture_bytes);
    ClassDB::bind_method(D_METHOD("sun_visibility", "i"), &SolarSystem::sun_visibility);
    ClassDB::bind_method(D_METHOD("shadow_casters", "i"), &SolarSystem::shadow_casters);
    ClassDB::bind_method(D_METHOD("set_hi_texture_budget", "bytes"), &SolarSystem::set_hi_texture_budget);
    ClassDB::bind_method(D_METHOD("reference_surface", "w", "h"), &SolarSystem::reference_surface);
    ClassDB::bind_method(D_METHOD("set_body_color", "i", "color"), &SolarSystem::set_body_color);
    ClassDB::bind_method(D_METHOD("add_surface_marker", "body", "lat", "lon", "alt_m", "size_m", "color"),
                         &SolarSystem::add_surface_marker);
    ClassDB::bind_method(D_METHOD("set_zoom", "zoom"), &SolarSystem::set_zoom);
    ClassDB::bind_method(D_METHOD("get_zoom"), &SolarSystem::get_zoom);
    ClassDB::bind_method(D_METHOD("set_view_fov", "deg"), &SolarSystem::set_view_fov);
    ClassDB::bind_method(D_METHOD("get_view_fov"), &SolarSystem::get_view_fov);
    ClassDB::bind_method(D_METHOD("camera_look_from", "body", "dir", "dist_km", "body_frame"), &SolarSystem::camera_look_from,
                         DEFVAL(true));
    ClassDB::bind_method(D_METHOD("marker_render_position", "id"), &SolarSystem::marker_render_position);
    ClassDB::bind_method(D_METHOD("reference_classes", "w", "h"), &SolarSystem::reference_classes);
    ClassDB::bind_method(D_METHOD("visible_chunk_count"), &SolarSystem::visible_chunk_count);
    ClassDB::bind_method(D_METHOD("chunk_level_max", "body"), &SolarSystem::chunk_level_max);
}

}  // namespace godot
