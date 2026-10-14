// Поверхность тела — кубосфера: шесть граней куба, каждая — квадродерево чанков. Точка грани переводится на сферу
// равномерным отображением (x·√(1 − y²/2 − z²/2 + y²z²/3) и т. д.), затем растягивается до эллипсоида (a, b, c).
// У каждого чанка свой центр: вершины хранятся относительно него во float, сам центр — в double. Поэтому
// у поверхности нет дрожания float32 даже на Земле с камерой в метре над землёй.
// Стыки чанков разного уровня закрыты «юбками» — полосами, опущенными вниз по нормали.
#pragma once

#include <cstdint>
#include <vector>

#include "vec3.hpp"

namespace sc {

struct ChunkId {
    int face = 0, level = 0;
    uint32_t x = 0, y = 0;
    uint64_t key() const {
        return (static_cast<uint64_t>(face) << 61) | (static_cast<uint64_t>(level) << 56) |
               (static_cast<uint64_t>(x) << 28) | static_cast<uint64_t>(y);
    }
};

struct ChunkMesh {
    V3 center;                   // км, система тела
    double radius = 0;           // радиус описанной сферы, км
    std::vector<float> pos;      // xyz относительно центра, метры
    std::vector<float> normal;   // нормаль эллипсоида
    std::vector<float> dir;      // направление из центра тела (для долготы и широты), единичное
    std::vector<int32_t> index;  // треугольники
};

class CubeSphere {
public:
    V3 radii{1, 1, 1};  // км
    int grid = 32;      // ячеек на сторону чанка
    int max_level = 20;

    // Наибольший уровень: ячейка не меньше min_cell_m метров.
    void set_body(const V3 &radii_km, double min_cell_m = 0.1);

    // Чанки для камеры в точке cam (км, система тела): делим, пока размер/расстояние > split.
    // Чанки за горизонтом отбрасываются. view_dir (единичный, система тела) и half_angle (рад) — конус зрения:
    // чанки вне него не делятся и не выводятся; half_angle ≥ π — без отсечения.
    void select(const V3 &cam, double split, std::vector<ChunkId> &out, const V3 &view_dir = V3{0, 0, 1},
                double half_angle = 4.0) const;
    void build(const ChunkId &id, ChunkMesh &out) const;

    V3 surface(int face, double s, double t) const;  // км, s, t ∈ [0, 1]
    void bounds(const ChunkId &id, V3 &center, double &radius) const;
    // Радиус эллипсоида в направлении u (единичный вектор, система тела).
    double radius_along(const V3 &u) const;
    // Пересечение луча (o + k·d, k ≥ 0) с эллипсоидом; false — промах.
    bool ray_hit(const V3 &o, const V3 &d, double &k) const;

private:
    struct Cone {
        V3 dir;
        double half = 4.0;
    };
    void visit(const ChunkId &id, const V3 &cam, const V3 &cam_scaled, double split, const Cone &cone,
               std::vector<ChunkId> &out) const;
};

}  // namespace sc
