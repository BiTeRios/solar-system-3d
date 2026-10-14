// Звёзды и Млечный Путь (этап 7): каталог Hipparcos из game/data/stars.bin (pipeline/build_stars.py) и карта
// NASA SVS у дальней плоскости. Звёзды и небо не зависят от положения камеры (бесконечно далеко), только от поворота.
#include <cmath>
#include <cstring>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "gd_util.hpp"
#include "solar_system.hpp"

namespace godot {

using sc::M3;
using sc::V3;
using namespace gdu;

namespace {
constexpr double kJulianYear = 365.25 * 86400.0;
}

void SolarSystem::load_sky(const String &dir) {
    RenderingServer *rs = RenderingServer::get_singleton();
    const RID scenario = get_world_3d()->get_scenario();
    // ---- звёзды
    Ref<FileAccess> f = FileAccess::open(dir + String("stars.bin"), FileAccess::READ);
    if (f.is_valid() && f->get_buffer(4).get_string_from_ascii() == String("STAR") && f->get_32() == 1) {
        const int64_t n = f->get_32();
        star_epoch_ = f->get_double();
        const PackedByteArray raw = f->get_buffer(n * 44);
        if (raw.size() == n * 44) {
            PackedVector3Array verts;
            PackedVector2Array uvs;
            PackedFloat32Array c0, c1;
            PackedInt32Array idx;
            verts.resize(n * 4);
            uvs.resize(n * 4);
            c0.resize(n * 16);
            c1.resize(n * 16);
            idx.resize(n * 6);
            star_data_.assign(static_cast<size_t>(n) * 6, 0.0f);
            star_hip_.clear();
            const uint8_t *p = raw.ptr();
            const Vector2 corners[4] = {Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)};
            for (int64_t s = 0; s < n; ++s) {
                uint32_t hip;
                float v[10];  // направление, собственное движение, V, цвет
                std::memcpy(&hip, p + s * 44, 4);
                std::memcpy(v, p + s * 44 + 4, 40);
                star_hip_[static_cast<int>(hip)] = static_cast<size_t>(s);
                std::copy(v, v + 6, star_data_.begin() + s * 6);
                for (int k = 0; k < 4; ++k) {
                    const int64_t q = s * 4 + k;
                    verts.set(q, Vector3(v[0], v[1], v[2]));
                    uvs.set(q, corners[k]);
                    float *a = c0.ptrw() + q * 4, *b = c1.ptrw() + q * 4;
                    a[0] = v[3], a[1] = v[4], a[2] = v[5], a[3] = v[6];
                    b[0] = v[7], b[1] = v[8], b[2] = v[9], b[3] = 1.0f;
                }
                const int32_t q = static_cast<int32_t>(s * 4);
                const int32_t tri[6] = {q, q + 1, q + 2, q, q + 2, q + 3};
                std::copy(tri, tri + 6, idx.ptrw() + s * 6);
            }
            Array arrays;
            arrays.resize(RenderingServer::ARRAY_MAX);
            arrays[RenderingServer::ARRAY_VERTEX] = verts;
            arrays[RenderingServer::ARRAY_TEX_UV] = uvs;
            arrays[RenderingServer::ARRAY_CUSTOM0] = c0;
            arrays[RenderingServer::ARRAY_CUSTOM1] = c1;
            arrays[RenderingServer::ARRAY_INDEX] = idx;
            stars_mesh_ = rs->mesh_create();
            rs->mesh_add_surface_from_arrays(
                stars_mesh_, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(),
                (static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA_FLOAT) << RenderingServer::ARRAY_FORMAT_CUSTOM0_SHIFT) |
                    (static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGBA_FLOAT) << RenderingServer::ARRAY_FORMAT_CUSTOM1_SHIFT));
            stars_material_ = make_material("res://shaders/stars.gdshader");
            stars_material_->set_render_priority(-100);
            stars_instance_ = rs->instance_create2(stars_mesh_, scenario);
            rs->instance_geometry_set_material_override(stars_instance_, stars_material_->get_rid());
            rs->instance_geometry_set_cast_shadows_setting(stars_instance_, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            rs->instance_set_custom_aabb(stars_instance_, kHugeAabb);
        }
    }
    // ---- Млечный Путь: квадрат на весь экран
    Ref<Texture2D> tex = ResourceLoader::get_singleton()->load("res://assets/sky/milkyway_8k.exr");
    if (tex.is_valid()) {
        PackedVector3Array verts;
        verts.push_back(Vector3(-1, -1, 0));
        verts.push_back(Vector3(1, -1, 0));
        verts.push_back(Vector3(1, 1, 0));
        verts.push_back(Vector3(-1, 1, 0));
        PackedInt32Array idx;
        for (int k : {0, 1, 2, 0, 2, 3}) idx.push_back(k);
        Array arrays;
        arrays.resize(RenderingServer::ARRAY_MAX);
        arrays[RenderingServer::ARRAY_VERTEX] = verts;
        arrays[RenderingServer::ARRAY_INDEX] = idx;
        milkyway_mesh_ = rs->mesh_create();
        rs->mesh_add_surface_from_arrays(milkyway_mesh_, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
        milkyway_material_ = make_material("res://shaders/milkyway.gdshader");
        milkyway_material_->set_render_priority(-101);
        milkyway_material_->set_shader_parameter("milkyway", tex);
        milkyway_instance_ = rs->instance_create2(milkyway_mesh_, scenario);
        rs->instance_geometry_set_material_override(milkyway_instance_, milkyway_material_->get_rid());
        rs->instance_geometry_set_cast_shadows_setting(milkyway_instance_, RenderingServer::SHADOW_CASTING_SETTING_OFF);
        rs->instance_set_custom_aabb(milkyway_instance_, kHugeAabb);
    }
}

void SolarSystem::update_sky() {
    RenderingServer *rs = RenderingServer::get_singleton();
    const bool normal = !flat_ && debug_view_ == 0;
    if (stars_instance_.is_valid()) {
        rs->instance_set_visible(stars_instance_, normal && fx_stars_);
        stars_material_->set_shader_parameter("icrf_to_view", to_basis(view_));
        stars_material_->set_shader_parameter("years", et_ / kJulianYear + (2000.0 - star_epoch_));
    }
    if (milkyway_instance_.is_valid()) {
        rs->instance_set_visible(milkyway_instance_, (normal && fx_milkyway_) || sky_debug_ != 0);
        milkyway_material_->set_shader_parameter("view_to_icrf", to_basis(view_.transposed()));
    }
}

void SolarSystem::set_sky_param(const String &name, double value) {
    if (name == "star_flux0" && stars_material_.is_valid()) stars_material_->set_shader_parameter("flux0", value);
    if (name == "milkyway_scale" && milkyway_material_.is_valid()) milkyway_material_->set_shader_parameter("scale", value);
    if (name == "milkyway_debug" && milkyway_material_.is_valid()) {
        sky_debug_ = static_cast<int>(value);
        milkyway_material_->set_shader_parameter("debug_view", sky_debug_);
    }
}

PackedFloat64Array SolarSystem::star_direction(int hip) const {
    PackedFloat64Array out;
    auto it = star_hip_.find(hip);
    if (it == star_hip_.end()) return out;
    const float *s = star_data_.data() + it->second * 6;
    const double t = et_ / kJulianYear + (2000.0 - star_epoch_);
    const V3 d = unit(V3{s[0] + s[3] * t, s[1] + s[4] * t, s[2] + s[5] * t});
    out.push_back(d.x);
    out.push_back(d.y);
    out.push_back(d.z);
    return out;
}

void SolarSystem::free_sky() {
    RenderingServer *rs = RenderingServer::get_singleton();
    for (RID *r : {&stars_instance_, &stars_mesh_, &milkyway_instance_, &milkyway_mesh_})
        if (r->is_valid()) {
            rs->free_rid(*r);
            *r = RID();
        }
}

}  // namespace godot
