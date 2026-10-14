"""Разбор сохранённых страниц NASA (data/raw/nasa/): Planetary Fact Sheet, JPL SSD (спутники), science.nasa.gov.

Значения не правятся и не округляются: берётся число, как написано на странице, плюс исходная строка.
"""
from __future__ import annotations

import html
import json
import re
from pathlib import Path

from kernels import ROOT

NASA = ROOT / "data" / "raw" / "nasa"
FACT_URL = "https://nssdc.gsfc.nasa.gov/planetary/factsheet/"

# поле → подпись строки таблицы на странице Fact Sheet
TABLE_FIELDS = {
    "mass_1e24kg": "Mass (1024 kg)",
    "radius_mean_km": "Volumetric mean radius (km)",
    "density_kgm3": "Mean density (kg/m3)",
    "bond_albedo": "Bond albedo",
    "geometric_albedo": "Geometric albedo",
    "blackbody_K": "Black-body temperature (K)",
    "j2_1e6": "J2 (x 10-6)",
    "rotation_h": "Sidereal rotation period (hrs)",
    "obliquity_deg": "Obliquity to orbit (deg)",
}
SUN_FIELDS = {
    "mass_1e24kg": "Mass (1024 kg)",
    "radius_mean_km": "Volumetric mean radius (km)",
    "density_kgm3": "Mean density (kg/m3)",
    "rotation_h": "Sidereal rotation period (hrs)*",
    "obliquity_deg": "Obliquity to ecliptic (deg.)",
}
# поле → подпись в текстовых строках «Подпись: значение»
TEXT_FIELDS = {
    "mean_temp_K": ["Average temperature", "Temperature at 1 bar", "Surface temperature", "Effective temperature"],
    "scale_height_km": ["Scale height"],
    "mean_molecular_weight": ["Mean molecular weight"],
    "surface_pressure": ["Surface pressure", "Surface Pressure"],
}
PAGES = ["sun", "mercury", "venus", "earth", "moon", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"]
NUM = r"[-+]?\d[\d,]*\.?\d*(?:[eE][-+]?\d+)?"


def _num(s: str) -> float | None:
    m = re.search(NUM, s.replace("−", "-"))
    return float(m.group(0).replace(",", "")) if m else None


def _strip(s: str) -> str:
    return re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", s))).strip()


def _pressure_bar(val: str) -> float | None:
    """«1014 mb», «92 bars», «~13 microbar», «<~5 x 10-15 bar» → бары. «>>1000 bars» (нет поверхности) → None."""
    if ">" in val:
        return None
    sci = re.search(rf"({NUM})\s*x\s*10\s*([-+]?\d+)", val)  # «5 x 10-15 bar»: показатель степени без <sup>
    v = float(sci.group(1)) * 10.0 ** int(sci.group(2)) if sci else _num(val)
    if v is None:
        return None
    unit = val.lower()
    if "microbar" in unit:
        return v * 1e-6
    if "mb" in unit or "millibar" in unit:
        return v * 1e-3
    if "pa" in unit and "bar" not in unit:
        return v * 1e-5
    return v


def fact_sheet(name: str) -> dict:
    """Значения со страницы <name>fact.html: {поле: {"v": число, "raw": строка, "src": "nasa_fact_<name>"}}."""
    page = (NASA / f"{name}fact.html").read_text(encoding="utf-8", errors="replace")
    rows = {}
    for m in re.finditer(r"<tr>\s*<th[^>]*>(.*?)</th>\s*<td[^>]*>(.*?)</td>", page, flags=re.S | re.I):
        rows.setdefault(_strip(m.group(1)), _strip(m.group(2)))  # ниже на странице бывают таблицы спутников
    out = {}
    text = html.unescape(re.sub(r"<[^>]+>", "", page))
    lines = [re.sub(r"\s+", " ", ln).strip() for ln in text.splitlines()]
    fields = SUN_FIELDS if name == "sun" else TABLE_FIELDS
    for key, label in fields.items():
        if label in rows and _num(rows[label]) is not None:
            out[key] = {"v": _num(rows[label]), "raw": f"{label}: {rows[label]}"}
            continue
        # страница Земли свёрстана текстом: «Подпись     значение»
        ln = next((x for x in lines if x.startswith(label + " ") and _num(x[len(label):]) is not None), None)
        if ln:
            out[key] = {"v": _num(ln[len(label):]), "raw": ln}
    for key, labels in TEXT_FIELDS.items():
        for label in labels:
            ln = next((x for x in lines if x.startswith(label + ":")), None)
            if ln:
                val = ln.split(":", 1)[1].strip()
                rec = {"raw": ln, "v": _num(val)}
                rng = re.match(rf"~?({NUM})\s*-\s*({NUM})\s*K", val)
                if key == "mean_temp_K" and rng:  # диапазон: «24 - 38 K» → середина, диапазон сохраняем
                    lo, hi = float(rng.group(1)), float(rng.group(2))
                    rec.update(v=(lo + hi) / 2, range=[lo, hi])
                if key == "surface_pressure":
                    rec["v"] = _pressure_bar(val)
                    if "<" in val:
                        rec["upper_limit"] = True
                    key = "surface_pressure_bar"
                out[key] = rec
                break
    for rec in out.values():
        rec["src"] = f"nasa_fact_{name}"
    return out


def ssd_satellites() -> dict[int, dict]:
    """Таблица JPL SSD: код NAIF → GM, средний радиус, плотность."""
    page = (NASA / "ssd_sats_phys_par.html").read_text(encoding="utf-8", errors="replace")
    body = page[page.find("<tbody>"):page.find("</tbody>")]
    out = {}
    for row in re.findall(r"<tr>(.*?)</tr>", body, flags=re.S):
        cells = [_strip(c) for c in re.split(r"<td[^>]*>", row)[1:]]
        if len(cells) < 12:
            continue
        code = int(cells[2])
        out[code] = {
            "gm": {"v": _num(cells[3]), "sigma": _num(cells[4]), "ref": cells[5], "src": "jpl_ssd_sats"},
            "radius_mean_km": {"v": _num(cells[6]), "sigma": _num(cells[7]), "src": "jpl_ssd_sats"},
            "density_gcm3": {"v": _num(cells[9]), "sigma": _num(cells[10]), "src": "jpl_ssd_sats"},
        }
    return out


SAT_PAGES = ["jovian", "saturnian", "uranian", "neptunian"]


def satellite_albedos() -> dict[str, dict]:
    """Геометрическое альбедо спутников из таблиц NASA «<система>satfact.html» (столбец «Visual geometric albedo»).

    Ключ — английское имя спутника в нижнем регистре («io», «triton»). У Япета на странице два значения
    («0.05 / 0.5» — тёмное и светлое полушария): v — их среднее, оба сохраняются в "range".
    """
    out: dict[str, dict] = {}
    for system in SAT_PAGES:
        page = (NASA / f"{system}satfact.html").read_text(encoding="utf-8", errors="replace")
        table = page[page.find("<caption>Bulk parameters"):]
        table = table[:table.find("</table>")]
        for m in re.finditer(r"<tr>\s*<th>(.*?)</th>(.*?)</tr>", table, flags=re.S):
            name = _strip(m.group(1))
            cells = [_strip(c) for c in re.findall(r"<td[^>]*>(.*?)</td>", m.group(2), flags=re.S)]
            if len(cells) < 4 or not name:
                continue
            raw = cells[3]
            vals = [float(x) for x in re.findall(r"\d+\.?\d*", raw)]
            if not vals:
                continue
            rec = {"v": sum(vals) / len(vals), "raw": f"{name}: Visual geometric albedo {raw}",
                   "src": f"nasa_fact_{system}sat"}
            if len(vals) > 1:
                rec["range"] = [min(vals), max(vals)]
            out[name.split(" ")[0].lower()] = rec
    return out


def science_dwarf(name: str) -> dict:
    """Размер со страницы science.nasa.gov: диаметр или радиус в километрах (как написано на странице)."""
    t = (NASA / f"science_{name}.html").read_text(encoding="utf-8", errors="replace")
    t = re.sub(r"<script.*?</script>|<style.*?</style>", "", t, flags=re.S)
    t = re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", " ", t)))
    out = {}
    m = re.search(r"(equatorial diameter|radius) of (?:about |approximately )?[\d,]+ miles \((?:about )?([\d,]+) kilometers\)", t)
    if m:
        km = float(m.group(2).replace(",", ""))
        radius = km / 2 if "diameter" in m.group(1) else km
        out["radius_mean_km"] = {"v": radius, "raw": m.group(0), "approx": True, "src": f"nasa_science_{name}"}
    return out


def _range(s: str) -> list[float] | None:
    """«0.05 - 0.35» → [0.05, 0.35]; «0.2» → [0.2, 0.2]; «10<sup>-5</sup>» после очистки «10-5» → [1e-5, 1e-5]."""
    m = re.fullmatch(r"10-(\d+)", s)
    if m:
        return [10.0 ** -int(m.group(1))] * 2
    m = re.fullmatch(rf"({NUM})\s*-\s*({NUM})", s)
    if m:
        return [_num(m.group(1)), _num(m.group(2))]
    v = _num(s) if re.fullmatch(NUM, s) else None
    return [v, v] if v is not None else None


def saturn_rings() -> list[dict]:
    """Кольца Сатурна со страницы satringfact.html: [{name, inner_km, outer_km, optical_depth, albedo, src}].

    Берутся кольца, у которых на странице есть оба края («X inner edge» и «X outer edge»). Оптическая толщина
    и альбедо — диапазоны [мин, макс], как на странице; у края без значения — None.
    """
    page = (NASA / "satringfact.html").read_text(encoding="utf-8", errors="replace")
    rows = {}
    for m in re.finditer(r"<tr>\s*<th[^>]*>(.*?)</th>(.*?)</tr>", page, flags=re.S | re.I):
        cells = [_strip(c) for c in re.findall(r"<td[^>]*>(.*?)</td>", m.group(2), flags=re.S | re.I)]
        if cells and _num(cells[0]) is not None:
            rows[_strip(m.group(1))] = cells
    out = []
    for label, cells in rows.items():
        if not label.endswith(" inner edge"):
            continue
        name = label[:-len(" inner edge")]
        outer = rows.get(f"{name} outer edge")
        if outer is None:
            continue
        out.append({"name": name, "inner_km": _num(cells[0]), "outer_km": _num(outer[0]),
                    "optical_depth": _range(cells[2]) if len(cells) > 2 else None,
                    "albedo": _range(cells[3]) if len(cells) > 3 else None,
                    "raw": {"inner": " | ".join([label] + cells), "outer": " | ".join([f"{name} outer edge"] + outer)},
                    "src": "nasa_fact_satring"})
    return out


def sources() -> dict:
    log = json.loads((NASA / "fetch_log.json").read_text(encoding="utf-8"))
    src = {}
    for fname, rec in log.items():
        if fname == "satringfact.html":
            key, title = "nasa_fact_satring", "NASA Saturnian Rings Fact Sheet (NSSDCA)"
        elif fname.endswith("satfact.html"):
            key = "nasa_fact_" + fname[:-len("fact.html")]
            title = "NASA Planetary Fact Sheet (NSSDCA), спутники: " + fname[:-len("satfact.html")]
        elif fname.endswith("fact.html") and fname != "factsheet_index.html":
            key = "nasa_fact_" + fname[:-len("fact.html")]
            title = "NASA Planetary Fact Sheet (NSSDCA)"
        elif fname == "ssd_sats_phys_par.html":
            key, title = "jpl_ssd_sats", "JPL SSD, Planetary Satellite Physical Parameters"
        elif fname.startswith("science_"):
            key, title = "nasa_science_" + fname[8:-5], "NASA Science, Dwarf Planets"
        else:
            continue
        src[key] = {"title": title, "url": rec["url"], "fetched_utc": rec["fetched_utc"]}
    return src
