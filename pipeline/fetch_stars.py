"""Звёзды и Млечный Путь для этапа 7 → data/raw/stars/ (загрузки разрешены 14.10.2026).

Размер и MD5 каждого файла пишутся в data/raw/stars_fetch_log.json. Уже скачанные файлы повторно не качаются.

    python -I pipeline/fetch_stars.py            # всё
    python -I pipeline/fetch_stars.py hip_main   # только названные ключи
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import sys
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernels import ROOT  # noqa: E402

RAW = ROOT / "data" / "raw" / "stars"
CDS = "https://cdsarc.cds.unistra.fr/ftp/I/239/"

# ключ: (имя файла, URL)
FILES: dict[str, tuple[str, str]] = {
    "hip_readme": ("hip_ReadMe.txt", CDS + "ReadMe"),
    "hip_main": ("hip_main.dat", CDS + "hip_main.dat"),  # сжатого .gz на сервере нет
    "svs_page": ("svs_4851.html", "https://svs.gsfc.nasa.gov/4851/"),
    "milkyway": ("milkyway_2020_8k.exr", "https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/milkyway_2020_8k.exr"),
}


def fetch(key: str) -> dict:
    name, url = FILES[key]
    path = RAW / name
    if not path.exists():
        r = requests.get(url, timeout=120, headers={"User-Agent": "SolarSystem-pipeline/1.0"})
        r.raise_for_status()
        RAW.mkdir(parents=True, exist_ok=True)
        path.write_bytes(r.content)
    data = path.read_bytes()
    return {"file": name, "url": url, "bytes": len(data), "md5": hashlib.md5(data).hexdigest(),
            "accessed_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M")}


def main() -> int:
    keys = sys.argv[1:] or list(FILES)
    log_path = ROOT / "data" / "raw" / "stars_fetch_log.json"
    log = json.loads(log_path.read_text(encoding="utf-8")) if log_path.exists() else {}
    for key in keys:
        rec = fetch(key)
        print(key, rec["bytes"], rec["md5"])
        log[key] = rec
        log_path.write_text(json.dumps(log, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
