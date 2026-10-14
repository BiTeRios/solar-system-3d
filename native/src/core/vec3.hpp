// Трёхмерный вектор в double. Ядро не зависит от Godot: Vector3 там во float32.
#pragma once

#include <cmath>

namespace sc {

struct V3 {
    double x = 0.0, y = 0.0, z = 0.0;

    V3() = default;
    V3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    V3 operator+(const V3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator-() const { return {-x, -y, -z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    V3 operator/(double s) const { return {x / s, y / s, z / s}; }
    V3 &operator+=(const V3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
    V3 &operator-=(const V3 &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    V3 &operator*=(double s) { x *= s; y *= s; z *= s; return *this; }

    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    double &operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

inline V3 operator*(double s, const V3 &v) { return v * s; }
inline double dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3 &a, const V3 &b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm(const V3 &a) { return std::sqrt(dot(a, a)); }

// Матрица 3×3 по строкам: v' = M·v.
struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    V3 operator*(const V3 &v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    M3 operator*(const M3 &o) const {
        M3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }
    M3 transposed() const {
        M3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[j][i];
        return r;
    }
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kC = 299792.458;  // скорость света, км/с
constexpr double kDay = 86400.0;

}  // namespace sc
