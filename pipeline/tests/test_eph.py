"""Сверка .eph со SPICE: каждое тело на 10 000 случайных эпохах, плюс границы сегментов и записей.

Сегменты копируются побитно, поэтому расхождение — только округление double при вычислении
полинома. Схема Кленшоу повторяет CHBINT из SPICE, допуск — 1 мм по положению и 1 нм/с по скорости.
"""
import numpy as np
import pytest
import spiceypy as sp

from eph_format import EphFile
from kernels import RAW, ROOT, SPK_KERNELS

EPH = ROOT / "game" / "data" / "ephemeris.eph"
N_RANDOM = 10_000
POS_ABS_KM = 1e-6          # 1 мм
POS_REL = 0.0           # схема Кленшоу как в SPICE: относительный допуск не нужен
VEL_ABS_KMS = 1e-12        # 1 нм/с
VEL_REL = 0.0


@pytest.fixture(scope="module")
def eph():
    if not EPH.exists():
        pytest.skip(f"нет {EPH}: запустите pipeline/export_eph.py")
    return EphFile(EPH)


def _spice_states(target, center, ets):
    out = np.empty((ets.size, 6))
    for i, et in enumerate(ets):
        out[i], _ = sp.spkgeo(target, float(et), "J2000", center)
    return out


def _epochs(eph, target, rng):
    """Случайные эпохи внутри покрытия плюс края сегментов и границы записей."""
    cov = eph.coverage(target)
    lens = np.array([b - a for a, b in cov])
    pick = rng.choice(len(cov), size=N_RANDOM, p=lens / lens.sum())
    a = np.array([c[0] for c in cov])[pick]
    b = np.array([c[1] for c in cov])[pick]
    ets = a + rng.random(N_RANDOM) * (b - a)
    edges = []
    for row in eph.segments_for(target):
        s, e, init, il = float(row["start_et"]), float(row["end_et"]), float(row["init"]), float(row["intlen"])
        edges += [s, e]
        k = rng.integers(1, max(2, int(row["n_records"])), size=20)
        edges += list(np.clip(init + k * il, s, e))
    return np.concatenate([ets, edges])


def _check(ours_p, ours_v, ref, label):
    dp = np.linalg.norm(ours_p - ref[:, :3], axis=1)
    dv = np.linalg.norm(ours_v - ref[:, 3:], axis=1)
    rp = np.linalg.norm(ref[:, :3], axis=1)
    rv = np.linalg.norm(ref[:, 3:], axis=1)
    tol_p = np.maximum(POS_ABS_KM, POS_REL * rp)
    tol_v = np.maximum(VEL_ABS_KMS, VEL_REL * rv)
    worst = int(np.argmax(dp / tol_p))
    assert (dp <= tol_p).all(), f"{label}: |Δr| = {dp[worst]:.3e} км при |r| = {rp[worst]:.3e} км"
    assert (dv <= tol_v).all(), f"{label}: |Δv| = {dv.max():.3e} км/с"
    return dp.max(), dv.max()


@pytest.mark.parametrize("kernel", list(SPK_KERNELS))
def test_segments_match_spice(eph, kernel):
    """Цель относительно центра, как в сегменте. В SPICE загружено только это ядро."""
    sp.kclear()
    sp.furnsh(str(RAW / "naif0012.tls"))
    sp.furnsh(str(RAW / kernel))
    rng = np.random.default_rng(abs(hash(kernel)) % 2**32)
    try:
        for target, center in SPK_KERNELS[kernel][2]:
            ets = _epochs(eph, target, rng)
            p, v = eph.state_rel(target, ets)
            dp, dv = _check(p, v, _spice_states(target, center, ets), f"{kernel}: {target} от {center}")
            print(f"{kernel:18s} {target:4d} от {center}: max|Δr| {dp * 1e6:8.3f} мм, max|Δv| {dv * 1e9:8.4f} нм/с")
    finally:
        sp.kclear()


def test_ssb_chain_matches_spice(eph):
    """Цепочка до барицентра. Тела берутся из разных ядер (Уран и Нептун — DE442, остальные планеты — DE440),
    а SPICE выбирает сегмент по порядку загрузки файлов, поэтому эталон собирается по звеньям:
    каждое звено считается с одним своим ядром."""
    rng = np.random.default_rng(7)
    sp.kclear()
    sp.furnsh(str(RAW / "naif0012.tls"))
    lo = sp.str2et("1900-01-01")
    hi = sp.str2et("2100-01-01")
    ets = lo + rng.random(2000) * (hi - lo)
    links = {}
    try:
        for kernel, (_, _, pairs) in SPK_KERNELS.items():
            sp.kclear()
            sp.furnsh(str(RAW / "naif0012.tls"))
            sp.furnsh(str(RAW / kernel))
            for target, center in pairs:
                links[target] = _spice_states(target, center, ets)
    finally:
        sp.kclear()
    for target in sorted(t for t in eph.center_of if t < 1000):  # малые тела — отдельный тест
        ref = links[target].copy()
        c = eph.center_of[target]
        while c != 0:
            ref += links[c]
            c = eph.center_of[c]
        p, v = eph.state_ssb(target, ets)
        _check(p, v, ref, f"{target} от SSB")


def test_out_of_range_raises(eph):
    """Вне покрытия — исключение, а не экстраполяция."""
    with pytest.raises(ValueError):
        eph.state_rel(501, -3.2e9)  # 1898 год, до начала диапазона спутников


def test_header(eph):
    h = eph.header
    assert h["frame"].startswith("ICRF")
    from refit_small_bodies import SMALL_BODIES
    assert {s["file"] for s in h["sources"]} == set(SPK_KERNELS) | {f"{i}.bsp" for *_, i in SMALL_BODIES}
    assert all(len(s.get("md5", "")) == 32 for s in h["sources"])


SMALL_POS_KM = 1e-3        # переподгонка: 1 м
SMALL_VEL_KMS = 1e-6       # 1 мм/с


def test_small_bodies_match_horizons_spk(eph):
    """Малые тела: .eph (переподгонка) против исходных SPK Horizons, центр — барицентр, Солнце из DE440."""
    from refit_small_bodies import SMALL_BODIES, furnsh_all
    sp.kclear()
    sp.furnsh(str(RAW / "naif0012.tls"))
    furnsh_all()
    rng = np.random.default_rng(21)
    try:
        for key, naif, spk_id in SMALL_BODIES:
            ets = _epochs(eph, naif, rng)
            p, v = eph.state_rel(naif, ets)
            ref = _spice_states(spk_id, 0, ets)
            dp = np.linalg.norm(p - ref[:, :3], axis=1)
            dv = np.linalg.norm(v - ref[:, 3:], axis=1)
            # точки ровно на разрезах (скачки исходника) пропускаем: там SPICE и .eph берут разные стороны
            cuts = np.array([float(r["start_et"]) for r in eph.segments_for(naif)])
            far = np.min(np.abs(ets[:, None] - cuts[None, :]), axis=1) > 1e-3
            print(f"{key:9s}: max|Δr| {dp[far].max() * 1e3:7.3f} м, max|Δv| {dv[far].max() * 1e6:7.3f} мм/с")
            assert dp[far].max() <= SMALL_POS_KM, f"{key}: {dp[far].max() * 1e3:.3f} м"
            assert dv[far].max() <= SMALL_VEL_KMS, f"{key}: {dv[far].max() * 1e6:.3f} мм/с"
    finally:
        sp.kclear()
