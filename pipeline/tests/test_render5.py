"""Этап 5: внешний вид тел в окне Godot (game/tests/scenarios.gd), сравнение со SPICE и с исходными картами.

Пороги заданы до запуска:
  - у каждого видимого тела в материале стоит карта из манифеста (ни одной заглушки), у тел без карты — пометка;
  - снимок каждого тела с картой не однотонный (средний модуль лапласиана яркости > 1,5 единицы sRGB) и нужного
    оттенка: хроматичность освещённого диска отличается от средней по карте не больше чем на 0,04 (Земля — 0,06:
    атмосфера и облака);
  - карта 8K подгружается, когда тело на экране шире 600 px, и выгружается через 3 с после того, как оно стало
    меньше 300 px; видеопамять возвращается к исходной;
  - подсолнечная точка (по кадру «Ламберт») совпадает со SPICE до 0,1° для Земли, Марса, Луны и Юпитера;
  - координаты карты в шейдере совпадают с эталоном по лучам лучше 0,5 px;
  - центр полной фазы тени Луны на Земле 12.08.2026 — в пределах 0,5° от оси Солнце — Луна по SPICE; Ио в тени
    Юпитера по SPICE — без света и в кадре;
  - потемнение Солнца к краю совпадает с законом μ^α (Hestroffer & Magnan 1998): яркость в пределах 2 %,
    показатель α — до 0,02.
"""
import json
import math
import shutil
import subprocess

import numpy as np
import pytest
import spiceypy as sp
from PIL import Image

import render_geometry as rg
from kernels import ROOT
from test_render import GODOT, _save, imported, run_scenario  # noqa: F401 (фикстура)

pytestmark = pytest.mark.window
MAN = json.loads((ROOT / "game" / "data" / "appearance.json").read_text(encoding="utf-8"))
BODIES = {b["key"]: b for b in json.loads((ROOT / "game" / "data" / "bodies.json").read_text(encoding="utf-8"))["bodies"]}


@pytest.fixture(scope="module")
def spice():
    rg.furnish()  # повторная загрузка тех же ядер SPICE безвредна; kclear не зовём — ядра нужны и другим модулям


@pytest.fixture(scope="module")
def maps(imported):
    return run_scenario("maps")


def _expected_chroma(key: str) -> np.ndarray:
    rec = MAN["bodies"][key]
    col = rec["layers"].get("color")
    if not col:
        e = np.array(rec["uniform_rgb"], dtype=float)
        return e / e.sum()
    files = col["files"]
    f = files["2048"] if "2048" in files else files[min(files, key=int)]
    t = np.asarray(Image.open(ROOT / "game" / f.replace("res://", "")).convert("RGB"))
    lin = t / 255.0 if col.get("encoding") == "linear" else rg.srgb_to_linear(t)
    lin = np.minimum(lin * (rec.get("albedo") or {}).get("scale", 1.0), 1.0)
    w = np.cos((0.5 - (np.arange(lin.shape[0]) + 0.5) / lin.shape[0]) * np.pi)[:, None, None]
    e = (lin * w).sum((0, 1)) / (w.sum() * lin.shape[1])
    return e / e.sum()


def test_no_placeholders(maps):
    meta, _ = maps
    rows = {}
    for b in meta["bodies"]:
        key, ap = b["key"], b["appearance"]
        rec = MAN["bodies"][key]
        if key == "sun":
            assert "corona" in ap
            continue
        if rec["layers"].get("color"):
            assert b["texture_width"] >= 1440, f"{key}: в материале нет карты"
            assert ap.get("map_source"), key
        else:
            assert ap.get("no_map") or ap.get("note"), f"{key}: нет пометки, почему нет карты"
        rows[key] = b["texture_width"]
    _save("stage5_textures", rows)
    assert len(rows) == len(MAN["bodies"]) - 1


def test_snapshots_detail_and_hue(maps):
    meta, out = maps
    w, h = int(meta["viewport"][0]), int(meta["viewport"][1])
    rows = {}
    for b in meta["bodies"]:
        key = b["key"]
        if key == "sun":
            continue
        assert b["sun_visibility"] > 0.999, f"{key} в тени в момент съёмки"
        cls = np.fromfile(out / f"ref_{key}.bin", dtype=np.uint8).reshape(h, w)
        img = np.asarray(Image.open(out / b["file"]).convert("RGB"))
        lin = rg.srgb_to_linear(img)
        lum = lin @ np.array([0.2126, 0.7152, 0.0722])
        lit = rg.interior(cls == b["index"] + 1, 3) & (lum > 0.02)
        assert lit.sum() > 20000, f"{key}: освещённого диска почти нет ({lit.sum()} px)"
        g = np.asarray(Image.open(out / b["file"]).convert("L"), dtype=float)
        lap = np.abs(4 * g - np.roll(g, 1, 0) - np.roll(g, -1, 0) - np.roll(g, 1, 1) - np.roll(g, -1, 1))
        detail = float(lap[rg.interior(lit, 2)].mean())
        mean = lin[lit].mean(axis=0)
        chroma = mean / mean.sum()
        expect = _expected_chroma(key)
        diff = float(np.abs(chroma - expect).max())
        rows[key] = {"detail": detail, "chroma": chroma.round(4).tolist(), "expected": expect.round(4).tolist(),
                     "chroma_diff": diff}
        if MAN["bodies"][key]["layers"].get("color"):
            assert detail > 1.5, f"{key}: снимок однотонный (лапласиан {detail:.2f})"
        assert diff < (0.06 if key == "earth" else 0.04), f"{key}: оттенок {chroma} вместо {expect}"
    _save("stage5_snapshots", rows)


def test_sun_limb_darkening(maps):
    meta, out = maps
    w, h = int(meta["viewport"][0]), int(meta["viewport"][1])
    sun = next(b for b in meta["bodies"] if b["key"] == "sun")
    lin = rg.srgb_to_linear(np.asarray(Image.open(out / sun["file"]).convert("RGB")))
    cx, cy = sun["center_px"]
    f = (h / 2) / math.tan(math.radians(60.0 / 2))
    ys, xs = np.mgrid[0:h, 0:w]
    th = np.arctan(np.hypot(xs + 0.5 - cx, ys + 0.5 - cy) / f)
    mu2 = 1.0 - (np.sin(th) / (sun["radii_km"][0] / sun["distance_km"])) ** 2
    cls = np.fromfile(out / "ref_sun.bin", dtype=np.uint8).reshape(h, w)
    mask = rg.interior(cls == 1, 3) & (mu2 > 0.0225)  # μ > 0,15
    mu = np.sqrt(np.clip(mu2, 0.0, 1.0))[mask]
    ld = MAN["bodies"]["sun"]["limb_darkening"]
    alpha = ld["a0"] + ld["a1"] / np.array(MAN["rgb_wavelengths_um"])
    disk_mean = (149597870.7 / BODIES["sun"]["phys"]["radius_mean_km"]["v"]) ** 2
    center = disk_mean * (alpha + 2) / 2 * 0.25 * 2.0 ** sun["exposure_ev"]  # 0,25 — sun_scale в sun.gdshader
    rows = []
    for c in range(3):
        obs = lin[..., c][mask]
        ratio = obs / (center[c] * mu ** alpha[c])
        a = np.vstack([np.ones(mu.size), np.log(mu)]).T
        coef = np.linalg.lstsq(a, np.log(np.maximum(obs, 1e-6)), rcond=None)[0]
        rows.append({"median_ratio": float(np.median(ratio)), "alpha_fit": float(coef[1]), "alpha": float(alpha[c])})
        assert abs(np.median(ratio) - 1.0) < 0.02, rows[-1]
        assert abs(coef[1] - alpha[c]) < 0.02, rows[-1]
    _save("stage5_sun_limb", rows)


def test_hi_texture_streaming(imported):
    meta, _ = run_scenario("lod")
    start, near, far = meta["start"], meta["near"], meta["far"]
    _save("stage5_lod", {"start": start, "near": near, "far": far})
    assert start["width"] == 2048 and start["hi_bytes"] == 0
    assert near["px"] > 600 and near["width"] == 8192, near
    assert near["hi_bytes"] > 0
    # видеопамять выросла на столько, сколько весят карты 8K Земли (оценка в C++), с точностью 2 %
    assert near["vram"] - start["vram"] == pytest.approx(near["hi_bytes"], rel=0.02)
    assert far["px"] < 300 and far["width"] == 2048 and far["hi_bytes"] == 0, far
    assert 180 <= far["frames"] <= 200, far  # 3 с при 60 кадрах/с
    assert far["vram"] == pytest.approx(start["vram"], rel=0.01)


@pytest.fixture(scope="module")
def geometry(imported):
    return run_scenario("geometry")


def test_subsolar_point(geometry, spice):
    meta, out = geometry
    w, h = meta["size"]
    rows = {}
    for shot in meta["shots"]:
        key = shot["key"]
        assert shot["visibility"] > 0.999
        ref = rg.load_ref(out / f"{key}_ref.bin", w, h)
        s, npx = rg.fit_sun_direction(out / f"{key}_v1.png", ref, shot["index"])
        lat, lon = math.degrees(math.asin(s[2])), math.degrees(math.atan2(s[1], s[0]))
        slat, slon = rg.spice_subsolar(key, meta["et"])
        err = rg.angle_deg(lat, lon, slat, slon)
        rows[key] = {"render": [lat, lon], "spice": [slat, slon], "err_deg": err, "pixels": npx}
        assert err < 0.1, (key, rows[key])
    _save("stage5_subsolar", rows)


def test_map_coordinates_in_shader(geometry):
    """Шейдер берёт карту в той точке, куда смотрит пиксель: ошибка меньше 0,5 px (по эталону лучей в double)."""
    meta, out = geometry
    w, h = meta["size"]
    rows = {}
    for shot in meta["shots"]:
        key = shot["key"]
        ref = rg.load_ref(out / f"{key}_ref.bin", w, h)
        mask = rg.interior(ref[..., 0] == shot["index"] + 1, 3) & (np.abs(ref[..., 2]) < 179.5)
        lat = ref[..., 3] if key == "earth" else ref[..., 1]
        p = rg.geodetic_normal(lat, ref[..., 2])
        u, v = rg.decode_uv(out / f"{key}_v2.png"), rg.decode_uv(out / f"{key}_v3.png")
        q = rg.geodetic_normal(90.0 - v * 180.0, u * 360.0 - 180.0)
        err = np.degrees(np.arccos(np.clip((p * q).sum(-1), -1, 1)))
        step = np.maximum(np.degrees(np.arccos(np.clip((p * np.roll(p, -1, 1)).sum(-1), -1, 1))),
                          np.degrees(np.arccos(np.clip((p * np.roll(p, -1, 0)).sum(-1), -1, 1))))
        e = (err / np.maximum(step, 1e-9))[mask]
        rows[key] = {"px_median": float(np.median(e)), "px_p99": float(np.percentile(e, 99)), "px_max": float(e.max()),
                     "deg_max": float(err[mask].max()), "pixels": int(mask.sum())}
        assert e.max() < 0.5, (key, rows[key])
    _save("stage5_uv", rows)


def test_eclipse_shadows(imported, spice):
    meta, out = run_scenario("eclipse")
    w, h = meta["size"]
    rows = []
    for shot in meta["shots"]:
        ref = rg.load_ref(out / f"earth_{shot['tag']}_ref.bin", w, h)
        s = rg.spice_sun_dir_body("earth", shot["et"])
        lat, lon, npx = rg.umbra_center(out / f"earth_{shot['tag']}.png", ref, 3, s)
        alat, alon = rg.spice_shadow_axis(shot["et"])
        err = rg.angle_deg(lat, lon, alat, alon)
        rows.append({"utc": shot["utc"], "render": [lat, lon], "spice": [alat, alon], "err_deg": err, "pixels": npx})
        assert npx > 50 and err < 0.5, rows[-1]
    # Ио: по SPICE центр Ио в тени Юпитера (Солнце за диском Юпитера) — свет не доходит
    et = meta["io_et"]
    s_dir, _ = sp.spkpos("SUN", et, "J2000", "NONE", "IO")
    j_dir, _ = sp.spkpos("JUPITER", et, "J2000", "NONE", "IO")
    sep = math.degrees(sp.vsep(s_dir, j_dir))
    jup_r = math.degrees(math.asin(sp.bodvrd("JUPITER", "RADII", 3)[1][0] / sp.vnorm(j_dir)))
    rows.append({"io_sep_deg": sep, "jupiter_radius_deg": jup_r, "io_visibility": meta["io_visibility"]})
    assert sep < jup_r - 1.0 and meta["io_visibility"] < 1e-6, rows[-1]
    _save("stage5_eclipse", rows)


EXPORT_DIR = ROOT / "build" / "export"
PRESET = ROOT / "game" / "export_presets.cfg"


@pytest.mark.slow
def test_assets_in_exported_build(imported):
    """Экспорт игры и запуск сценария карт внутри exe: карты грузятся из .pck, заглушек нет."""
    assert PRESET.exists(), "нет game/export_presets.cfg"
    if EXPORT_DIR.exists():
        shutil.rmtree(EXPORT_DIR)
    EXPORT_DIR.mkdir(parents=True)
    exe = EXPORT_DIR / "SolarSystem.exe"
    r = subprocess.run([GODOT, "--headless", "--path", str(ROOT / "game"), "--export-debug", "Windows Desktop", str(exe)],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800)
    assert r.returncode == 0 and exe.exists(), (r.stdout + r.stderr)[-3000:]
    out = ROOT / "build" / "render_tests" / "maps_export"
    out.mkdir(parents=True, exist_ok=True)
    keys = "earth,moon,mars,io,europa,saturn,pluto,eris,venus,sun"
    console = EXPORT_DIR / "SolarSystem.console.exe"  # обёртка с консолью: в неё идёт вывод сценария
    r = subprocess.run([str(console if console.exists() else exe), "--fixed-fps", "60", "--resolution", "1280x720", "--", "--scenario=maps", f"--out={out}",
                        f"--keys={keys}"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
    log = r.stdout + r.stderr
    assert r.returncode == 0, log[-3000:]
    assert not [ln for ln in log.splitlines() if "ERROR" in ln], log[-3000:]
    meta = json.loads((out / "meta.json").read_text(encoding="utf-8"))
    got = {b["key"]: b for b in meta["bodies"]}
    assert set(got) == set(keys.split(","))
    for key, b in got.items():
        if MAN["bodies"][key]["layers"].get("color"):
            assert b["texture_width"] >= 2048, key
    _save("stage5_export", {k: v["texture_width"] for k, v in got.items()} | {"exe_bytes": exe.stat().st_size,
                                                                               "pck_bytes": (EXPORT_DIR / "SolarSystem.pck").stat().st_size})
