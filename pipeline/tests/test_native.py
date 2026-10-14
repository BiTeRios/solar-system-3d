"""C++ ядро против эталона на Python: эфемериды, время и ориентация должны совпадать до бита.
Плюс GDExtension в Godot 4.7.2 (headless): те же числа из GDScript.
"""
import json
import subprocess

import numpy as np
import pytest

from eph_format import EphFile
from export_constants import OUT as CONSTS, j2000_to_body, system_of
from kernels import ROOT
from timeconv import TimeModel

from native import DLL, Native, NativeEph, NativeTime, orient_matrix

EPH = ROOT / "game" / "data" / "ephemeris.eph"
GODOT = r"C:\Tools\Godot\Godot_v4.7.2-stable_win64_console.exe"


@pytest.fixture(scope="module")
def nat():
    if not DLL.exists():
        pytest.skip("нет native/bin/spacecore_c.dll: запустите tools/build_native.ps1")
    return Native()


@pytest.fixture(scope="module")
def eph():
    return EphFile(EPH)


def test_ephemeris_bitwise(nat, eph):
    ne = NativeEph(nat, EPH)
    rng = np.random.default_rng(31)
    for target in sorted(eph.center_of):
        cov = eph.coverage(target)
        lo, hi = cov[0][0], cov[-1][1]
        ets = lo + rng.random(300) * (hi - lo)
        py_p, py_v = eph.state_rel(target, ets)
        for k, et in enumerate(ets):
            got = ne.state(target, float(et), False)
            assert got is not None, (target, et)
            ref = np.concatenate([py_p[k], py_v[k]])
            assert np.array_equal(got, ref), f"{target} на {et}: {got - ref}"
    # цепочка до барицентра
    for target in (301, 399, 501, 606, 801, 999, 2000001, 1000036):
        et = 8.1e8
        p, v = eph.state_ssb(target, et)
        assert np.array_equal(ne.state(target, et, True), np.concatenate([p[0], v[0]]))
    assert ne.state(501, -3.2e9, False) is None  # вне диапазона


def test_time_bitwise(nat):
    tm = TimeModel.from_json(ROOT / "game" / "data" / "time.json")
    nt = NativeTime(nat, tm)
    rng = np.random.default_rng(32)
    for _ in range(20_000):
        y = int(rng.integers(-3000, 5000))
        mo, d = int(rng.integers(1, 13)), int(rng.integers(1, 29))
        h, mi = int(rng.integers(0, 24)), int(rng.integers(0, 60))
        s = float(rng.random() * 60)
        et = tm.utc_to_tdb(y, mo, d, h, mi, s)
        assert nt.utc_to_tdb(y, mo, d, h, mi, s) == et
        py = tm.tdb_to_utc(et)
        cc = nt.tdb_to_utc(et)
        assert cc[:5] == py[:5] and cc[5] == py[5], (py, cc)
        assert nt.delta_et_utc(et) == tm.delta_et_utc(et)


def test_orientation_bitwise(nat):
    consts = json.loads(CONSTS.read_text(encoding="utf-8"))
    rng = np.random.default_rng(33)
    for naif, b in consts["bodies"].items():
        if "pole_ra" not in b:
            continue
        sysk = system_of(int(naif))
        system = consts["systems"].get(str(sysk)) if sysk else None
        for et in rng.uniform(-1.4e10, 2.0e10, 200):
            ref = j2000_to_body(b, system, float(et))
            got = orient_matrix(nat, b, system, float(et))
            assert np.abs(got - ref).max() <= 4e-16, (naif, et, np.abs(got - ref).max())


def test_gdextension_in_godot(eph):
    dll = ROOT / "game" / "bin" / "spacecore.windows.template_debug.x86_64.dll"
    if not dll.exists():
        pytest.skip("нет GDExtension: запустите tools/build_native.ps1")
    et = 8.1e8
    targets = [10, 399, 301, 599, 501, 606, 999, 2136199]
    # Расширения подключаются по .godot/extension_list.cfg. Если его создаёт сам импорт, Godot 4.7.2 подгружает
    # расширение на ходу и падает при выходе (docs/architecture.md, известные ограничения). Пишем список заранее.
    ext_list = ROOT / "game" / ".godot" / "extension_list.cfg"
    ext_list.parent.mkdir(exist_ok=True)
    ext_list.write_text("res://spacecore.gdextension\n", encoding="utf-8")
    imp = subprocess.run([GODOT, "--headless", "--path", str(ROOT / "game"), "--import"], capture_output=True,
                         text=True, encoding="utf-8", timeout=600)
    assert imp.returncode == 0, imp.stdout + imp.stderr
    r = subprocess.run([GODOT, "--headless", "--path", str(ROOT / "game"), "-s", "tests/test_gdextension.gd",
                        "--", repr(et), *map(str, targets)], capture_output=True, text=True, encoding="utf-8",
                       timeout=180)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "VERSION spacecore 0.2 (этап 4)" in r.stdout
    got = {}
    for line in r.stdout.splitlines():
        if line.startswith("STATE "):
            _, code, hexs = line.split()
            got[int(code)] = np.frombuffer(bytes.fromhex(hexs), dtype="<f8")
    assert set(got) == set(targets)
    for t in targets:
        p, v = eph.state_ssb(t, et)
        assert np.array_equal(got[t], np.concatenate([p[0], v[0]])), t
