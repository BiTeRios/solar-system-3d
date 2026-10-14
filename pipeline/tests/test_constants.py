"""Константы и ориентация тел против SPICE (gdpool, bodvrd, pxform)."""
import json

import numpy as np
import pytest
import spiceypy as sp

from bodies import BODIES
from export_constants import OUT, export, j2000_to_body, system_of
from kernels import RAW

N_EPOCHS = 2000
# Предел double для угла W: у Фобоса к 2650 году W ≈ 4,5·10⁸°, шаг double ≈ 6·10⁻⁸° ≈ 10⁻⁹ рад.
# 3·10⁻⁹ рад — это 2 см на поверхности Земли.
ANGLE_TOL = 3e-9  # рад


@pytest.fixture(scope="module")
def consts():
    export()
    sp.kclear()
    for k in ("naif0012.tls", "pck00011.tpc", "gm_de440.tpc"):
        sp.furnsh(str(RAW / k))
    yield json.loads(OUT.read_text(encoding="utf-8"))
    sp.kclear()


def test_gm_and_radii_exact(consts):
    for naif, b in consts["bodies"].items():
        if "gm" in b:
            assert b["gm"] == sp.bodvrd(naif, "GM", 1)[1][0]
        if "radii" in b:
            assert b["radii"] == list(sp.bodvrd(naif, "RADII", 3)[1])
    for bary, s in consts["systems"].items():
        assert s["gm"] == sp.bodvrd(bary, "GM", 1)[1][0]


def test_all_mvp_bodies_present(consts):
    """Каждое тело MVP в списке. Без констант SPICE остаются только Эрида, Хаумеа, Макемаке и Галлей."""
    no_spice = {n for _, n, *_ in BODIES if "gm" not in consts["bodies"][str(n)]}
    assert no_spice == {2136199, 2136108, 2136472, 1000036}


def test_orientation_matches_pxform(consts):
    rng = np.random.default_rng(11)
    lo, hi = sp.str2et("1550-01-01"), sp.str2et("2650-01-01")
    ets = lo + rng.random(N_EPOCHS) * (hi - lo)
    report = []
    for naif, b in consts["bodies"].items():
        if "pole_ra" not in b:
            continue
        frame = sp.cidfrm(int(naif))[1]
        sysk = system_of(int(naif))
        system = consts["systems"].get(str(sysk)) if sysk else None
        worst = 0.0
        for et in ets:
            m = j2000_to_body(b, system, float(et))
            ref = np.array(sp.pxform("J2000", frame, float(et)))
            # угол поворота между матрицами
            c = (np.trace(m @ ref.T) - 1.0) / 2.0
            ang = float(np.arccos(np.clip(c, -1.0, 1.0)))
            if ang < 1e-7:  # arccos теряет точность у нуля — считаем по антисимметричной части
                d = m @ ref.T
                ang = float(np.linalg.norm([d[2, 1] - d[1, 2], d[0, 2] - d[2, 0], d[1, 0] - d[0, 1]]) / 2)
            worst = max(worst, ang)
        report.append((frame, worst))
        assert worst < ANGLE_TOL, f"{frame}: {worst:.3e} рад"
    for frame, w in report:
        print(f"{frame:14s} max угол {w:.2e} рад")
