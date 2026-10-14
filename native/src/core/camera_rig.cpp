#include "camera_rig.hpp"

#include <algorithm>
#include <cmath>

namespace sc {

namespace {

V3 unit(const V3 &a) {
    const double n = norm(a);
    return n > 0 ? a / n : V3{};
}

double smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// Сферическая интерполяция единичных векторов.
V3 slerp_vec(const V3 &a, const V3 &b, double t) {
    const double c = std::clamp(dot(a, b), -1.0, 1.0);
    const double ang = std::acos(c);
    if (ang < 1e-9) return a;
    const double s = std::sin(ang);
    if (s < 1e-9) {  // противоположные: поворот через любой перпендикуляр
        V3 p = cross(a, V3{1, 0, 0});
        if (norm(p) < 1e-6) p = cross(a, V3{0, 1, 0});
        p = unit(p);
        return unit(a * std::cos(kPi * t) + p * std::sin(kPi * t));
    }
    return unit(a * (std::sin((1 - t) * ang) / s) + b * (std::sin(t * ang) / s));
}

// Горизонтальные оси для облёта: X — проекция оси X системы на плоскость, перпендикулярную up.
void horizon_axes(const V3 &up, V3 &xh, V3 &yh) {
    xh = V3{1, 0, 0} - up * up.x;
    if (norm(xh) < 1e-6) xh = V3{0, 1, 0} - up * up.y;
    xh = unit(xh);
    yh = cross(up, xh);
}

Quat look_quat(const V3 &pos, const V3 &up) {
    const V3 f = unit(pos * -1.0);
    V3 r = cross(f, up);
    if (norm(r) < 1e-9) {
        V3 xh, yh;
        horizon_axes(up, xh, yh);
        r = cross(f, xh);
    }
    r = unit(r);
    const V3 u = cross(r, f);
    M3 m;
    for (int i = 0; i < 3; ++i) {
        m.m[i][0] = r[i];
        m.m[i][1] = u[i];
        m.m[i][2] = -f[i];
    }
    return Quat::from_matrix(m);
}

}  // namespace

Quat Quat::normalized() const {
    const double n = std::sqrt(w * w + x * x + y * y + z * z);
    return n > 0 ? Quat{w / n, x / n, y / n, z / n} : Quat{};
}

M3 Quat::matrix() const {
    M3 m;
    m.m[0][0] = 1 - 2 * (y * y + z * z);
    m.m[0][1] = 2 * (x * y - w * z);
    m.m[0][2] = 2 * (x * z + w * y);
    m.m[1][0] = 2 * (x * y + w * z);
    m.m[1][1] = 1 - 2 * (x * x + z * z);
    m.m[1][2] = 2 * (y * z - w * x);
    m.m[2][0] = 2 * (x * z - w * y);
    m.m[2][1] = 2 * (y * z + w * x);
    m.m[2][2] = 1 - 2 * (x * x + y * y);
    return m;
}

Quat Quat::from_matrix(const M3 &m) {
    Quat q;
    const double tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q.w = 0.25 * s;
        q.x = (m.m[2][1] - m.m[1][2]) / s;
        q.y = (m.m[0][2] - m.m[2][0]) / s;
        q.z = (m.m[1][0] - m.m[0][1]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const double s = std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25 * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        const double s = std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25 * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25 * s;
    }
    return q.normalized();
}

Quat Quat::axis_angle(const V3 &axis, double angle) {
    const V3 a = unit(axis);
    const double s = std::sin(angle / 2);
    return {std::cos(angle / 2), a.x * s, a.y * s, a.z * s};
}

Quat Quat::slerp(const Quat &a, Quat b, double t) {
    double c = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (c < 0) {
        b = {-b.w, -b.x, -b.y, -b.z};
        c = -c;
    }
    if (c > 0.9995) {
        return Quat{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}
            .normalized();
    }
    const double ang = std::acos(c), s = std::sin(ang);
    const double ka = std::sin((1 - t) * ang) / s, kb = std::sin(t * ang) / s;
    return Quat{a.w * ka + b.w * kb, a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb}.normalized();
}

double CameraRig::speed_from_slider(double u) {
    u = std::clamp(u, 0.0, 1.0);
    if (u == 0.0) return kMinSpeed;
    if (u == 1.0) return kMaxSpeed;
    return kMinSpeed * std::exp(u * std::log(kMaxSpeed / kMinSpeed));
}

double CameraRig::slider_from_speed(double v) {
    v = std::clamp(v, kMinSpeed, kMaxSpeed);
    return std::log(v / kMinSpeed) / std::log(kMaxSpeed / kMinSpeed);
}

void CameraRig::keep_above_surface(const CamFrame &f) {
    if (f.surface_r <= 0) return;
    const double r = norm(pos), rmin = f.surface_r + kMinAltitude;
    if (r >= rmin || r == 0) return;
    const V3 u = pos / r;
    pos = u * rmin;
    const double vin = dot(vel, u);
    if (vin < 0) vel -= u * vin;  // гасим скорость внутрь тела
}

void CameraRig::update_free(const V3 &move, const V3 &rot, double dt, const CamFrame &f) {
    mode = CamMode::Free;
    orient = (orient * Quat::axis_angle({1, 0, 0}, rot.x) * Quat::axis_angle({0, 1, 0}, rot.y) *
              Quat::axis_angle({0, 0, 1}, rot.z))
                 .normalized();
    V3 m = move;
    if (norm(m) > 1) m = unit(m);
    const V3 target = orient.matrix() * m * speed;
    const double k = inertia > 0 ? 1 - std::exp(-dt / inertia) : 1.0;
    vel += (target - vel) * k;
    pos += vel * dt;
    keep_above_surface(f);
}

void CameraRig::look_at_center(const V3 &up) { orient = look_quat(pos, up); }

void CameraRig::angles_from_pos(const CamFrame &f) {
    V3 xh, yh;
    horizon_axes(f.up, xh, yh);
    const V3 u = unit(pos);
    pitch = std::asin(std::clamp(dot(u, f.up), -1.0, 1.0));
    yaw = std::atan2(dot(u, yh), dot(u, xh));
    orbit_dist = norm(pos);
}

V3 CameraRig::pos_from_angles(const CamFrame &f) const {
    V3 xh, yh;
    horizon_axes(f.up, xh, yh);
    return (xh * (std::cos(pitch) * std::cos(yaw)) + yh * (std::cos(pitch) * std::sin(yaw)) + f.up * std::sin(pitch)) *
           orbit_dist;
}

void CameraRig::start_orbit(const CamFrame &f) {
    mode = CamMode::Orbit;
    vel = V3{};
    angles_from_pos(f);
    look_at_center(f.up);
}

void CameraRig::update_orbit(double dyaw, double dpitch, double zoom_steps, const CamFrame &f) {
    mode = CamMode::Orbit;
    yaw += dyaw;
    const double lim = 89.9 * kDeg;
    pitch = std::clamp(pitch + dpitch, -lim, lim);
    if (zoom_steps != 0) {
        const double base = f.surface_r > 0 ? f.surface_r : 0.0;
        double h = std::max(orbit_dist - base, kMinAltitude);
        h = std::max(h * std::pow(1.2, zoom_steps), kMinAltitude);
        orbit_dist = base + h;
    }
    if (f.surface_r > 0) orbit_dist = std::max(orbit_dist, f.surface_r + kMinAltitude);
    pos = pos_from_angles(f);
    look_at_center(f.up);
}

double CameraRig::start_flyto(const CamFrame &f, double final_dist) {
    mode = CamMode::FlyTo;
    vel = V3{};
    fly_t_ = 0;
    fly_d0_ = std::max(norm(pos), 1e-9);
    fly_d1_ = final_dist;
    fly_u0_ = unit(pos);
    const V3 s = unit(f.sun_dir);
    const V3 mid = fly_u0_ + s;  // подлетаем к освещённой стороне
    fly_u1_ = (norm(s) > 0 && norm(mid) > 0.3) ? unit(mid) : fly_u0_;
    fly_q0_ = orient;
    fly_dur_ = 2.0 + 3.0 * std::clamp(std::fabs(std::log(fly_d0_ / fly_d1_)) / std::log(1e8), 0.0, 1.0);
    return fly_dur_;
}

void CameraRig::update_flyto(double dt, const CamFrame &f) {
    if (mode != CamMode::FlyTo) return;
    fly_t_ += dt;
    const double tau = std::min(1.0, fly_t_ / fly_dur_);
    const double e = smooth(tau);
    const double d = std::exp(std::log(fly_d0_) + (std::log(fly_d1_) - std::log(fly_d0_)) * e);
    pos = slerp_vec(fly_u0_, fly_u1_, e) * d;
    orient = Quat::slerp(fly_q0_, look_quat(pos, f.up), smooth(tau / 0.35));
    if (tau >= 1.0) start_orbit(f);
}

}  // namespace sc
