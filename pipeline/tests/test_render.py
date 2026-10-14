"""Этап 4: оконные сценарии Godot (game/tests/scenarios.gd) — рендер, floating origin, полёт.

Каждый тест запускает игру в окне 1280×720 с фиксированным шагом кадра 1/60 с (--fixed-fps 60), сценарий пишет
кадры и числа в build/render_tests/<сценарий>/, а проверки — здесь. Пороги заданы до запуска (docs/architecture.md):
  - дрожание метки на поверхности Земли с камерой в 1 м над ней — меньше 0,5 px (1 с/с и 1 сут/с);
  - нет z-fighting: кадр совпадает с эталоном (лучи в double) везде, кроме 2 px у границ, на расстояниях до 10¹¹ км;
  - скорость полёта от 1 м/с до 1000 c — в пределах 1 % от заданной, концы шкалы — ровно 1 м/с и 1000 c;
  - перелёт за 2–5 с доходит до 4 радиусов цели, цель в центре кадра;
  - облёт при 1 сут/с держит тело в центре кадра (меньше 0,5 px) в «Эфемеридах» и в «Симуляции».
Окна открываются на экране на несколько секунд каждое.
"""
import json
import os
import subprocess

import numpy as np
import pytest
from PIL import Image

from kernels import ROOT

GODOT = r"C:\Tools\Godot\Godot_v4.7.2-stable_win64_console.exe"
OUT = ROOT / "build" / "render_tests"
C_KMS = 299792.458
REPORT = ROOT / "build" / "render_report.json"

pytestmark = pytest.mark.window


def _save(key, value):
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    data = json.loads(REPORT.read_text(encoding="utf-8")) if REPORT.exists() else {}
    data[key] = value
    REPORT.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")


@pytest.fixture(scope="module")
def imported():
    dll = ROOT / "game" / "bin" / "spacecore.windows.template_debug.x86_64.dll"
    if not dll.exists():
        pytest.skip("нет GDExtension: запустите tools/build_native.ps1")
    # список расширений — заранее (иначе Godot 4.7.2 падает при выходе из первого импорта, docs/architecture.md)
    ext_list = ROOT / "game" / ".godot" / "extension_list.cfg"
    ext_list.parent.mkdir(exist_ok=True)
    ext_list.write_text("res://spacecore.gdextension\n", encoding="utf-8")
    imp = subprocess.run([GODOT, "--headless", "--path", str(ROOT / "game"), "--import"], capture_output=True,
                         text=True, encoding="utf-8", timeout=600)
    assert imp.returncode == 0, imp.stdout + imp.stderr
    return True


def run_scenario(name: str, resolution="1280x720", fixed=True, timeout=600) -> tuple[dict, object]:
    out = OUT / name
    out.mkdir(parents=True, exist_ok=True)
    for f in out.iterdir():
        if f.is_file():
            f.unlink()
    # SPACE_EXE — собранная игра (tools/run_tests.ps1 -Exported): тот же сценарий внутри exe, данные из .pck
    exe = os.environ.get("SPACE_EXE")
    args = ([exe] if exe else [GODOT, "--path", str(ROOT / "game")]) + ["--resolution", resolution]
    if fixed:
        args += ["--fixed-fps", "60"]
    args += ["--", f"--scenario={name}", f"--out={out}"]
    r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    log = r.stdout + r.stderr
    assert r.returncode == 0, log[-4000:]
    errors = [ln for ln in log.splitlines() if "ERROR" in ln or "SCRIPT ERROR" in ln]
    assert not errors, "\n".join(errors[:20])
    meta = json.loads((out / "meta.json").read_text(encoding="utf-8"))
    return meta, out


def _centroids(raw: np.ndarray, channel: str) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Центр масс «чистого» цвета в каждой вырезке: красный или синий минус остальные каналы."""
    a = raw.astype(float)
    if channel == "red":
        w = np.clip(a[..., 0] - np.maximum(a[..., 1], a[..., 2]), 0, None)
    else:
        w = np.clip(a[..., 2] - np.maximum(a[..., 0], a[..., 1]), 0, None)
    ys, xs = np.mgrid[0:raw.shape[1], 0:raw.shape[2]]
    tot = w.sum((1, 2))
    return (w * xs).sum((1, 2)) / tot, (w * ys).sum((1, 2)) / tot, tot / 255.0


def test_jitter_at_surface(imported):
    meta, out = run_scenario("jitter")
    c = meta["crop"]
    assert meta["altitude_km"] == pytest.approx(0.001, abs=1e-6)
    res = {}
    for ph in ("1s", "1d"):
        raw = np.fromfile(out / f"jitter_{ph}.rgb", dtype=np.uint8).reshape(-1, c, c, 3)
        assert len(raw) == meta[f"frames_{ph}"]
        cx, cy, px = _centroids(raw, "red")
        assert px.min() > 50, "метка не видна"
        dev = float(np.max(np.hypot(cx - cx.mean(), cy - cy.mean())))
        proj = np.array(meta[f"projected_{ph}"])
        pdev = float(np.max(np.ptp(proj, axis=0)))
        res[ph] = {"max_dev_px": dev, "projected_spread_px": pdev, "marker_px": float(px.mean())}
        assert dev < 0.5, (ph, dev)
    _save("jitter", res)


PAL = np.array([[0, 0, 0], [0, 0, 255], [255, 255, 0], [0, 255, 0]], float)  # фон, цель, кольцо, прочие тела


def test_no_z_fighting(imported):
    meta, out = run_scenario("zfight")
    rows = []
    for v in meta["views"]:
        img = np.asarray(Image.open(out / f"zf_{v['label']}.png").convert("RGB")).astype(float)
        ref = np.fromfile(out / f"zf_{v['label']}.cls", dtype=np.uint8).reshape(v["h"], v["w"])
        rc = np.full(ref.shape, 3, np.int8)
        rc[ref == 0] = 0
        rc[ref == v["body"] + 1] = 1
        rc[ref == 255] = 2
        dist = np.linalg.norm(img[:, :, None, :] - PAL[None, None], axis=3)
        cls = dist.argmin(axis=2)
        pure = dist.min(axis=2) < 40
        pad = np.pad(rc, 2, mode="edge")
        edge = np.zeros(rc.shape, bool)
        for dy in range(5):
            for dx in range(5):
                edge |= pad[dy:dy + rc.shape[0], dx:dx + rc.shape[1]] != rc
        # считаем пиксели цели, колец и (у Юпитера) Ио. Мелкие спутники Сатурна — меньше пикселя: эталон берёт
        # один луч на пиксель, а MSAA рисует их по покрытию, поэтому их окрестность (2 px) не сравниваем
        relevant = np.isin(rc, (1, 2)) | np.isin(cls, (1, 2))
        if v["label"].startswith("jupiter_io"):
            relevant |= (rc == 3) | (cls == 3)
        else:
            # спутник меньше пикселя виден только как примесь зелёного к цвету цели (синий + зелёный, без красного)
            tinted = img[..., 1] > img[..., 0] + 40
            other = np.pad((rc == 3) | (cls == 3) | tinted, 2)
            near_other = np.zeros(rc.shape, bool)
            for dy in range(5):
                for dx in range(5):
                    near_other |= other[dy:dy + rc.shape[0], dx:dx + rc.shape[1]]
            relevant &= ~near_other
        bad = ~edge & relevant & ((cls != rc) | ~pure)
        rows.append({"label": v["label"], "dist_km": v["dist_km"], "zoom": v["zoom"], "far_over_near": v["far"] / v["near"],
                     "target_px": int((rc == 1).sum()), "ring_px": int((rc == 2).sum()),
                     "other_px": int((rc == 3).sum()), "mismatch_px": int(bad.sum())})
    _save("zfight", rows)
    for r in rows:
        assert r["target_px"] > 20000, r
        if r["label"].startswith("saturn"):
            assert r["ring_px"] > 5000, r
        else:
            assert r["other_px"] > 300, r  # Ио на фоне диска
        assert r["mismatch_px"] == 0, r


def test_flight_speed_range(imported):
    meta, _ = run_scenario("speed")
    res = []
    for r in meta["runs"]:
        sp = np.array(r["speeds"])
        an = np.array(r["anchors"])
        same = an[-60:] == an[-61:-1]  # кадры со сменой опорного тела не считаем
        mean = float(sp[-60:][same].mean())
        res.append({"set_kms": r["set_kms"], "measured_kms": mean, "rel_err": mean / r["set_kms"] - 1})
    _save("speed", {"runs": res, "slider_min_kms": meta["slider_min_kms"], "slider_max_kms": meta["slider_max_kms"]})
    assert [r["set_kms"] for r in res] == [1e-3, 1.0, C_KMS, 1000 * C_KMS]
    for r in res:
        assert abs(r["rel_err"]) < 0.01, r
    assert meta["slider_min_kms"] == 1e-3
    assert meta["slider_max_kms"] == pytest.approx(1000 * C_KMS, rel=1e-12)


def test_fly_to(imported):
    meta, out = run_scenario("flyto")
    res = []
    for f in meta["flights"]:
        d = np.array(f["dist"])
        img = np.asarray(Image.open(out / f"flyto_{f['label']}.png").convert("RGB"))
        cx, cy, px = _centroids(img[None], "blue")
        # эталон: лучи в double через эллипсоиды; у трёхосного тела вблизи центр силуэта ≠ проекция центра
        ref = np.fromfile(out / f"flyto_{f['label']}.cls", dtype=np.uint8).reshape(f["h"], f["w"]) == f["body"] + 1
        ys, xs = np.nonzero(ref)
        rx, ry = xs.mean(), ys.mean()
        vc = f["viewport_center"]
        res.append({"label": f["label"], "seconds": f["seconds"], "final_over_4R": f["final_dist_km"] / (4 * f["radius_km"]),
                    "center_px_err": float(np.hypot(f["center_px"][0] - vc[0], f["center_px"][1] - vc[1])),
                    "image_centroid_err": float(np.hypot(cx[0] - rx, cy[0] - ry)),
                    "disk_px": float(px[0]), "ref_px": int(ref.sum()),
                    "monotonic": bool(np.all(np.diff(d) <= 1e-9 * d[1:]))})
    _save("flyto", res)
    for f, r in zip(meta["flights"], res):
        assert 2.0 <= r["seconds"] <= 5.0, r
        assert f["mode"] == 1, r  # закончился облётом
        assert r["final_over_4R"] == pytest.approx(1.0, abs=1e-6), r
        assert r["center_px_err"] < 0.5, r
        assert r["image_centroid_err"] < 2.0, r
        assert r["monotonic"], r


def test_orbit_attach_at_one_day_per_second(imported):
    meta, out = run_scenario("attach")
    res = []
    for r in meta["runs"]:
        c = r["crop"]
        raw = np.fromfile(out / f"attach_{r['label']}.rgb", dtype=np.uint8).reshape(-1, c, c, 3)
        cx, cy, px = _centroids(raw, "red" if r["label"].startswith("moon") else "blue")
        p = np.array(r["projected"])
        span_days = (p[-1, 3] - p[0, 3]) / 86400.0
        res.append({"label": r["label"], "frames": len(raw), "max_dev_px": float(np.max(np.hypot(cx - cx.mean(), cy - cy.mean()))),
                    "projected_spread_px": float(np.max(np.ptp(p[:, :2], axis=0))),
                    "dist_rel_spread": float(np.ptp(p[:, 2]) / p[:, 2].mean()), "sim_days": span_days,
                    "pixels": float(px.mean())})
    _save("attach", {"runs": res, "sim_level": meta["sim_level"]})
    for r in res:
        assert r["pixels"] > 30, r
        assert r["max_dev_px"] < 0.5, r
        assert r["projected_spread_px"] < 0.01, r
        assert r["dist_rel_spread"] < 1e-9, r
        # 240 кадров по 1/60 с при 1 сут/с — 3,98 сут модельного времени: скорость времени выдержана
        assert r["sim_days"] == pytest.approx(239 / 60, rel=0.01), r
    assert meta["sim_level"] == 0  # 1 сут/с полная модель успевает


def test_mode_switch_at_ephemeris_edge(imported):
    meta, _ = run_scenario("modes")
    r = np.array([[float(x) for x in row] for row in meta["rows"]])
    et, mode, auto = r[:, 0], r[:, 1], r[:, 2]
    e, v = r[:, 3:6], r[:, 6:9]
    changes = np.nonzero(np.diff(mode))[0]
    assert len(changes) == 2, changes
    assert mode[changes[0] + 1] == 1 and auto[changes[0] + 1] == 1  # за краем — «Симуляция» сама
    assert mode[changes[1] + 1] == 0  # вернулись в эфемериды
    hi = meta["eph_range"][1]
    assert et[changes[0]] <= hi and et.max() > hi + 86400  # переключились на краю и ушли за него
    # назад в «Эфемериды» — с запасом max(1 сут, 0,5 с·скорость) = 5 сут от края, чтобы режим не дребезжал
    assert np.all(et[mode == 1] >= hi - 5 * 86400 - 14400 - 1.0)
    assert et[changes[1] + 1] < hi - 5 * 86400
    dt = np.diff(et)
    jump = np.linalg.norm(np.diff(e, axis=0) - (v[1:] + v[:-1]) / 2 * dt[:, None], axis=1)
    _save("modes", {"switch_frames": changes.tolist(), "max_jump_km": float(jump.max()), "step_s": float(np.abs(dt).max())})
    # при шаге 4 ч модельного времени формула трапеций сама ошибается на ~0,3 км; скачок смены режима был бы больше
    assert jump.max() < 2.0, jump.max()


def test_frame_rate(imported):
    meta, out = run_scenario("perf", resolution="1600x900", fixed=False)
    _save("perf", meta["views"])
    for v in meta["views"]:
        assert v["fps"] >= 60, v
