"""Параметры внешнего вида, которых нет в ядрах SPICE и таблицах NASA: значения из статей (решения 13.10.2026).

Каждое значение переписано из первоисточника; в "quote" — где именно оно стоит. Что сюда не вошло и почему:
- пыль Марса — нет свободно доступных типичной оптической толщины и альбедо однократного рассеяния в синем;
- рэлеевское рассеяние на CO₂ (Марс) — формула показателя преломления CO₂ (Sneep & Ubachs 2005) недоступна;
- дымка Титана — нет альбедо в красной полосе.
Эти тела рисуются без атмосферы, в appearance.json у них пометка.
"""
from __future__ import annotations

import math

SOURCES = {
    "lit_mallama2017": {"title": "Mallama A., Krobusek B., Pavlov H. Comprehensive wide-band magnitudes and albedos for "
                                 "the planets, with applications to exo-planets and Planet Nine. Icarus 282, 19 (2017)",
                        "url": "https://arxiv.org/abs/1609.05048", "doi": "10.1016/j.icarus.2016.09.023"},
    "lit_hestroffer1998": {"title": "Hestroffer D., Magnan C. Wavelength dependency of the Solar limb darkening. "
                                    "A&A 333, 338 (1998); подгонка по данным Neckel & Labs 1994 и Pierce & Slaughter 1977. "
                                    "Формула приведена по arXiv:2407.21583, разд. 3.2.1",
                           "url": "https://arxiv.org/abs/2407.21583"},
    "lit_baumbach1937": {"title": "Baumbach S. Astron. Nachr. 263, 121 (1937): яркость короны в 10⁻⁶ яркости центра диска. "
                                  "Формула приведена по Turyshev & Toth, MNRAS 515, 6122 (2022), ур. (9); единица 10⁻⁶ "
                                  "центральной яркости — по Allen, MNRAS 107, 426 (1947)",
                         "url": "https://arxiv.org/abs/2204.04866"},
    "lit_kinne2019": {"title": "Kinne S. Aerosol radiative effects with MACv2. Atmos. Chem. Phys. 19, 10919 (2019)",
                      "url": "https://doi.org/10.5194/acp-19-10919-2019"},
    "lit_bruneton2008": {"title": "Bruneton E., Neyret F. Precomputed Atmospheric Scattering. Computer Graphics Forum 27(4), "
                                  "1079 (2008); шкала высот аэрозоля 1,2 км сверена по реализации автора (BSD): "
                                  "github.com/ebruneton/precomputed_atmospheric_scattering, demo.cc, kMieScaleHeight",
                         "url": "https://doi.org/10.1111/j.1467-8659.2008.01245.x"},
    "lit_bodhaine1999": {"title": "Bodhaine B. A., Wood N. B., Dutton E. G., Slusser J. R. On Rayleigh optical depth "
                                  "calculations. J. Atmos. Oceanic Technol. 16, 1854 (1999): ур. (2), (4)–(6), Ns",
                         "url": "https://doi.org/10.1175/1520-0426(1999)016<1854:ORODC>2.0.CO;2"},
}

# Длины волн, которыми представлены каналы R, G, B (как у Bruneton & Neyret 2008), мкм
RGB_UM = (0.680, 0.550, 0.440)

# Геометрическое альбедо Венеры по полосам Джонсона (Mallama et al. 2017, табл. 7): облачный слой
VENUS_BAND_ALBEDO = {"B": 0.658, "V": 0.689, "R": 0.708, "src": "lit_mallama2017",
                     "quote": "Table 7. Geometric albedos, Johnson-Cousins: Venus U 0.348, B 0.658, V 0.689, R 0.708, I 0.584"}

# Потемнение Солнца к краю: I(μ)/I(1) = μ^α(λ), α = −0.023 + 0.292/λ[мкм], 416–1099 нм
SUN_LIMB = {"a0": -0.023, "a1": 0.292, "src": "lit_hestroffer1998",
            "quote": "«α ∼ −0.023 + 0.292 λ^−1 for λ in units of μm and 416 nm ≲ λ ≲ 1099 nm» (u = 1)"}

# Корона (Baumbach 1937): I(r)/I_центра = 10⁻⁶·(c1·r^−p1 + c2·r^−p2 + c3·r^−p3), r — в радиусах Солнца
SUN_CORONA = {"terms": [[2.565, 17.0], [1.425, 7.0], [0.0532, 2.5]], "unit": 1e-6, "src": "lit_baumbach1937",
              "quote": "B_cor(θ) = 18.9[2.565(θ0/θ)^17 + 1.425(θ0/θ)^7 + 5.32×10^-2 (θ0/θ)^2.5] W/(m² sr)"}

# Аэрозоль Земли: среднее по Земле на 550 нм (MACv2), шкала высот — Bruneton & Neyret 2008
EARTH_AEROSOL = {"aod_550": 0.12, "aaod_550": 0.0072, "asymmetry": 0.7, "scale_height_km": 1.2,
                 "src": ["lit_kinne2019", "lit_bruneton2008"],
                 "quote": "MACv2: «the total AOD at 550 nm (of 0.12)», «the total absorption AAOD at 550 nm (of 0.0072)», "
                          "ASY 0.7 (аннотация препринта acp-2018-949); SSA = 1 − AAOD/AOD"}

# Рэлеевское рассеяние воздуха (Bodhaine et al. 1999)
NS_288 = 2.546899e19  # молекул/см³ при 288,15 К и 1013,25 мбар
CO2_PERCENT = 0.03    # стандартный воздух 300 ppm (так в ур. (4))


def air_refractivity(lam_um: float) -> float:
    """n_s − 1 стандартного воздуха (Peck & Reeder 1972; Bodhaine et al. 1999, ур. 4)."""
    s2 = lam_um ** -2
    return (8060.51 + 2480990.0 / (132.274 - s2) + 17455.7 / (39.32957 - s2)) * 1e-8


def air_king_factor(lam_um: float, co2_percent: float = CO2_PERCENT) -> float:
    """F(air) по Bates 1984 (Bodhaine et al. 1999, ур. 5, 6): N₂, O₂, Ar = 1,00, CO₂ = 1,15."""
    s2 = lam_um ** -2
    f_n2 = 1.034 + 3.17e-4 * s2
    f_o2 = 1.096 + 1.385e-3 * s2 + 1.448e-4 * s2 * s2
    return (78.084 * f_n2 + 20.946 * f_o2 + 0.934 * 1.00 + co2_percent * 1.15) / (78.084 + 20.946 + 0.934 + co2_percent)


def air_rayleigh_cross_section_cm2(lam_um: float) -> float:
    """σ = 24π³(n²−1)²/(λ⁴ Ns² (n²+2)²)·F(air), см² на молекулу (Bodhaine et al. 1999, ур. 2)."""
    n = 1.0 + air_refractivity(lam_um)
    lam_cm = lam_um * 1e-4
    return 24 * math.pi ** 3 * (n * n - 1) ** 2 / (lam_cm ** 4 * NS_288 ** 2 * (n * n + 2) ** 2) * air_king_factor(lam_um)


def earth_atmosphere(surface_pressure_bar: float, mean_temp_k: float, scale_height_km: float) -> dict:
    """Параметры однократного рассеяния у поверхности Земли, м⁻¹: Рэлей по каналам RGB и аэрозоль (серый)."""
    k_b = 1.380649e-23
    n0 = surface_pressure_bar * 1e5 / (k_b * mean_temp_k)  # молекул/м³
    beta_r = [air_rayleigh_cross_section_cm2(lam) * 1e-4 * n0 for lam in RGB_UM]
    a = EARTH_AEROSOL
    h_m = a["scale_height_km"] * 1000.0
    ext_m = a["aod_550"] / h_m
    ssa = 1.0 - a["aaod_550"] / a["aod_550"]
    return {
        "rayleigh_beta_m": beta_r, "rayleigh_scale_height_m": scale_height_km * 1000.0,
        "rayleigh_tau": [b * scale_height_km * 1000.0 for b in beta_r],
        "mie_extinction_m": ext_m, "mie_scattering_m": ext_m * ssa, "mie_scale_height_m": h_m,
        "mie_g": a["asymmetry"], "mie_ssa": ssa,
        "number_density_m3": n0,
    }
