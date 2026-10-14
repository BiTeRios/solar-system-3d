"""Копии страниц NASA с физическими параметрами (разрешено 11.10.2026) → data/raw/nasa/.

    python -I pipeline/fetch_nasa.py                     # все страницы
    python -I pipeline/fetch_nasa.py satringfact.html    # только названные (журнал дополняется)

satringfact.html (кольца Сатурна) разрешено скачать на этапе 4, таблицы спутников (*satfact.html) — на этапе 5
(13.10.2026): из них геометрическое альбедо спутников.
"""
from __future__ import annotations

import datetime as dt
import json
import sys
import time
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernels import ROOT  # noqa: E402

OUT = ROOT / "data" / "raw" / "nasa"
FACT = "https://nssdc.gsfc.nasa.gov/planetary/factsheet/"
PAGES = {
    "factsheet_index.html": FACT,
    **{f"{n}fact.html": f"{FACT}{n}fact.html" for n in
       ["sun", "mercury", "venus", "earth", "moon", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"]},
    "satringfact.html": f"{FACT}satringfact.html",
    **{f"{n}satfact.html": f"{FACT}{n}satfact.html" for n in ["jovian", "saturnian", "uranian", "neptunian"]},
    "ssd_sats_phys_par.html": "https://ssd.jpl.nasa.gov/sats/phys_par/",
    "science_eris.html": "https://science.nasa.gov/dwarf-planets/eris/",
    "science_haumea.html": "https://science.nasa.gov/dwarf-planets/haumea/",
    "science_makemake.html": "https://science.nasa.gov/dwarf-planets/makemake/",
}


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    only = sys.argv[1:]
    unknown = [n for n in only if n not in PAGES]
    if unknown:
        print("нет таких страниц:", ", ".join(unknown))
        return 2
    log_path = OUT / "fetch_log.json"
    log = json.loads(log_path.read_text(encoding="utf-8")) if only and log_path.exists() else {}
    for name, url in PAGES.items():
        if only and name not in only:
            continue
        r = requests.get(url, timeout=60, headers={"User-Agent": "SolarSystem3D-pipeline/1.0"})
        status = r.status_code
        if r.ok:
            (OUT / name).write_bytes(r.content)
        log[name] = {"url": url, "status": status, "bytes": len(r.content),
                     "fetched_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}
        print(f"{name:26s} {status} {len(r.content) / 1024:7.1f} КиБ")
        time.sleep(1)
    log_path.write_text(json.dumps(log, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0 if all(v["status"] == 200 for v in log.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
