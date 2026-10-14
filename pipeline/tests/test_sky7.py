"""Этап 7: звёзды Hipparcos и Млечный Путь. Допуски ослаблены по решению 14.10.2026 (функция важнее идеала)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import build_stars as bs  # noqa: E402
from kernels import ROOT  # noqa: E402
from test_render import imported, run_scenario  # noqa: E402, F401 (фикстура)

ARCSEC = np.pi / (180 * 3600)
HIP = {"sirius": 32349, "betelgeuse": 27989, "polaris": 11767, "rigel": 24436}


def _stars() -> np.ndarray:
    raw = (ROOT / "game" / "data" / "stars.bin").read_bytes()
    assert raw[:4] == b"STAR"
    _, n, epoch = struct.unpack("<IId", raw[4:20])
    assert epoch == bs.EPOCH
    dt = np.dtype([("hip", "<u4"), ("u", "<f4", 3), ("pm", "<f4", 3), ("v", "<f4"), ("rgb", "<f4", 3)])
    return np.frombuffer(raw[20:], dtype=dt, count=n)


def _srgb_lin(f: Path) -> np.ndarray:
    im = np.asarray(Image.open(f).convert("RGB")).astype(float) / 255
    return np.where(im <= 0.04045, im / 12.92, ((im + 0.055) / 1.055) ** 2.4)


def test_star_catalog():
    s = _stars()
    cat = bs.read_hipparcos()
    assert len(s) == len(cat["hip"]) > 117000
    row = {int(h): k for k, h in enumerate(s["hip"])}
    # цвет: Бетельгейзе (B−V 1,85) красная, Ригель (−0,03) голубая
    bet, rig = s[row[HIP["betelgeuse"]]]["rgb"], s[row[HIP["rigel"]]]["rgb"]
    assert bet[0] > bet[1] > bet[2] and rig[2] > rig[0]


@pytest.mark.window
def test_sky_render(imported):
    meta, _ = run_scenario("sky", resolution="1600x900")
    out = ROOT / "build" / "render_tests" / "sky"
    cat = bs.read_hipparcos()
    idx = {int(h): k for k, h in enumerate(cat["hip"])}
    year = 2026.0 - 0.5 / 365.25 + 69.18 / (365.25 * 86400)  # 2026-01-01 00:00 UTC, юлианский год TDB
    for key in ("sirius", "betelgeuse", "polaris"):
        # направление в игре (C++, float32, собственное движение) против расчёта по каталогу в double: до 1″
        ref = bs.direction_at(cat, idx[HIP[key]], year)
        got = np.array(meta["dir_" + key])
        assert np.degrees(np.arccos(np.clip(ref @ got, -1, 1))) * 3600 < 1.0, key
        # звезда в центре кадра (середина экрана — между пикселями: −0,5 в координатах вырезки), до 0,25 px
        lum = _srgb_lin(out / f"star_{key}.png").sum(2)[14:27, 14:27]
        yy, xx = np.mgrid[14:27, 14:27]
        cx, cy = (lum * xx).sum() / lum.sum() - 20, (lum * yy).sum() / lum.sum() - 20
        assert abs(cx + 0.5) < 0.25 and abs(cy + 0.5) < 0.25, (key, cx, cy)
    # поток звезды при повороте на 1/8 пикселя меняется меньше чем на 5 %
    flux = [_srgb_lin(out / f"shift_{k}.png")[13:28, 13:28].sum() for k in range(9)]
    assert (max(flux) - min(flux)) / np.mean(flux) < 0.05
    # центр Галактики: α, δ карты в центре кадра против RA 266,405°, Dec −28,936° — до 0,1°
    code = [np.asarray(Image.open(out / f"gc_code{m}.png").convert("RGB")).astype(float)[2, 2] for m in (1, 2)]
    ra = (code[0][0] + code[0][1] / 255) / 255 * 360
    dec = ((code[1][0] + code[1][1] / 255) / 255 - 0.5) * 180
    assert abs((ra - 266.405) * np.cos(np.radians(dec))) < 0.1 and abs(dec + 28.936) < 0.1, (ra, dec)
    # к центру Галактики небо ярче, чем к её северному полюсу (карта не перевёрнута)
    gc, ngp = (_srgb_lin(out / f"mw_{k}.png")[350:550, 700:900].mean() for k in ("gc", "ngp"))
    assert gc > 3 * ngp, (gc, ngp)
    print(f"\nзвёзды: центр ГЦ RA {ra:.3f}° Dec {dec:.3f}°; поток ±{(max(flux) - min(flux)) / np.mean(flux):.4f}; "
          f"яркость ГЦ/полюс {gc / ngp:.1f}")
