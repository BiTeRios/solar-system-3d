"""Список ядер SPICE, что из них берём, и проверка целостности.

Ядра лежат в data/raw/naif/ (скачивает tools/fetch_naif.sh, список согласован 11.10.2026).
"""
from __future__ import annotations

import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data" / "raw" / "naif"
BASE_URL = "https://naif.jpl.nasa.gov/pub/naif/generic_kernels"

# Текстовые ядра: файл -> путь на сервере. Контрольных сумм NAIF для них не публикует.
TEXT_KERNELS = {
    "naif0012.tls": "lsk/naif0012.tls",
    "pck00011.tpc": "pck/pck00011.tpc",
    "gm_de440.tpc": "pck/gm_de440.tpc",
}

# SPK: файл -> (путь на сервере, файл контрольных сумм, [(цель, центр)]). Планетные ядра входят целиком (1550–2650),
# спутниковые — в диапазоне --sat-range.
SPK_KERNELS = {
    "de440.bsp": ("spk/planets/de440.bsp", "spk_planets_aa_checksums.txt", [
        (1, 0), (2, 0), (3, 0), (4, 0), (5, 0), (6, 0), (9, 0), (10, 0),
        (199, 1), (299, 2), (301, 3), (399, 3),
    ]),
    # барицентры Урана и Нептуна — по DE442, как в Horizons (ura184 и nep098 построены на DE442; решение 11.10.2026)
    "de442.bsp": ("spk/planets/de442.bsp", "spk_planets_aa_checksums.txt", [
        (7, 0), (8, 0),
    ]),
    "mar099.bsp": ("spk/satellites/mar099.bsp", "spk_satellites_aa_checksums.txt", [
        (401, 4), (402, 4), (499, 4),
    ]),
    "jup365.bsp": ("spk/satellites/jup365.bsp", "spk_satellites_aa_checksums.txt", [
        (501, 5), (502, 5), (503, 5), (504, 5), (599, 5),
    ]),
    "sat441.bsp": ("spk/satellites/sat441.bsp", "spk_satellites_aa_checksums.txt", [
        (601, 6), (602, 6), (603, 6), (604, 6), (605, 6), (606, 6), (608, 6), (699, 6),
    ]),
    "ura184_part-3.bsp": ("spk/satellites/ura184_part-3.bsp", "spk_satellites_aa_checksums.txt", [
        (701, 7), (702, 7), (703, 7), (704, 7), (705, 7), (799, 7),
    ]),
    "nep098_part-1.bsp": ("spk/satellites/nep098_part-1.bsp", "spk_satellites_aa_checksums.txt", [
        (801, 8), (899, 8),
    ]),
    "plu060.bsp": ("spk/satellites/plu060.bsp", "spk_satellites_aa_checksums.txt", [
        (901, 9), (999, 9),
    ]),
}


def md5(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def published_md5(checksums_file: str, name: str) -> str | None:
    path = RAW / checksums_file
    for line in path.read_text(encoding="ascii", errors="replace").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] == name:
            return parts[0].lower()
    return None


def verify_all() -> dict[str, dict]:
    """MD5 всех ядер. Для SPK сверяется с aa_checksums.txt NAIF; при несовпадении — исключение."""
    info: dict[str, dict] = {}
    for name, server_path in TEXT_KERNELS.items():
        p = RAW / name
        info[name] = {"url": f"{BASE_URL}/{server_path}", "md5": md5(p), "bytes": p.stat().st_size,
                      "md5_published": None}
    for name, (server_path, sums, _) in SPK_KERNELS.items():
        p = RAW / name
        got = md5(p)
        want = published_md5(sums, name)
        if want is None:
            raise RuntimeError(f"{name}: нет в {sums}")
        if got != want:
            raise RuntimeError(f"{name}: MD5 {got}, у NAIF {want}. Файл повреждён, перекачайте.")
        info[name] = {"url": f"{BASE_URL}/{server_path}", "md5": got, "bytes": p.stat().st_size,
                      "md5_published": want}
    return info
