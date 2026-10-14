"""Формат эфемерид .eph: запись и чтение (эталонная реализация для тестов и для C++-читателя).

Файл little-endian:
    0   char[8]  сигнатура b"SSEPH\\0\\0\\1"
    8   u32      версия формата (1)
    12  u32      длина заголовка JSON в байтах
    16  u32      число сегментов
    20  u32      резерв (0)
    24  JSON     заголовок (UTF-8), дополнен пробелами до кратности 8
    ..  таблица сегментов: по SEG_SIZE байт на сегмент (см. SEG_DTYPE)
    ..  данные: float64, записи Чебышёва подряд, смещение каждого сегмента кратно 8

Сегмент повторяет SPK тип 2: запись = [MID, RADIUS, X0..Xn, Y0..Yn, Z0..Zn], n = degree.
Положение в км, время — секунды TDB от J2000, система ICRF (в SPICE — J2000).
Скорость — производная полинома, делённая на RADIUS, как в SPICE (spke02).
"""
from __future__ import annotations

import json
from dataclasses import dataclass

import numpy as np

MAGIC = b"SSEPH\x00\x00\x01"
VERSION = 1
HEAD_SIZE = 24

SEG_DTYPE = np.dtype([
    ("target", "<i4"),
    ("center", "<i4"),
    ("spk_type", "<i4"),
    ("degree", "<i4"),
    ("start_et", "<f8"),   # начало покрытия
    ("end_et", "<f8"),     # конец покрытия
    ("init", "<f8"),       # начало первой записи
    ("intlen", "<f8"),     # длина записи, с
    ("n_records", "<u8"),
    ("data_offset", "<u8"),  # смещение данных от начала файла, байт
    ("rsize", "<u4"),      # длина записи в float64
    ("source", "<u4"),     # индекс в header["sources"]
])
SEG_SIZE = SEG_DTYPE.itemsize  # 72


@dataclass
class Segment:
    target: int
    center: int
    start_et: float
    end_et: float
    init: float
    intlen: float
    rsize: int
    records: np.ndarray  # (n_records, rsize) float64
    source: int = 0

    @property
    def degree(self) -> int:
        return (self.rsize - 2) // 3 - 1


def write_eph(path, header: dict, segments: list[Segment]) -> None:
    head = dict(header)
    head["format"] = "SSEPH"
    head["version"] = VERSION
    js = json.dumps(head, ensure_ascii=False, indent=1).encode("utf-8")
    js += b" " * ((-len(js)) % 8)
    table = np.zeros(len(segments), dtype=SEG_DTYPE)
    offset = HEAD_SIZE + len(js) + SEG_SIZE * len(segments)
    offset += (-offset) % 8
    data_start = offset
    for i, s in enumerate(segments):
        assert s.records.dtype == np.float64 and s.records.shape[1] == s.rsize
        assert (s.rsize - 2) % 3 == 0, "ожидается SPK тип 2"
        table[i] = (s.target, s.center, 2, s.degree, s.start_et, s.end_et, s.init, s.intlen,
                    s.records.shape[0], offset, s.rsize, s.source)
        offset += s.records.nbytes
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(np.array([VERSION, len(js), len(segments), 0], dtype="<u4").tobytes())
        f.write(js)
        f.write(table.tobytes())
        f.write(b"\x00" * (data_start - f.tell()))
        for s in segments:
            f.write(np.ascontiguousarray(s.records, dtype="<f8").tobytes())


class EphFile:
    """Чтение .eph через memmap. Методы векторизованы по времени."""

    def __init__(self, path):
        with open(path, "rb") as f:
            if f.read(8) != MAGIC:
                raise ValueError(f"{path}: не файл .eph")
            version, js_len, n_seg, _ = np.frombuffer(f.read(16), dtype="<u4")
            if version != VERSION:
                raise ValueError(f"{path}: версия {version}, ожидается {VERSION}")
            self.header = json.loads(f.read(int(js_len)).decode("utf-8"))
            self.table = np.frombuffer(f.read(SEG_SIZE * int(n_seg)), dtype=SEG_DTYPE).copy()
        self._mm = np.memmap(path, dtype="<f8", mode="r")
        # центр для каждой цели (у одной цели может быть несколько сегментов по времени, центр общий)
        self.center_of: dict[int, int] = {}
        for row in self.table:
            t, c = int(row["target"]), int(row["center"])
            if self.center_of.setdefault(t, c) != c:
                raise ValueError(f"цель {t}: разные центры {self.center_of[t]} и {c}")

    def segments_for(self, target: int):
        return [r for r in self.table if int(r["target"]) == target]

    def records(self, row) -> np.ndarray:
        start = int(row["data_offset"]) // 8
        n, rsize = int(row["n_records"]), int(row["rsize"])
        return self._mm[start:start + n * rsize].reshape(n, rsize)

    def coverage(self, target: int) -> list[tuple[float, float]]:
        segs = sorted(self.segments_for(target), key=lambda r: r["start_et"])
        return [(float(r["start_et"]), float(r["end_et"])) for r in segs]

    def state_rel(self, target: int, et) -> tuple[np.ndarray, np.ndarray]:
        """Положение (км) и скорость (км/с) цели относительно её центра. et — скаляр или массив."""
        et = np.atleast_1d(np.asarray(et, dtype=np.float64))
        pos = np.full((et.size, 3), np.nan)
        vel = np.full((et.size, 3), np.nan)
        # как в SPICE: при перекрытии побеждает сегмент, записанный позже
        for row in self.segments_for(target):
            mask = (et >= row["start_et"]) & (et <= row["end_et"])
            if not mask.any():
                continue
            p, v = _cheb_eval(self.records(row), float(row["init"]), float(row["intlen"]),
                              int(row["degree"]), et[mask])
            pos[mask], vel[mask] = p, v
        if np.isnan(pos).any():
            bad = et[np.isnan(pos[:, 0])][0]
            raise ValueError(f"цель {target}: нет данных на ET {bad}")
        return pos, vel

    def state_ssb(self, target: int, et) -> tuple[np.ndarray, np.ndarray]:
        """Состояние относительно барицентра Солнечной системы (0) по цепочке центров."""
        pos, vel = self.state_rel(target, et)
        c = self.center_of[target]
        while c != 0:
            p, v = self.state_rel(c, et)
            pos, vel = pos + p, vel + v
            c = self.center_of[c]
        return pos, vel


def _cheb_eval(rec: np.ndarray, init: float, intlen: float, deg: int, et: np.ndarray):
    """Схема Кленшоу в том же порядке операций, что CHBINT в SPICE: результат совпадает до последнего бита."""
    n = rec.shape[0]
    # номер записи: как в SPKR02, (et - init) / intlen с отсечением по краям
    idx = np.floor((et - init) / intlen).astype(np.int64)
    idx = np.clip(idx, 0, n - 1)
    r = np.asarray(rec[idx])  # (m, rsize)
    mid, radius = r[:, 0], r[:, 1]
    s = (et - mid[:]) / radius
    s2 = 2.0 * s
    ncoef = deg + 1
    coef = r[:, 2:].reshape(-1, 3, ncoef)  # (m, xyz, k)
    s_, s2_ = s[:, None], s2[:, None]
    w1 = w2 = w3 = np.zeros(coef.shape[:2])
    d1 = d2 = d3 = np.zeros(coef.shape[:2])
    for j in range(ncoef - 1, 0, -1):
        w3, w2 = w2, w1
        w1 = coef[:, :, j] + (s2_ * w2 - w3)
        d3, d2 = d2, d1
        d1 = w2 * 2.0 + d2 * s2_ - d3
    pos = coef[:, :, 0] + (s_ * w1 - w2)
    vel = (w1 + s_ * d1 - d2) / radius[:, None]
    return pos, vel
