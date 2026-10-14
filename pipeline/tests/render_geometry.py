"""Разбор кадров сценариев geometry и eclipse (game/tests/scenarios.gd): подсолнечная точка, координаты карты,
тень Луны на Земле. Эталоны — SPICE (spiceypy) по тем же ядрам, из которых собраны данные игры.
"""
from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import spiceypy as sp
from PIL import Image

from kernels import RAW

KERNELS = ["naif0012.tls", "pck00011.tpc", "de440.bsp", "mar099.bsp", "jup365.bsp"]
SPICE_NAME = {"earth": ("EARTH", "IAU_EARTH"), "mars": ("MARS", "IAU_MARS"), "moon": ("MOON", "IAU_MOON"),
              "jupiter": ("JUPITER", "IAU_JUPITER")}


def furnish() -> None:
    for k in KERNELS:
        sp.furnsh(str(RAW / k))


def srgb_to_linear(a: np.ndarray) -> np.ndarray:
    x = a.astype(np.float64) / 255.0
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def load_ref(path: Path, w: int, h: int) -> np.ndarray:
    """Эталон по лучам: (h, w, 4) — номер тела + 1, планетоцентрическая широта, долгота, геодезическая широта."""
    return np.fromfile(path, dtype="<f4").reshape(h, w, 4)


def interior(mask: np.ndarray, margin: int = 2) -> np.ndarray:
    """Пиксели маски, у которых соседи на margin пикселей тоже в маске (без краёв: там MSAA смешивает)."""
    m = mask.copy()
    for _ in range(margin):
        m = m & np.roll(m, 1, 0) & np.roll(m, -1, 0) & np.roll(m, 1, 1) & np.roll(m, -1, 1)
    return m


def geodetic_normal(latg_deg: np.ndarray, lon_deg: np.ndarray) -> np.ndarray:
    la, lo = np.radians(latg_deg), np.radians(lon_deg)
    return np.stack([np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)], axis=-1)


def fit_sun_direction(img_path: Path, ref: np.ndarray, body_index: int) -> tuple[np.ndarray, int]:
    """По кадру «Ламберт»: яркость = n·s (n — нормаль эллипсоида). МНК по пикселям с яркостью 0,05…0,95."""
    lin = srgb_to_linear(np.asarray(Image.open(img_path).convert("RGB")))[..., 1]
    mask = interior(ref[..., 0] == body_index + 1) & (lin > 0.05) & (lin < 0.95)
    n = geodetic_normal(ref[..., 3][mask], ref[..., 2][mask])
    s, *_ = np.linalg.lstsq(n, lin[mask], rcond=None)
    return s / np.linalg.norm(s), int(mask.sum())


def spice_subsolar(key: str, et: float) -> tuple[float, float]:
    """Подсолнечная точка SPICE: метод «NEAR POINT/ELLIPSOID» (там, где нормаль смотрит на Солнце), без поправок
    на время света. Возвращает геодезические широту и восточную долготу, градусы."""
    target, frame = SPICE_NAME[key]
    spoint, _, _ = sp.subslr("NEAR POINT/ELLIPSOID", target, et, frame, "NONE", "SUN")
    radii = sp.bodvrd(target, "RADII", 3)[1]
    f = (radii[0] - radii[2]) / radii[0]
    lon, lat, _ = sp.recgeo(spoint, radii[0], f)
    return math.degrees(lat), math.degrees(lon)


def angle_deg(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    a = [math.cos(math.radians(lat1)) * math.cos(math.radians(lon1)), math.cos(math.radians(lat1)) * math.sin(math.radians(lon1)),
         math.sin(math.radians(lat1))]
    b = [math.cos(math.radians(lat2)) * math.cos(math.radians(lon2)), math.cos(math.radians(lat2)) * math.sin(math.radians(lon2)),
         math.sin(math.radians(lat2))]
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))))


def decode_uv(img_path: Path) -> np.ndarray:
    """Код координаты карты из кадра: старшие 8 бит в R, младшие — в G (значения sRGB, без перевода)."""
    a = np.asarray(Image.open(img_path).convert("RGB"), dtype=np.float64)
    return (a[..., 0] + a[..., 1] / 255.0) / 255.0


def uv_errors(key: str, out: Path, ref: np.ndarray, body_index: int, geodetic: bool) -> dict:
    """Расхождение координат карты в шейдере с эталоном по лучам: по долготе (в градусах дуги на экваторе) и широте."""
    mask = interior(ref[..., 0] == body_index + 1, 3)
    mask &= np.abs(ref[..., 2]) < 179.5  # пиксель на шве ±180° содержит обе стороны шва
    u = decode_uv(out / f"{key}_v2.png")[mask]
    v = decode_uv(out / f"{key}_v3.png")[mask]
    lon = ref[..., 2][mask]
    lat = (ref[..., 3] if geodetic else ref[..., 1])[mask]
    lon_shader = u * 360.0 - 180.0
    lat_shader = 90.0 - v * 180.0
    dlon = (lon_shader - lon + 180.0) % 360.0 - 180.0
    dlon_arc = dlon * np.cos(np.radians(lat))  # долгота у полюсов сжимается: меряем дугой
    dlat = lat_shader - lat
    return {"pixels": int(mask.sum()), "lon_max_deg": float(np.abs(dlon_arc).max()), "lat_max_deg": float(np.abs(dlat).max()),
            "lon_p99_deg": float(np.percentile(np.abs(dlon_arc), 99)), "lat_p99_deg": float(np.percentile(np.abs(dlat), 99))}


def umbra_center(img_path: Path, ref: np.ndarray, body_index: int, sun_dir_body: np.ndarray,
                 level: float = 0.02) -> tuple[float, float, int]:
    """Тень Луны на Земле: пиксели, где яркость меньше level от ламбертовой (полная фаза), — их центр на сфере.
    Возвращает геодезические широту и долготу центра и число таких пикселей."""
    lin = srgb_to_linear(np.asarray(Image.open(img_path).convert("RGB")))[..., 1]
    mask = interior(ref[..., 0] == body_index + 1, 3)
    n = geodetic_normal(ref[..., 3], ref[..., 2])
    lam = np.clip(n @ sun_dir_body, 0.0, None)
    dark = mask & (lam > 0.1) & (lin < level * lam)
    if not dark.any():
        return float("nan"), float("nan"), 0
    c = n[dark].sum(axis=0)
    c /= np.linalg.norm(c)
    return math.degrees(math.asin(c[2])), math.degrees(math.atan2(c[1], c[0])), int(dark.sum())


def spice_shadow_axis(et: float) -> tuple[float, float]:
    """Точка, где ось Солнце → Луна пересекает Землю (центр тени), геодезические широта и долгота."""
    sun, _ = sp.spkpos("SUN", et, "J2000", "NONE", "EARTH")
    moon, _ = sp.spkpos("MOON", et, "J2000", "NONE", "EARTH")
    d = sp.vhat(sp.vsub(moon, sun))
    rot = sp.pxform("J2000", "IAU_EARTH", et)
    o = sp.mxv(rot, moon)
    dd = sp.mxv(rot, d)
    radii = sp.bodvrd("EARTH", "RADII", 3)[1]
    try:
        point = sp.surfpt(o, dd, radii[0], radii[1], radii[2])
    except sp.stypes.NotFoundError:
        return float("nan"), float("nan")
    f = (radii[0] - radii[2]) / radii[0]
    lon, lat, _ = sp.recgeo(point, radii[0], f)
    return math.degrees(lat), math.degrees(lon)


def spice_sun_dir_body(key: str, et: float) -> np.ndarray:
    target, frame = SPICE_NAME[key]
    s, _ = sp.spkpos("SUN", et, frame, "NONE", target)
    return np.array(sp.vhat(s))
