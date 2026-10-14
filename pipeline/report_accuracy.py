"""Таблицы этапа 3 для docs/accuracy.md из отчётов тестов (build/physics_report.json, build/horizons_report.json).

    python -I pipeline/report_accuracy.py > build/accuracy_stage3.md
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAMES = {"sun": "Солнце", "mercury": "Меркурий", "venus": "Венера", "earth": "Земля", "moon": "Луна", "mars": "Марс",
         "phobos": "Фобос", "deimos": "Деймос", "jupiter": "Юпитер", "io": "Ио", "europa": "Европа",
         "ganymede": "Ганимед", "callisto": "Каллисто", "saturn": "Сатурн", "mimas": "Мимас", "enceladus": "Энцелад",
         "tethys": "Тефия", "dione": "Диона", "rhea": "Рея", "titan": "Титан", "iapetus": "Япет", "uranus": "Уран",
         "miranda": "Миранда", "ariel": "Ариэль", "umbriel": "Умбриэль", "titania": "Титания", "oberon": "Оберон",
         "neptune": "Нептун", "triton": "Тритон", "pluto": "Плутон", "charon": "Харон", "ceres": "Церера",
         "eris": "Эрида", "haumea": "Хаумеа", "makemake": "Макемаке", "halley": "Галлей", "pallas": "Паллада",
         "vesta": "Веста", "hygiea": "Гигея"}
CONTROL = {"10": "Солнце", "1": "бар. Меркурия", "2": "бар. Венеры", "3": "бар. Земля–Луна", "4": "бар. Марса",
           "5": "бар. Юпитера", "6": "бар. Сатурна", "7": "бар. Урана", "8": "бар. Нептуна", "9": "бар. Плутона",
           "399": "Земля", "301": "Луна"}


SUP = str.maketrans("-0123456789", "⁻⁰¹²³⁴⁵⁶⁷⁸⁹")


def sci(x: float) -> str:
    """2.6e-15 → «2,6·10⁻¹⁵»."""
    m, e = f"{x:.2g}".split("e") if "e" in f"{x:.2g}" else (f"{x:.2g}", None)
    if e is None:
        return m.replace(".", ",")
    return f"{m.replace('.', ',')}·10{str(int(e)).translate(SUP)}"


def fmt(x: float) -> str:
    """Километры: меньше 1 м — в метрах, от 1000 — целым числом с пробелами между разрядами."""
    if x == 0:
        return "0"
    if x < 1e-3:
        return f"{x * 1e3:.2g} м".replace(".", ",")
    if x >= 1000:
        return f"{x:,.0f}".replace(",", " ")
    return f"{x:.3g}".replace(".", ",")


def main() -> int:
    phys = json.loads((ROOT / "build" / "physics_report.json").read_text(encoding="utf-8"))
    hz = json.loads((ROOT / "build" / "horizons_report.json").read_text(encoding="utf-8"))
    out = []
    w = out.append
    w("### «Эфемериды» против JPL Horizons\n")
    w("50 дат в 1900–2100, барицентр, ICRF. Порог: планеты, Луна и спутники — 1 км, малые тела — 10 км.\n")
    w("| Тело | max \\|Δr\\|, км | Источник Horizons |")
    w("| --- | --- | --- |")
    for key, r in hz.items():
        if key.startswith("control_"):
            continue
        src = r["horizons_sources"][0].split("{source:")[1].rstrip("}").strip() if r["horizons_sources"] else ""
        w(f"| {NAMES.get(key, key)} | {fmt(r['max_km'])} | {src} |")
    w("\nМалые тела: Horizons считает их векторы новым интегрированием по решению орбиты, а не из того же SPK, "
      "поэтому расхождение — километры (против самих SPK — меньше 0,34 м, см. этап 2).")
    w("\nКонтрольные даты 1600 и 2600 годов. Барицентры Урана и Нептуна у нас из DE442 на всём диапазоне, а Horizons "
      "вне покрытия ura184 и nep098 переходит на DE441; эти две строки только записываются, порогом не проверяются.\n")
    w("| Тело | max \\|Δr\\|, км |")
    w("| --- | --- |")
    for key, r in hz.items():
        if key.startswith("control_"):
            w(f"| {CONTROL[key[8:]]} | {fmt(r['max_km'])} |")
    for title, k in (("1 сутки, 1 месяц, 1 год", "sim_vs_eph_km"), ("10 лет", "sim_vs_eph_km_10yr")):
        if k not in phys:
            continue
        t = phys[k]
        spans = list(t)
        w(f"\n### «Симуляция» против эфемерид: {title}\n")
        w("Старт 8,135·10⁸ с TDB (октябрь 2025), Ньютон + ОТО + поля планет по `dynamics.json`. |Δr|, км.\n")
        w("| Тело | " + " | ".join(spans) + " |")
        w("| --- |" + " --- |" * len(spans))
        for key in t[spans[0]]:
            w(f"| {NAMES.get(key, key)} | " + " | ".join(fmt(t[s][key]) for s in spans) + " |")
    for k, title in (("conservation_10yr", "10 лет"), ("conservation_100yr", "100 лет")):
        if k in phys:
            c = phys[k]
            w(f"\nСохранение за {title} (39 тел, Ньютон): |ΔE/E| = {sci(c['dE/E'])}, |ΔL/L| = {sci(c['dL/L'])}, "
              f"шагов {c['steps']}, отказов {c['rejected']}, счёт {c['wall_s']:.0f} с.")
    if "rebound_jupiter_doubled" in phys:
        r = phys["rebound_jupiter_doubled"]
        w(f"\nУдвоенная масса Юпитера, 100 лет: эффект на Сатурн {fmt(r['saturn_effect_km'])} км, наш IAS15 против REBOUND "
          f"{r['rebound_version']} — {fmt(r['saturn_ours_vs_rebound_km'])}; наибольшее относительное расхождение "
          f"{sci(r['max_relative_difference'])}.")
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
