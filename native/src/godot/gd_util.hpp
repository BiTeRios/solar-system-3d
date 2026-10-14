// Мелкие помощники для классов Godot-части: перевод double → float, чтение файлов, материалы.
// Общие для solar_system.cpp и solar_system_look.cpp.
#pragma once

#include <string>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "core/vec3.hpp"

namespace godot {
namespace gdu {

using sc::M3;
using sc::V3;

constexpr double kKm = 1000.0;  // единица сцены — метр
// Отсечение по пирамиде видимости Godot делает по несжатым координатам, а дальняя плоскость у нас в сжатых —
// поэтому у наших объектов «бесконечный» AABB, а лишние чанки отбрасывает CubeSphere::select (конус зрения).
inline const AABB kHugeAabb(Vector3(-1e30f, -1e30f, -1e30f), Vector3(2e30f, 2e30f, 2e30f));

inline V3 unit(const V3 &a) {
    const double n = sc::norm(a);
    return n > 0 ? a / n : V3{};
}

inline Vector3 to_vec3(const V3 &a) {
    return Vector3(static_cast<real_t>(a.x), static_cast<real_t>(a.y), static_cast<real_t>(a.z));
}

inline Basis to_basis(const M3 &m) {
    Basis b;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) b.rows[i][j] = static_cast<real_t>(m.m[i][j]);
    return b;
}

// double → две части float: x ≈ hi + lo с точностью ~1e-14 относительно x.
inline void split(const V3 &x, Vector3 &hi, Vector3 &lo) {
    hi = to_vec3(x);
    lo = Vector3(static_cast<real_t>(x.x - static_cast<double>(hi.x)), static_cast<real_t>(x.y - static_cast<double>(hi.y)),
                 static_cast<real_t>(x.z - static_cast<double>(hi.z)));
}

inline String u8(const std::string &s) { return String::utf8(s.c_str()); }

inline std::string read_all(const String &path, bool &ok) {
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
    ok = f.is_valid();
    if (!ok) return {};
    const PackedByteArray b = f->get_buffer(static_cast<int64_t>(f->get_length()));
    return std::string(reinterpret_cast<const char *>(b.ptr()), static_cast<size_t>(b.size()));
}

inline Ref<ShaderMaterial> make_material(const String &shader_path) {
    Ref<Shader> sh = ResourceLoader::get_singleton()->load(shader_path);
    Ref<ShaderMaterial> m;
    m.instantiate();
    m->set_shader(sh);
    return m;
}

}  // namespace gdu
}  // namespace godot
