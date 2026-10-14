// Правки параметров тел (этап 6): команды с отменой и повтором, «было → стало», предупреждения, сценарий JSON.
// Логика правок — sc::Scenario (core/scenario.*), здесь — связь с режимами времени и видом тел.
#include <algorithm>
#include <cmath>

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "gd_util.hpp"
#include "solar_system.hpp"

namespace godot {

using sc::V3;
using namespace gdu;

String SolarSystem::edit_param(int i, int p, double value) {
    if (!loaded_) return String::utf8("данные не загружены");
    std::string err;
    if (!scen_.set(i, p, value, err)) return u8(err);
    on_edits(true);
    return String();
}

bool SolarSystem::edit_undo() {
    if (!loaded_ || !scen_.undo()) return false;
    on_edits(false);
    return true;
}

bool SolarSystem::edit_redo() {
    if (!loaded_ || !scen_.redo()) return false;
    on_edits(false);
    return true;
}

bool SolarSystem::edit_reset() {
    if (!loaded_ || !scen_.reset()) return false;
    on_edits(false);
    return true;
}

PackedStringArray SolarSystem::edit_history() const {
    PackedStringArray out;
    for (const std::string &s : scen_.history()) out.push_back(u8(s));
    return out;
}

// После любой команды. Правки массы, скорости и расстояния — «Симуляция» заново от epoch правок до текущего
// момента. Новая правка в режиме «Эфемериды» ставит epoch на текущий момент; отмена и повтор оставляют прежний epoch,
// поэтому восстанавливают состояние точно. Без таких правок «Симуляция», включённая правками, возвращается
// к эфемеридам. Радиус, вращение и альбедо «Симуляцию» не перезапускают.
void SolarSystem::on_edits(bool fresh) {
    apply_edit_view();
    sc::Scenario::Values dyn = scen_.dynamic_edits();
    const bool changed = dyn != last_dyn_;
    last_dyn_ = std::move(dyn);
    if (changed && !last_dyn_.empty()) {
        if (mode_ == 0) edit_sim_ = true;
        if (mode_ == 0 && fresh) {
            enter_sim(et_, false);
        } else {
            mode_ = 1;
            if (runner_.seed(scen_.epoch, error_s_))
                catch_up_ = et_;
            else
                error_ = u8(error_s_);
        }
    } else if (changed && mode_ == 1) {
        if (edit_sim_ && eph_covers(et_)) {
            set_mode(0);
        } else if (runner_.seed(scen_.epoch, error_s_)) {
            catch_up_ = et_;
        }
    }
    compute_states();
}

// Радиус, период вращения и альбедо меняют вид тела: эллипсоид (чанки строятся заново), угол поворота, яркость карты.
void SolarSystem::apply_edit_view() {
    RenderingServer *rs = RenderingServer::get_singleton();
    for (size_t i = 0; i < cat_.bodies.size(); ++i) {
        sc::BodyInfo &b = cat_.bodies[i];
        BodyView &bv = views_[i];
        const int k = static_cast<int>(i);
        b.rot = scen_.rotation(k);
        const V3 r = scen_.radii(k);
        if (r.x != b.radii.x || r.y != b.radii.y || r.z != b.radii.z) {
            b.radii = r;
            b.mean_radius = scen_.value(k, sc::kRadius);
            bv.sphere.set_body(r);
            for (auto &kv : bv.chunks) {
                rs->free_rid(kv.second.instance);
                rs->free_rid(kv.second.mesh);
            }
            bv.chunks.clear();
            bv.shown.clear();
            if (bv.material.is_valid()) bv.material->set_shader_parameter("radii", to_vec3(r));
            if (bv.sky_material.is_valid()) bv.sky_material->set_shader_parameter("radii", to_vec3(r));
            if (bv.has_rings && ring_material_.is_valid()) ring_material_->set_shader_parameter("radii", to_vec3(r));
        }
        const double base = bv.info.get("albedo_scale", -1.0);
        const double a0 = scen_.real(k, sc::kAlbedo), a1 = scen_.value(k, sc::kAlbedo);
        if (base > 0 && a0 > 0 && bv.material.is_valid())
            bv.material->set_shader_parameter("albedo_scale", base * a1 / a0);
    }
}

void SolarSystem::real_states(std::vector<V3> &x, std::vector<V3> &v) const {
    x = x_;
    v = v_;
    if (mode_ == 0 || !eph_covers(et_)) return;
    for (size_t i = 0; i < cat_.bodies.size(); ++i) eph_.state_ssb(cat_.bodies[i].naif, et_, x[i], v[i]);
}

Array SolarSystem::body_params(int i) const {
    Array out;
    if (!loaded_ || i < 0 || i >= body_count()) return out;
    std::vector<V3> x0, v0;
    real_states(x0, v0);
    for (const sc::TableRow &r : scen_.table(i, x0, v0, x_, v_)) {
        Dictionary d;
        d["name"] = u8(r.name);
        d["unit"] = u8(r.unit);
        d["before"] = r.before;
        d["after"] = r.after;
        d["param"] = r.param;
        d["edited"] = r.param >= 0 && scen_.edited(i, r.param);
        out.push_back(d);
    }
    return out;
}

Array SolarSystem::stability_warnings() const {
    Array out;
    if (!loaded_) return out;
    for (const sc::Warning &w : scen_.check(x_, v_)) {
        Dictionary d;
        d["kind"] = static_cast<int>(w.kind);
        d["body"] = w.body;
        d["other"] = w.other;
        d["text"] = u8(w.text);
        out.push_back(d);
    }
    return out;
}

String SolarSystem::scenario_to_json(const Array &bookmarks) {
    scen_.mode = mode_;
    if (!scen_.dynamic() && mode_ == 0) scen_.epoch = et_;  // без «Симуляции» сценарий — это просто момент
    scen_.bookmarks.clear();
    for (int k = 0; k < bookmarks.size(); ++k) {
        const PackedFloat64Array a = bookmarks[k];
        scen_.bookmarks.emplace_back(a.ptr(), a.ptr() + a.size());
    }
    const std::string s = scen_.to_json();
    return u8(s);
}

String SolarSystem::scenario_from_json(const String &text) {
    if (!loaded_) return String::utf8("данные не загружены");
    const CharString u = text.utf8();
    std::string err;
    if (!scen_.from_json(std::string(u.get_data(), static_cast<size_t>(u.length())), err)) return u8(err);
    apply_edit_view();
    const double epoch = scen_.epoch;
    if (scen_.dynamic() || scen_.mode == 1 || !eph_covers(epoch)) {
        enter_sim(eph_covers(epoch) ? epoch : std::clamp(epoch, eph_lo_, eph_hi_), false);
        edit_sim_ = scen_.dynamic();
        last_dyn_ = scen_.dynamic_edits();
        if (!eph_covers(epoch)) runner_.request(epoch, 1e9);
    } else {
        if (mode_ == 1) set_mode(0);
        et_ = epoch;
    }
    compute_states();
    return String();
}

Array SolarSystem::scenario_bookmarks() const {
    Array out;
    for (const std::vector<double> &b : scen_.bookmarks) {
        PackedFloat64Array a;
        for (double v : b) a.push_back(v);
        out.push_back(a);
    }
    return out;
}

void SolarSystem::bind_edit_methods() {
    ClassDB::bind_method(D_METHOD("edit_param", "body", "param", "value"), &SolarSystem::edit_param);
    ClassDB::bind_method(D_METHOD("edit_undo"), &SolarSystem::edit_undo);
    ClassDB::bind_method(D_METHOD("edit_redo"), &SolarSystem::edit_redo);
    ClassDB::bind_method(D_METHOD("edit_reset"), &SolarSystem::edit_reset);
    ClassDB::bind_method(D_METHOD("edit_can_undo"), &SolarSystem::edit_can_undo);
    ClassDB::bind_method(D_METHOD("edit_can_redo"), &SolarSystem::edit_can_redo);
    ClassDB::bind_method(D_METHOD("edit_history"), &SolarSystem::edit_history);
    ClassDB::bind_method(D_METHOD("edit_history_position"), &SolarSystem::edit_history_position);
    ClassDB::bind_method(D_METHOD("edit_revision"), &SolarSystem::edit_revision);
    ClassDB::bind_method(D_METHOD("body_params", "body"), &SolarSystem::body_params);
    ClassDB::bind_method(D_METHOD("stability_warnings"), &SolarSystem::stability_warnings);
    ClassDB::bind_method(D_METHOD("scenario_to_json", "bookmarks"), &SolarSystem::scenario_to_json);
    ClassDB::bind_method(D_METHOD("scenario_from_json", "text"), &SolarSystem::scenario_from_json);
    ClassDB::bind_method(D_METHOD("scenario_bookmarks"), &SolarSystem::scenario_bookmarks);
}

}  // namespace godot
