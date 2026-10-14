"""Начальное состояние Солнечной системы для N-body из .eph, bodies.json, spice_constants.json и dynamics.json.

Планетные системы (Марс … Плутон) — по dynamics.json: GM, Jₙ, опорный радиус и полюс, с которыми JPL строила
спутниковые эфемериды. Земля — J2 из Fact Sheet и полюс pck00011. Остальные GM — gm_de440 (карлики — из статей).
Мелкие спутники, которых нет в модели, добавляются к массе планеты (по тому же спутниковому ядру).
"""
from __future__ import annotations

import json
import math

import numpy as np

from eph_format import EphFile
from export_constants import j2000_to_body
from kernels import ROOT

DATA = ROOT / "game" / "data"


def load_tables():
    bodies = json.loads((DATA / "bodies.json").read_text(encoding="utf-8"))["bodies"]
    consts = json.loads((DATA / "spice_constants.json").read_text(encoding="utf-8"))
    dyn = json.loads((DATA / "dynamics.json").read_text(encoding="utf-8"))
    return bodies, consts, dyn


def _pole_vector(ra_deg: float, dec_deg: float) -> np.ndarray:
    ra, dec = math.radians(ra_deg), math.radians(dec_deg)
    return np.array([math.cos(dec) * math.cos(ra), math.cos(dec) * math.sin(ra), math.sin(dec)])


def system_bodies(eph: EphFile, et: float, keys: set[str] | None = None, zonal: bool = True):
    """Список тел: dict(key, naif, gm, xv, zonal, r_ref, pole). keys — только эти тела (None — все)."""
    bodies, consts, dyn = load_tables()
    by_naif = {}
    for s in dyn["systems"].values():
        for code, gm in s["gm"].items():
            by_naif[int(code)] = gm
    # Если в подмножестве нет ни одного спутника системы, планета ставится в барицентр системы с её полной массой
    # (иначе масса спутников теряется и смещается центр масс).
    chosen = {b["key"] for b in bodies if keys is None or b["key"] in keys}
    whole = {}
    for s in dyn["systems"].values():
        moon_keys = {b["key"] for b in bodies if b["naif"] in s["moons"]}
        if not (moon_keys & chosen):
            whole[s["planet"]] = s
    out = []
    for b in bodies:
        if b["key"] not in chosen:
            continue
        naif = b["naif"]
        gm = by_naif.get(naif, b.get("gm_km3s2", {}).get("v", 0.0))
        if naif in whole:
            sysd = whole[naif]
            p, v = eph.state_ssb(sysd["barycenter"], et)
            gm = sysd["system_gm_in_file"]
            out.append({"key": b["key"], "naif": naif, "gm": gm, "xv": np.concatenate([p[0], v[0]]),
                        "zonal": {}, "r_ref": 0.0, "pole": np.array([0.0, 0.0, 1.0]), "system": True})
            continue
        p, v = eph.state_ssb(naif, et)
        out.append({"key": b["key"], "naif": naif, "gm": gm, "xv": np.concatenate([p[0], v[0]]),
                    "zonal": {}, "r_ref": 0.0, "pole": np.array([0.0, 0.0, 1.0])})
    rec = {r["naif"]: r for r in out}
    if zonal:
        for s in dyn["systems"].values():
            r = rec.get(s["planet"])
            if r is None or not s["zonal"] or r.get("system"):
                continue
            pl = s["pole"]
            t_cy = (et / 86400.0 - (pl["epoch_jd_tdb"] - 2451545.0)) / 36525.0
            r.update(zonal=s["zonal"], r_ref=s["radius_km"],
                     pole=_pole_vector(pl["ra_deg"] + pl["ra_rate_deg_cy"] * t_cy,
                                       pl["dec_deg"] + pl["dec_rate_deg_cy"] * t_cy))
            if s.get("field"):  # полное поле с вращением (Марс)
                r.update(field=s["field"], zonal={})
        if 399 in rec:
            m = j2000_to_body(consts["bodies"]["399"], None, et)
            rec[399].update(zonal=dyn["earth"]["zonal"], r_ref=dyn["earth"]["radius_km"], pole=m[2].copy())
    # мелкие спутники из того же спутникового ядра (Амальтея, Гиперион, Феба, Никта…) — массой на планету.
    # Полную GM системы до DE440 не подгоняем: у plu060 и ura184 она своя, и подгонка портит спутники.
    for s in dyn["systems"].values():
        r = rec.get(s["planet"])
        if r is not None and not r.get("system"):
            # масса спутников, которых нет в подмножестве, тоже идёт на планету
            missing = sum(gm for code, gm in s["gm"].items() if int(code) in s["moons"] and int(code) not in rec)
            r["gm"] += s["unmodeled_gm"] + missing
            # Перенос массы на планету теряет орбитальную скорость мелких спутников: у Протея это 2,9 мм/с центра масс
            # системы Нептуна, ~90 км в год. Сдвигаем всю смоделированную систему (планету и спутники) на один вектор,
            # чтобы её центр масс совпал с барицентром из эфемерид. Взаимные орбиты внутри системы не меняются.
            members = [r] + [rec[int(c)] for c in s["moons"] if int(c) in rec]
            pb, vb = eph.state_ssb(s["barycenter"], et)
            total = sum(m["gm"] for m in members)
            com = sum(m["gm"] * m["xv"] for m in members) / total
            shift = np.concatenate([pb[0], vb[0]]) - com
            for m in members:
                m["xv"] = m["xv"] + shift
    return out


def build_sim(sim, bodies, hierarchical: bool = True):
    """Спутники (код NNN, кроме NN99; Луна 301) — относительно своей планеты."""
    index = {r["naif"]: i for i, r in enumerate(bodies)}
    for r in bodies:
        naif = r["naif"]
        parent = -1
        if hierarchical and 100 < naif < 1000 and naif % 100 != 99:
            parent = index.get((naif // 100) * 100 + 99, -1)
        i = sim.add(r["gm"], r["xv"], 0.0, r["r_ref"], r["pole"], parent)
        sim.set_zonal(i, r["zonal"])
        if r.get("field"):
            sim.set_field(i, r["field"], (r["field"]["rotation"]["epoch_jd_tdb"] - 2451545.0) * 86400.0)
    return sim
