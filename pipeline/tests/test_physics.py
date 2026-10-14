"""Физика N-body (C++ через C-API): сохранение, сравнение с DE440 и с REBOUND.

Пороги сохранения заданы до запуска (docs/architecture.md): за 100 лет |ΔE/E| ≤ 1e-10, |ΔL|/|L| ≤ 1e-11.
Долгий тест помечен slow: pytest -m slow.
"""
import json
import time

import numpy as np
import pytest

from eph_format import EphFile
from kernels import ROOT
from native import DLL, Native, Sim
from solar_system import build_sim, system_bodies

EPH = ROOT / "game" / "data" / "ephemeris.eph"
YEAR = 365.25 * 86400.0
ET0 = 8.1350e8  # 2025-10-12 около полудня TDB: внутри всех диапазонов данных
REPORT = ROOT / "build" / "physics_report.json"


@pytest.fixture(scope="module")
def nat():
    if not DLL.exists():
        pytest.skip("нет native/bin/spacecore_c.dll")
    return Native()


@pytest.fixture(scope="module")
def eph():
    return EphFile(EPH)


def _save(key, value):
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    data = json.loads(REPORT.read_text(encoding="utf-8")) if REPORT.exists() else {}
    data[key] = value
    REPORT.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")


def _conservation(nat, eph, years):
    bs = system_bodies(eph, ET0)
    s = build_sim(Sim(nat, gr=False, j2=False, t0=ET0, dt=600.0), bs)
    e0, l0, p0 = s.energy(), s.angmom(), s.momentum()
    t = time.perf_counter()
    s.integrate(ET0 + years * YEAR)
    wall = time.perf_counter() - t
    de = abs((s.energy() - e0) / e0)
    dl = np.linalg.norm(s.angmom() - l0) / np.linalg.norm(l0)
    dp = np.linalg.norm(s.momentum() - p0)
    st = s.stats()
    res = {"years": years, "bodies": len(bs), "dE/E": de, "dL/L": dl, "dP_km_s_GM": dp,
           "steps": st["steps"], "rejected": st["rejected"], "not_converged": st["not_converged"], "wall_s": wall}
    print(res)
    return res


def test_conservation_10yr(nat, eph):
    """Быстрый вариант: 10 лет, все 39 тел, Ньютон. Пороги — те же, что за 100 лет."""
    r = _conservation(nat, eph, 10)
    _save("conservation_10yr", r)
    assert r["dE/E"] <= 1e-10
    assert r["dL/L"] <= 1e-11


@pytest.mark.slow
def test_conservation_100yr(nat, eph):
    r = _conservation(nat, eph, 100)
    _save("conservation_100yr", r)
    assert r["dE/E"] <= 1e-10
    assert r["dL/L"] <= 1e-11


SPANS = {"1 сутки": 86400.0, "1 месяц": 30 * 86400.0, "1 год": YEAR}
PLANETS = ("sun", "mercury", "venus", "earth", "moon", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto")
PLANET_LIMIT_KM = 100.0  # допуск для планет с 11.10.2026 (docs/architecture.md)


def _sim_table(nat, eph, spans):
    bs = system_bodies(eph, ET0)
    s = build_sim(Sim(nat, gr=True, j2=True, t0=ET0, dt=600.0), bs)
    table = {}
    for label, span in spans.items():
        s.integrate(ET0 + span)
        st = s.states()
        table[label] = {r["key"]: float(np.linalg.norm(x[:3] - eph.state_ssb(r["naif"], ET0 + span)[0][0]))
                        for r, x in zip(bs, st)}
    return table


def test_simulation_vs_de440(nat, eph):
    """«Симуляция» (Ньютон + ОТО + поля планет по dynamics.json) из состояния эфемерид против эфемерид.
    Планеты и Луна через год — в пределах 100 км. Спутники — таблица в build/physics_report.json."""
    table = _sim_table(nat, eph, SPANS)
    _save("sim_vs_eph_km", table)
    for key in ("earth", "moon", "mars", "jupiter", "neptune", "io", "mimas", "phobos", "triton"):
        print(key, {k: f"{v[key]:.3g}" for k, v in table.items()})
    worst = max(PLANETS, key=lambda k: table["1 год"][k])
    assert table["1 год"][worst] < PLANET_LIMIT_KM, f"{worst}: {table['1 год'][worst]:.1f} км за год"
    # грубая проверка спутников на поломку (сутки)
    assert max(table["1 сутки"][k] for k in table["1 сутки"] if k not in PLANETS) < 5.0


@pytest.mark.slow
def test_simulation_vs_de440_10yr(nat, eph):
    table = _sim_table(nat, eph, {"10 лет": 10 * YEAR})
    _save("sim_vs_eph_km_10yr", table)


def test_gr_reduces_error(nat, eph):
    """Без ОТО Меркурий за год уходит от эфемерид дальше, чем с ОТО."""
    keys = {"sun", "mercury", "venus", "earth", "moon", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"}
    err = {}
    for gr in (False, True):
        s = build_sim(Sim(nat, gr=gr, j2=True, t0=ET0, dt=600.0), system_bodies(eph, ET0, keys))
        s.integrate(ET0 + YEAR)
        i = [r["key"] for r in system_bodies(eph, ET0, keys)].index("mercury")
        p, _ = eph.state_ssb(199, ET0 + YEAR)
        err[gr] = float(np.linalg.norm(s.states()[i, :3] - p[0]))
    print("Меркурий через год, км: без ОТО", err[False], "с ОТО", err[True])
    assert err[True] < err[False] / 3


def test_jupiter_doubled_vs_rebound(nat, eph):
    """Удвоенная масса Юпитера, 100 лет, Ньютон: наш IAS15 против IAS15 из REBOUND (только в тестах, GPL-3)."""
    rebound = pytest.importorskip("rebound")
    bary = [10, 1, 2, 3, 4, 5, 6, 7, 8, 9]
    consts = json.loads((ROOT / "game" / "data" / "spice_constants.json").read_text(encoding="utf-8"))
    gms = [consts["bodies"]["10"]["gm"]] + [consts["systems"][str(b)]["gm"] for b in bary[1:]]
    states = []
    for b in bary:
        p, v = eph.state_ssb(b, ET0)
        states.append(np.concatenate([p[0], v[0]]))
    t_end = 100 * YEAR

    def run_ours(factor):
        s = Sim(nat, gr=False, j2=False, t0=0.0, dt=86400.0)
        for i, (gm, xv) in enumerate(zip(gms, states)):
            s.add(gm * (factor if bary[i] == 5 else 1.0), xv)
        s.integrate(t_end)
        return s.states()

    def run_rebound(factor):
        sim = rebound.Simulation()
        sim.G = 1.0  # массы — это GM, единицы км и с
        sim.integrator = "ias15"
        for i, (gm, xv) in enumerate(zip(gms, states)):
            sim.add(m=gm * (factor if bary[i] == 5 else 1.0), x=xv[0], y=xv[1], z=xv[2], vx=xv[3], vy=xv[4], vz=xv[5])
        sim.dt = 86400.0
        sim.integrate(t_end, exact_finish_time=1)
        return np.array([[p.x, p.y, p.z, p.vx, p.vy, p.vz] for p in sim.particles])

    ours1, ours2 = run_ours(1.0), run_ours(2.0)
    reb1, reb2 = run_rebound(1.0), run_rebound(2.0)
    sat = bary.index(6)
    effect = np.linalg.norm(ours2[sat, :3] - ours1[sat, :3])
    diff = np.linalg.norm(ours2[sat, :3] - reb2[sat, :3])
    worst = max(np.linalg.norm(ours2[i, :3] - reb2[i, :3]) / np.linalg.norm(reb2[i, :3]) for i in range(len(bary)))
    _save("rebound_jupiter_doubled", {"saturn_effect_km": effect, "saturn_ours_vs_rebound_km": diff,
                                      "max_relative_difference": worst,
                                      "rebound_version": rebound.__version__})
    print(f"Сатурн: эффект удвоения {effect:.4g} км, наш против REBOUND {diff:.3g} км, max отн. {worst:.2e}")
    assert effect > 1e6          # удвоение Юпитера заметно сдвигает Сатурн
    assert diff < 1e-6 * effect  # реализации совпадают на уровне 10⁻⁶ эффекта
    assert worst < 1e-9
    # и без удвоения
    assert max(np.linalg.norm(ours1[i, :3] - reb1[i, :3]) / np.linalg.norm(reb1[i, :3]) for i in range(len(bary))) < 1e-9
