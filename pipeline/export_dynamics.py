"""Модель динамики для «Симуляции» → game/data/dynamics.json.

Спутниковые эфемериды JPL построены со своими GM, опорным радиусом, зональными гармониками J2…J10 и полюсом
планеты (файлы комментариев *.cmt NAIF, скачаны 11.10.2026). Чтобы N-body совпадал с эфемеридами, берём именно их.
GM в gm_de440.tpc для спутников Урана и Плутона взяты из более старых решений и с ядрами ura184 и plu060 не совпадают.

Не моделируются (есть в *.cmt): форма самих спутников (S*J2, C22), приливы (K2, LAG), массы колец Сатурна,
периодические члены полюса (RA_AMPL…), сезонные члены поля Марса (J400P1, J403C1/S1).
Поле Марса берётся целиком: J2…J10 и тессеральные Cₙₘ, Sₙₘ до степени 6, с вращением планеты.

    python -I pipeline/export_dynamics.py
"""
from __future__ import annotations

import datetime as dt
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernels import RAW, ROOT, md5  # noqa: E402
from parse_nasa import fact_sheet  # noqa: E402

OUT = ROOT / "game" / "data" / "dynamics.json"

# система → (файл, барицентр, код планеты, коды спутников модели)
SYSTEMS = {
    "mars": ("mar099.cmt", 4, 499, [401, 402]),
    "jupiter": ("jup365.cmt", 5, 599, [501, 502, 503, 504]),
    "saturn": ("sat441.cmt", 6, 699, [601, 602, 603, 604, 605, 606, 608]),
    "uranus": ("ura184_part-3.cmt", 7, 799, [701, 702, 703, 704, 705]),
    "neptune": ("nep098_part-1.cmt", 8, 899, [801]),
    "pluto": ("plu060.cmt", 9, 999, [901]),
}
NUM = r"[-+]?\d+\.\d+E[-+]\d+"


def parse_blocks(text: str) -> list[dict]:
    """Блоки «Bodies on the File … Planet gravitational harmonics are unnormalized»."""
    blocks = []
    for m in re.finditer(r"Bodies on the File:(.*?)Planet gravitational harmonics are unnormalized", text, re.S):
        body = m.group(1)
        gms = {int(n): float(v) for n, v in re.findall(rf"^\s+\S+\s+(\d{{3}})\s+({NUM})", body, re.M)}
        consts = {k: float(v) for k, v in re.findall(rf"\b([A-Z0-9_]+)\s+({NUM})", body.split("Additional Constants")[1])}
        system = re.search(rf"System\s+({NUM})", body)
        blocks.append({"gm": gms, "const": consts, "system_gm": float(system.group(1)) if system else None})
    return blocks


def system_model(name: str) -> dict:
    fname, bary, planet, moons = SYSTEMS[name]
    path = RAW / fname
    blocks = [b for b in parse_blocks(path.read_text(encoding="latin-1"))
              if planet in b["gm"] and all(m in b["gm"] for m in moons)]
    if not blocks:
        raise RuntimeError(f"{fname}: нет блока с {planet} и {moons}")
    ref = blocks[0]
    for b in blocks[1:]:  # блоки одного файла должны совпадать в том, что мы берём
        for k in [planet, *moons]:
            if b["gm"][k] != ref["gm"][k]:
                raise RuntimeError(f"{fname}: блоки расходятся по GM {k}")
    c = ref["const"]
    zonal = {}
    for n in range(2, 11):
        key = f"J{bary}{n:02d}"
        if key in c and c[key] != 0.0:
            zonal[str(n)] = c[key]
    # тессеральные гармоники (сейчас есть только у Марса): C<бар><nn><mm>, S<бар><nn><mm>
    tess_c = {(int(k[2:4]), int(k[4:6])): v for k, v in c.items() if re.fullmatch(rf"C{bary}\d{{4}}", k)}
    tess_s = {(int(k[2:4]), int(k[4:6])): v for k, v in c.items() if re.fullmatch(rf"S{bary}\d{{4}}", k)}
    field = None
    if tess_c:
        nmax = max([int(n) for n in zonal] + [n for n, _ in tess_c])
        field = {
            "nmax": nmax,
            "radius_km": c["RADIUS"],
            "C": {**{f"{n},0": -v for n, v in ((int(k), v) for k, v in zonal.items())},
                  **{f"{n},{m}": v for (n, m), v in tess_c.items()}},
            "S": {f"{n},{m}": v for (n, m), v in tess_s.items()},
            "rotation": {"epoch_jd_tdb": c["POLTIM"], "ra_deg": c[f"ZACPL{bary}"], "dec_deg": c[f"ZDEPL{bary}"],
                         "ra_rate_deg_cy": c.get(f"DACPL{bary}", 0.0), "dec_rate_deg_cy": c.get(f"DDEPL{bary}", 0.0),
                         "w_deg": c[f"WPL{bary}"], "w_rate_deg_day": c[f"WDP{bary}"]},
        }
    return {
        "field": field,  # полное поле с вращением тела; если есть — вместо zonal
        "source": {"file": fname, "md5": md5(path),
                   "url": f"https://naif.jpl.nasa.gov/pub/naif/generic_kernels/spk/satellites/{fname}"},
        "barycenter": bary,
        "planet": planet,
        "moons": moons,
        "gm": {str(k): ref["gm"][k] for k in [planet, *moons]},
        "system_gm_in_file": ref["system_gm"],
        # мелкие спутники из того же файла, которых нет в модели: их масса прибавляется к планете
        "unmodeled_gm": ref["system_gm"] - sum(ref["gm"][k] for k in [planet, *moons]),
        "radius_km": c["RADIUS"],
        "zonal": zonal,  # J_n, ненормированные
        "pole": {  # полюс планеты: RA, DEC на POLTIM и скорости, град и град/век
            "epoch_jd_tdb": c["POLTIM"],
            "ra_deg": c[f"ZACPL{bary}"], "dec_deg": c[f"ZDEPL{bary}"],
            "ra_rate_deg_cy": c.get(f"DACPL{bary}", 0.0), "dec_rate_deg_cy": c.get(f"DDEPL{bary}", 0.0),
        },
    }


def export() -> dict:
    systems = {name: system_model(name) for name in SYSTEMS}
    earth = fact_sheet("earth")
    data = {
        "created_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "note": "модель динамики для N-body; GM, J_n и полюса согласованы со спутниковыми ядрами JPL",
        "systems": systems,
        "earth": {"zonal": {"2": earth["j2_1e6"]["v"] * 1e-6}, "radius_km": 6378.137,
                  "source": "NASA Earth Fact Sheet: J2 1082.63e-6, экваториальный радиус 6378.137 км",
                  "pole": "pck00011"},
        "not_modeled": ["форма спутников (J2, C22)", "приливы (K2, LAG)", "кольца Сатурна (RINGM)",
                        "периодические члены полюсов (RA_AMPL…)", "сезонные члены поля Марса",
                        "343 астероида DE440 (только Церера, Паллада, Веста, Гигея)", "фигура Луны и либрации"],
    }
    OUT.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
    return data


if __name__ == "__main__":
    d = export()
    for k, s in d["systems"].items():
        print(f"{k:8s} R={s['radius_km']:8.1f} км  J_n: {', '.join(f'J{n}={v:.4g}' for n, v in s['zonal'].items())}")
