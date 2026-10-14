// Ориентация тел по модели IAU (pck00011), как BODEUL/TISBOD в SPICE. Эталон — pipeline/export_constants.py.
#pragma once

#include <vector>

#include "vec3.hpp"

namespace sc {

struct RotationModel {
    std::vector<double> pole_ra, pole_dec, pm;               // град; град/век и град/сутки
    std::vector<double> nut_ra, nut_dec, nut_pm;             // множители sin/cos углов системы
    std::vector<double> system_angles;                       // углы системы: по (max_phase_degree + 1) коэффициентов
    int max_phase_degree = 1;

    bool valid() const { return pole_ra.size() >= 1 && pole_dec.size() >= 1 && pm.size() >= 1; }

    // RA и DEC полюса и угол W нулевого меридиана, радианы.
    void euler(double et, double &ra, double &dec, double &w) const;
    // Матрица J2000 → система тела: [W]₃·[π/2 − δ]₁·[π/2 + α]₃.
    M3 j2000_to_body(double et) const;
    // Северный полюс тела в J2000 (единичный вектор).
    V3 pole(double et) const;
};

}  // namespace sc
