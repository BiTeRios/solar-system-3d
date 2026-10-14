"""Этап 5: карты поверхностей и параметры внешнего вида (без окна).

- манифест game/data/appearance.json: у каждого видимого тела есть карта или явная пометка «карты нет» с причиной;
  все файлы карт и их .import на месте, у каждой карты указан источник;
- привязка карт: известные детали поверхности (Gazetteer of Planetary Nomenclature) стоят там, где надо, а не
  на обратной стороне и не в зеркальном отражении; карты JPL совпадают с USGS без зеркала (по Фобосу);
- яркость: множитель приводит среднюю яркость карты к ламбертову альбедо 1,5·p (не больше 1);
- рэлеевское рассеяние воздуха совпадает с таблицей 3 Bodhaine et al. 1999;
- маска воды Земли покрывает долю поверхности, близкую к реальной (71 %).
"""
import json
import math

import numpy as np
import pytest
from PIL import Image

import appearance_params as ap
from build_textures import FEATURES
from kernels import ROOT

GAME = ROOT / "game"
MAN = json.loads((GAME / "data" / "appearance.json").read_text(encoding="utf-8"))
BODIES = json.loads((GAME / "data" / "bodies.json").read_text(encoding="utf-8"))["bodies"]


def _res(path: str):
    return GAME / path.replace("res://", "")


def _color(body: str, width: int | None = None) -> np.ndarray:
    files = MAN["bodies"][body]["layers"]["color"]["files"]
    w = str(width or max(int(k) for k in files))
    return np.asarray(Image.open(_res(files[w])).convert("RGB"), dtype=np.float64)


def test_manifest_covers_all_visible_bodies():
    for b in BODIES:
        if not b["visible"]:
            continue
        rec = MAN["bodies"].get(b["key"])
        assert rec is not None, f"{b['key']}: нет в appearance.json"
        if b["key"] == "sun":
            assert rec.get("emissive") and rec.get("limb_darkening") and rec.get("corona")
            continue
        color = rec["layers"].get("color")
        if color is None:
            # без карты — только с объяснением и цветом из данных
            assert rec.get("uniform_rgb"), b["key"]
            assert rec.get("no_map") or rec.get("note"), f"{b['key']}: нет пометки, почему нет карты"
            continue
        assert color["src"] in MAN["sources"], color["src"]
        for layer in rec["layers"].values():
            for path in layer["files"].values():
                f = _res(path)
                assert f.exists(), path
                assert f.with_suffix(f.suffix + ".import").exists(), path + ".import"


def test_levels_and_sizes():
    """Уровни детализации: «2K» не шире 2048, «8K» не шире 8192, высота — ровно половина ширины.
    Маска воды Земли — один уровень 4096."""
    for key, rec in MAN["bodies"].items():
        for name, layer in rec.get("layers", {}).items():
            widths = sorted(int(k) for k in layer["files"])
            assert widths[-1] <= 8192, (key, name, widths)
            assert widths[0] <= 2048 or (len(widths) == 1 and name == "ocean"), (key, name, widths)
            for w in widths:
                im = Image.open(_res(layer["files"][str(w)]))
                assert im.size == (w, w // 2), (key, name, im.size)


def _window(a: np.ndarray, lat: float, lon: float, half_deg: float) -> np.ndarray:
    h, w = a.shape[:2]
    lon = (lon + 180.0) % 360.0 - 180.0
    x = int((lon + 180.0) / 360.0 * w)
    y = int((90.0 - lat) / 180.0 * h)
    r = max(1, int(w / 360.0 * half_deg))
    xs = np.arange(x - r, x + r + 1) % w
    ys = np.clip(np.arange(y - r, y + r + 1), 0, h - 1)
    return a[np.ix_(ys, xs)]


@pytest.mark.parametrize("body,name,lat,lon,expect", [f for f in FEATURES if f[4] in ("bright", "dark")])
def test_feature_registration(body, name, lat, lon, expect):
    """Деталь ярче (темнее), чем то же место на обратной стороне и в зеркальном отражении по долготе
    (зеркальное — если оно дальше 60° от детали, иначе это то же место)."""
    a = _color(body, 2048).mean(axis=2)
    here = _window(a, lat, lon, 2.0).mean()
    others = [lon + 180.0]
    if abs((2 * lon + 180.0) % 360.0 - 180.0) >= 60.0:
        others.append(-lon)
    for other_lon in others:
        there = _window(a, lat, other_lon, 2.0).mean()
        if expect == "bright":
            assert here > there, f"{body} {name}: {here:.1f} не ярче {there:.1f} (долгота {other_lon})"
        else:
            assert here < there, f"{body} {name}: {here:.1f} не темнее {there:.1f} (долгота {other_lon})"


def test_iapetus_dichotomy():
    """Япет: ведущее полушарие (долготы −180…0, центр 90° з. д.) темнее ведомого (0…180).
    Мозаика USGS выровнена по яркости, поэтому разница небольшая (на самом Япете — в 10 раз)."""
    a = _color("iapetus", 2048).mean(axis=2)
    w = a.shape[1]
    lead, trail = a[:, :w // 2].mean(), a[:, w // 2:].mean()
    assert trail > 1.04 * lead, (lead, trail)


def test_io_pele_red():
    """Ио: кольцо выбросов Пеле (центр 18,7° ю. ш., 255,3° з. д., радиус ~8–16°) краснее, чем такое же кольцо
    вокруг зеркальной точки и вокруг точки на обратной стороне."""
    a = _color("io", 2048)
    h, w, _ = a.shape
    lat = (0.5 - (np.arange(h) + 0.5) / h) * math.pi
    lon = ((np.arange(w) + 0.5) / w - 0.5) * 2 * math.pi
    la, lo = np.meshgrid(lat, lon, indexing="ij")
    red = a[..., 0] - a[..., 1]

    def ring(lat0, lon0):
        c = np.sin(la) * math.sin(lat0) + np.cos(la) * math.cos(lat0) * np.cos(lo - lon0)
        d = np.degrees(np.arccos(np.clip(c, -1, 1)))
        return red[(d >= 8) & (d < 16)].mean()

    lat0, lon0 = math.radians(-18.7), math.radians(-255.3 + 360.0)
    pele = ring(lat0, lon0)
    assert pele > ring(lat0, -lon0) + 20 and pele > ring(lat0, lon0 + math.pi) + 20, pele


def test_jpl_series_matches_usgs():
    """Карты JPL (Деймос, спутники Урана) — без зеркала и без большого сдвига: проверено по Фобосу."""
    reg = MAN["jpl_registration"]
    assert reg["direct"]["corr"] > 2.0 * reg["mirror"]["corr"], reg
    assert abs(reg["direct"]["shift_deg"]) <= 5.0, reg


def test_albedo_normalization():
    """Средняя по площади линейная яркость карты × множитель = min(1, 1,5·p)."""
    checked = 0
    for key, rec in MAN["bodies"].items():
        alb = rec.get("albedo") or {}
        color = rec.get("layers", {}).get("color")
        if not color or "lambert" not in alb:
            continue
        m = color["mean_linear_rgb"]
        y = 0.2126 * m[0] + 0.7152 * m[1] + 0.0722 * m[2]
        assert alb["lambert"] == pytest.approx(min(1.0, 1.5 * alb["p_v"]))
        assert y * alb["scale"] == pytest.approx(alb["lambert"], rel=1e-9), key
        checked += 1
    assert checked >= 25


def test_uniform_bodies_from_data():
    """Тела без карты: серый по альбедо (Эрида, Хаумеа, Макемаке — статьи, Галлей — SBDB), Венера — по полосам."""
    for key in ("eris", "haumea", "makemake", "halley"):
        rec = MAN["bodies"][key]
        p = rec["albedo"]["p_v"]
        assert rec["uniform_rgb"] == [pytest.approx(min(1.0, 1.5 * p))] * 3, key
    v = MAN["bodies"]["venus"]["uniform_rgb"]
    assert v[0] >= v[1] >= v[2]  # R ≥ G ≥ B: облака слегка жёлтые (Mallama et al. 2017)


@pytest.mark.parametrize("lam,sigma", [(0.44, 1.1273e-26), (0.55, 4.5105e-27)])
def test_rayleigh_matches_bodhaine(lam, sigma):
    """Сечение рэлеевского рассеяния воздуха — таблица 3 Bodhaine et al. 1999 (360 ppm CO₂, у нас 300 ppm)."""
    assert ap.air_rayleigh_cross_section_cm2(lam) == pytest.approx(sigma, rel=5e-4)


def test_earth_atmosphere_from_nasa():
    atm = MAN["bodies"]["earth"]["atmosphere"]
    # оптическая толщина Рэлея у поверхности на 550 нм ≈ 0,097 (Bodhaine et al. 1999, табл. 3: 0,0973)
    assert atm["rayleigh_tau"][1] == pytest.approx(0.0973, rel=0.02)
    assert atm["mie_extinction_m"] * atm["mie_scale_height_m"] == pytest.approx(0.12)  # AOD MACv2
    assert atm["mie_ssa"] == pytest.approx(0.94)


def test_ocean_fraction():
    """Маска воды Земли (ETOPO ниже уровня моря и синий цвет дневной карты): доля площади ≈ 0,71."""
    f = MAN["bodies"]["earth"]["layers"]["ocean"]["files"]["4096"]
    a = np.asarray(Image.open(_res(f)), dtype=np.float64) / 255.0
    lat = (0.5 - (np.arange(a.shape[0]) + 0.5) / a.shape[0]) * math.pi
    w = np.cos(lat)[:, None]
    frac = float((a * w).sum() / (w.sum() * a.shape[1]))
    assert 0.66 < frac < 0.73, frac


def test_relief_normals():
    """Карты нормалей: Луна изрезана сильнее Марса, Марс — сильнее Земли (у Земли гладкий океан)."""
    tilt = {k: MAN["bodies"][k]["layers"]["normal"]["stats"]["2048"]["tilt_mean_deg"] for k in ("moon", "mars", "earth")}
    assert tilt["moon"] > tilt["mars"] > tilt["earth"] > 0, tilt
