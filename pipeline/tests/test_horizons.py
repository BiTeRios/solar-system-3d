"""Режим «Эфемериды» против JPL Horizons (фикстура от 11.10.2026, pipeline/fetch_horizons_vectors.py).

Пороги: планеты и Луна — 1 км (ТЗ); спутники — 1 км; малые тела — 10 км: Horizons считает их векторы
новым интегрированием по решению орбиты, а не из того же SPK, что мы скачали (расхождение — километры на 40 а.е.).
Таблица фактических расхождений — build/horizons_report.json и docs/accuracy.md.
"""
import json

import numpy as np
import pytest

from eph_format import EphFile
from kernels import ROOT

FIX = ROOT / "pipeline" / "tests" / "fixtures" / "horizons_vectors.json"
EPH = ROOT / "game" / "data" / "ephemeris.eph"
REPORT = ROOT / "build" / "horizons_report.json"

PLANETS = {"sun", "mercury", "venus", "earth", "moon", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"}
SMALL = {"ceres", "eris", "haumea", "makemake", "halley"}
LIMIT_KM = {"planet": 1.0, "control": 1.0, "moon_sat": 1.0, "small": 10.0}
# Барицентры Урана и Нептуна берём из DE442 на всём диапазоне. Horizons берёт DE442 только внутри покрытия
# ura184 (1600–2400) и nep098 (1800–2200), а на 1600 и 2600 годах — DE441: там расхождение — десятки тысяч км.
# ТЗ требует точности на современные даты, поэтому эти две точки только записываются в отчёт.
REPORT_ONLY = {"control_7", "control_8"}


@pytest.fixture(scope="module")
def data():
    return EphFile(EPH), json.loads(FIX.read_text(encoding="utf-8"))


def _kind(key):
    if key.startswith("control_"):
        return "control"
    if key in PLANETS:
        return "planet"
    if key in SMALL:
        return "small"
    return "moon_sat"


def test_ephemeris_vs_horizons(data):
    eph, fx = data
    report, failures = {}, []
    for key, b in fx["bodies"].items():
        naif = b["naif"]
        target = naif * 100 + 99 if naif in (1, 2) else naif  # Horizons отдаёт для «1», «2» Меркурий и Венеру
        et = (np.array(b["jd_tdb"]) - 2451545.0) * 86400.0
        ref = np.array(b["states"])
        p, v = eph.state_ssb(target, et)
        dr = np.linalg.norm(p - ref[:, :3], axis=1)
        dv = np.linalg.norm(v - ref[:, 3:], axis=1)
        kind = _kind(key)
        report[key] = {"kind": kind, "max_km": float(dr.max()), "median_km": float(np.median(dr)),
                       "max_mm_s": float(dv.max() * 1e6), "horizons_sources": b["sources"]}
        if key not in REPORT_ONLY and dr.max() > LIMIT_KM[kind]:
            failures.append(f"{key}: {dr.max():.3f} км > {LIMIT_KM[kind]} км")
    REPORT.parent.mkdir(exist_ok=True)
    REPORT.write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    for key, r in report.items():
        print(f"{key:14s} {r['max_km']:10.4f} км")
    assert not failures, "\n".join(failures)
