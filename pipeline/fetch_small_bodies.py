"""Загрузка SPK малых тел из JPL Horizons и физических параметров из SBDB (разрешено 11.10.2026).

Файлы: data/raw/horizons/<SPK-ID>.bsp, <SPK-ID>_horizons.txt (заголовок ответа), sbdb_<ключ>.json.

    python -I pipeline/fetch_small_bodies.py [--spk-only]
"""
from __future__ import annotations

import base64
import json
import sys
import time
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernels import ROOT  # noqa: E402

OUT = ROOT / "data" / "raw" / "horizons"
HORIZONS = "https://ssd.jpl.nasa.gov/api/horizons.api"
SBDB = "https://ssd-api.jpl.nasa.gov/sbdb.api"
# Horizons строит SPK малых тел только внутри 1600–2500 (руководство Horizons, раздел SPK)
START, STOP = "1600-01-01", "2500-01-01"

# ключ, команда Horizons, строка поиска SBDB
TARGETS = [
    ("ceres", "DES=2000001;", "1"),
    ("eris", "DES=2136199;", "136199"),
    ("haumea", "DES=2136108;", "136108"),
    ("makemake", "DES=2136472;", "136472"),
    ("halley", "DES=1P;CAP;NOFRAG;", "1P"),
    ("pallas", "DES=2000002;", "2"),
    ("vesta", "DES=2000004;", "4"),
    ("hygiea", "DES=2000010;", "10"),
]


def fetch_spk(key: str, command: str) -> dict:
    params = {"format": "json", "EPHEM_TYPE": "SPK", "OBJ_DATA": "YES", "COMMAND": f"'{command}'",
              "START_TIME": f"'{START}'", "STOP_TIME": f"'{STOP}'"}
    r = requests.get(HORIZONS, params=params, timeout=300)
    r.raise_for_status()
    j = r.json()
    if "spk" not in j:
        raise RuntimeError(f"{key}: нет SPK в ответе:\n{j.get('result', j)[:2000]}")
    spk_id = j["spk_file_id"]
    data = base64.b64decode(j["spk"])
    (OUT / f"{spk_id}.bsp").write_bytes(data)
    (OUT / f"{spk_id}_horizons.txt").write_text(j.get("result", ""), encoding="utf-8")
    return {"key": key, "spk_id": spk_id, "bytes": len(data), "signature": j.get("signature")}


def fetch_sbdb(key: str, sstr: str) -> int:
    params = {"sstr": sstr, "full-prec": "true", "phys-par": "true"}
    r = requests.get(SBDB, params=params, timeout=120)
    r.raise_for_status()
    (OUT / f"sbdb_{key}.json").write_text(json.dumps(r.json(), ensure_ascii=False, indent=1), encoding="utf-8")
    return len(r.content)


def main(spk_only: bool = "--spk-only" in sys.argv) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    report = []
    for key, command, sstr in TARGETS:
        try:
            info = fetch_spk(key, command)
            print(f"{key:9s} SPK {info['spk_id']}: {info['bytes'] / 1024:.0f} КиБ")
            report.append(info)
        except Exception as e:  # noqa: BLE001 — показать все ошибки разом
            print(f"{key:9s} SPK ОШИБКА: {e}")
            report.append({"key": key, "error": str(e)})
        time.sleep(2)
        if not spk_only:
            n = fetch_sbdb(key, sstr)
            print(f"{key:9s} SBDB: {n} байт")
            time.sleep(1)
    (OUT / "fetch_report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0 if all("error" not in r for r in report) else 1


if __name__ == "__main__":
    sys.exit(main())
