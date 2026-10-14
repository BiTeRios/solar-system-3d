"""Время UTC ↔ TDB по алгоритму SPICE: таблица високосных секунд и формула TDB−TT из naif0012.tls.

Это эталон для C++ (native/src/time). Календарь — пролептический григорианский, как по умолчанию в SPICE.
Секунды UTC «формальные»: сутки по 86400 с, отсчёт от 2000-01-01 12:00:00.

    python -I pipeline/timeconv.py            # пишет game/data/time.json
"""
from __future__ import annotations

import bisect
import datetime as dt
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernels import RAW, ROOT, md5  # noqa: E402

J2000_DAYS_FROM_1970 = 10957  # 2000-01-01 от 1970-01-01


def days_from_civil(y: int, m: int, d: int) -> int:
    """Дни от 1970-01-01, пролептический григорианский календарь (алгоритм Хиннанта)."""
    y -= m <= 2
    era = y // 400  # деление в Python округляет вниз, поправка Хиннанта для C++ здесь не нужна
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def civil_from_days(z: int) -> tuple[int, int, int]:
    z += 719468
    era = z // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + (3 if mp < 10 else -9)
    return y + (m <= 2), m, d


@dataclass
class TimeModel:
    delta_t_a: float           # TT − TAI, 32.184 с
    k: float                   # амплитуда TDB − TT, с
    eb: float                  # эксцентриситет орбиты Земли-Луны
    m0: float                  # средняя аномалия на J2000, рад
    m1: float                  # её скорость, рад/с
    leap_epochs: list[float]   # формальные секунды UTC от J2000 начала суток, с которых действует ΔAT
    leap_values: list[float]   # ΔAT = TAI − UTC, с

    @classmethod
    def from_json(cls, path: Path) -> "TimeModel":
        j = json.loads(Path(path).read_text(encoding="utf-8"))
        return cls(j["DELTA_T_A"], j["K"], j["EB"], j["M"][0], j["M"][1],
                   [e["utc_formal_s"] for e in j["leap_seconds"]],
                   [e["delta_at"] for e in j["leap_seconds"]])

    def delta_at(self, utc_formal: float) -> float:
        """ΔAT на момент UTC. До первой записи (1972-01-01) SPICE берёт первое значение минус 1 с (9 с)."""
        i = bisect.bisect_right(self.leap_epochs, utc_formal) - 1
        return self.leap_values[i] if i >= 0 else self.leap_values[0] - 1.0

    def _tdb_minus_tt(self, t: float) -> float:
        m = self.m0 + self.m1 * t
        return self.k * math.sin(m + self.eb * math.sin(m))

    def utc_to_tdb(self, y: int, mo: int, d: int, h: int = 0, mi: int = 0, s: float = 0.0) -> float:
        """Календарная дата UTC → секунды TDB от J2000. Секунды могут быть ≥ 60 внутри високосной секунды."""
        day0 = (days_from_civil(y, mo, d) - J2000_DAYS_FROM_1970) * 86400.0 - 43200.0
        tai = day0 + self.delta_at(day0) + (h * 3600 + mi * 60) + s
        tt = tai + self.delta_t_a
        return tt + self._tdb_minus_tt(tt)

    def tdb_to_utc(self, et: float) -> tuple[int, int, int, int, int, float]:
        """Секунды TDB от J2000 → (год, месяц, день, час, минута, секунды). Високосная секунда — 23:59:60.x."""
        tai = et - self._tdb_minus_tt(et) - self.delta_t_a
        day = math.floor((tai - self.delta_at(tai) + 43200.0) / 86400.0)
        for _ in range(4):
            day0 = day * 86400.0 - 43200.0
            sod = tai - (day0 + self.delta_at(day0))
            if sod < 0.0:
                day -= 1
                continue
            day_len = 86400.0 + self.delta_at(day0 + 86400.0) - self.delta_at(day0)
            if sod >= day_len:
                day += 1
                continue
            break
        y, mo, d = civil_from_days(day + J2000_DAYS_FROM_1970)
        if sod >= 86400.0:  # внутри високосной секунды
            return y, mo, d, 23, 59, sod - 86340.0
        h = int(sod // 3600)
        mi = int((sod - h * 3600) // 60)
        return y, mo, d, h, mi, sod - h * 3600 - mi * 60

    def delta_et_utc(self, et: float) -> float:
        """ET − UTC для эпохи ET (аналог deltet_c(..., "ET"))."""
        tt = et - self._tdb_minus_tt(et)
        tai = tt - self.delta_t_a
        dat = self.leap_values[0] - 1.0
        for e, v in zip(self.leap_epochs, self.leap_values):
            if tai >= e + v:
                dat = v
        return self.delta_t_a + dat + self._tdb_minus_tt(et)


def export(out: Path) -> dict:
    import spiceypy as sp
    sp.kclear()
    lsk = RAW / "naif0012.tls"
    sp.furnsh(str(lsk))
    raw = sp.gdpool("DELTET/DELTA_AT", 0, 1000)
    leaps = []
    for v, e in zip(raw[0::2], raw[1::2]):
        day = round((e + 43200.0) / 86400.0)
        y, m, d = civil_from_days(day + J2000_DAYS_FROM_1970)
        leaps.append({"utc": f"{y:04d}-{m:02d}-{d:02d}", "utc_formal_s": float(e), "delta_at": float(v)})
    j = {
        "source": {"file": "naif0012.tls", "md5": md5(lsk),
                   "url": "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/lsk/naif0012.tls"},
        "created_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "calendar": "пролептический григорианский (по умолчанию в SPICE)",
        "DELTA_T_A": float(sp.gdpool("DELTET/DELTA_T_A", 0, 1)[0]),
        "K": float(sp.gdpool("DELTET/K", 0, 1)[0]),
        "EB": float(sp.gdpool("DELTET/EB", 0, 1)[0]),
        "M": [float(x) for x in sp.gdpool("DELTET/M", 0, 2)],
        "leap_seconds": leaps,
    }
    sp.kclear()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(j, ensure_ascii=False, indent=1), encoding="utf-8")
    return j


if __name__ == "__main__":
    target = ROOT / "game" / "data" / "time.json"
    data = export(target)
    print(f"записано {target}: {len(data['leap_seconds'])} записей ΔAT, последняя {data['leap_seconds'][-1]['utc']}")
