"""Сборка game/data/bodies.json и game/data/sources.json.

Каждое число хранится как {"v": значение, "src": ключ источника}; ключи описаны в sources.json.
Порядок источников: ядра SPICE → таблицы NASA/JPL → SBDB → статьи (literature.py) → science.nasa.gov (приблизительно).
Если значения нет ни в одном источнике, поле не заполняется и тело попадает в список "missing".

    python -I pipeline/build_bodies.py
"""
from __future__ import annotations

import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bodies import BODIES, PERTURBERS  # noqa: E402
from kernels import ROOT, verify_all  # noqa: E402
from literature import LITERATURE, SOURCES as LIT_SOURCES  # noqa: E402
from parse_nasa import (fact_sheet, satellite_albedos, saturn_rings, science_dwarf, sources as nasa_sources,  # noqa: E402
                        ssd_satellites)

DATA = ROOT / "game" / "data"
HZ = ROOT / "data" / "raw" / "horizons"
FACT_PAGES = {"sun", "mercury", "venus", "earth", "moon", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"}
SBDB_KEYS = {"ceres", "eris", "haumea", "makemake", "halley", "pallas", "vesta", "hygiea"}


def sbdb(key: str) -> dict:
    j = json.loads((HZ / f"sbdb_{key}.json").read_text(encoding="utf-8"))
    out = {}
    for p in j.get("phys_par", []):
        out[p["name"]] = {"v": p.get("value"), "units": p.get("units"), "ref": p.get("ref"), "src": f"sbdb_{key}"}
    return out


def build() -> tuple[dict, dict]:
    spice = json.loads((DATA / "spice_constants.json").read_text(encoding="utf-8"))
    ssd = ssd_satellites()
    sat_alb = satellite_albedos()
    srcs = {
        "gm_de440": {"title": "NAIF gm_de440.tpc", "url": spice["sources"][1]["url"], "md5": spice["sources"][1]["md5"]},
        "pck00011": {"title": "NAIF pck00011.tpc (IAU WGCCRE)", "url": spice["sources"][0]["url"], "md5": spice["sources"][0]["md5"]},
        **nasa_sources(),
        **LIT_SOURCES,
    }
    for key in SBDB_KEYS:
        srcs[f"sbdb_{key}"] = {"title": "JPL SBDB API", "url": f"https://ssd-api.jpl.nasa.gov/sbdb.api (sstr, phys-par), {key}"}
    for k, info in verify_all().items():
        srcs[f"naif_{k}"] = {"title": f"NAIF {k}", "url": info["url"], "md5": info["md5"]}

    bodies, missing = [], {}
    all_bodies = [(*b, True) for b in BODIES] + [(k, n, ru, en, "sun", "perturber", False) for k, n, ru, en in PERTURBERS]
    for key, naif, name, name_en, parent, kind, visible in all_bodies:
        sc = spice["bodies"].get(str(naif), {})
        rec: dict = {"key": key, "naif": naif, "name": name, "name_en": name_en, "parent": parent,
                     "kind": kind, "visible": visible}
        if "gm" in sc:
            rec["gm_km3s2"] = {"v": sc["gm"], "src": "gm_de440"}
        if "radii" in sc:
            rec["radii_km"] = {"v": sc["radii"], "src": "pck00011"}
        rec["orientation"] = "pck00011" if "pole_ra" in sc else None
        phys: dict = {}
        if key in FACT_PAGES:
            phys.update(fact_sheet(key))
        if naif in ssd:
            phys.setdefault("radius_mean_km", ssd[naif]["radius_mean_km"])
            phys.setdefault("density_gcm3", ssd[naif]["density_gcm3"])
        if kind == "moon" and name_en.lower() in sat_alb:
            phys.setdefault("geometric_albedo", sat_alb[name_en.lower()])
        if key in SBDB_KEYS:
            sb = sbdb(key)
            if "diameter" in sb:
                phys.setdefault("radius_mean_km", {"v": float(sb["diameter"]["v"]) / 2, "src": sb["diameter"]["src"],
                                                    "raw": f"diameter {sb['diameter']['v']} km ({sb['diameter']['ref']})"})
            if "albedo" in sb:
                phys.setdefault("geometric_albedo", {"v": float(sb["albedo"]["v"]), "src": sb["albedo"]["src"],
                                                      "raw": sb["albedo"]["ref"]})
            if "rot_per" in sb:
                phys.setdefault("rotation_h", {"v": float(sb["rot_per"]["v"]), "src": sb["rot_per"]["src"],
                                                "raw": sb["rot_per"]["ref"]})
            if "extent" in sb:
                phys["extent_km"] = {"v": sb["extent"]["v"], "src": sb["extent"]["src"], "raw": sb["extent"]["ref"]}
            if "density" in sb:
                phys.setdefault("density_gcm3", {"v": float(sb["density"]["v"]), "src": sb["density"]["src"]})
        if key in ("eris", "haumea", "makemake"):
            for k2, v2 in science_dwarf(key).items():
                phys.setdefault(k2, v2)
        # значения из статей (решение 11.10.2026) важнее приблизительных со страниц NASA
        for field, val in LITERATURE.get(key, {}).items():
            if field in ("gm_km3s2", "radii_km"):
                rec[field] = val
            else:
                phys[field] = val
        rec["phys"] = phys
        if key == "saturn":
            rec["rings"] = saturn_rings()
        # SPK-источник положений
        if naif < 1000:
            rec["ephemeris"] = {"eph_target": naif}
        else:
            rec["ephemeris"] = {"eph_target": naif, "note": "переподгонка SPK Horizons, 1600–2500"}
        gaps = [f for f in ("gm_km3s2",) if f not in rec] + \
               [f for f in ("radius_mean_km",) if f not in phys and "radii_km" not in rec]
        if visible and kind != "star":
            gaps += [f for f in ("bond_albedo", "geometric_albedo") if f not in phys]
        if gaps:
            missing[key] = gaps
        bodies.append(rec)
    out = {"created_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
           "note": "значение = {v, src}; src — ключ в sources.json",
           "bodies": bodies, "missing": missing}
    return out, srcs


if __name__ == "__main__":
    bodies, srcs = build()
    (DATA / "bodies.json").write_text(json.dumps(bodies, ensure_ascii=False, indent=1), encoding="utf-8")
    (DATA / "sources.json").write_text(json.dumps(srcs, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"записано bodies.json: {len(bodies['bodies'])} тел; sources.json: {len(srcs)} источников")
    print("пропуски:")
    for k, v in bodies["missing"].items():
        print(f"  {k:10s} {', '.join(v)}")
