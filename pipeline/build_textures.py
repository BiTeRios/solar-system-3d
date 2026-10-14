"""Карты поверхностей для игры: game/assets/maps/<тело>/*.jpg|png и манифест game/data/appearance.json.

Все карты приводятся к одной раскладке — равнопромежуточная проекция (широта и долгота линейны), левый край —
долгота −180° (восточная), долгота растёт вправо, верх — северный полюс (+90°). Широта планетоцентрическая,
кроме Земли: у карт Земли (Blue Marble в Solar System Scope, ETOPO) широта геодезическая — шейдер это учитывает.

Откуда карты (разрешения 11.10.2026 и 13.10.2026, подробно — docs/sources.md):
- Solar System Scope (CC BY 4.0) — копии из справочного проекта: Земля (день, ночь, облака), Луна, Меркурий, Марс,
  Юпитер, Сатурн, Уран, Нептун;
- USGS Astrogeology (общественное достояние) — спутники, Плутон, Харон, Церера, Фобос;
- JPL Solar System Simulator (NASA/JPL-Caltech) — Деймос и спутники Урана, 1440×720, без геопривязки:
  раскладка серии проверена сравнением карты Фобоса JPL с картой USGS (см. registration_jpl());
- рельеф: LOLA (Луна), MOLA (Марс), MESSENGER/USGS (Меркурий), ETOPO 2022 (Земля) → карты нормалей.

Яркость: средняя по площади линейная яркость карты приводится к ламбертову альбедо A = 1,5·p (p — геометрическое
альбедо тела из bodies.json; для ламбертова шара p = 2A/3), но не больше 1. Множитель пишется в манифест,
сама карта не меняется. Земля — без множителя: карта «Blue Marble» уже в отражательной способности, облака отдельно.

    python -I pipeline/build_textures.py           # всё
    python -I pipeline/build_textures.py io moon   # только эти тела (манифест пересобирается целиком)
"""
from __future__ import annotations

import datetime as dt
import io
import json
import math
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import appearance_params as ap  # noqa: E402
from kernels import ROOT  # noqa: E402

Image.MAX_IMAGE_PIXELS = None

RAW = ROOT / "data" / "raw"
SSS = Path(r"D:\work\Vanya\space\godot\assets\textures")  # справочный проект: карты Solar System Scope
OUT = ROOT / "game" / "assets" / "maps"
DATA = ROOT / "game" / "data"
BUILD = ROOT / "build"
HI_MAX = 8192  # ширина «8K»
LO_W = 2048    # ширина «2K»

SOURCES = {
    "sss": {"title": "Solar System Scope, текстуры планет (Textures by Solar System Scope)",
            "url": "https://www.solarsystemscope.com/textures/", "license": "CC BY 4.0",
            "note": "копии из справочного проекта D:/work/Vanya/space/godot/assets/textures (разрешено 11.10.2026)"},
    "usgs_mosaic": {"title": "USGS Astrogeology Science Center, глобальные мозаики (planetarymaps.usgs.gov/mosaic)",
                    "url": "https://planetarymaps.usgs.gov/mosaic/", "license": "общественное достояние (USGS/NASA)"},
    "jpl_tmaps": {"title": "JPL Solar System Simulator, texture maps (Caltech/JPL/USGS, Voyager/Viking)",
                  "url": "https://maps.jpl.nasa.gov/tmaps/", "license": "правила JPL: с указанием NASA/JPL-Caltech",
                  "note": "без геопривязки; раскладка проверена по карте Фобоса той же серии (mar1kuu2.tif)"},
    "pds_lola": {"title": "LRO LOLA GDR, LDEM_16 (PDS Geosciences Node)", "license": "общественное достояние (NASA)",
                 "url": "https://pds-geosciences.wustl.edu/lro/lro-l-lola-3-rdr-v1/lrolol_1xxx/data/lola_gdr/cylindrical/img/"},
    "pds_mola": {"title": "MGS MOLA MEGDR, megt90n000eb, 16 px/° (PDS Geosciences Node)", "license": "общественное достояние (NASA)",
                 "url": "https://pds-geosciences.wustl.edu/mgs/mgs-m-mola-5-megdr-l3-v1/mgsl_300x/meg016/"},
    "usgs_mercury_dem": {"title": "USGS Mercury MESSENGER Global DEM 665 m v2", "license": "общественное достояние (USGS/NASA)",
                         "url": "https://planetarymaps.usgs.gov/mosaic/Mercury_Messenger_USGS_DEM_Global_665m_v2_32bit_clon0.tif"},
    "noaa_etopo2022": {"title": "NOAA NCEI ETOPO 2022, 60″, ice surface (doi:10.25921/fd45-gt74)",
                       "license": "общественное достояние (NOAA)",
                       "url": "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/60s_surface_elev_gtif/"},
}

# Карты цвета. lon0 — восточная долгота центра проекции (по метке PDS3: CENTER_LONGITUDE, у западных — со знаком
# минус), левый край вычисляется по точке привязки GeoTIFF: L = lon0 + x_left/R. У JPL и SSS — левый край −180°.
USGS = {
    #          файл                                                      lon0   примечание к метке PDS3
    "io": ("Io_GalileoSSI-Voyager_Global_Mosaic_ClrMerge_1km.tif", 0.0, "WEST, CENTER 0"),
    "europa": ("Europa_Voyager_GalileoSSI_global_mosaic_500m.tif", -180.0, "WEST, CENTER 180"),
    "ganymede": ("Ganymede_Voyager_GalileoSSI_Global_ClrMosaic_1435m.tif", 180.0, "EAST, CENTER 180"),
    "callisto": ("Callisto_Voyager_GalileoSSI_global_mosaic_1km.tif", -180.0, "WEST, CENTER 180"),
    "enceladus": ("Enceladus_Cassini_mosaic_global_110m.tif", -180.0, "WEST, CENTER 180"),
    "tethys": ("Tethys_Cassini_mosaic_global_293m.tif", 0.0, "EAST, CENTER 0"),
    "dione": ("Dione_Cassini_Voyager_mosaic_global_154m.tif", 0.0, "EAST, CENTER 0"),
    "rhea": ("Rhea_Cassini_Voyager_mosaic_global_417m.tif", -180.0, "WEST, CENTER 180"),
    "titan": ("Titan_ISS_P19658_Mosaic_Global_4km.tif", -180.0, "WEST, CENTER 180"),
    "iapetus": ("Iapetus_Cassini_Voyager_mosaic_global_783m.tif", 0.0, "WEST, CENTER 0"),
    "pluto": ("Pluto_NewHorizons_Global_Mosaic_300m_Jul2017_8bit.tif", 180.0, "EAST, CENTER 180"),
    "charon": ("Charon_NewHorizons_Global_Mosaic_300m_Jul2017_8bit.tif", 0.0, "EAST, CENTER 0"),
    "triton": ("Triton_Voyager2_ClrMosaic_GlobalFill_600m.tif", 0.0, "EAST, CENTER 0"),
    "ceres": ("Ceres_Dawn_FC_DLR_global_20ppd_Oct2015.tif", 180.0, "EAST, CENTER 180"),
    "phobos": ("Phobos_ME_SRC_Mosaic_Global_16ppd.tif", 0.0, "EAST, CENTER 0"),
}
JPL = {"deimos": "mar2kuu2.tif", "ariel": "ura1vuu2.tif", "umbriel": "ura2vuu2.tif", "titania": "ura3vuu2.tif",
       "oberon": "ura4vuu2.tif", "miranda": "ura5vuu2.tif"}
SSS_MAPS = {  # тело: {слой: имя файла в 8k/ или 2k/}
    "earth": {"color": "earth_day", "night": "earth_night", "clouds": "earth_clouds"},
    "moon": {"color": "moon"}, "mercury": {"color": "mercury"}, "mars": {"color": "mars"},
    "jupiter": {"color": "jupiter"}, "saturn": {"color": "saturn"}, "uranus": {"color": "uranus"},
    "neptune": {"color": "neptune"},
}
NO_MAP = {
    "eris": "глобальных снимков поверхности нет (только наземные наблюдения)",
    "haumea": "глобальных снимков поверхности нет (только наземные наблюдения)",
    "makemake": "глобальных снимков поверхности нет (только наземные наблюдения)",
    "halley": "ядро снято вблизи только «Джотто» и «Вегами» в 1986 году, глобальной карты нет",
}

# Известные детали поверхности для проверки привязки: (тело, название, широта, восточная долгота, ожидание).
# Координаты — IAU/USGS Gazetteer of Planetary Nomenclature (центр детали); «bright»/«dark» — ярче/темнее окрестностей.
FEATURES = [
    ("io", "Пеле (кольцо выбросов)", -18.7, -255.3 + 360, "red"),
    ("europa", "кратер Пвилл", -25.2, -271.4 + 360, "bright"),
    ("callisto", "Вальхалла (центр)", 14.7, -56.0 + 360, "bright"),
    ("ganymede", "область Галилея", 35.0, -145.0 + 360, "dark"),
    ("iapetus", "область Кассини", 0.0, -90.0, "dark"),
    ("rhea", "кратер Инктоми (лучи)", -14.1, -112.1, "bright"),
    ("mimas", "кратер Гершель", -1.4, -111.8, "crater"),
    ("tethys", "кратер Одиссей", 32.8, -128.9, "crater"),
    ("pluto", "равнина Спутника", 20.0, 175.0, "bright"),
    ("ceres", "кратер Оккатор (яркие пятна)", 19.8, 239.3, "bright"),
    ("phobos", "кратер Стикни", 1.0, -49.0, "crater"),
    ("earth", "Сахара", 23.0, 13.0, "bright"),
    ("earth", "Тихий океан", 0.0, -150.0, "dark"),
    ("moon", "Море Кризисов", 17.0, 59.1, "dark"),
    ("moon", "кратер Тихо (лучи)", -43.3, -11.2, "bright"),
    ("mars", "Большой Сирт", 8.4, 69.5, "dark"),
]


# ---------------------------------------------------------------- общие функции

def srgb_to_linear(x: np.ndarray) -> np.ndarray:
    x = x.astype(np.float64) / 255.0
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def area_weights(h: int) -> np.ndarray:
    lat = (0.5 - (np.arange(h) + 0.5) / h) * math.pi
    return np.cos(lat)


def mean_linear(img: Image.Image, valid: np.ndarray | None = None, encoding: str = "srgb") -> list[float]:
    """Средний линейный цвет карты с весом cos(широта); valid — маска пикселей с данными (того же размера).

    encoding: "srgb" — значения закодированы как sRGB (снимки для показа), "linear" — пропорциональны I/F.
    """
    a = np.asarray(img.convert("RGB"))
    lin = srgb_to_linear(a) if encoding == "srgb" else a.astype(np.float64) / 255.0
    w = np.repeat(area_weights(a.shape[0])[:, None], a.shape[1], axis=1)
    if valid is not None:
        w = w * valid
    return [float((lin[..., c] * w).sum() / w.sum()) for c in range(3)]


def luminance(rgb: list[float]) -> float:
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def shift_columns(a: np.ndarray, shift_px: float) -> np.ndarray:
    """Циклический сдвиг столбцов на дробное число пикселей вправо (линейная интерполяция)."""
    w = a.shape[1]
    k = math.floor(shift_px)
    f = shift_px - k
    b = np.roll(a, k, axis=1).astype(np.float64)
    if f > 1e-9:
        b = (1 - f) * b + f * np.roll(b, 1, axis=1)
    return b


def canonical(img: Image.Image, left_lon: float, width: int) -> Image.Image:
    """Ресемплинг в раскладку игры: ширина width, высота width/2, левый край −180°.

    Сначала площадное уменьшение (BOX: края пикселей переходят линейно), потом циклический сдвиг по долготе.
    """
    h = width // 2
    mode = img.mode
    small = img.resize((width, h), Image.BOX) if img.size != (width, h) else img
    a = np.asarray(small)
    shift = ((left_lon + 180.0) / 360.0 % 1.0) * width  # где окажется столбец с долготой left_lon
    b = shift_columns(a, shift)
    return Image.fromarray(np.clip(np.rint(b), 0, 255).astype(np.uint8), mode)


def fill_nodata(img: Image.Image) -> tuple[Image.Image, np.ndarray]:
    """Пиксели без данных (0 во всех каналах) заливаются средним цветом снятой части. Возвращает карту и маску данных."""
    a = np.asarray(img).copy()
    valid = (a > 0) if a.ndim == 2 else (a.max(axis=2) > 0)
    if valid.all():
        return img, valid
    w = np.repeat(area_weights(a.shape[0])[:, None], a.shape[1], axis=1) * valid
    if a.ndim == 2:
        a[~valid] = np.uint8(round(float((a * w).sum() / w.sum())))
    else:
        for c in range(a.shape[2]):
            a[..., c][~valid] = np.uint8(round(float((a[..., c] * w).sum() / w.sum())))
    return Image.fromarray(a, img.mode), valid


def geotiff_left_lon(img: Image.Image, lon0: float, radius_m: float | None = None) -> float:
    """Восточная долгота левого края по точке привязки (ModelTiepoint) и центру проекции lon0."""
    tie = img.tag_v2[33922]
    x_left = tie[3]
    geo = img.tag_v2.get(34737, "")
    if "GCS Name" in geo and "SimpleCylindrical" not in geo and "Equirectangular" not in geo and "EQUIDISTANT" not in geo:
        return x_left  # географическая система: x — уже долгота в градусах
    r = radius_m
    if r is None:
        # шаг пикселя × ширина = 2πR
        r = img.tag_v2[33550][0] * img.size[0] / (2 * math.pi)
    return lon0 + math.degrees(x_left / r)


def save_levels(body: str, layer: str, img: Image.Image, ext: str = "jpg") -> dict:
    """Уровни детализации: «2K» (не шире 2048) и «8K» (не шире 8192, но не больше исходной ширины)."""
    d = OUT / body
    d.mkdir(parents=True, exist_ok=True)
    src_w = img.size[0]
    widths = sorted({min(LO_W, src_w), min(HI_MAX, src_w)})
    files = {}
    for w in widths:
        w4 = w - w % 4
        lvl = img if img.size == (w4, w4 // 2) else img.resize((w4, w4 // 2), Image.LANCZOS)
        name = f"{layer}_{w4}.{ext}"
        if ext == "jpg":
            extra = {"subsampling": 0} if lvl.mode == "RGB" else {}
            lvl.save(d / name, quality=92, optimize=True, **extra)
        else:
            lvl.save(d / name, optimize=True)
        files[str(w4)] = f"res://assets/maps/{body}/{name}"
    return files


# ---------------------------------------------------------------- источники

def load_usgs(body: str) -> tuple[Image.Image, float, str]:
    fname, lon0, note = USGS[body]
    img = Image.open(RAW / "maps" / fname)
    left = geotiff_left_lon(img, lon0)
    img.load()
    return img, left, f"{fname}; метка PDS3: {note}; левый край {left:+.3f}°"


def load_mimas() -> tuple[Image.Image, float, str]:
    z = zipfile.ZipFile(RAW / "maps" / "Cassini_DLR_Mimas.zip")
    name = "Cassini_DLR/MI_170630_DLR_basemap_degrees.tif"
    img = Image.open(io.BytesIO(z.read(name)))
    left = img.tag_v2[33922][3]  # географическая система «Mimas 2009», x — долгота
    img.load()
    return img, left, f"Cassini_DLR_Mimas.zip/{name}; левый край {left:+.3f}°"


def load_jpl(body: str) -> tuple[Image.Image, float, str]:
    img = Image.open(RAW / "maps" / JPL[body])
    img.load()
    return img, -180.0, f"{JPL[body]}; левый край −180° (как у серии, см. registration_jpl)"


def load_sss(name: str) -> tuple[Image.Image, str]:
    for folder in ("8k", "2k"):
        p = SSS / folder / f"{name}.jpg.bin"
        if p.exists():
            img = Image.open(p)
            img.load()
            return img, f"{folder}/{name}.jpg ({img.size[0]}×{img.size[1]})"
    raise FileNotFoundError(name)


def registration_jpl() -> dict:
    """Сдвиг и зеркальность карт JPL относительно USGS — по Фобосу: корреляция градиентов на широтах ±60°."""
    from PIL import ImageFilter

    def prep(im: Image.Image) -> np.ndarray:
        im = im.convert("L").resize((720, 360), Image.BILINEAR).filter(ImageFilter.GaussianBlur(1.5))
        x = np.asarray(im, dtype=float)
        gy, gx = np.gradient(x)
        g = np.hypot(gx, gy)[60:300]
        return (g - g.mean()) / g.std()

    usgs, left_u, _ = load_usgs("phobos")
    usgs = canonical(usgs.convert("L"), left_u, 1440)
    a = prep(Image.open(RAW / "maps" / "mar1kuu2.tif"))
    b = prep(usgs)
    res = {}
    for flip in (False, True):
        aa = a[:, ::-1] if flip else a
        cc = np.real(np.fft.ifft(np.fft.fft(aa, axis=1) * np.conj(np.fft.fft(b, axis=1)), axis=1)).sum(0) / aa.size
        k = int(np.argmax(cc))
        res["mirror" if flip else "direct"] = {"corr": float(cc[k]), "shift_deg": (k if k <= 360 else k - 720) * 0.5}
    return res


# ---------------------------------------------------------------- рельеф → нормали

def dem_array(body: str) -> tuple[np.ndarray, float, float, str, str]:
    """Высоты (м), левый край (° в. д.), радиус (м), источник, тип широты."""
    if body == "moon":
        a = np.fromfile(RAW / "dem" / "ldem_16.img", dtype="<i2").reshape(2880, 5760).astype(np.float32) * 0.5
        return a, 0.0, 1737400.0, "pds_lola", "centric"
    if body == "mars":
        a = np.fromfile(RAW / "dem" / "megt90n000eb.img", dtype=">i2").reshape(2880, 5760).astype(np.float32)
        return a, 0.0, 3396000.0, "pds_mola", "centric"
    if body == "mercury":
        img = Image.open(RAW / "dem" / "Mercury_Messenger_USGS_DEM_Global_665m_v2_32bit_clon0.tif")
        a = np.asarray(img, dtype=np.float32)
        a = np.where(a <= -32768, np.nan, a)
        left = geotiff_left_lon(img, 0.0, 2439400.0)
        return a, left, 2439400.0, "usgs_mercury_dem", "centric"
    if body == "earth":
        img = Image.open(RAW / "dem" / "ETOPO_2022_v1_60s_N90W180_surface.tif")
        a = np.asarray(img, dtype=np.float32)
        return a, -180.0, 6371000.0, "noaa_etopo2022", "geodetic"
    raise KeyError(body)


def resample_dem(a: np.ndarray, left: float, width: int) -> np.ndarray:
    """Высоты в раскладку игры (площадное уменьшение, затем сдвиг по долготе); пропуски — среднее по строке."""
    if np.isnan(a).any():
        row_mean = np.nanmean(a, axis=1, keepdims=True)
        a = np.where(np.isnan(a), row_mean, a)
    img = Image.fromarray(a.astype(np.float32), "F").resize((width, width // 2), Image.BOX)
    b = np.asarray(img, dtype=np.float64)
    return shift_columns(b, ((left + 180.0) / 360.0 % 1.0) * width)


def normal_map(h: np.ndarray, radius_m: float) -> np.ndarray:
    """Нормаль в местном базисе (восток, север, вверх) по высотам: n = (−∂h/∂x, −∂h/∂y, 1) нормированная."""
    rows, cols = h.shape
    dlon = 2 * math.pi / cols
    dlat = math.pi / rows
    lat = (0.5 - (np.arange(rows) + 0.5) / rows) * math.pi
    coslat = np.maximum(np.cos(lat), 1e-3)[:, None]
    dhdx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) / (2 * radius_m * dlon * coslat)
    north = np.vstack([h[:1], h[:-1]])   # строка выше — севернее
    south = np.vstack([h[1:], h[-1:]])
    dhdy = (north - south) / (2 * radius_m * dlat)
    n = np.stack([-dhdx, -dhdy, np.ones_like(h)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n


def save_normals(body: str, h_full: np.ndarray, left: float, radius_m: float) -> dict:
    d = OUT / body
    d.mkdir(parents=True, exist_ok=True)
    src_w = h_full.shape[1]
    files = {}
    stats = {}
    for w in sorted({min(LO_W, src_w), min(HI_MAX, src_w)}):
        w4 = w - w % 4
        hh = resample_dem(h_full, left, w4)
        n = normal_map(hh, radius_m)
        rgb = np.clip(np.rint((n * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8)
        name = f"normal_{w4}.png"
        Image.fromarray(rgb, "RGB").save(d / name, optimize=True)
        files[str(w4)] = f"res://assets/maps/{body}/{name}"
        tilt = np.degrees(np.arccos(np.clip(n[..., 2], -1, 1)))
        stats[str(w4)] = {"tilt_mean_deg": float(tilt.mean()), "tilt_p99_deg": float(np.percentile(tilt, 99))}
    return {"files": files, "stats": stats}


# ---------------------------------------------------------------- сборка

def write_import(res_path: str, normal: bool = False) -> None:
    """Файл .import рядом с картой: сжатие для видеокарты (S3TC/BPTC) с мип-уровнями, нормали — BC5 (RGTC).

    Godot дописывает в файл остальные поля; если наши значения там уже стоят, файл не трогаем (иначе переимпорт).
    """
    p = ROOT / "game" / res_path.replace("res://", "")
    params = {"compress/mode": "2", "compress/high_quality": "false", "compress/normal_map": "1" if normal else "2",
              "compress/channel_pack": "0", "mipmaps/generate": "true", "mipmaps/limit": "-1",
              "process/fix_alpha_border": "true", "process/size_limit": "0", "detect_3d/compress_to": "0"}
    imp = p.with_suffix(p.suffix + ".import")
    if imp.exists():
        have = dict(ln.split("=", 1) for ln in imp.read_text(encoding="utf-8").splitlines() if "=" in ln)
        if all(have.get(k) == v for k, v in params.items()):
            return
    lines = ["[remap]", "", 'importer="texture"', 'type="CompressedTexture2D"', "", "[params]", ""]
    lines += [f"{k}={v}" for k, v in params.items()] + [""]
    imp.write_text("\n".join(lines), encoding="utf-8")


def body_albedo(rec: dict) -> dict | None:
    ph = rec.get("phys", {})
    g = ph.get("geometric_albedo")
    if g is None or g.get("v") is None:
        return None
    return {"p_v": g["v"], "src": g["src"], "raw": g.get("raw") or g.get("quote")}


def build(only: list[str]) -> dict:
    bodies = json.loads((DATA / "bodies.json").read_text(encoding="utf-8"))["bodies"]
    by_key = {b["key"]: b for b in bodies}
    old = {}
    man_path = DATA / "appearance.json"
    if only and man_path.exists():
        old = json.loads(man_path.read_text(encoding="utf-8")).get("bodies", {})
    out: dict = {}
    for b in bodies:
        key = b["key"]
        if not b["visible"]:
            continue
        if only and key not in only and key in old:
            out[key] = old[key]
            continue
        print(f"{key} …", flush=True)
        rec: dict = {"layers": {}}
        alb = body_albedo(b)
        a_target = min(1.0, 1.5 * alb["p_v"]) if alb else None
        # ---- цвет
        img, left, note, src = None, None, "", None
        if key in USGS:
            img, left, note = load_usgs(key)
            src = "usgs_mosaic"
        elif key == "mimas":
            img, left, note = load_mimas()
            src = "usgs_mosaic"
        elif key in JPL:
            img, left, note = load_jpl(key)
            src = "jpl_tmaps"
        elif key in SSS_MAPS:
            img, note = load_sss(SSS_MAPS[key]["color"])
            left, src = -180.0, "sss"
        if key == "sun":
            rec["emissive"] = True
            rec["note"] = "поверхность процедурная: грануляция — шум, потемнение к краю и корона — по статьям"
            rec["limb_darkening"] = {k: ap.SUN_LIMB[k] for k in ("a0", "a1", "src")}
            rec["corona"] = {k: ap.SUN_CORONA[k] for k in ("terms", "unit", "src")}
        elif key == "venus":
            va = ap.VENUS_BAND_ALBEDO
            rgb = [1.5 * va["R"], 1.5 * va["V"], 1.5 * va["B"]]
            m = max(rgb)
            rec["uniform_rgb"] = [c / m if m > 1 else c for c in rgb]
            rec["note"] = "облачный слой: цвет по геометрическому альбедо в полосах R, V, B (Mallama et al. 2017)"
            rec["albedo"] = {"p_v": va["V"], "src": va["src"], "bands": {k: va[k] for k in ("B", "V", "R")}}
        elif img is None:
            rec["no_map"] = NO_MAP.get(key, "карты нет")
            if alb:
                rec["uniform_rgb"] = [a_target] * 3
                rec["albedo"] = {**alb, "lambert": a_target}
            else:
                rec["uniform_rgb"] = [0.5, 0.5, 0.5]
                rec["albedo"] = None
                rec["no_albedo"] = "альбедо нет в ядрах, таблицах NASA и выбранных статьях: серый 0,5 условно"
        else:
            grey = img.mode not in ("RGB", "RGBA")
            img = img.convert("L" if grey else "RGB")
            hi_w = min(HI_MAX, img.size[0])
            hi_w -= hi_w % 4
            canon = canonical(img, left, hi_w)
            canon, valid = fill_nodata(canon)
            # Серые мозаики USGS — данные: DN линейно связаны с I/F (у Европы в метке прямо SCALE/OFFSET к I/F).
            # Цветные композиты, SSS и JPL — изображения для показа, закодированы как sRGB.
            encoding = "linear" if (src == "usgs_mosaic" and grey) else "srgb"
            mean = mean_linear(canon, valid, encoding)
            files = save_levels(key, "color", canon)
            for f in files.values():
                write_import(f)
            layer = {"files": files, "src": src, "origin": note, "grey": grey, "left_lon_deg": left,
                     "lat": "geodetic" if key == "earth" else "centric",
                     "coverage": float((valid * area_weights(valid.shape[0])[:, None]).sum() /
                                       (area_weights(valid.shape[0]).sum() * valid.shape[1])),
                     "mean_linear_rgb": mean, "encoding": encoding}
            rec["layers"]["color"] = layer
            if key == "earth":
                rec["albedo"] = {**(alb or {}), "scale": 1.0,
                                 "note": "Blue Marble — отражательная способность поверхности; облака отдельным слоем"}
            elif alb:
                y = luminance(mean)
                rec["albedo"] = {**alb, "lambert": a_target, "map_mean_luminance": y, "scale": a_target / y}
            else:
                rec["albedo"] = {"scale": 1.0}
                rec["no_albedo"] = "альбедо нет в таблицах NASA: яркость как в исходной карте"
            if layer["coverage"] < 0.999:
                layer["fill_note"] = "области без снимков залиты средним цветом снятой части"
        # ---- Земля: ночь, облака, океан
        if key == "earth":
            for lay in ("night", "clouds"):
                im, nt = load_sss(SSS_MAPS["earth"][lay])
                im = im.convert("L" if lay == "clouds" else "RGB")
                c = canonical(im, -180.0, min(HI_MAX, im.size[0]))
                files = save_levels(key, lay, c)
                for f in files.values():
                    write_import(f)
                rec["layers"][lay] = {"files": files, "src": "sss", "origin": nt, "grey": lay == "clouds",
                                      "lat": "geodetic", "left_lon_deg": -180.0}
        # ---- рельеф
        if key in ("moon", "mars", "mercury", "earth"):
            h, left_d, r_m, dsrc, lat_t = dem_array(key)
            if key == "earth":
                ocean_src = h
                h = np.maximum(h, 0.0)  # море гладкое
            nm = save_normals(key, h, left_d, r_m)
            for f in nm["files"].values():
                write_import(f, normal=True)
            rec["layers"]["normal"] = {"files": nm["files"], "src": dsrc, "lat": lat_t, "stats": nm["stats"],
                                       "radius_m": r_m}
            if key == "earth":
                # вода для блеска: ниже уровня моря по ETOPO и синяя на дневной карте (иначе блестели бы впадины суши)
                w = 4096
                hh = resample_dem(ocean_src, -180.0, w)
                day = np.asarray(canonical(load_sss("earth_day")[0].convert("RGB"), -180.0, w), dtype=np.int16)
                blue = (day[..., 2] > day[..., 0] + 10) & (day[..., 2] > day[..., 1])
                mask = ((hh <= 0.0) & blue).astype(np.uint8) * 255
                (OUT / key).mkdir(parents=True, exist_ok=True)
                Image.fromarray(mask, "L").save(OUT / key / "ocean_4096.png", optimize=True)
                rp = f"res://assets/maps/{key}/ocean_4096.png"
                write_import(rp)
                rec["layers"]["ocean"] = {"files": {"4096": rp}, "src": ["noaa_etopo2022", "sss"], "lat": "geodetic",
                                          "fraction": float((mask > 0).mean())}
        # ---- атмосфера
        ph = b.get("phys", {})
        if key == "earth":
            atm = ap.earth_atmosphere(ph["surface_pressure_bar"]["v"], ph["mean_temp_K"]["v"], ph["scale_height_km"]["v"])
            atm["src"] = {"rayleigh": ["lit_bodhaine1999", "nasa_fact_earth"], "aerosol": ["lit_kinne2019", "lit_bruneton2008"]}
            atm["rgb_um"] = list(ap.RGB_UM)
            atm["top_m"] = 12 * atm["rayleigh_scale_height_m"]
            rec["atmosphere"] = atm
        elif key == "mars":
            rec["atmosphere_note"] = "не рисуется: для пыли нет данных, для CO₂ — формулы показателя преломления (см. docs/sources.md)"
        elif key == "titan":
            rec["atmosphere_note"] = "дымка не рисуется: нет альбедо в красной полосе; карта — ИК-снимок поверхности (938 нм)"
        out[key] = rec
    return out


def check_sheet(man: dict) -> list[dict]:
    """Значения карт в точках известных деталей (для теста и для глаз): build/maps_check.png."""
    from PIL import ImageDraw
    res = []
    tiles = []
    for body, name, lat, lon, expect in FEATURES:
        lay = man.get(body, {}).get("layers", {}).get("color")
        if not lay:
            continue
        w = max(int(k) for k in lay["files"])
        img = Image.open(ROOT / "game" / lay["files"][str(w)].replace("res://", "")).convert("RGB")
        a = np.asarray(img, dtype=np.float64)
        h = a.shape[0]
        lon_e = (lon + 180.0) % 360.0 - 180.0
        x = (lon_e + 180.0) / 360.0 * w
        y = (90.0 - lat) / 180.0 * h
        r = max(2, int(w / 360 * 2))  # окно ±2°
        xs = np.arange(int(x) - r, int(x) + r + 1) % w
        ys = np.clip(np.arange(int(y) - r, int(y) + r + 1), 0, h - 1)
        win = a[np.ix_(ys, xs)]
        R = 5 * r  # окрестность ±10°
        xs2 = np.arange(int(x) - R, int(x) + R + 1) % w
        ys2 = np.clip(np.arange(int(y) - R, int(y) + R + 1), 0, h - 1)
        ring = a[np.ix_(ys2, xs2)]
        lum_c, lum_r = win.mean(), ring.mean()
        red_c = float(win[..., 0].mean() - win[..., 2].mean())
        red_r = float(ring[..., 0].mean() - ring[..., 2].mean())
        res.append({"body": body, "feature": name, "lat": lat, "lon_e": lon_e, "expect": expect,
                    "center": float(lum_c), "around": float(lum_r), "red_center": red_c, "red_around": red_r})
        crop = img.crop((int(x) - 4 * R, int(y) - 2 * R, int(x) + 4 * R, int(y) + 2 * R)).resize((320, 160))
        d = ImageDraw.Draw(crop)
        d.line([(150, 80), (170, 80)], fill=(255, 0, 0))
        d.line([(160, 70), (160, 90)], fill=(255, 0, 0))
        d.text((4, 2), f"{body}: {expect}", fill=(255, 255, 0))
        tiles.append(crop)
    if tiles:
        cols = 4
        sheet = Image.new("RGB", (320 * cols, 160 * ((len(tiles) + cols - 1) // cols)))
        for i, t in enumerate(tiles):
            sheet.paste(t, ((i % cols) * 320, (i // cols) * 160))
        BUILD.mkdir(exist_ok=True)
        sheet.save(BUILD / "maps_check.png")
    return res


def main() -> int:
    only = sys.argv[1:]
    bodies = build(only)
    jpl = registration_jpl()
    man = {"created_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
           "layout": "равнопромежуточная: левый край −180° в. д., долгота растёт вправо, верх +90°",
           "lambert_from_geometric": 1.5,
           "rgb_wavelengths_um": list(ap.RGB_UM),
           "jpl_registration": jpl,
           "bodies": bodies,
           "sources": {**SOURCES, **ap.SOURCES}}
    man["features"] = check_sheet(bodies)
    (DATA / "appearance.json").write_text(json.dumps(man, ensure_ascii=False, indent=1), encoding="utf-8")
    print("записано game/data/appearance.json; проверка привязки — build/maps_check.png")
    print("JPL против USGS (Фобос):", jpl)
    for f in man["features"]:
        print(f"  {f['body']:9s} {f['feature']:28s} {f['expect']:6s} центр {f['center']:6.1f} вокруг {f['around']:6.1f}"
              f"  красный {f['red_center']:+6.1f}/{f['red_around']:+6.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
