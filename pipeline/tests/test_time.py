"""UTC ↔ TDB против SPICE (str2et, et2utc, deltet).

Допуск — 1 мкс или 4 ulp(ET), что больше: у double на 2650 году шаг ET около 4 мкс.
"""
import math

import numpy as np
import pytest
import spiceypy as sp

from kernels import RAW, ROOT
from timeconv import TimeModel, civil_from_days, days_from_civil, export

TIME_JSON = ROOT / "game" / "data" / "time.json"


@pytest.fixture(scope="module")
def tm():
    export(TIME_JSON)
    sp.kclear()
    sp.furnsh(str(RAW / "naif0012.tls"))
    yield TimeModel.from_json(TIME_JSON)
    sp.kclear()


def tol(et):
    return max(1e-6, 4 * math.ulp(abs(et)))


def test_calendar_roundtrip():
    for z in range(-800_000, 800_000, 997):
        assert days_from_civil(*civil_from_days(z)) == z
    assert days_from_civil(2000, 1, 1) == 10957
    # 400 лет григорианского календаря — ровно 146097 суток, в том числе для лет до нашей эры
    for y in range(-5000, 5000, 37):
        assert days_from_civil(y + 400, 3, 1) - days_from_civil(y, 3, 1) == 146097
        leap = y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)
        assert civil_from_days(days_from_civil(y, 2, 28) + 1) == ((y, 2, 29) if leap else (y, 3, 1))
    assert days_from_civil(0, 3, 1) - days_from_civil(-1, 3, 1) == 366  # год 0 високосный
    assert days_from_civil(1582, 10, 15) - days_from_civil(1582, 10, 4) == 11  # без юлианского разрыва


def test_leap_table_matches_kernel(tm):
    raw = sp.gdpool("DELTET/DELTA_AT", 0, 1000)
    assert tm.leap_values == list(raw[0::2])
    assert tm.leap_epochs == list(raw[1::2])
    assert tm.leap_values[-1] == 37.0  # последняя високосная секунда — 2016-12-31


def test_utc_to_tdb_random(tm):
    rng = np.random.default_rng(1)
    worst = 0.0
    for _ in range(20_000):
        y = int(rng.integers(1550, 2651))
        mo = int(rng.integers(1, 13))
        d = int(rng.integers(1, 29))
        h, mi = int(rng.integers(0, 24)), int(rng.integers(0, 60))
        s = float(rng.integers(0, 60_000_000)) / 1e6
        ours = tm.utc_to_tdb(y, mo, d, h, mi, s)
        ref = sp.str2et(f"{y:04d}-{mo:02d}-{d:02d}T{h:02d}:{mi:02d}:{s:09.6f}")
        assert abs(ours - ref) <= tol(ref), f"{y}-{mo}-{d} {h}:{mi}:{s}: {ours - ref:.3e} с"
        worst = max(worst, abs(ours - ref))
    print(f"UTC→TDB: max |Δ| = {worst * 1e6:.3f} мкс")


def test_tdb_to_utc_random(tm):
    rng = np.random.default_rng(2)
    lo, hi = sp.str2et("1550-01-01"), sp.str2et("2650-01-01")
    for et in lo + rng.random(20_000) * (hi - lo):
        y, mo, d, h, mi, s = tm.tdb_to_utc(et)
        back = tm.utc_to_tdb(y, mo, d, h, mi, s)
        assert abs(back - et) <= tol(et)
        ref = sp.str2et(sp.et2utc(et, "ISOC", 6))  # строка SPICE округлена до 1 мкс
        assert abs(tm.utc_to_tdb(y, mo, d, h, mi, s) - ref) <= 1e-6 + tol(et)


def test_leap_second_boundaries(tm):
    for day in [e for e in tm.leap_epochs[1:]]:
        y, mo, d = civil_from_days(round((day + 43200) / 86400) - 1 + 10957)  # последний день перед скачком
        nxt = civil_from_days(round((day + 43200) / 86400) + 10957)
        cases = [((y, mo, d, 23, 59, 59.5), f"{y:04d}-{mo:02d}-{d:02d}T23:59:59.5"),
                 ((y, mo, d, 23, 59, 60.0), f"{y:04d}-{mo:02d}-{d:02d}T23:59:60.0"),
                 ((y, mo, d, 23, 59, 60.5), f"{y:04d}-{mo:02d}-{d:02d}T23:59:60.5"),
                 ((*nxt, 0, 0, 0.0), f"{nxt[0]:04d}-{nxt[1]:02d}-{nxt[2]:02d}T00:00:00.0"),
                 ((*nxt, 0, 0, 0.5), f"{nxt[0]:04d}-{nxt[1]:02d}-{nxt[2]:02d}T00:00:00.5")]
        for args, text in cases:
            ref = sp.str2et(text)
            assert abs(tm.utc_to_tdb(*args) - ref) <= tol(ref), text
            # обратно: внутри скачка — 23:59:60.x
            yy, mm, dd, h, mi, s = tm.tdb_to_utc(ref)
            assert (yy, mm, dd, h, mi) == args[:5] and abs(s - args[5]) < 1e-6, (text, (yy, mm, dd, h, mi, s))
            assert sp.et2utc(ref, "ISOC", 1).replace("T", " ")[11:19] == f"{h:02d}:{mi:02d}:{int(s):02d}"


def test_deltet(tm):
    rng = np.random.default_rng(3)
    lo, hi = sp.str2et("1960-01-01"), sp.str2et("2100-01-01")
    for et in lo + rng.random(5_000) * (hi - lo):
        assert abs(tm.delta_et_utc(et) - sp.deltet(et, "ET")) < 1e-9
