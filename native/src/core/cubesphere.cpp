#include "cubesphere.hpp"

#include <algorithm>
#include <cmath>

namespace sc {

namespace {

// Грани: нормаль n и оси u, v с u × v = n.
const V3 kN[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const V3 kU[6] = {{0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {0, 1, 0}};
const V3 kV[6] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0}};

// Точка куба → единичная сфера (равномерное отображение).
V3 cube_to_sphere(const V3 &c) {
    const double x2 = c.x * c.x, y2 = c.y * c.y, z2 = c.z * c.z;
    return {c.x * std::sqrt(std::max(0.0, 1 - y2 / 2 - z2 / 2 + y2 * z2 / 3)),
            c.y * std::sqrt(std::max(0.0, 1 - z2 / 2 - x2 / 2 + z2 * x2 / 3)),
            c.z * std::sqrt(std::max(0.0, 1 - x2 / 2 - y2 / 2 + x2 * y2 / 3))};
}

V3 unit(const V3 &a) {
    const double n = norm(a);
    return n > 0 ? a / n : V3{};
}

}  // namespace

void CubeSphere::set_body(const V3 &radii_km, double min_cell_m) {
    radii = radii_km;
    const double rmax = std::max(radii.x, std::max(radii.y, radii.z));
    const double face_cell_m = rmax * 1000.0 * (kPi / 2) / grid;
    max_level = std::clamp(static_cast<int>(std::ceil(std::log2(face_cell_m / min_cell_m))), 0, 26);
}

V3 CubeSphere::surface(int face, double s, double t) const {
    const V3 c = kN[face] + kU[face] * (2 * s - 1) + kV[face] * (2 * t - 1);
    const V3 u = cube_to_sphere(c);
    return {u.x * radii.x, u.y * radii.y, u.z * radii.z};
}

double CubeSphere::radius_along(const V3 &u) const {
    const double a = u.x / radii.x, b = u.y / radii.y, c = u.z / radii.z;
    return 1.0 / std::sqrt(a * a + b * b + c * c);
}

bool CubeSphere::ray_hit(const V3 &o, const V3 &d, double &k) const {
    // В масштабированных координатах эллипсоид — единичная сфера. Через точку наибольшего сближения, а не через
    // дискриминант b² − ac: издалека (|o| ~ 10⁷ радиусов) b² и ac ~ 10¹⁵ и их разность теряет все знаки.
    const V3 os{o.x / radii.x, o.y / radii.y, o.z / radii.z}, ds{d.x / radii.x, d.y / radii.y, d.z / radii.z};
    const double a = dot(ds, ds);
    if (a == 0) return false;
    const double t0 = -dot(os, ds) / a;
    const V3 p = os + ds * t0;  // ближайшая к центру точка прямой
    const double h = 1.0 - dot(p, p);
    if (h < 0) return false;
    const double half = std::sqrt(h / a);
    double k0 = t0 - half;
    if (k0 < 0) k0 = t0 + half;
    if (k0 < 0) return false;
    k = k0;
    return true;
}

void CubeSphere::bounds(const ChunkId &id, V3 &center, double &radius) const {
    const double n = static_cast<double>(1u << id.level);
    const double s0 = id.x / n, s1 = (id.x + 1) / n, t0 = id.y / n, t1 = (id.y + 1) / n;
    center = surface(id.face, (s0 + s1) / 2, (t0 + t1) / 2);
    radius = 0;
    for (int i = 0; i <= 2; ++i)
        for (int j = 0; j <= 2; ++j) {
            const V3 p = surface(id.face, s0 + (s1 - s0) * i / 2, t0 + (t1 - t0) * j / 2);
            radius = std::max(radius, norm(p - center));
        }
    radius *= 1.02;
}

void CubeSphere::visit(const ChunkId &id, const V3 &cam, const V3 &cam_scaled, double split, const Cone &cone,
                       std::vector<ChunkId> &out) const {
    V3 c;
    double r;
    bounds(id, c, r);
    // за горизонтом: в масштабированных координатах точка X видна, если X·C ≥ 1
    const double rmin = std::min(radii.x, std::min(radii.y, radii.z));
    const V3 cs{c.x / radii.x, c.y / radii.y, c.z / radii.z};
    const double cn = norm(cam_scaled);
    if (cn > 1.0 && dot(cs, cam_scaled) + (r / rmin) * cn < 1.0) return;
    // вне конуса зрения
    const double dc = norm(c - cam);
    if (cone.half < kPi && dc > r) {
        const double ang = std::acos(std::clamp(dot((c - cam) / dc, cone.dir), -1.0, 1.0));
        if (ang - std::asin(r / dc) > cone.half) return;
    }
    const double d = std::max(dc - r, 1e-9);
    if (id.level < max_level && 2 * r / d > split) {
        for (uint32_t k = 0; k < 4; ++k) {
            ChunkId ch;
            ch.face = id.face;
            ch.level = id.level + 1;
            ch.x = id.x * 2 + (k & 1);
            ch.y = id.y * 2 + (k >> 1);
            visit(ch, cam, cam_scaled, split, cone, out);
        }
        return;
    }
    out.push_back(id);
}

void CubeSphere::select(const V3 &cam, double split, std::vector<ChunkId> &out, const V3 &view_dir,
                        double half_angle) const {
    out.clear();
    const V3 cs{cam.x / radii.x, cam.y / radii.y, cam.z / radii.z};
    Cone cone;
    cone.dir = view_dir;
    cone.half = half_angle;
    for (int f = 0; f < 6; ++f) {
        ChunkId id;
        id.face = f;
        visit(id, cam, cs, split, cone, out);
    }
}

void CubeSphere::build(const ChunkId &id, ChunkMesh &out) const {
    bounds(id, out.center, out.radius);
    const int g = grid, row = g + 1;
    // s = (x·g + i) / (2^level·g): числитель целый, знаменатель — степень двойки на g, поэтому общие края соседних
    // чанков (и граней) получают одни и те же s и t до бита — без щелей.
    const double den = static_cast<double>(1u << id.level) * g;
    const int64_t si0 = static_cast<int64_t>(id.x) * g, ti0 = static_cast<int64_t>(id.y) * g;
    const size_t nv = static_cast<size_t>(row * row + 4 * row);
    out.pos.resize(nv * 3);
    out.normal.resize(nv * 3);
    out.dir.resize(nv * 3);
    auto put = [&](size_t k, const V3 &p) {
        const V3 rel = (p - out.center) * 1000.0;
        const V3 nr = unit(V3{p.x / (radii.x * radii.x), p.y / (radii.y * radii.y), p.z / (radii.z * radii.z)});
        const V3 dr = unit(p);
        for (int c = 0; c < 3; ++c) {
            out.pos[k * 3 + c] = static_cast<float>(rel[c]);
            out.normal[k * 3 + c] = static_cast<float>(nr[c]);
            out.dir[k * 3 + c] = static_cast<float>(dr[c]);
        }
    };
    std::vector<V3> pts(static_cast<size_t>(row * row));
    for (int j = 0; j <= g; ++j)
        for (int i = 0; i <= g; ++i) {
            const V3 p = surface(id.face, static_cast<double>(si0 + i) / den, static_cast<double>(ti0 + j) / den);
            pts[static_cast<size_t>(j * row + i)] = p;
            put(static_cast<size_t>(j * row + i), p);
        }
    // юбки: копии краёв, опущенные по нормали на 2 % размера чанка
    const double skirt = 0.02 * 2 * out.radius;
    size_t k = static_cast<size_t>(row * row);
    auto edge = [&](int i, int j) {
        const V3 &p = pts[static_cast<size_t>(j * row + i)];
        const V3 nr = unit(V3{p.x / (radii.x * radii.x), p.y / (radii.y * radii.y), p.z / (radii.z * radii.z)});
        put(k, p - nr * skirt);
        // нормаль и направление юбки — как у края, чтобы освещение совпадало
        for (int c = 0; c < 3; ++c) {
            out.normal[k * 3 + c] = out.normal[static_cast<size_t>(j * row + i) * 3 + c];
            out.dir[k * 3 + c] = out.dir[static_cast<size_t>(j * row + i) * 3 + c];
        }
        ++k;
    };
    for (int i = 0; i <= g; ++i) edge(i, 0);
    for (int i = 0; i <= g; ++i) edge(i, g);
    for (int j = 0; j <= g; ++j) edge(0, j);
    for (int j = 0; j <= g; ++j) edge(g, j);

    // Godot: лицевая сторона — обход по часовой стрелке, если смотреть снаружи.
    out.index.clear();
    out.index.reserve(static_cast<size_t>(g * g * 6 + 4 * g * 12));
    for (int j = 0; j < g; ++j)
        for (int i = 0; i < g; ++i) {
            const int a = j * row + i, b = a + 1, c = a + row, d = c + 1;
            out.index.insert(out.index.end(), {a, c, b, b, c, d});
        }
    const int base = row * row;
    auto strip = [&](int e0, int stride, int sk0) {
        for (int m = 0; m < g; ++m) {
            const int a = e0 + m * stride, b = e0 + (m + 1) * stride, sa = sk0 + m, sb = sk0 + m + 1;
            // юбка видна с обеих сторон: обе ориентации треугольников
            out.index.insert(out.index.end(), {a, b, sa, b, sb, sa, a, sa, b, b, sa, sb});
        }
    };
    strip(0, 1, base);                      // нижний край (j = 0)
    strip(g * row, 1, base + row);          // верхний (j = g)
    strip(0, row, base + 2 * row);          // левый (i = 0)
    strip(g, row, base + 3 * row);          // правый (i = g)
}

}  // namespace sc
