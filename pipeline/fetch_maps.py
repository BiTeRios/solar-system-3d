"""Карты поверхностей и рельеф (DEM) для этапа 5 → data/raw/maps/ и data/raw/dem/ (загрузки разрешены 13.10.2026).

Размер каждого файла сверяется со списком ниже (взят из листинга сервера), MD5 — с файлом .md5 рядом, если источник
его публикует (USGS). Уже скачанные целые файлы повторно не качаются.

    python -I pipeline/fetch_maps.py            # всё
    python -I pipeline/fetch_maps.py io pluto   # только названные ключи
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import sys
import time
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernels import ROOT  # noqa: E402

RAW = ROOT / "data" / "raw"
USGS = "https://planetarymaps.usgs.gov/mosaic/"
JPL = "https://maps.jpl.nasa.gov/tmaps/pix/"
PDS = "https://pds-geosciences.wustl.edu/"

# ключ: (папка, URL, размер в байтах, есть ли рядом .md5)
FILES: dict[str, tuple[str, str, int, bool]] = {
    "io": ("maps", USGS + "Io_GalileoSSI-Voyager_Global_Mosaic_ClrMerge_1km.tif", 196637696, True),
    "europa": ("maps", USGS + "Europa_Voyager_GalileoSSI_global_mosaic_500m.tif", 192777263, True),
    "ganymede": ("maps", USGS + "Ganymede_Voyager_GalileoSSI_Global_ClrMosaic_1435m.tif", 199204541, True),
    "callisto": ("maps", USGS + "Callisto_Voyager_GalileoSSI_global_mosaic_1km.tif", 114640717, True),
    "mimas": ("maps", USGS + "Mimas/Cassini_DLR_Mimas.zip", 39663174, False),
    "enceladus": ("maps", USGS + "Enceladus_Cassini_mosaic_global_110m.tif", 103759864, True),
    "tethys": ("maps", USGS + "Tethys_Cassini_mosaic_global_293m.tif", 66401923, True),
    "dione": ("maps", USGS + "Dione_Cassini_Voyager_mosaic_global_154m.tif", 265490551, True),
    "rhea": ("maps", USGS + "Rhea_Cassini_Voyager_mosaic_global_417m.tif", 66401915, True),
    "titan": ("maps", USGS + "Titan_ISS_P19658_Mosaic_Global_4km.tif", 8169529, True),
    "iapetus": ("maps", USGS + "Iapetus_Cassini_Voyager_mosaic_global_783m.tif", 16612487, True),
    "pluto": ("maps", USGS + "Pluto_NewHorizons_Global_Mosaic_300m_Jul2017_8bit.tif", 309806463, True),
    "charon": ("maps", USGS + "Charon_NewHorizons_Global_Mosaic_300m_Jul2017_8bit.tif", 80613890, True),
    "triton": ("maps", USGS + "Triton_Voyager2_ClrMosaic_GlobalFill_600m.tif", 299994887, True),
    "ceres": ("maps", USGS + "Ceres_Dawn_FC_DLR_global_20ppd_Oct2015.tif", 27280830, True),
    "phobos": ("maps", USGS + "Phobos_ME_SRC_Mosaic_Global_16ppd.tif", 16612483, True),
    "deimos": ("maps", JPL + "mar2kuu2.tif", 743096, False),
    "ariel": ("maps", JPL + "ura1vuu2.tif", 261190, False),
    "umbriel": ("maps", JPL + "ura2vuu2.tif", 211300, False),
    "titania": ("maps", JPL + "ura3vuu2.tif", 323574, False),
    "oberon": ("maps", JPL + "ura4vuu2.tif", 327600, False),
    "miranda": ("maps", JPL + "ura5vuu2.tif", 385974, False),
    # Фобос той же серии JPL — только чтобы найти привязку долгот карт JPL по карте USGS (разрешено 13.10.2026)
    "phobos_jpl": ("maps", JPL + "mar1kuu2.tif", 887628, False),
    "dem_moon": ("dem", PDS + "lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/lola_gdr/cylindrical/img/ldem_16.img", 33177600, False),
    "dem_moon_lbl": ("dem", PDS + "lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/lola_gdr/cylindrical/img/ldem_16.lbl", 5121, False),
    "dem_mars": ("dem", PDS + "mgs/mgs-m-mola-5-megdr-l3-v1/mgsl_300x/meg016/megt90n000eb.img", 33177600, False),
    "dem_mars_lbl": ("dem", PDS + "mgs/mgs-m-mola-5-megdr-l3-v1/mgsl_300x/meg016/megt90n000eb.lbl", 4813, False),
    "dem_earth": ("dem", "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/60s_surface_elev_gtif/"
                         "ETOPO_2022_v1_60s_N90W180_surface.tif", 465969062, False),
    "dem_mercury": ("dem", USGS + "Mercury_Messenger_USGS_DEM_Global_665m_v2_32bit_clon0.tif", 418720356, True),
}

HEADERS = {"User-Agent": "SolarSystem3D-pipeline/1.0"}


def md5(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def local_path(key: str) -> Path:
    sub, url, _, _ = FILES[key]
    return RAW / sub / url.rsplit("/", 1)[1]


def fetch(key: str) -> dict:
    sub, url, size, has_md5 = FILES[key]
    path = local_path(key)
    path.parent.mkdir(parents=True, exist_ok=True)
    want_md5 = None
    if has_md5:
        r = requests.get(url + ".md5", timeout=60, headers=HEADERS)
        r.raise_for_status()
        want_md5 = r.text.split()[0].lower()
    if not (path.exists() and path.stat().st_size == size):
        tmp = path.with_suffix(path.suffix + ".part")
        with requests.get(url, stream=True, timeout=120, headers=HEADERS) as r:
            r.raise_for_status()
            with open(tmp, "wb") as f:
                for chunk in r.iter_content(1 << 20):
                    f.write(chunk)
        tmp.replace(path)
    got_size = path.stat().st_size
    got_md5 = md5(path)
    ok = got_size == size and (want_md5 is None or got_md5 == want_md5)
    return {"url": url, "bytes": got_size, "bytes_expected": size, "md5": got_md5, "md5_published": want_md5,
            "ok": ok, "fetched_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}


def main() -> int:
    only = sys.argv[1:]
    unknown = [k for k in only if k not in FILES]
    if unknown:
        print("нет таких ключей:", ", ".join(unknown))
        return 2
    log_path = RAW / "maps_fetch_log.json"
    log = json.loads(log_path.read_text(encoding="utf-8")) if log_path.exists() else {}
    bad = []
    for key in FILES:
        if only and key not in only:
            continue
        t0 = time.time()
        try:
            rec = fetch(key)
        except Exception as ex:  # сеть: запишем и пойдём дальше
            rec = {"url": FILES[key][1], "ok": False, "error": str(ex)}
        log[key] = rec
        log_path.write_text(json.dumps(log, ensure_ascii=False, indent=1), encoding="utf-8")
        mark = "ok" if rec["ok"] else "ОШИБКА"
        print(f"{key:13s} {mark:6s} {rec.get('bytes', 0) / 1e6:8.1f} МБ  {time.time() - t0:6.1f} с  {rec.get('error', '')}",
              flush=True)
        if not rec["ok"]:
            bad.append(key)
    if bad:
        print("не скачались или не сошлись MD5/размер:", ", ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
