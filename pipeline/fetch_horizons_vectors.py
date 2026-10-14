"""Векторы JPL Horizons для сверки режима «Эфемериды» (разрешено 11.10.2026).

Сохраняет ответы в pipeline/tests/fixtures/horizons/ и разобранные состояния в pipeline/tests/fixtures/horizons_vectors.json.
Система — ICRF относительно барицентра Солнечной системы (500@0), км и км/с, время TDB.

    python -I pipeline/fetch_horizons_vectors.py
"""
from __future__ import annotations

import datetime as dt
import json
import re
import sys
import time
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bodies import BODIES  # noqa: E402

FIX = Path(__file__).resolve().parent / "tests" / "fixtures"
URL = "https://ssd.jpl.nasa.gov/api/horizons.api"
JD_1900, JD_2100 = 2415020.5, 2488069.5

# 50 дат в 1900–2100, со сдвигом от полуночи, чтобы не попадать на границы записей Чебышёва
MODERN = [round(JD_1900 + 0.123456 + k * (JD_2100 - JD_1900 - 1.0) / 49.0, 6) for k in range(50)]
# контрольные даты для DE440 вне диапазона спутников: 1600 и 2600 годы
CONTROL = [2305447.5 + 0.25, 2670690.5 + 0.25]

HORIZONS_CMD = {"ceres": "DES=2000001;", "eris": "DES=2136199;", "haumea": "DES=2136108;",
                "makemake": "DES=2136472;", "halley": "DES=1P;CAP;NOFRAG;"}


def query(command: str, jds: list[float]) -> str:
    params = {"format": "json", "COMMAND": f"'{command}'", "OBJ_DATA": "NO", "MAKE_EPHEM": "YES",
              "EPHEM_TYPE": "VECTORS", "CENTER": "'500@0'", "REF_PLANE": "FRAME", "REF_SYSTEM": "ICRF",
              "OUT_UNITS": "KM-S", "VEC_TABLE": "2", "VEC_CORR": "NONE", "CSV_FORMAT": "YES",
              "TIME_TYPE": "TDB", "TLIST_TYPE": "JD", "TLIST": " ".join(f"'{j:.6f}'" for j in jds)}
    r = requests.get(URL, params=params, timeout=300)
    r.raise_for_status()
    return r.json()["result"]


def parse(text: str) -> tuple[list[str], list[list[float]], list[float]]:
    src = [ln.strip() for ln in text.splitlines() if "{source:" in ln]
    block = text.split("$$SOE")[1].split("$$EOE")[0]
    states, jds = [], []
    for ln in block.strip().splitlines():
        parts = [p.strip() for p in ln.split(",") if p.strip()]
        jds.append(float(parts[0]))
        states.append([float(x) for x in parts[2:8]])
    return src, states, jds


def main() -> int:
    (FIX / "horizons").mkdir(parents=True, exist_ok=True)
    jobs = [(key, str(naif) if naif < 1000 else HORIZONS_CMD[key], naif, MODERN) for key, naif, *_ in BODIES]
    jobs += [(f"control_{c}", c, int(c), CONTROL) for c in ["10", "1", "2", "3", "4", "5", "6", "7", "8", "9", "399", "301"]]
    out = {"fetched_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
           "url": URL, "center": "500@0 (SSB)", "frame": "ICRF", "units": "km, km/s", "time": "TDB (JD)", "bodies": {}}
    for key, command, naif, jds in jobs:
        text = query(command, jds)
        (FIX / "horizons" / f"{key}.txt").write_text(text, encoding="utf-8")
        src, states, got_jds = parse(text)
        if [round(j, 6) for j in got_jds] != [round(j, 6) for j in jds]:
            raise RuntimeError(f"{key}: эпохи в ответе не совпадают с запросом")
        out["bodies"][key] = {"naif": naif, "command": command, "sources": src, "jd_tdb": got_jds, "states": states}
        print(f"{key:14s} {len(states)} эпох; {'; '.join(re.sub(r'\\s+', ' ', s) for s in src)[:110]}")
        time.sleep(1)
    (FIX / "horizons_vectors.json").write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
