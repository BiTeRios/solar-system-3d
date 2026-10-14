"""Константы тел из pck00011.tpc и gm_de440.tpc → game/data/spice_constants.json.

Плюс эталонная ориентация (как BODEUL/TISBOD в SPICE) для тестов и для C++.

    python -I pipeline/export_constants.py
"""
from __future__ import annotations

import datetime as dt
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bodies import BARYCENTERS, BODIES, PERTURBERS  # noqa: E402
from kernels import RAW, ROOT, md5  # noqa: E402

OUT = ROOT / "game" / "data" / "spice_constants.json"
ORIENT_VARS = ["POLE_RA", "POLE_DEC", "PM", "NUT_PREC_RA", "NUT_PREC_DEC", "NUT_PREC_PM", "RADII", "GM"]


def _get(sp, name):
    if not sp.expool(name):
        return None
    return [float(x) for x in sp.gdpool(name, 0, 1000)]


def system_of(naif: int) -> int | None:
    """Барицентр, чьи NUT_PREC_ANGLES использует тело (как в SPICE: 501 → 5, 399 → 3)."""
    if 100 < naif < 1000:
        return naif // 100
    return None


def export() -> dict:
    import spiceypy as sp
    sp.kclear()
    pck, gm = RAW / "pck00011.tpc", RAW / "gm_de440.tpc"
    sp.furnsh(str(pck))
    sp.furnsh(str(gm))
    bodies = {}
    for key, naif, *_ in BODIES + [(k, n, *_r) for k, n, *_r in PERTURBERS]:
        rec = {"key": key}
        for v in ORIENT_VARS:
            val = _get(sp, f"BODY{naif}_{v}")
            if val is not None:
                rec[v.lower()] = val[0] if v == "GM" else val
        rec["src"] = {"gm": "gm_de440.tpc" if "gm" in rec else None,
                      "radii": "pck00011.tpc" if "radii" in rec else None,
                      "orientation": "pck00011.tpc" if "pole_ra" in rec else None}
        bodies[str(naif)] = rec
    systems = {}
    for b in BARYCENTERS:
        angles = _get(sp, f"BODY{b}_NUT_PREC_ANGLES")
        deg = _get(sp, f"BODY{b}_MAX_PHASE_DEGREE")
        systems[str(b)] = {"gm": _get(sp, f"BODY{b}_GM")[0],
                           "max_phase_degree": int(deg[0]) if deg else 1,
                           "nut_prec_angles": angles or []}
    sp.kclear()
    data = {
        "sources": [
            {"file": "pck00011.tpc", "md5": md5(pck), "url": "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/pck00011.tpc"},
            {"file": "gm_de440.tpc", "md5": md5(gm), "url": "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/gm_de440.tpc"},
        ],
        "created_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "units": {"gm": "km^3/s^2", "radii": "km", "pole_ra/pole_dec": "deg, deg/век, deg/век^2",
                  "pm": "deg, deg/сутки, deg/сутки^2", "nut_prec_angles": "deg, deg/век (по max_phase_degree)"},
        "bodies": bodies,
        "systems": systems,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
    return data


# ---------- ориентация (эталон для C++) ----------

def euler_angles(body: dict, system: dict | None, et: float) -> tuple[float, float, float]:
    """RA, DEC, W в радианах для эпохи et (с TDB от J2000), как BODEUL в SPICE."""
    d = et / 86400.0
    t = d / 36525.0
    def poly(c, x):  # схема Горнера
        acc = 0.0
        for ci in reversed(c):
            acc = acc * x + ci
        return acc
    ra = poly(body["pole_ra"], t)
    dec = poly(body["pole_dec"], t)
    w = poly(body["pm"], d)
    if system and system["nut_prec_angles"] and "nut_prec_ra" in body:
        n = system["max_phase_degree"] + 1
        ang = system["nut_prec_angles"]
        thetas = [math.radians(poly(ang[k:k + n], t)) for k in range(0, len(ang), n)]
        for i, th in enumerate(thetas):
            if i < len(body.get("nut_prec_ra", [])):
                ra += body["nut_prec_ra"][i] * math.sin(th)
            if i < len(body.get("nut_prec_dec", [])):
                dec += body["nut_prec_dec"][i] * math.cos(th)
            if i < len(body.get("nut_prec_pm", [])):
                w += body["nut_prec_pm"][i] * math.sin(th)
    w = math.fmod(w, 360.0)
    return math.radians(ra), math.radians(dec), math.radians(w)


def rot3(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, s, 0.0], [-s, c, 0.0], [0.0, 0.0, 1.0]])


def rot1(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[1.0, 0.0, 0.0], [0.0, c, s], [0.0, -s, c]])


def j2000_to_body(body: dict, system: dict | None, et: float) -> np.ndarray:
    """Матрица J2000 → тело: [W]_3 · [π/2 − δ]_1 · [π/2 + α]_3."""
    ra, dec, w = euler_angles(body, system, et)
    return rot3(w) @ rot1(math.pi / 2 - dec) @ rot3(math.pi / 2 + ra)


if __name__ == "__main__":
    data = export()
    n_orient = sum(1 for b in data["bodies"].values() if "pole_ra" in b)
    print(f"записано {OUT}: тел {len(data['bodies'])}, с моделью вращения {n_orient}")
