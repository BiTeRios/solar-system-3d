"""Доступ к C++ ядру из тестов: ctypes-обёртка над native/bin/spacecore_c.dll."""
from __future__ import annotations

import ctypes as C
from pathlib import Path

import numpy as np

from kernels import ROOT

DLL = ROOT / "native" / "bin" / "spacecore_c.dll"
D = C.c_double
DP = C.POINTER(C.c_double)
VP = C.c_void_p
IP = C.POINTER(C.c_int)


def _arr(a) -> tuple:
    a = np.ascontiguousarray(a, dtype=np.float64)
    return a, a.ctypes.data_as(DP)


class Native:
    def __init__(self, path: Path = DLL):
        self.lib = lib = C.CDLL(str(path))
        sig = {
            "sc_sim_new": ([], VP), "sc_sim_free": ([VP], None),
            "sc_sim_add": ([VP, D, DP, D, D, DP, C.c_int], C.c_int),
            "sc_sim_configure": ([VP, C.c_int, C.c_int, D, D, D], None),
            "sc_sim_set_zonal": ([VP, C.c_int, DP, C.c_int], None),
            "sc_sim_set_field": ([VP, C.c_int, C.c_int, DP, DP] + [D] * 8, None),
            "sc_sim_integrate": ([VP, D], C.c_int), "sc_sim_count": ([VP], C.c_int),
            "sc_sim_time": ([VP], D), "sc_sim_error": ([VP], C.c_char_p),
            "sc_sim_states": ([VP, DP], None), "sc_sim_stats": ([VP, C.POINTER(C.c_longlong)], None),
            "sc_sim_energy": ([VP], D), "sc_sim_angmom": ([VP, DP], None), "sc_sim_momentum": ([VP, DP], None),
            "sc_eph_open": ([C.c_char_p], VP), "sc_eph_close": ([VP], None),
            "sc_eph_state": ([VP, C.c_int, C.c_int, D, DP], C.c_int),
            "sc_time_new": ([D, D, D, D, D, C.c_int, DP, DP], VP), "sc_time_free": ([VP], None),
            "sc_time_utc_to_tdb": ([VP, C.c_longlong, C.c_int, C.c_int, C.c_int, C.c_int, D], D),
            "sc_time_tdb_to_utc": ([VP, D, C.POINTER(C.c_longlong), DP], None),
            "sc_time_delta_et_utc": ([VP, D], D),
            "sc_orient_matrix": ([DP, C.c_int] * 7 + [C.c_int, D, DP], None),
            "sc_world_new": ([C.c_char_p], VP), "sc_world_free": ([VP], None),
            "sc_world_count": ([VP], C.c_int), "sc_world_naif": ([VP, C.c_int], C.c_int),
            "sc_world_error": ([VP], C.c_char_p),
            "sc_world_initial_state": ([VP, D, IP, DP, DP, DP, DP, IP, IP, IP, DP, C.c_int], C.c_int),
            "sc_runner_seed": ([VP, D, C.c_int, C.c_int, C.c_int], C.c_int),
            "sc_runner_request": ([VP, D, D], None), "sc_runner_range": ([VP, DP, DP], C.c_int),
            "sc_runner_state": ([VP, D, DP], C.c_int), "sc_runner_level": ([VP], C.c_int),
            "sc_runner_switches": ([VP, DP, C.c_int], C.c_int),
            "sc_runner_throughput": ([VP, C.c_int], D), "sc_runner_steps": ([VP], C.c_longlong),
            "sc_runner_running": ([VP], C.c_int), "sc_runner_stop": ([VP], None),
            "sc_runner_error": ([VP], C.c_char_p),
        }
        for name, (args, res) in sig.items():
            f = getattr(lib, name)
            f.argtypes, f.restype = args, res


class Sim:
    """N-body в C++. Тела добавляются по одному; состояния — массив (N, 6)."""

    def __init__(self, nat: Native, gr=True, j2=True, epsilon=1e-9, t0=0.0, dt=3600.0):
        self.n = nat.lib
        self.h = self.n.sc_sim_new()
        self.n.sc_sim_configure(self.h, int(gr), int(j2), epsilon, t0, dt)

    def __del__(self):
        if getattr(self, "h", None):
            self.n.sc_sim_free(self.h)

    def add(self, gm, xv, j2=0.0, r_ref=0.0, pole=(0.0, 0.0, 1.0), parent=-1) -> int:
        """parent — индекс планеты для спутника: он интегрируется относительно неё."""
        xv, pxv = _arr(xv)
        pl, ppl = _arr(pole)
        return self.n.sc_sim_add(self.h, gm, pxv, j2, r_ref, ppl, parent)

    def set_zonal(self, index: int, jn: dict):
        """jn: {n: Jₙ}, n ≥ 2."""
        if not jn:
            return
        n_max = max(int(k) for k in jn)
        arr = np.zeros(n_max + 1)
        for k, v in jn.items():
            arr[int(k)] = v
        arr, p = _arr(arr)
        self.n.sc_sim_set_zonal(self.h, index, p, n_max)

    def set_field(self, index: int, field: dict, epoch_et: float):
        """Полное поле: field = {"nmax", "radius_km", "C": {"n,m": v}, "S": {...}, "rotation": {...}}."""
        nmax = field["nmax"]
        size = (nmax + 1) * (nmax + 2) // 2
        c, s = np.zeros(size), np.zeros(size)
        for key, v in field["C"].items():
            n, m = map(int, key.split(","))
            c[n * (n + 1) // 2 + m] = v
        for key, v in field["S"].items():
            n, m = map(int, key.split(","))
            s[n * (n + 1) // 2 + m] = v
        c, pc = _arr(c)
        s, ps = _arr(s)
        rot = field["rotation"]
        self.n.sc_sim_set_field(self.h, index, nmax, pc, ps, field["radius_km"], rot["ra_deg"], rot["dec_deg"],
                                rot["ra_rate_deg_cy"], rot["dec_rate_deg_cy"], rot["w_deg"], rot["w_rate_deg_day"],
                                epoch_et)

    def integrate(self, t_end: float):
        if not self.n.sc_sim_integrate(self.h, t_end):
            raise RuntimeError(self.n.sc_sim_error(self.h).decode("utf-8"))

    @property
    def t(self) -> float:
        return self.n.sc_sim_time(self.h)

    def states(self) -> np.ndarray:
        out = np.empty((self.n.sc_sim_count(self.h), 6))
        self.n.sc_sim_states(self.h, out.ctypes.data_as(DP))
        return out

    def stats(self) -> dict:
        out = (C.c_longlong * 4)()
        self.n.sc_sim_stats(self.h, out)
        return dict(zip(("steps", "rejected", "iterations", "not_converged"), out))

    def energy(self) -> float:
        return self.n.sc_sim_energy(self.h)

    def angmom(self) -> np.ndarray:
        out = np.empty(3)
        self.n.sc_sim_angmom(self.h, out.ctypes.data_as(DP))
        return out

    def momentum(self) -> np.ndarray:
        out = np.empty(3)
        self.n.sc_sim_momentum(self.h, out.ctypes.data_as(DP))
        return out


class NativeEph:
    def __init__(self, nat: Native, path: Path):
        self.n = nat.lib
        self.h = self.n.sc_eph_open(str(path).encode("utf-8"))
        if not self.h:
            raise RuntimeError(f"не открылся {path}")

    def __del__(self):
        if getattr(self, "h", None):
            self.n.sc_eph_close(self.h)

    def state(self, target: int, et: float, ssb: bool) -> np.ndarray | None:
        out = np.empty(6)
        ok = self.n.sc_eph_state(self.h, target, int(ssb), et, out.ctypes.data_as(DP))
        return out if ok else None


class NativeTime:
    def __init__(self, nat: Native, model):
        self.n = nat.lib
        self._e, pe = _arr(model.leap_epochs)
        self._v, pv = _arr(model.leap_values)
        self.h = self.n.sc_time_new(model.delta_t_a, model.k, model.eb, model.m0, model.m1,
                                    len(model.leap_epochs), pe, pv)

    def __del__(self):
        if getattr(self, "h", None):
            self.n.sc_time_free(self.h)

    def utc_to_tdb(self, y, mo, d, h=0, mi=0, s=0.0) -> float:
        return self.n.sc_time_utc_to_tdb(self.h, y, mo, d, h, mi, s)

    def tdb_to_utc(self, et):
        ymdhm = (C.c_longlong * 5)()
        sec = C.c_double()
        self.n.sc_time_tdb_to_utc(self.h, et, ymdhm, C.byref(sec))
        return (*ymdhm, sec.value)

    def delta_et_utc(self, et) -> float:
        return self.n.sc_time_delta_et_utc(self.h, et)


def orient_matrix(nat: Native, body: dict, system: dict | None, et: float) -> np.ndarray:
    keep = []

    def a(name, src):
        arr, ptr = _arr(src.get(name, []) if src else [])
        keep.append(arr)
        return ptr, arr.size

    args = []
    for name in ("pole_ra", "pole_dec", "pm", "nut_prec_ra", "nut_prec_dec", "nut_prec_pm"):
        args += a(name, body)
    args += a("nut_prec_angles", system)
    out = np.empty(9)
    nat.lib.sc_orient_matrix(*args, int(system["max_phase_degree"]) if system else 1, et, out.ctypes.data_as(DP))
    return out.reshape(3, 3)


class NativeWorld:
    """Каталог тел, эфемериды и «Симуляция» в потоке (sc_world_*, sc_runner_*)."""

    ZONAL = 11

    def __init__(self, nat: Native, data_dir: Path):
        self.n = nat.lib
        self.h = self.n.sc_world_new(str(data_dir).encode("utf-8"))
        if not self.h:
            raise RuntimeError(f"не открылись данные {data_dir}")
        self.count = self.n.sc_world_count(self.h)
        self.naif = [self.n.sc_world_naif(self.h, i) for i in range(self.count)]

    def __del__(self):
        if getattr(self, "h", None):
            self.n.sc_world_free(self.h)

    def initial_state(self, et: float, include=None) -> list[dict]:
        n = self.count
        xv, gm, rr, pole = np.zeros((n, 6)), np.zeros(n), np.zeros(n), np.zeros((n, 3))
        parent, naif, nz = (C.c_int * n)(), (C.c_int * n)(), (C.c_int * n)()
        zon = np.zeros((n, self.ZONAL))
        inc = (C.c_int * n)(*[int(x) for x in include]) if include is not None else None
        m = self.n.sc_world_initial_state(self.h, et, inc, xv.ctypes.data_as(DP), gm.ctypes.data_as(DP),
                                          rr.ctypes.data_as(DP), pole.ctypes.data_as(DP), parent, naif, nz,
                                          zon.ctypes.data_as(DP), self.ZONAL)
        if m < 0:
            raise RuntimeError(self.n.sc_world_error(self.h).decode("utf-8"))
        return [{"naif": naif[i], "xv": xv[i], "gm": gm[i], "r_ref": rr[i], "pole": pole[i], "parent": parent[i],
                 "nzonal": nz[i], "zonal": zon[i, :max(nz[i], 0)]} for i in range(m)]

    def seed(self, et: float, min_level=0, max_level=3, gr=True):
        if not self.n.sc_runner_seed(self.h, et, min_level, max_level, int(gr)):
            raise RuntimeError(self.n.sc_world_error(self.h).decode("utf-8"))

    def request(self, t: float, rate: float):
        self.n.sc_runner_request(self.h, t, rate)

    def range(self):
        lo, hi = C.c_double(), C.c_double()
        return (lo.value, hi.value) if self.n.sc_runner_range(self.h, C.byref(lo), C.byref(hi)) else None

    def state(self, t: float) -> np.ndarray | None:
        out = np.empty((self.count, 6))
        return out if self.n.sc_runner_state(self.h, t, out.ctypes.data_as(DP)) else None

    def level(self) -> int:
        return self.n.sc_runner_level(self.h)

    def switches(self) -> list[float]:
        out = np.zeros(256)
        n = self.n.sc_runner_switches(self.h, out.ctypes.data_as(DP), 256)
        return out[:n].tolist()

    def throughput(self, level: int) -> float:
        return self.n.sc_runner_throughput(self.h, level)

    def steps(self) -> int:
        return self.n.sc_runner_steps(self.h)

    def error(self) -> str:
        return self.n.sc_runner_error(self.h).decode("utf-8")

    def running(self) -> bool:
        return bool(self.n.sc_runner_running(self.h))

    def stop(self):
        self.n.sc_runner_stop(self.h)

    def wait_until(self, t: float, rate: float, timeout: float = 600.0) -> float:
        """Запросить момент t и ждать, пока он посчитается. Возвращает время ожидания, с."""
        import time
        t0 = time.perf_counter()
        self.request(t, rate)
        while True:
            r = self.range()
            if r and r[0] <= t <= r[1]:
                return time.perf_counter() - t0
            err = self.error()
            if err:
                raise RuntimeError(err)
            if time.perf_counter() - t0 > timeout:
                raise TimeoutError(f"момент {t} не посчитан за {timeout} с; посчитано {r}")
            time.sleep(0.002)
