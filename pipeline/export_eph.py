"""Экспорт выбранных сегментов SPK в формат .eph.

Сегменты SPK типа 2 копируются побитно; при ограничении диапазона дат отбрасываются только целые записи.

    python -I pipeline/export_eph.py --out game/data/ephemeris.eph
    python -I pipeline/export_eph.py --dry-run --sat-range 1900-01-01 2100-01-01
"""
from __future__ import annotations

import argparse
import datetime as dt
import sys
from pathlib import Path

import numpy as np
import spiceypy as sp

sys.path.insert(0, str(Path(__file__).resolve().parent))
from eph_format import Segment, write_eph  # noqa: E402
from kernels import RAW, ROOT, SPK_KERNELS, md5, verify_all  # noqa: E402
from refit_small_bodies import HZ, SMALL_BODIES, furnsh_all, load_or_refit  # noqa: E402


def read_segments(path: Path, wanted: set[tuple[int, int]]) -> list[dict]:
    """Сводки и данные нужных сегментов. Данные — memmap исходного файла (адреса DAF — номера float64 с 1)."""
    with open(path, "rb") as f:
        rec = f.read(1024)
    if rec[:8] != b"DAF/SPK " or rec[88:96] != b"LTL-IEEE":
        raise RuntimeError(f"{path.name}: ожидается DAF/SPK LTL-IEEE, получено {rec[:8]!r} {rec[88:96]!r}")
    mm = np.memmap(path, dtype="<f8", mode="r")
    out = []
    h = sp.dafopr(str(path))
    try:
        sp.dafbfs(h)
        while sp.daffna():
            dc, ic = sp.dafus(sp.dafgs(125), 2, 6)
            tgt, ctr, frame, typ, b, e = (int(x) for x in ic)
            if (tgt, ctr) not in wanted:
                continue
            if frame != 1 or typ != 2:
                raise RuntimeError(f"{path.name}: {tgt} wrt {ctr} — frame {frame}, type {typ}; ожидается J2000, тип 2")
            words = mm[b - 1:e]
            init, intlen, rsize, n = (float(x) for x in words[-4:])
            rsize, n = int(rsize), int(n)
            if rsize * n + 4 != words.size:
                raise RuntimeError(f"{path.name}: {tgt}: размер сегмента не сходится")
            out.append({"target": tgt, "center": ctr, "start": float(dc[0]), "end": float(dc[1]),
                        "init": init, "intlen": intlen, "rsize": rsize,
                        "records": words[:-4].reshape(n, rsize)})
    finally:
        sp.dafcls(h)
    missing = wanted - {(s["target"], s["center"]) for s in out}
    if missing:
        raise RuntimeError(f"{path.name}: нет сегментов {sorted(missing)}")
    return out


def clip(seg: dict, t0: float | None, t1: float | None) -> dict | None:
    """Оставляет записи, пересекающие [t0, t1]. None — если сегмент целиком вне диапазона."""
    start = seg["start"] if t0 is None else max(seg["start"], t0)
    end = seg["end"] if t1 is None else min(seg["end"], t1)
    if start > end:
        return None
    n = seg["records"].shape[0]
    i0 = int(np.clip(np.floor((start - seg["init"]) / seg["intlen"]), 0, n - 1))
    i1 = int(np.clip(np.floor((end - seg["init"]) / seg["intlen"]), 0, n - 1))
    out = dict(seg)
    out.update(start=start, end=end, init=seg["init"] + i0 * seg["intlen"],
               records=seg["records"][i0:i1 + 1])
    return out


def utc_to_et(s: str) -> float:
    return float(sp.str2et(s))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=ROOT / "game" / "data" / "ephemeris.eph")
    ap.add_argument("--sat-range", nargs=2, metavar=("UTC0", "UTC1"),
                    help="диапазон для спутниковых ядер (планетные DE440 и DE442 всегда целиком)")
    ap.add_argument("--dry-run", action="store_true", help="только посчитать размеры")
    ap.add_argument("--skip-md5", action="store_true", help="не сверять MD5 (быстрее, для отладки)")
    args = ap.parse_args(argv)

    sp.kclear()
    sp.furnsh(str(RAW / "naif0012.tls"))
    t0 = t1 = None
    if args.sat_range:
        t0, t1 = utc_to_et(args.sat_range[0]), utc_to_et(args.sat_range[1])

    sources = []
    if not args.skip_md5:
        info = verify_all()
    segments: list[Segment] = []
    total = 0
    for idx, (name, (_, _, pairs)) in enumerate(SPK_KERNELS.items()):
        path = RAW / name
        segs = read_segments(path, set(pairs))
        rng = (None, None) if name.startswith("de44") else (t0, t1)
        size = 0
        for s in segs:
            c = clip(s, *rng)
            if c is None:
                continue
            size += c["records"].nbytes
            segments.append(Segment(c["target"], c["center"], c["start"], c["end"], c["init"],
                                    c["intlen"], c["rsize"], np.asarray(c["records"]), source=idx))
        total += size
        src = {"file": name, "bytes": path.stat().st_size}
        if not args.skip_md5:
            src.update(url=info[name]["url"], md5=info[name]["md5"])
        sources.append(src)
        print(f"{name:20s} {size / 2**20:9.1f} МиБ")
    # малые тела: переподгонка SPK тип 21 из Horizons (кэш в data/raw/horizons/refit_*.npz)
    furnsh_all()
    small = []
    for key, naif, spk_id in SMALL_BODIES:
        idx = len(sources)
        segs, stats = load_or_refit(spk_id, naif, idx)
        size = sum(s.records.nbytes for s in segs)
        total += size
        segments.extend(segs)
        bsp = HZ / f"{spk_id}.bsp"
        solution = next((ln.strip() for ln in (HZ / f"{spk_id}_horizons.txt").read_text(encoding="utf-8").splitlines()
                         if "Soln.date" in ln), "")
        sources.append({"file": bsp.name, "bytes": bsp.stat().st_size, "md5": md5(bsp),
                        "url": "https://ssd.jpl.nasa.gov/api/horizons.api (EPHEM_TYPE=SPK)",
                        "horizons_solution": solution, "refit": stats})
        small.append({"key": key, "naif": naif, "horizons_spk_id": spk_id})
        print(f"{key + ' (Horizons)':20s} {size / 2**20:9.1f} МиБ, сегментов {len(segs)}, "
              f"скачков исходника > 0,1 м: {stats['splits']}, max {stats['max_jump_m']:.2f} м")
    print(f"{'итого':20s} {total / 2**20:9.1f} МиБ, сегментов {len(segments)}")
    if args.dry_run:
        return 0

    header = {
        "frame": "ICRF (SPICE J2000)",
        "time": "TDB, секунды от J2000 (2000-01-01 12:00:00 TDB)",
        "units": {"position": "km", "velocity": "km/s"},
        "record": "SPK type 2: [MID, RADIUS, X0..Xn, Y0..Yn, Z0..Zn]",
        "created_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "sat_range_utc": args.sat_range,
        "sources": sources,
        "small_bodies": small,
        "small_bodies_note": "переподгонка SPK тип 21 из Horizons в полиномы Чебышёва, допуск 1 м; центр — барицентр (0), Солнце из DE440",
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    write_eph(args.out, header, segments)
    print(f"записано {args.out} ({args.out.stat().st_size / 2**20:.1f} МиБ)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
