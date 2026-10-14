// Камера в double: положение относительно «опорного» тела, ориентация, режимы полёта.
// Godot получает только итог: поворот камеры и положения тел относительно неё (floating origin).
//
// Опорное тело — центр системы отсчёта камеры. Система инерциальная (оси ICRF) или вращается вместе с телом
// (оси тела по pck00011, body_fixed). Камера Godot смотрит вдоль −Z, вверх — +Y.
#pragma once

#include "vec3.hpp"

namespace sc {

struct Quat {
    double w = 1, x = 0, y = 0, z = 0;

    Quat operator*(const Quat &o) const {
        return {w * o.w - x * o.x - y * o.y - z * o.z, w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w};
    }
    Quat normalized() const;
    M3 matrix() const;  // столбцы — оси камеры в системе отсчёта
    static Quat from_matrix(const M3 &m);
    static Quat axis_angle(const V3 &axis, double angle);
    static Quat slerp(const Quat &a, Quat b, double t);
};

enum class CamMode { Free = 0, Orbit = 1, FlyTo = 2 };

// Что ригу нужно знать о системе отсчёта на текущий кадр (даёт привязка к Godot).
struct CamFrame {
    double surface_r = 0;    // радиус поверхности опорного тела по направлению на камеру, км (0 — нет тела)
    double body_radius = 0;  // средний радиус опорного тела, км
    V3 sun_dir;              // направление от опорного тела на Солнце (в системе отсчёта камеры)
    V3 up{0, 0, 1};          // «верх» для облёта (полюс эклиптики или полюс тела), в системе отсчёта
};

class CameraRig {
public:
    static constexpr double kMinSpeed = 1e-3;              // 1 м/с, км/с
    static constexpr double kMaxSpeed = 1000.0 * kC;       // 1000 c, км/с
    static constexpr double kMinAltitude = 1e-4;           // 10 см над поверхностью, км

    int anchor = -1;         // индекс тела в каталоге; −1 — барицентр Солнечной системы
    bool body_fixed = false;
    V3 pos;                  // км от центра опорного тела, в системе отсчёта
    Quat orient;             // камера → система отсчёта
    V3 vel;                  // км/с в системе отсчёта (свободный полёт)
    double speed = 1.0;      // заданная скорость свободного полёта, км/с
    double inertia = 0.25;   // постоянная времени разгона и торможения, с
    CamMode mode = CamMode::Free;

    // облёт: расстояние от центра и углы в системе «верха» (долгота и широта камеры относительно тела)
    double orbit_dist = 0, yaw = 0, pitch = 0;

    // Свободный полёт: move — направление в осях камеры (−1…1 по каждой оси), rot — повороты (рад) вокруг
    // осей камеры X (тангаж), Y (рысканье), Z (крен). dt — реальное время кадра, с.
    void update_free(const V3 &move, const V3 &rot, double dt, const CamFrame &f);
    // Облёт: смещения углов (рад) и шаги колеса (зум: высота над поверхностью умножается на 1,2 за шаг).
    void update_orbit(double dyaw, double dpitch, double zoom_steps, const CamFrame &f);
    // Перелёт: продвинуть на dt. По окончании — режим облёта.
    void update_flyto(double dt, const CamFrame &f);

    // Начать облёт опорного тела с текущего места (смотрим на центр).
    void start_orbit(const CamFrame &f);
    // Начать перелёт к опорному телу; pos уже пересчитан в его систему. Возвращает длительность, с.
    double start_flyto(const CamFrame &f, double final_dist);
    // Шкала скорости 0…1 ↔ 1 м/с…1000 c (логарифмическая).
    static double speed_from_slider(double u);
    static double slider_from_speed(double v);

    double flyto_elapsed() const { return fly_t_; }
    double flyto_duration() const { return fly_dur_; }

private:
    void look_at_center(const V3 &up);
    void angles_from_pos(const CamFrame &f);
    V3 pos_from_angles(const CamFrame &f) const;
    void keep_above_surface(const CamFrame &f);

    // перелёт
    double fly_t_ = 0, fly_dur_ = 0, fly_d0_ = 0, fly_d1_ = 0;
    V3 fly_u0_, fly_u1_;
    Quat fly_q0_;
};

}  // namespace sc
