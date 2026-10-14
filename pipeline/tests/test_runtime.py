"""Этап 4: каталог и начальное состояние на C++ (для игры) и «Симуляция» в потоке с интерполяцией Эрмита.

Пороги заданы до запуска:
  - начальное состояние C++ совпадает с pipeline/tests/solar_system.py до бита (положения, скорости, GM), полюса — 1e-15;
  - интерполяция между снимками потока отличается от прямого интегрирования не больше чем на 1 м;
  - на каждом уровне упрощения планеты через год — не дальше 100 км от эфемерид (допуск «Симуляции»);
  - смена уровня не даёт скачка: положения по обе стороны смены совпадают с точностью 1 м;
  - счёт назад во времени возвращает эфемериды с той же точностью, что и вперёд.
"""
import json
import time

import numpy as np
import pytest

from eph_format import EphFile
from kernels import ROOT
from native import DLL, Native, NativeWorld, Sim
from solar_system import build_sim, system_bodies

EPH = ROOT / "game" / "data" / "ephemeris.eph"
DATA = ROOT / "game" / "data"
ET0 = 8.135e8
DAY = 86400.0
YEAR = 365.25 * DAY
PLANETS = ["mercury", "venus", "earth", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"]
REPORT = ROOT / "build" / "runtime_report.json"


def _save(key, value):
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    data = json.loads(REPORT.read_text(encoding="utf-8")) if REPORT.exists() else {}
    data[key] = value
    REPORT.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")


@pytest.fixture(scope="module")
def nat():
    if not DLL.exists():
        pytest.skip("нет native/bin/spacecore_c.dll")
    return Native()


@pytest.fixture(scope="module")
def eph():
    return EphFile(EPH)


@pytest.fixture()
def world(nat):
    w = NativeWorld(nat, DATA)
    yield w
    w.stop()


def _keys(eph):
    return [b["key"] for b in system_bodies(eph, ET0)]


@pytest.mark.parametrize("et", [ET0, -2.9e9, 3.1e9, 1.234567e8])
def test_initial_state_matches_python(world, eph, et):
    cs = world.initial_state(et)
    ps = system_bodies(eph, et)
    assert [c["naif"] for c in cs] == [p["naif"] for p in ps]
    index = {r["naif"]: i for i, r in enumerate(ps)}
    for c, p in zip(cs, ps):
        assert np.array_equal(c["xv"], p["xv"]), p["key"]
        assert c["gm"] == p["gm"], p["key"]
        assert c["r_ref"] == p["r_ref"], p["key"]
        assert np.abs(c["pole"] - p["pole"]).max() <= 1e-15, p["key"]
        naif = p["naif"]
        parent = index.get((naif // 100) * 100 + 99, -1) if 100 < naif < 1000 and naif % 100 != 99 else -1
        assert c["parent"] == parent, p["key"]
        if p.get("field"):
            assert c["nzonal"] == -1, p["key"]  # полное поле вместо зональных
        elif p["zonal"]:
            jn = np.zeros(max(int(k) for k in p["zonal"]) + 1)
            for k, v in p["zonal"].items():
                jn[int(k)] = v
            assert np.array_equal(c["zonal"], jn), p["key"]


def test_initial_state_subset(world, eph):
    """Без спутников Юпитера: Юпитер ставится в барицентр системы с её полной GM, как в Python."""
    keys = _keys(eph)
    drop = {"io", "europa", "ganymede", "callisto"}
    cs = world.initial_state(ET0, [k not in drop for k in keys])
    ps = system_bodies(eph, ET0, {k for k in keys if k not in drop})
    assert len(cs) == len(ps)
    for c, p in zip(cs, ps):
        assert np.array_equal(c["xv"], p["xv"]) and c["gm"] == p["gm"], p["key"]


def test_runner_interpolation_vs_direct(world, nat, eph):
    world.seed(ET0, 0, 0)
    span = 5 * DAY
    wall = world.wait_until(ET0 + span, 3 * DAY)
    rng = np.random.default_rng(41)
    ts = np.sort(ET0 + rng.random(80) * span)
    s = build_sim(Sim(nat, t0=ET0, dt=600.0), system_bodies(eph, ET0))
    keys = _keys(eph)
    err = np.zeros(len(keys))
    for t in ts:
        s.integrate(float(t))
        r = world.state(float(t))
        assert r is not None, t
        err = np.maximum(err, np.linalg.norm(r[:, :3] - s.states()[:, :3], axis=1))
    _save("interpolation_vs_direct_m", {k: float(e * 1000) for k, e in zip(keys, err)} | {"wall_s_5d": wall})
    assert err.max() < 1e-3, dict(zip(keys, err))  # км


def test_levels_accuracy_and_speed(world, eph):
    keys = _keys(eph)
    res = {}
    for level, span in ((0, 30 * DAY), (1, 30 * DAY), (2, YEAR)):
        world.seed(ET0, level, level)
        t = ET0 + span
        t0 = time.perf_counter()
        world.wait_until(t, span)
        wall = time.perf_counter() - t0
        st = world.state(t)
        errs = {}
        for i, naif in enumerate(world.naif):
            p, _ = eph.state_ssb(naif, t)
            errs[keys[i]] = float(np.linalg.norm(st[i, :3] - p[0]))
        res[level] = {"span_days": span / DAY, "wall_s": wall, "days_per_s": span / DAY / wall,
                      "planets_max_km": max(errs[k] for k in PLANETS), "errors_km": errs}
        world.stop()
        assert res[level]["planets_max_km"] < 100.0, (level, {k: errs[k] for k in PLANETS})
    _save("levels", res)
    # уровни действительно ускоряют счёт
    assert res[1]["days_per_s"] > 2 * res[0]["days_per_s"]
    assert res[2]["days_per_s"] > 10 * res[1]["days_per_s"]


def test_level_switch_is_continuous(world, eph):
    """Поток сам меняет уровень по скорости времени: 1 сут/с → 30 сут/с → 2 года/с → обратно 1 сут/с."""
    world.seed(ET0, 0, 2)
    t = ET0
    seen = []
    for rate, secs in ((DAY, 2.0), (30 * DAY, 2.0), (2 * YEAR, 2.0), (DAY, 3.0)):
        t_end = t + rate * secs
        world.wait_until(t_end, rate)
        time.sleep(0.3)  # запрос обрабатывает поток: дать ему проснуться и выбрать уровень
        seen.append(world.level())
        t = t_end
    assert seen == [0, 2, 2, 0], seen  # после замедления поток вернулся к полной модели сразу
    # непрерывность: по обе стороны каждой смены уровня (±1 мс) положения сходятся с точностью до v·2 мс
    sw = world.switches()
    assert sw, "смен уровня в буфере нет"
    keys = _keys(eph)
    planets = [keys.index(k) for k in PLANETS]
    worst_planet = worst_moon = 0.0
    for ts in sw:
        a, b = world.state(ts - 1e-3), world.state(ts + 1e-3)
        jump = np.linalg.norm(b[:, :3] - a[:, :3] - (a[:, 3:] + b[:, 3:]) / 2 * 2e-3, axis=1)
        worst_planet = max(worst_planet, float(jump[planets].max()))
        worst_moon = max(worst_moon, float(jump.max()))
    _save("level_switch", {"levels_seen": seen, "switches": len(sw), "max_jump_planet_km": worst_planet,
                           "max_jump_any_km": worst_moon})
    assert worst_planet < 1e-3 and worst_moon < 1e-3, (worst_planet, worst_moon)


def test_backward_integration(world, eph):
    world.seed(ET0, 0, 0)
    t = ET0 - 10 * DAY
    world.wait_until(t, -3 * DAY)
    st = world.state(t)
    keys = _keys(eph)
    errs = {}
    for i, naif in enumerate(world.naif):
        p, _ = eph.state_ssb(naif, t)
        errs[keys[i]] = float(np.linalg.norm(st[i, :3] - p[0]))
    _save("backward_10d_km", errs)
    assert max(errs[k] for k in PLANETS) < 1.0
