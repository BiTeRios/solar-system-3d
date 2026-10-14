"""Звёзды и Млечный Путь (этап 7) → game/data/stars.bin и game/assets/sky/.

Звёзды — каталог Hipparcos (ESA 1997, CDS I/239, hip_main.dat): направление ICRS на эпоху J1991.25, собственное
движение, блеск V, цвет. Цвет: B−V → температура по Ballesteros 2012 (EPL 97, 34008), затем закон Планка на длинах
волн R, G, B (те же, что на этапе 5) с балансом белого по Солнцу (5772 K — белый). Звёзды без B−V — белые.

stars.bin (little-endian): "STAR", версия u32, число звёзд u32, эпоха каталога f64 (юлианский год),
затем на звезду: HIP u32, направление f32×3, собственное движение f32×3 (рад/юл. год, производная направления),
V f32, цвет f32×3 (G = 1 у белой звезды).

Млечный Путь — NASA SVS 4851 «Deep Star Maps 2020», milkyway_2020_8k.exr: фон без звёзд Hipparcos и Tycho,
ICRF, plate carrée, в центре α = 0h, α растёт влево; линейный HDR. Копируется как есть, Godot сжимает его в BC6H.

    python -I -X utf8 pipeline/build_stars.py
"""
from __future__ import annotations

import json
import shutil
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from appearance_params import RGB_UM  # noqa: E402
from kernels import ROOT  # noqa: E402

RAW = ROOT / "data" / "raw" / "stars"
OUT = ROOT / "game" / "data" / "stars.bin"
SKY = ROOT / "game" / "assets" / "sky"
EPOCH = 1991.25
MAS = np.pi / (180 * 3600 * 1000)
T_SUN = 5772.0  # K, NASA Sun Fact Sheet (эффективная температура)


def read_hipparcos(path: Path = RAW / "hip_main.dat") -> dict[str, np.ndarray]:
    """Столбцы по ReadMe I/239 (байты с 1, включительно). Звёзды без координат или V пропускаются."""
    cols = {"hip": (9, 14), "vmag": (42, 46), "ra": (52, 63), "de": (65, 76), "pmra": (88, 95), "pmde": (97, 104),
            "bv": (246, 251)}
    rows = {k: [] for k in cols}
    for line in path.read_text(encoding="ascii").splitlines():
        f = {k: line[a - 1:b].strip() for k, (a, b) in cols.items()}
        if not (f["ra"] and f["de"] and f["vmag"]):
            continue
        for k, v in f.items():
            rows[k].append(float(v) if v else np.nan)
    out = {k: np.array(v) for k, v in rows.items()}
    out["hip"] = out["hip"].astype(np.uint32)
    for k in ("pmra", "pmde"):
        out[k] = np.nan_to_num(out[k])
    return out


def directions(c: dict[str, np.ndarray]) -> tuple[np.ndarray, np.ndarray]:
    """Направление на J1991.25 и его производная по времени (рад/год) из μα* и μδ."""
    a, d = np.radians(c["ra"]), np.radians(c["de"])
    u = np.stack([np.cos(d) * np.cos(a), np.cos(d) * np.sin(a), np.sin(d)], 1)
    e_a = np.stack([-np.sin(a), np.cos(a), np.zeros_like(a)], 1)
    e_d = np.stack([-np.sin(d) * np.cos(a), -np.sin(d) * np.sin(a), np.cos(d)], 1)
    pm = e_a * (c["pmra"] * MAS)[:, None] + e_d * (c["pmde"] * MAS)[:, None]
    return u, pm


def direction_at(c: dict[str, np.ndarray], i: int, year: float) -> np.ndarray:
    u, pm = directions({k: v[i:i + 1] for k, v in c.items()})
    v = u[0] + pm[0] * (year - EPOCH)
    return v / np.linalg.norm(v)


def temperature(bv: np.ndarray) -> np.ndarray:
    """Ballesteros 2012: T = 4600 K · (1/(0,92·(B−V) + 1,7) + 1/(0,92·(B−V) + 0,62))."""
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62))


def planck_rgb(t: np.ndarray) -> np.ndarray:
    """Планк на длинах волн R, G, B, деленный на тот же расчёт для Солнца; затем G = 1."""
    lam = np.array(RGB_UM) * 1e-6
    c2 = 1.438776877e-2  # м·К, вторая радиационная постоянная hc/k (точно по SI)
    b = 1.0 / (lam ** 5 * np.expm1(c2 / (lam[None, :] * t[:, None])))
    sun = 1.0 / (lam ** 5 * np.expm1(c2 / (lam * T_SUN)))
    rgb = b / sun
    return rgb / rgb[:, 1:2]


def write_import(path: Path) -> None:
    """HDR-карта: сжатие BC6H для видеокарты, мип-уровни."""
    params = {"compress/mode": "2", "compress/high_quality": "false", "compress/hdr_compression": "1",
              "compress/channel_pack": "0", "mipmaps/generate": "true", "mipmaps/limit": "-1",
              "process/size_limit": "0", "detect_3d/compress_to": "0"}
    imp = path.with_suffix(path.suffix + ".import")
    if imp.exists():
        have = dict(ln.split("=", 1) for ln in imp.read_text(encoding="utf-8").splitlines() if "=" in ln)
        if all(have.get(k) == v for k, v in params.items()):
            return
    lines = ["[remap]", "", 'importer="texture"', 'type="CompressedTexture2D"', "", "[params]", ""]
    imp.write_text("\n".join(lines + [f"{k}={v}" for k, v in params.items()] + [""]), encoding="utf-8")


def write_sources() -> None:
    """game/data/sources_extra.json — источники этапов 6–7 для окна «Источники» (остальные — sources.json
    и appearance.json)."""
    log = json.loads((ROOT / "data" / "raw" / "stars_fetch_log.json").read_text(encoding="utf-8"))
    src = {
        "hipparcos": {"title": "ESA 1997, The Hipparcos and Tycho Catalogues (ESA SP-1200), CDS I/239, hip_main.dat",
                      "url": log["hip_main"]["url"], "md5": log["hip_main"]["md5"],
                      "fetched_utc": log["hip_main"]["accessed_utc"], "license": "ESA/CDS, с указанием источника"},
        "svs_4851": {"title": "NASA SVS 4851, Deep Star Maps 2020: milkyway_2020_8k.exr (Млечный Путь без ярких звёзд)",
                     "url": log["milkyway"]["url"], "md5": log["milkyway"]["md5"],
                     "fetched_utc": log["milkyway"]["accessed_utc"],
                     "license": "NASA/Goddard Space Flight Center Scientific Visualization Studio. Gaia DR2: ESA/Gaia/DPAC"},
        "ballesteros2012": {"title": "Ballesteros F. J. 2012, New insights into black bodies, EPL 97, 34008 "
                                     "(температура звезды по B−V)", "doi": "10.1209/0295-5075/97/34008"},
        "codata2018_g": {"title": "CODATA 2018 (NIST): гравитационная постоянная G = 6,67430·10⁻¹¹ м³/(кг·с²) "
                                  "(масса = GM / G)", "url": "https://physics.nist.gov/cgi-bin/cuu/Value?bg"},
        "si_constants": {"title": "SI (2019): c = 299 792 458 м/с, c₂ = hc/k = 1,438776877·10⁻² м·К — точные значения",
                         "url": "https://www.bipm.org/en/publications/si-brochure"},
    }
    (ROOT / "game" / "data" / "sources_extra.json").write_text(json.dumps({"sources": src}, ensure_ascii=False, indent=1),
                                                               encoding="utf-8")


def main() -> int:
    c = read_hipparcos()
    u, pm = directions(c)
    bv = c["bv"]
    rgb = np.ones((len(bv), 3))
    ok = np.isfinite(bv)
    rgb[ok] = planck_rgb(temperature(np.clip(bv[ok], -0.4, 2.0)))
    rec = np.zeros(len(bv), dtype=[("hip", "<u4"), ("u", "<f4", 3), ("pm", "<f4", 3), ("v", "<f4"), ("rgb", "<f4", 3)])
    rec["hip"], rec["u"], rec["pm"], rec["v"], rec["rgb"] = c["hip"], u, pm, c["vmag"], rgb
    OUT.write_bytes(b"STAR" + struct.pack("<IId", 1, len(rec), EPOCH) + rec.tobytes())
    SKY.mkdir(parents=True, exist_ok=True)
    dst = SKY / "milkyway_8k.exr"
    if not dst.exists() or dst.stat().st_size != (RAW / "milkyway_2020_8k.exr").stat().st_size:
        shutil.copyfile(RAW / "milkyway_2020_8k.exr", dst)
    write_import(dst)
    write_sources()
    print(json.dumps({"stars": len(rec), "no_bv": int((~ok).sum()), "brighter_6.5": int((c["vmag"] < 6.5).sum()),
                      "bytes": OUT.stat().st_size}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
