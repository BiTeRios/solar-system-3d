"""Значения из статей для тел, которых нет в ядрах SPICE и таблицах NASA (решения 11.10.2026 и 13.10.2026).

Каждое значение переписано из первоисточника; в "quote" — где именно оно стоит.
GM = G·M, G = 6,67430·10⁻¹¹ м³/(кг·с²) (CODATA 2018), кроме Макемаке: там GM = 4π²a³/P² по орбите MK2.
"""
from __future__ import annotations

import math

G_KM3 = 6.67430e-20  # км³/(кг·с²), CODATA 2018

SOURCES = {
    "codata2018_G": {"title": "CODATA 2018: G = 6.67430e-11 m^3 kg^-1 s^-2",
                     "url": "https://physics.nist.gov/cgi-bin/cuu/Value?bg"},
    "lit_holler2021": {"title": "Holler B. J., Grundy W. M., Buie M. W., Noll K. S. The Eris/Dysnomia system I: "
                                "The orbit of Dysnomia. Icarus 355, 114130 (2021)",
                       "url": "https://arxiv.org/abs/2009.13733", "doi": "10.1016/j.icarus.2020.114130"},
    "lit_sicardy2011": {"title": "Sicardy B. et al. A Pluto-like radius and a high albedo for the dwarf planet Eris "
                                 "from an occultation. Nature 478, 493 (2011); значение приводится по Holler et al. 2021, разд. 4",
                        "url": "https://arxiv.org/abs/2009.13733"},
    "lit_ragozzine2009": {"title": "Ragozzine D., Brown M. E. Orbits and Masses of the Satellites of the Dwarf Planet "
                                   "Haumea = 2003 EL61. AJ 137, 4766 (2009)",
                          "url": "https://arxiv.org/abs/0903.4213"},
    "lit_ortiz2017": {"title": "Ortiz J. L. et al. The size, shape, density and ring of the dwarf planet Haumea "
                               "from a stellar occultation. Nature 550, 219 (2017)",
                      "url": "https://arxiv.org/abs/2006.03113"},
    "lit_ortiz2012": {"title": "Ortiz J. L. et al. Albedo and atmospheric constraints of dwarf planet Makemake "
                               "from a stellar occultation. Nature 491, 566 (2012)",
                      "url": "https://doi.org/10.1038/nature11597"},
    "preprint_bamberger2025": {"title": "Bamberger D. A preliminary orbit for the satellite of dwarf planet (136472) "
                                        "Makemake. arXiv:2509.05880 (v3, 2026-02-28). ПРЕПРИНТ, без рецензии",
                               "url": "https://arxiv.org/abs/2509.05880", "preprint": True},
}

_P_MK2_S = 18.023 * 86400.0
_A_MK2_KM = 22250.0
_GM_MAKEMAKE = 4.0 * math.pi ** 2 * _A_MK2_KM ** 3 / _P_MK2_S ** 2
# относительная ошибка: 3·σa/a + 2·σP/P
_GM_MAKEMAKE_SIGMA = _GM_MAKEMAKE * (3 * 780.0 / _A_MK2_KM + 2 * 0.017 / 18.023)

LITERATURE = {
    "eris": {
        "gm_km3s2": {"v": G_KM3 * 1.6466e22, "sigma": G_KM3 * 0.0085e22, "src": "lit_holler2021",
                     "quote": "масса системы Эрида+Дисномия (1.6466 ± 0.0085)×10^22 кг, табл. 2, столбец Combined; × G (CODATA 2018)",
                     "note": "масса всей системы; Дисномия ~0,5 % и как тело не моделируется"},
        "radius_mean_km": {"v": 1163.0, "sigma": 6.0, "src": "lit_sicardy2011",
                           "quote": "«a more precise radius measurement of 1163±6 km» (Holler et al. 2021, разд. 4)"},
        "geometric_albedo": {"v": 0.96, "range": [0.92, 1.05], "src": "lit_sicardy2011",
                             "quote": "аннотация: visible geometric albedo p(V) = 0.96 (+0.09, −0.04)"},
    },
    "haumea": {
        "gm_km3s2": {"v": G_KM3 * 4.006e21, "sigma": G_KM3 * 0.040e21, "src": "lit_ragozzine2009",
                     "quote": "Mass 4.006 ± 0.040 ×10^21 кг, табл. 2; × G (CODATA 2018)"},
        "radii_km": {"v": [1161.0, 852.0, 513.0], "sigma": [30.0, 2.0, 16.0], "src": "lit_ortiz2017",
                     "quote": "«a × b × c = (1,161 ± 30) × (852 ± 2) × (513 ± 16) km»"},
        "geometric_albedo": {"v": 0.51, "sigma": 0.02, "src": "lit_ortiz2017",
                             "quote": "аннотация: «a geometric albedo of 0.51 ± 0.02»"},
    },
    "makemake": {
        "gm_km3s2": {"v": _GM_MAKEMAKE, "sigma": _GM_MAKEMAKE_SIGMA, "src": "preprint_bamberger2025",
                     "quote": "орбита MK2: P = 18.023 ± 0.017 сут, a = 22250 ± 780 км (аннотация); GM = 4π²a³/P²",
                     "note": "препринт без рецензии; масса всей системы, MK2 считается пренебрежимо малым"},
        "geometric_albedo": {"v": 0.77, "sigma": 0.03, "src": "lit_ortiz2012",
                             "quote": "аннотация: V-band geometric albedo 0.77 ± 0.03"},
    },
}
