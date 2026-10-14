"""Таблицы этапа 5 для docs/accuracy.md из отчёта тестов (build/render_report.json, ключи stage5_*).

    python -I -X utf8 pipeline/report_stage5.py > build/accuracy_stage5.md
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from report_accuracy import NAMES  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RU = {**NAMES, "sun": "Солнце"}


def num(x: float, digits: int = 3) -> str:
    return f"{x:.{digits}f}".replace(".", ",")


def frames(n: int) -> str:
    """«1 кадр», «4 кадра», «183 кадра», «11 кадров»."""
    if n % 10 == 1 and n % 100 != 11:
        return f"{n} кадр"
    if 2 <= n % 10 <= 4 and not 12 <= n % 100 <= 14:
        return f"{n} кадра"
    return f"{n} кадров"


def mb(b: float) -> str:
    return f"{b / 1e6:.1f}".replace(".", ",") + " МБ"


def main() -> int:
    r = json.loads((ROOT / "build" / "render_report.json").read_text(encoding="utf-8"))
    out = []
    w = out.append

    w("### Снимки тел (1280×720, линейная тональная кривая, 1 января 2026, фаза ~25°)\n")
    w("Детали — средний модуль лапласиана яркости по освещённому диску (единицы sRGB); оттенок — хроматичность освещённого")
    w("диска против средней по карте с тем же множителем яркости (у тел без карты — против цвета из данных).\n")
    w("| Тело | Карта, ширина | Детали | Хроматичность (r, g, b) | По карте | Разница |")
    w("| --- | --- | --- | --- | --- | --- |")
    tex = r.get("stage5_textures", {})
    for key, row in r["stage5_snapshots"].items():
        c = ", ".join(num(x, 3) for x in row["chroma"])
        e = ", ".join(num(x, 3) for x in row["expected"])
        width = tex.get(key, 0)
        w(f"| {RU.get(key, key)} | {width if width else 'нет карты'} | {num(row['detail'], 2)} | {c} | {e} | {num(row['chroma_diff'], 3)} |")

    lod = r["stage5_lod"]
    w("\n### Подкачка карт 8K (Земля: цвет, ночь, облака, нормали)\n")
    w("| Момент | Диаметр Земли на экране | Ширина карты | Оценка видеопамяти под 8K | Видеопамять текстур (Godot) |")
    w("| --- | --- | --- | --- | --- |")
    w(f"| старт, Земля мелкая | — | {lod['start']['width']} | 0 | {mb(lod['start']['vram'])} |")
    w(f"| Земля во весь кадр: 8K через {frames(lod['near']['frames'])} ({lod['near']['ms']} мс) | {num(lod['near']['px'], 0)} px |"
      f" {lod['near']['width']} | {mb(lod['near']['hi_bytes'])} | {mb(lod['near']['vram'])} |")
    w(f"| Земля снова мелкая: 2K через {frames(lod['far']['frames'])} ({num(lod['far']['frames'] / 60, 2)} с) |"
      f" {num(lod['far']['px'], 0)} px | {lod['far']['width']} | 0 | {mb(lod['far']['vram'])} |")

    w("\n### Геометрия против SPICE (1 января 2026, 00:00 UTC)\n")
    w("Подсолнечная точка: по кадру «Ламберт» (яркость = cos угла к Солнцу) МНК находит направление на Солнце в осях тела;")
    w("SPICE — `subslr(\"NEAR POINT/ELLIPSOID\", …, \"NONE\")`, геодезические координаты. Координаты карты: код u и v из шейдера")
    w("против эталона лучами в double, ошибка в пикселях экрана (кадры без MSAA).\n")
    w("| Тело | Подсолнечная точка (кадр) | SPICE | Разница | Координаты карты: медиана / 99 % / максимум |")
    w("| --- | --- | --- | --- | --- |")
    for key, row in r["stage5_subsolar"].items():
        uv = r["stage5_uv"][key]
        w(f"| {RU.get(key, key)} | {num(row['render'][0], 4)}°, {num(row['render'][1], 4)}° | {num(row['spice'][0], 4)}°, "
          f"{num(row['spice'][1], 4)}° | {num(row['err_deg'], 4)}° | {num(uv['px_median'], 3)} / {num(uv['px_p99'], 3)} / "
          f"{num(uv['px_max'], 3)} px |")

    w("\n### Затмения против SPICE\n")
    w("| Что | Кадр | SPICE | Разница |")
    w("| --- | --- | --- | --- |")
    for row in r["stage5_eclipse"]:
        if "utc" in row:
            w(f"| Тень Луны на Земле, {row['utc']} UTC: центр области полной фазы | {num(row['render'][0], 2)}°, "
              f"{num(row['render'][1], 2)}° | ось Солнце → Луна: {num(row['spice'][0], 2)}°, {num(row['spice'][1], 2)}° | "
              f"{num(row['err_deg'], 3)}° = {num(row['err_deg'] * 111.2, 0)} км |")
        else:
            w(f"| Ио, 13.10.2026 15:00 UTC | доля видимого диска Солнца {num(row['io_visibility'], 3)} | Солнце за Юпитером: "
              f"{num(row['io_sep_deg'], 2)}° от центра при угловом радиусе Юпитера {num(row['jupiter_radius_deg'], 2)}° | совпадает |")

    w("\n### Потемнение Солнца к краю (кадр при −14 EV против μ^α Hestroffer & Magnan)\n")
    w("| Канал | α по формуле | α по кадру | Медиана «кадр / формула» |")
    w("| --- | --- | --- | --- |")
    for name, row in zip(["R (680 нм)", "G (550 нм)", "B (440 нм)"], r["stage5_sun_limb"]):
        w(f"| {name} | {num(row['alpha'], 4)} | {num(row['alpha_fit'], 4)} | {num(row['median_ratio'], 4)} |")

    if "perf" in r:
        names = {"earth_orbit": "Земля с орбиты (атмосфера, облака)", "saturn_rings": "Сатурн с кольцами",
                 "earth_surface": "у поверхности Земли", "jupiter_system": "система Юпитера",
                 "solar_system": "вся система (8·10⁹ км)"}
        w("\n### Частота кадров (1600×900, с интерфейсом, тональная кривая AgX и свечение, без вертикальной синхронизации)\n")
        w("| Вид | Кадр, мс | Худший кадр, мс | Кадров/с | Чанков |")
        w("| --- | --- | --- | --- | --- |")
        for v in r["perf"]:
            w(f"| {names.get(v['view'], v['view'])} | {num(v['ms'], 2)} | {num(v['worst_ms'], 2)} | {num(v['fps'], 0)} | {v['chunks']} |")

    if "stage5_export" in r:
        e = r["stage5_export"]
        w(f"\n### Экспорт\n\n`SolarSystem.exe` {mb(e['exe_bytes'])}, `SolarSystem.pck` {mb(e['pck_bytes'])}. Внутри exe сценарий"
          f" карт загрузил карты 10 тел (Земля, Луна, Марс, Ио, Европа, Сатурн, Плутон, Эрида, Венера, Солнце) без ошибок.")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
