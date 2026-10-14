// Внешний вид тел (этап 5): карты из game/data/appearance.json, подкачка крупных карт, освещение и затмения,
// Солнце с короной, оболочки атмосфер. Порядок кадра — в SolarSystem::update_frame (solar_system.cpp).
#include <algorithm>
#include <cmath>

#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/packed_vector4_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "core/json.hpp"
#include "gd_util.hpp"
#include "solar_system.hpp"

namespace godot {

using sc::M3;
using sc::V3;
using namespace gdu;

namespace {

// Крупные карты (8K): грузятся в фоне, когда тело на экране шире kHiOnPx, и выгружаются, когда оно уже kHiOffPx
// дольше kHiHoldS секунд. Нижний уровень (2K) загружен всегда.
constexpr double kHiOnPx = 600.0;
constexpr double kHiOffPx = 300.0;
constexpr double kHiHoldS = 3.0;
constexpr int kMaxOccluders = 4;
constexpr double kAuKm = 149597870.7;
constexpr double kCoronaExtent = 6.0;  // радиусов Солнца: дальше корона тусклее 10⁻⁹ диска

// Видеопамять под карту ширины w (высота w/2) с мип-уровнями: BC1/BC4 — 0,5 байта на пиксель, BC5 — 1 байт.
int64_t texture_bytes(int w, bool normal) {
    const double px = static_cast<double>(w) * (w / 2);
    return static_cast<int64_t>(px * (normal ? 1.0 : 0.5) * 4.0 / 3.0);
}

}  // namespace

void SolarSystem::load_appearance(const String &dir) {
    bool ok = false;
    const std::string text = read_all(dir + String("appearance.json"), ok);
    if (!ok) {
        UtilityFunctions::push_warning(String::utf8("нет appearance.json: тела без карт"));
        return;
    }
    std::string err;
    const sc::Json j = sc::Json::parse(text, &err);
    if (!err.empty()) {
        UtilityFunctions::push_error(String::utf8("appearance.json: ") + u8(err));
        return;
    }
    const sc::Json &bodies = j["bodies"];
    const sc::Json &sources = j["sources"];
    std::vector<double> lam = j["rgb_wavelengths_um"].numbers();
    if (lam.size() != 3) lam = {0.68, 0.55, 0.44};
    ResourceLoader *rl = ResourceLoader::get_singleton();
    RenderingServer *rs = RenderingServer::get_singleton();
    const RID scenario = get_world_3d()->get_scenario();

    for (size_t i = 0; i < cat_.bodies.size(); ++i) {
        const sc::BodyInfo &b = cat_.bodies[i];
        BodyView &bv = views_[i];
        if (!bodies.has(b.key)) continue;
        const sc::Json &a = bodies[b.key];
        Ref<ShaderMaterial> m = bv.material;
        Dictionary info;
        if (a["uniform_rgb"].is_array()) {
            const std::vector<double> c = a["uniform_rgb"].numbers();
            // base_color объявлен как source_color (sRGB), а в манифесте — линейное альбедо
            m->set_shader_parameter("base_color", Color(c[0], c[1], c[2]).linear_to_srgb());
        }
        auto add_slot = [&](const char *layer, const String &param, bool normal) -> bool {
            const sc::Json &L = a["layers"][layer];
            if (!L.is_object()) return false;
            TexSlot slot;
            slot.param = param;
            slot.normal = normal;
            for (const std::string &k : L["files"].keys()) slot.levels.emplace_back(std::stoi(k), u8(L["files"][k].str()));
            std::sort(slot.levels.begin(), slot.levels.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
            if (slot.levels.empty()) return false;
            Ref<Texture2D> t = rl->load(slot.levels[0].second);
            if (t.is_null()) {
                UtilityFunctions::push_error(String::utf8("карта не загрузилась: ") + slot.levels[0].second);
                return false;
            }
            slot.base = t;
            slot.shown = 0;
            m->set_shader_parameter(param, t);
            bv.slots.push_back(slot);
            return true;
        };
        const sc::Json &color = a["layers"]["color"];
        if (color.is_object()) {
            const bool linear = color["encoding"].str() == "linear";
            if (add_slot("color", linear ? "albedo_lin" : "albedo_srgb", false)) {
                m->set_shader_parameter("has_map", true);
                m->set_shader_parameter("map_linear", linear);
                m->set_shader_parameter("map_grey", color["grey"].boolean());
                m->set_shader_parameter("geodetic", color["lat"].str() == "geodetic");
                const sc::Json &src = sources[color["src"].str()];
                info["map_source"] = u8(src["title"].str());
                info["map_license"] = u8(src["license"].str());
                info["map_origin"] = u8(color["origin"].str());
                info["coverage"] = color["coverage"].num(1.0);
                if (color.has("fill_note")) info["fill_note"] = u8(color["fill_note"].str());
            }
        }
        if (add_slot("normal", "normal_tex", true)) {
            m->set_shader_parameter("has_normal", true);
            info["relief_source"] = u8(sources[a["layers"]["normal"]["src"].str()]["title"].str());
        }
        if (add_slot("night", "night_tex", false)) m->set_shader_parameter("has_night", true);
        if (add_slot("clouds", "clouds_tex", false)) m->set_shader_parameter("has_clouds", true);
        if (add_slot("ocean", "ocean_tex", false)) m->set_shader_parameter("has_ocean", true);
        const sc::Json &alb = a["albedo"];
        if (alb.is_object()) {
            m->set_shader_parameter("albedo_scale", alb["scale"].num(1.0));
            info["albedo_scale"] = alb["scale"].num(1.0);
            if (alb.has("p_v")) info["albedo_p"] = alb["p_v"].num();
        }
        for (const char *k : {"no_map", "note", "no_albedo", "atmosphere_note"})
            if (a.has(k)) info[k] = u8(a[k].str());
        // атмосфера: в шейдере поверхности (дымка до поверхности) и оболочка (ореол у края, небо)
        const sc::Json &atm = a["atmosphere"];
        if (atm.is_object()) {
            const std::vector<double> br = atm["rayleigh_beta_m"].numbers();
            auto set_atm = [&](Ref<ShaderMaterial> mat) {
                mat->set_shader_parameter("atm_on", true);
                mat->set_shader_parameter("atm_beta_r", Vector3(br[0] * 1e3, br[1] * 1e3, br[2] * 1e3));
                mat->set_shader_parameter("atm_h_r", atm["rayleigh_scale_height_m"].num() / 1e3);
                mat->set_shader_parameter("atm_beta_m_sca", atm["mie_scattering_m"].num() * 1e3);
                mat->set_shader_parameter("atm_beta_m_ext", atm["mie_extinction_m"].num() * 1e3);
                mat->set_shader_parameter("atm_h_m", atm["mie_scale_height_m"].num() / 1e3);
                mat->set_shader_parameter("atm_g", atm["mie_g"].num());
                mat->set_shader_parameter("atm_top", atm["top_m"].num() / 1e3);
                mat->set_shader_parameter("radii", to_vec3(b.radii));
            };
            set_atm(m);
            bv.sky_material = make_material("res://shaders/sky.gdshader");
            set_atm(bv.sky_material);
            Ref<SphereMesh> sm;
            sm.instantiate();
            const double top = std::max(b.radii.x, std::max(b.radii.y, b.radii.z)) + atm["top_m"].num() / 1e3;
            sm->set_radius(static_cast<float>(top * kKm));
            sm->set_height(static_cast<float>(2 * top * kKm));
            sm->set_radial_segments(128);
            sm->set_rings(64);
            bv.sky_ref = sm;
            bv.sky_instance = rs->instance_create2(sm->get_rid(), scenario);
            rs->instance_geometry_set_material_override(bv.sky_instance, bv.sky_material->get_rid());
            rs->instance_geometry_set_cast_shadows_setting(bv.sky_instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            rs->instance_set_custom_aabb(bv.sky_instance, kHugeAabb);
            rs->instance_set_visible(bv.sky_instance, false);
            bv.has_atmosphere = true;
            info["atmosphere"] = String::utf8("однократное рассеяние: Рэлей и аэрозоль");
        }
        // Солнце: потемнение к краю, корона
        if (a["limb_darkening"].is_object()) {
            const double a0 = a["limb_darkening"]["a0"].num(), a1 = a["limb_darkening"]["a1"].num();
            const double disk_mean = std::pow(kAuKm / b.mean_radius, 2);
            m->set_shader_parameter("limb_a0", a0);
            m->set_shader_parameter("limb_a1", a1);
            m->set_shader_parameter("lambda_um", Vector3(lam[0], lam[1], lam[2]));
            m->set_shader_parameter("disk_mean", disk_mean);
            corona_material_ = make_material("res://shaders/corona.gdshader");
            Vector3 center;
            for (int c = 0; c < 3; ++c) center[c] = static_cast<real_t>(disk_mean * (a0 + a1 / lam[c] + 2.0) / 2.0);
            corona_material_->set_shader_parameter("center_radiance", center);
            const sc::Json &cor = a["corona"];
            const sc::Json &t = cor["terms"];
            if (t.size() == 3) {
                corona_material_->set_shader_parameter(
                    "terms", Vector4(t[0][0].num(), t[0][1].num(), t[1][0].num(), t[1][1].num()));
                corona_material_->set_shader_parameter("term3", Vector2(t[2][0].num(), t[2][1].num()));
            }
            corona_material_->set_shader_parameter("unit", cor["unit"].num(1e-6));
            corona_material_->set_shader_parameter("sun_radius", b.mean_radius);
            corona_material_->set_shader_parameter("half_size", kCoronaExtent * b.mean_radius * kKm);
            Ref<QuadMesh> q;
            q.instantiate();
            q->set_size(Vector2(2, 2));
            corona_ref_ = q;
            corona_instance_ = rs->instance_create2(q->get_rid(), scenario);
            rs->instance_geometry_set_material_override(corona_instance_, corona_material_->get_rid());
            rs->instance_geometry_set_cast_shadows_setting(corona_instance_, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            rs->instance_set_custom_aabb(corona_instance_, kHugeAabb);
            info["corona"] = String::utf8("формула Баумбаха (1937)");
        }
        // кольца: тень на планете (кольца C, B, A; между B и A — деление Кассини)
        if (!b.rings.empty()) {
            double c_in = 0, cb = 0, b_out = 0, a_in = 0, a_out = 0, tc = 0, tb = 0, ta = 0;
            for (const sc::RingBand &rb : b.rings) {
                const double tau = 0.5 * (rb.tau_lo + rb.tau_hi);
                if (rb.name == "C") { c_in = rb.inner_km; cb = rb.outer_km; tc = tau; }
                if (rb.name == "B") { b_out = rb.outer_km; tb = tau; }
                if (rb.name == "A") { a_in = rb.inner_km; a_out = rb.outer_km; ta = tau; }
            }
            m->set_shader_parameter("ring_shadow", true);
            m->set_shader_parameter("ring_edges", Vector4(c_in, cb, b_out, a_in));
            m->set_shader_parameter("ring_outer", a_out);
            m->set_shader_parameter("ring_tau", Vector3(tc, tb, ta));
            if (ring_material_.is_valid()) ring_material_->set_shader_parameter("radii", to_vec3(b.radii));
            bv.has_rings = true;
        }
        bv.info = info;
    }
}

void SolarSystem::apply_slot(BodyView &bv, TexSlot &slot, int level) {
    bv.material->set_shader_parameter(slot.param, level == 0 ? slot.base : slot.hi);
    slot.shown = level;
}

void SolarSystem::update_textures(double now) {
    ResourceLoader *rl = ResourceLoader::get_singleton();
    // 1. нужна ли крупная карта: по размеру на экране, с задержкой выгрузки
    for (BodyView &bv : views_) {
        if (bv.px >= kHiOnPx) {
            bv.want_hi = true;
            bv.small_since = -1;
        } else if (bv.px < kHiOffPx) {
            if (bv.small_since < 0) bv.small_since = now;
            if (now - bv.small_since >= kHiHoldS) bv.want_hi = false;
        } else {
            bv.small_since = -1;
        }
    }
    // 2. бюджет видеопамяти: сначала самые крупные на экране
    std::vector<size_t> order;
    for (size_t i = 0; i < views_.size(); ++i)
        if (views_[i].want_hi) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return views_[x].px > views_[y].px; });
    int64_t used = 0;
    for (size_t i : order) {
        int64_t need = 0;
        for (const TexSlot &s : views_[i].slots)
            if (s.levels.size() > 1) need += texture_bytes(s.levels.back().first, s.normal);
        if (used + need > hi_budget_) {
            views_[i].want_hi = false;
        } else {
            used += need;
        }
    }
    // 3. загрузка в фоне и выгрузка
    for (BodyView &bv : views_) {
        for (TexSlot &slot : bv.slots) {
            if (slot.levels.size() < 2) continue;
            const int top = static_cast<int>(slot.levels.size()) - 1;
            if (bv.want_hi) {
                if (slot.shown != top && slot.pending == -1) {
                    const String &path = slot.levels[static_cast<size_t>(top)].second;
                    if (rl->load_threaded_request(path) == OK) {
                        slot.pending = top;
                    } else {
                        slot.pending = -2;  // не пробуем снова
                        UtilityFunctions::push_error(String::utf8("не удалось запросить ") + path);
                    }
                }
            } else if (slot.shown == top) {
                apply_slot(bv, slot, 0);
                slot.hi.unref();  // последняя ссылка: текстура уходит из видеопамяти
            }
            if (slot.pending >= 0) {
                const String &path = slot.levels[static_cast<size_t>(slot.pending)].second;
                const ResourceLoader::ThreadLoadStatus st = rl->load_threaded_get_status(path);
                if (st == ResourceLoader::THREAD_LOAD_LOADED) {
                    Ref<Texture2D> t = rl->load_threaded_get(path);
                    const int lv = slot.pending;
                    slot.pending = -1;
                    if (bv.want_hi && t.is_valid()) {
                        slot.hi = t;
                        apply_slot(bv, slot, lv);
                    }
                } else if (st == ResourceLoader::THREAD_LOAD_FAILED || st == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE) {
                    slot.pending = -2;
                    UtilityFunctions::push_error(String::utf8("не загрузилась карта ") + path);
                }
            }
        }
    }
}

void SolarSystem::update_lighting(int i, BodyView &bv) {
    const size_t bi = static_cast<size_t>(i);
    const M3 &R = rot_[bi];
    Ref<ShaderMaterial> m = bv.material;
    const V3 sun_rel = x_[0] - x_[bi];
    const V3 cam = R * (render_pos_[bi] * -1.0);
    m->set_shader_parameter("cam_pos", to_vec3(cam));
    if (i == 0) return;
    const V3 s_dir = R * unit(sun_rel);
    m->set_shader_parameter("sun_dir", to_vec3(s_dir));
    m->set_shader_parameter("sun_pos", to_vec3(R * sun_rel));
    m->set_shader_parameter("sun_radius", cat_.bodies[0].mean_radius);
    std::vector<size_t> cand;
    find_occluders(bi, cand);
    PackedVector4Array occ;
    occ.resize(kMaxOccluders);
    const int n = std::min(kMaxOccluders, static_cast<int>(cand.size()));
    for (int k = 0; k < n; ++k) {
        const size_t j = cand[static_cast<size_t>(k)];
        const V3 o = R * (x_[j] - x_[bi]);
        occ.set(k, Vector4(static_cast<real_t>(o.x), static_cast<real_t>(o.y), static_cast<real_t>(o.z),
                           static_cast<real_t>(cat_.bodies[j].mean_radius)));
    }
    m->set_shader_parameter("occluders", occ);
    m->set_shader_parameter("occluder_count", n);
    if (bv.has_atmosphere && bv.sky_material.is_valid()) {
        bv.sky_material->set_shader_parameter("sun_dir", to_vec3(s_dir));
        bv.sky_material->set_shader_parameter("cam_pos", to_vec3(cam));
    }
    if (bv.has_rings && ring_material_.is_valid()) {
        ring_material_->set_shader_parameter("sun_dir", to_vec3(s_dir));
        ring_material_->set_shader_parameter("cam_pos", to_vec3(cam));
    }
}

void SolarSystem::find_occluders(size_t i, std::vector<size_t> &out) const {
    out.clear();
    const V3 sun_rel = x_[0] - x_[i];
    const double d_s = sc::norm(sun_rel);
    const double a_s = std::asin(std::min(1.0, cat_.bodies[0].mean_radius / d_s));
    const V3 &ri = cat_.bodies[i].radii;
    const double r_i = std::max(ri.x, std::max(ri.y, ri.z));
    std::vector<std::pair<double, size_t>> cand;
    for (size_t k = 1; k < cat_.bodies.size(); ++k) {
        if (k == i || !cat_.bodies[k].visible) continue;
        const V3 o = x_[k] - x_[i];
        const double d_o = sc::norm(o);
        if (d_o >= d_s || sc::dot(o, sun_rel) <= 0) continue;
        const double sep = std::acos(std::clamp(sc::dot(o, sun_rel) / (d_o * d_s), -1.0, 1.0));
        const double a_o = std::asin(std::min(1.0, cat_.bodies[k].mean_radius / d_o));
        const double a_i = std::asin(std::min(1.0, r_i / d_o));  // точки самого тела видят заслонитель под другим углом
        if (sep < a_s + a_o + a_i) cand.emplace_back(a_o, k);
    }
    std::sort(cand.begin(), cand.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
    for (const auto &c : cand) out.push_back(c.second);
}

double SolarSystem::sun_visibility(int i) const {
    if (!loaded_ || i <= 0 || i >= body_count()) return 1.0;
    const size_t bi = static_cast<size_t>(i);
    std::vector<size_t> occ;
    find_occluders(bi, occ);
    const V3 vs = x_[0] - x_[bi];
    const double ds = sc::norm(vs);
    const double a = std::asin(std::min(1.0, cat_.bodies[0].mean_radius / ds));
    double vis = 1.0;
    for (size_t j : occ) {
        const V3 vo = x_[j] - x_[bi];
        const double d_o = sc::norm(vo);
        const double b = std::asin(std::min(1.0, cat_.bodies[j].mean_radius / d_o));
        const double d = std::acos(std::clamp(sc::dot(vs, vo) / (ds * d_o), -1.0, 1.0));
        double f;  // доля площади диска Солнца под диском заслонителя (как в body.gdshader)
        if (d >= a + b) {
            f = 0;
        } else if (d <= std::fabs(a - b)) {
            f = std::min(1.0, b * b / (a * a));
        } else {
            const double a2 = a * a, b2 = b * b, d2 = d * d;
            const double x = a2 * std::acos(std::clamp((d2 + a2 - b2) / (2 * d * a), -1.0, 1.0)) +
                             b2 * std::acos(std::clamp((d2 + b2 - a2) / (2 * d * b), -1.0, 1.0)) -
                             0.5 * std::sqrt(std::max((-d + a + b) * (d + a - b) * (d - a + b) * (d + a + b), 0.0));
            f = std::clamp(x / (sc::kPi * a2), 0.0, 1.0);
        }
        vis *= 1.0 - f;
    }
    return vis;
}

PackedInt32Array SolarSystem::shadow_casters(int i) const {
    PackedInt32Array a;
    if (!loaded_ || i <= 0 || i >= body_count()) return a;
    std::vector<size_t> occ;
    find_occluders(static_cast<size_t>(i), occ);
    for (size_t k = 0; k < occ.size() && static_cast<int>(k) < kMaxOccluders; ++k) a.push_back(static_cast<int32_t>(occ[k]));
    return a;
}

void SolarSystem::set_effect(const String &name, bool on) {
    String param;
    if (name == "atmosphere") {
        fx_atmosphere_ = on;
        param = "show_atmosphere";
    } else if (name == "clouds") {
        fx_clouds_ = on;
        param = "show_clouds";
    } else if (name == "night") {
        fx_night_ = on;
        param = "show_night";
    } else if (name == "stars") {
        fx_stars_ = on;
        return;
    } else if (name == "milkyway") {
        fx_milkyway_ = on;
        return;
    } else {
        return;
    }
    for (BodyView &bv : views_)
        if (bv.material.is_valid()) bv.material->set_shader_parameter(param, on);
}

bool SolarSystem::get_effect(const String &name) const {
    if (name == "atmosphere") return fx_atmosphere_;
    if (name == "clouds") return fx_clouds_;
    if (name == "night") return fx_night_;
    if (name == "stars") return fx_stars_;
    if (name == "milkyway") return fx_milkyway_;
    return false;
}

void SolarSystem::set_debug_view(int mode) {
    debug_view_ = std::clamp(mode, 0, 3);
    for (size_t i = 1; i < views_.size(); ++i)
        if (views_[i].material.is_valid()) views_[i].material->set_shader_parameter("debug_view", debug_view_);
}

Dictionary SolarSystem::appearance_info(int i) const {
    if (i < 0 || i >= body_count()) return Dictionary();
    const BodyView &bv = views_[static_cast<size_t>(i)];
    Dictionary d = bv.info.duplicate();
    d["texture_width"] = body_texture_width(i);
    for (const TexSlot &s : bv.slots)
        if (s.param == "albedo_srgb" || s.param == "albedo_lin") d["max_width"] = s.levels.back().first;
    d["px"] = bv.px;
    return d;
}

int SolarSystem::body_texture_width(int i) const {
    if (i < 0 || i >= body_count()) return 0;
    for (const TexSlot &s : views_[static_cast<size_t>(i)].slots)
        if (s.param == "albedo_srgb" || s.param == "albedo_lin")
            return s.shown >= 0 ? s.levels[static_cast<size_t>(s.shown)].first : 0;
    return 0;
}

int64_t SolarSystem::hi_texture_bytes() const {
    int64_t sum = 0;
    for (const BodyView &bv : views_)
        for (const TexSlot &s : bv.slots)
            if (s.levels.size() > 1 && s.shown == static_cast<int>(s.levels.size()) - 1)
                sum += texture_bytes(s.levels.back().first, s.normal);
    return sum;
}

PackedFloat32Array SolarSystem::reference_surface(int w, int h) const {
    PackedFloat32Array out;
    if (!loaded_ || w <= 0 || h <= 0) return out;
    out.resize(static_cast<int64_t>(w) * h * 4);
    float *px = out.ptrw();
    const double ty = std::tan(fov_deg_ * sc::kDeg / 2) / zoom_, tx = ty * w / h;
    const M3 cb = cam_frame_ * rig_.orient.matrix();  // оси камеры → ICRF
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double nx = (2.0 * (x + 0.5) / w - 1.0) * tx, ny = (1.0 - 2.0 * (y + 0.5) / h) * ty;
            const V3 d = unit(cb * V3{nx, ny, -1.0});
            double best = INFINITY;
            int hit = -1;
            V3 hit_p;
            for (size_t i = 0; i < cat_.bodies.size(); ++i) {
                if (!cat_.bodies[i].visible) continue;
                const M3 &R = rot_[i];
                const V3 o = R * (render_pos_[i] * -1.0), db = R * d;
                double k;
                if (views_[i].sphere.ray_hit(o, db, k) && k < best) {
                    best = k;
                    hit = static_cast<int>(i);
                    hit_p = o + db * k;
                }
            }
            float *q = px + (static_cast<size_t>(y) * w + x) * 4;
            if (hit < 0) {
                q[0] = q[1] = q[2] = q[3] = 0.0f;
                continue;
            }
            const V3 &r = cat_.bodies[static_cast<size_t>(hit)].radii;
            const V3 nrm = unit(V3{hit_p.x / (r.x * r.x), hit_p.y / (r.y * r.y), hit_p.z / (r.z * r.z)});
            q[0] = static_cast<float>(hit + 1);
            q[1] = static_cast<float>(std::asin(std::clamp(hit_p.z / sc::norm(hit_p), -1.0, 1.0)) / sc::kDeg);
            q[2] = static_cast<float>(std::atan2(hit_p.y, hit_p.x) / sc::kDeg);
            q[3] = static_cast<float>(std::asin(std::clamp(nrm.z, -1.0, 1.0)) / sc::kDeg);
        }
    return out;
}

}  // namespace godot
