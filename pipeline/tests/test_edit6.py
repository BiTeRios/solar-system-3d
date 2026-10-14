"""Этап 6 в окне: правки через узел SolarSystem — «Симуляция», пересчёт, отмена и повтор, сценарий JSON.

Логику правок (производные величины, предупреждения, точность отмены, JSON) проверяют тесты C++
(native/tests/test_stage6.cpp); здесь — что всё это доходит до игры.
"""
from __future__ import annotations

import pytest

from test_render import imported, run_scenario  # noqa: F401 (фикстура)

pytestmark = pytest.mark.window


def test_edit_flow(imported):
    meta, _ = run_scenario("edit", resolution="1600x900")
    st = {s["label"]: s for s in meta["steps"]}
    assert meta["err_moon"] == meta["err_mass"] == meta["err_spin"] == ""
    assert meta["err_bad"]  # альбедо 1,5 не принято
    # правка массы/скорости включает «Симуляцию», отмена всех таких правок возвращает эфемериды, повтор — снова «Симуляцию»
    assert [st[k]["mode"] for k in ("moon", "mass", "spin", "undo2", "undo3", "redo3")] == [1, 1, 1, 1, 0, 1]
    # пересчёт от момента правок до текущего закончился, время не ушло назад
    assert meta["recompute_frames"] < 1800 and meta["et_gap_after_recompute"] >= 0
    assert any("критического" in w for w in st["spin"]["warnings"])
    assert not st["undo3"]["warnings"]
    # сценарий: сохранить → загрузить → сохранить без изменений; после загрузки — тот же момент и то же состояние
    assert meta["load_error"] == "" and meta["json_same"]
    assert st["loaded"]["et"] == st["moon"]["et"]
    for a, b in zip(st["loaded"]["earth"], st["moon"]["earth"]):
        assert abs(a - b) <= 1e-6 * max(1.0, abs(b))
