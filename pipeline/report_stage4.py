"""Таблицы этапа 4 для docs/accuracy.md из отчётов тестов (build/render_report.json, build/runtime_report.json).

    python -I -X utf8 pipeline/report_stage4.py > build/accuracy_stage4.md
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from report_accuracy import NAMES, fmt, sci  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
C_KMS = 299792.458
PLANETS = ["mercury", "venus", "earth", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"]
MOONS = ["moon", "phobos", "io", "mimas", "titan", "triton", "charon"]


def num(x: float, digits: int = 2) -> str:
    return f"{x:.{digits}f}".replace(".", ",")


def speed(kms: float) -> str:
    if kms < 1:
        return f"{num(kms * 1000, 0)} м/с"
    if kms < 0.1 * C_KMS:
        return f"{num(kms, 0)} км/с"
    return f"{num(kms / C_KMS, 0)} c"


def main() -> int:
    r = json.loads((ROOT / "build" / "render_report.json").read_text(encoding="utf-8"))
    q = json.loads((ROOT / "build" / "runtime_report.json").read_text(encoding="utf-8"))
    out = []
    w = out.append

    w("### Оконные проверки (Godot 4.7.2, 1280×720, MSAA 4x, шаг кадра 1/60 с)\n")
    w("| Проверка | Порог | Результат |")
    w("| --- | --- | --- |")
    j = r["jitter"]
    w(f"| Метка 20 см на Земле, камера в 1 м над поверхностью, 180 кадров при 1 с/с и 180 при 1 сут/с | < 0,5 px | "
      f"{sci(max(j['1s']['max_dev_px'], j['1d']['max_dev_px']))} px (кадры совпадают попиксельно) |")
    zf = r["zfight"]
    total = sum(z["mismatch_px"] for z in zf)
    w(f"| z-fighting: Сатурн с кольцами и Ио перед Юпитером, {len(zf)} видов от 1,5·10⁶ до 10¹¹ км, кадр против лучей "
      f"в double | 0 px (кроме 2 px у границ) | {total} px |")
    sp = r["speed"]["runs"]
    worst = max(abs(x["rel_err"]) for x in sp)
    w(f"| Скорость полёта {', '.join(speed(x['set_kms']) for x in sp)}: средняя за 3-ю секунду | ±1 % | "
      f"{num(worst * 100, 3)} % (остаток разгона с инерцией 0,25 с) |")
    w(f"| Концы шкалы скорости | 1 м/с и 1000 c | {speed(r['speed']['slider_min_kms'])} и "
      f"{speed(r['speed']['slider_max_kms'])} |")
    for f in r["flyto"]:
        w(f"| Перелёт к {NAMES[f['label']].replace('Юпитер', 'Юпитеру').replace('Солнце', 'Солнцу').replace('Фобос', 'Фобосу')}"
          f" | 2–5 с, 4R, в центре | {num(f['seconds'])} с; расстояние/4R = {num(f['final_over_4R'], 6)}; "
          f"силуэт против эталона {num(f['image_centroid_err'], 3)} px |")
    for a in r["attach"]["runs"]:
        what = {"jupiter_eph": "Облёт Юпитера при 1 сут/с, «Эфемериды»",
                "jupiter_sim": "Облёт Юпитера при 1 сут/с, «Симуляция» (поток, Эрмит)",
                "moon_fixed": "Облёт Луны вместе с её вращением, метка на поверхности, 1 сут/с"}[a["label"]]
        w(f"| {what}: 240 кадров = {num(a['sim_days'])} сут | < 0,5 px | {sci(a['max_dev_px'])} px |")
    m = r["modes"]
    w(f"| Край эфемерид спутников (2100): «Эфемериды» → «Симуляция» → обратно, 10 сут/с | без скачка | "
      f"смена на кадрах {m['switch_frames'][0]} и {m['switch_frames'][1]}; отклонение от трапеций "
      f"{num(m['max_jump_km'])} км при шаге {num(m['step_s'] / 3600, 0)} ч |")

    w("\n### Частота кадров (1600×900, с интерфейсом, без вертикальной синхронизации)\n")
    w("| Вид | Кадр, мс | Худший кадр, мс | Кадров/с | Чанков |")
    w("| --- | --- | --- | --- | --- |")
    names = {"earth_orbit": "Земля с орбиты", "saturn_rings": "Сатурн с кольцами", "earth_surface": "у поверхности Земли",
             "jupiter_system": "система Юпитера", "solar_system": "вся система (8·10⁹ км)"}
    for p in r["perf"]:
        w(f"| {names[p['view']]} | {num(p['ms'])} | {num(p['worst_ms'])} | {num(p['fps'], 0)} | {p['chunks']} |")

    w("\n### «Симуляция» в потоке\n")
    im = {k: v for k, v in q["interpolation_vs_direct_m"].items() if k != "wall_s_5d"}
    k_max = max(im, key=im.get)
    w(f"Интерполяция Эрмита 5-й степени между шагами IAS15 против прямого интегрирования к тем же моментам (80 случайных "
      f"моментов за 5 суток): наибольшее расхождение {num(im[k_max] * 100, 1)} см ({NAMES[k_max]}). "
      f"Счёт назад на 10 суток: планеты не дальше {fmt(q['backward_10d_km'] and max(q['backward_10d_km'][k] for k in PLANETS))} км "
      "от эфемерид.\n")
    w("Расхождение с эфемеридами в конце отрезка, км. Скорость счёта зависит от загрузки машины (полная модель — "
      "от 4 до 5,4 сут/с в разных прогонах).\n")
    w("| Уровень | Что упрощено | Скорость счёта | Отрезок | Планеты, max | " + " | ".join(NAMES[k] for k in MOONS) + " |")
    w("| --- | --- | --- | --- | --- |" + " --- |" * len(MOONS))
    what = {"0": "ничего", "1": "спутники Марса на элементах", "2": "спутники всех систем, кроме Луны, на элементах"}
    for lvl, v in q["levels"].items():
        rate = v["days_per_s"]
        rate_s = f"{num(rate, 1)} сут/с" if rate < 365 else f"{num(rate / 365.25, 1)} года/с"
        w(f"| {lvl} | {what[lvl]} | {rate_s} | {num(v['span_days'], 0)} сут | {fmt(v['planets_max_km'])} | "
          + " | ".join(fmt(v["errors_km"][k]) for k in MOONS) + " |")
    s = q["level_switch"]
    w(f"\nСмена уровня внутри буфера (1 сут/с → 30 сут/с → 2 года/с → 1 сут/с, уровни {s['levels_seen']}): "
      f"по обе стороны смены положения сходятся до {num(s['max_jump_any_km'] * 1e5, 1)} см (планеты — "
      f"{num(s['max_jump_planet_km'] * 1e5, 2)} см).")
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
