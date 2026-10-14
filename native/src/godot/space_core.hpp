// Привязка ядра к Godot. Godot видит класс SpaceCore (RefCounted).
// Векторы отдаются как PackedFloat64Array: Vector3 в Godot хранит float32.
#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "core/ephemeris.hpp"

namespace godot {

class SpaceCore : public RefCounted {
    GDCLASS(SpaceCore, RefCounted)

public:
    // Открывает .eph (путь res://, user:// или обычный). false — ошибка, текст в last_error().
    bool open_ephemeris(const String &path);
    String last_error() const;
    // [x, y, z, vx, vy, vz] в км и км/с, ICRF. ssb = true — относительно барицентра, иначе — от своего центра.
    // Пустой массив — нет данных на эту эпоху.
    PackedFloat64Array ephemeris_state(int target, double et, bool ssb) const;
    String version() const;

protected:
    static void _bind_methods();

private:
    sc::Ephemeris eph_;
    String error_;
};

}  // namespace godot
