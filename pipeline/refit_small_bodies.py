"""Переподгонка SPK малых тел (тип 21 из Horizons) в полиномы Чебышёва (SPK тип 2) для .eph.

Интервал делится пополам, пока ошибка положения внутри записи больше FIT_TOL_KM.
Подряд идущие записи одной длины объединяются в сегмент.
Центр — барицентр Солнечной системы (0): относительно Солнца в положениях есть качание Солнца
(сотни км с периодами Венеры и Земли), и записи пришлось бы делать по 30–60 суток.
Солнце относительно барицентра берётся из DE440.

В SPK Horizons на границах шагов интегратора (записи MDA) положение скачет: обычно на миллиметры,
изредка до десятков метров вдали от эпохи орбиты (у Галлея до 14 м). Сетку записей режем
в точках скачков больше JUMP_SPLIT_KM, чтобы полином не тянулся через разрыв.
Результат кэшируется в data/raw/horizons/refit_<id>.npz.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import spiceypy as sp

sys.path.insert(0, str(Path(__file__).resolve().parent))
from eph_format import Segment  # noqa: E402
from kernels import RAW, ROOT  # noqa: E402

HZ = ROOT / "data" / "raw" / "horizons"
DEGREE = 13
FIT_TOL_KM = 3e-4        # 0,3 м внутри записи; итоговый допуск теста — 1 м
TOP_DAYS = 2048.0        # длина записи верхнего уровня
MIN_DAYS = 0.125
JUMP_SPLIT_KM = 1e-4     # скачки больше 0,1 м — границы сегментов
CENTER = 0

# ключ, NAIF-код в приложении, код в SPK Horizons
SMALL_BODIES = [
    ("ceres", 2000001, 20000001),
    ("eris", 2136199, 20136199),
    ("haumea", 2136108, 20136108),
    ("makemake", 2136472, 20136472),
    ("halley", 1000036, 1000036),
    ("pallas", 2000002, 20000002),
    ("vesta", 2000004, 20000004),
    ("hygiea", 2000010, 20000010),
]

_N = DEGREE + 1
_NODES = np.cos(np.pi * (np.arange(_N) + 0.5) / _N)          # узлы Чебышёва первого рода
_CHECK = np.cos(np.pi * np.arange(_N + 1) / _N)              # экстремумы T_N, между узлами и края
_T_NODES = np.cos(np.outer(np.arange(_N), np.arccos(_NODES)))  # T_j(x_k)


def _positions(spk_id: int, ets: np.ndarray) -> np.ndarray:
    return np.array([sp.spkgps(spk_id, float(t), "J2000", CENTER)[0] for t in ets])


def _fit(spk_id: int, a: float, b: float):
    mid, rad = 0.5 * (a + b), 0.5 * (b - a)
    f = _positions(spk_id, mid + rad * _NODES)                 # (N, 3)
    c = (2.0 / _N) * (_T_NODES @ f)                            # (N, 3)
    c[0] *= 0.5
    # края записи сдвинуты на 1 мс внутрь: в точке разреза SPICE берёт значение с другой стороны скачка
    shrink = 1.0 - min(1e-3 / rad, 1e-3)
    ref = _positions(spk_id, mid + rad * shrink * _CHECK)
    t_check = np.cos(np.outer(np.arange(_N), np.arccos(shrink * _CHECK)))
    err = np.linalg.norm(t_check.T @ c - ref, axis=1).max()
    return mid, rad, c, err


def _records(spk_id: int, a: float, b: float, out: list):
    mid, rad, c, err = _fit(spk_id, a, b)
    if err <= FIT_TOL_KM or (b - a) / 86400.0 <= MIN_DAYS:
        if err > FIT_TOL_KM:
            raise RuntimeError(f"{spk_id}: не сошлось на [{a}, {b}], ошибка {err * 1e3:.2f} м")
        out.append((a, b, np.concatenate([[mid, rad], c[:, 0], c[:, 1], c[:, 2]])))
        return
    m = 0.5 * (a + b)
    _records(spk_id, a, m, out)
    _records(spk_id, m, b, out)


def mda_boundaries(path: Path) -> np.ndarray:
    """Конечные эпохи записей MDA во всех сегментах SPK типа 21 (без крайних)."""
    mm = np.memmap(path, dtype="<f8", mode="r")
    out = []
    h = sp.dafopr(str(path))
    try:
        sp.dafbfs(h)
        while sp.daffna():
            _, ic = sp.dafus(sp.dafgs(125), 2, 6)
            if int(ic[3]) != 21:
                raise RuntimeError(f"{path.name}: тип {int(ic[3])}, ожидается 21")
            w = mm[int(ic[4]) - 1:int(ic[5])]
            n, maxdim = int(w[-1]), int(w[-2])
            dl = 4 * maxdim + 11  # длина записи MDA
            out.append(np.array(w[n * dl:n * dl + n]))
    finally:
        sp.dafcls(h)
    return np.unique(np.concatenate(out))[1:-1]


def jumps(spk_id: int, epochs: np.ndarray, eps: float = 1e-2) -> np.ndarray:
    """Скачок положения в каждой эпохе: |p(t+ε) − p(t−ε) − ε(v₋ + v₊)|, км."""
    out = np.empty(epochs.size)
    for i, t in enumerate(epochs):
        a = np.array(sp.spkgeo(spk_id, float(t - eps), "J2000", 10)[0])
        b = np.array(sp.spkgeo(spk_id, float(t + eps), "J2000", 10)[0])
        out[i] = np.linalg.norm(b[:3] - a[:3] - eps * (a[3:] + b[3:]))
    return out


def refit(spk_id: int, naif: int, source: int = 0) -> tuple[list[Segment], dict]:
    path = HZ / f"{spk_id}.bsp"
    cov = sp.spkcov(str(path), spk_id)
    start, end = sp.wnfetd(cov, 0)[0], sp.wnfetd(cov, sp.wncard(cov) - 1)[1]
    bounds = mda_boundaries(path)
    jmp = jumps(spk_id, bounds)
    cuts = bounds[jmp > JUMP_SPLIT_KM]
    top = TOP_DAYS * 86400.0
    recs: list = []
    edges = [start, *cuts, end]
    for p0, p1 in zip(edges[:-1], edges[1:]):
        a = p0
        while a < p1:
            _records(spk_id, a, min(a + top, p1), recs)
            a += top
    # склейка подряд идущих записей одинаковой длины в сегменты типа 2
    segs: list[Segment] = []
    run: list = []

    def flush():
        if run:
            arr = np.array([r[2] for r in run])
            il = run[0][1] - run[0][0]
            segs.append(Segment(naif, CENTER, run[0][0], run[-1][1], run[0][0], il, arr.shape[1], arr, source))

    for r in recs:
        if run and (not np.isclose(r[1] - r[0], run[0][1] - run[0][0], rtol=0, atol=1e-6)
                    or r[0] != run[-1][1]):
            flush()
            run = []
        run.append(r)
    flush()
    stats = {"mda_boundaries": int(bounds.size), "max_jump_m": float(jmp.max() * 1e3),
             "splits": int(cuts.size), "split_threshold_m": JUMP_SPLIT_KM * 1e3}
    return segs, stats


def load_or_refit(spk_id: int, naif: int, source: int) -> tuple[list[Segment], dict]:
    cache = HZ / f"refit_{spk_id}.npz"
    src_mtime = max((HZ / f"{spk_id}.bsp").stat().st_mtime, Path(__file__).stat().st_mtime)
    if cache.exists() and cache.stat().st_mtime > src_mtime:
        z = np.load(cache)
        meta = z["meta"]
        stats = {k: (float(v) if "m" in k else int(v)) for k, v in zip(z["stat_keys"], z["stat_vals"])}
        return [Segment(naif, CENTER, *m[:4], int(m[4]), z[f"r{i}"], source) for i, m in enumerate(meta)], stats
    segs, stats = refit(spk_id, naif, source)
    meta = np.array([[s.start_et, s.end_et, s.init, s.intlen, s.rsize] for s in segs])
    np.savez(cache, meta=meta, stat_keys=np.array(list(stats)), stat_vals=np.array(list(stats.values()), dtype=float),
             **{f"r{i}": s.records for i, s in enumerate(segs)})
    return segs, stats


def furnsh_all():
    """SPK малых тел и DE440 (Солнце относительно барицентра)."""
    sp.furnsh(str(RAW / "de440.bsp"))
    for _, _, spk_id in SMALL_BODIES:
        sp.furnsh(str(HZ / f"{spk_id}.bsp"))
