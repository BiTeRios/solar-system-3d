"""Тела MVP: ключ, код NAIF, названия, родитель, вид. Согласовано 11.10.2026 (docs/architecture.md)."""
from __future__ import annotations

# (ключ, NAIF, название, англ., родитель, вид)
BODIES = [
    ("sun", 10, "Солнце", "Sun", None, "star"),
    ("mercury", 199, "Меркурий", "Mercury", "sun", "planet"),
    ("venus", 299, "Венера", "Venus", "sun", "planet"),
    ("earth", 399, "Земля", "Earth", "sun", "planet"),
    ("moon", 301, "Луна", "Moon", "earth", "moon"),
    ("mars", 499, "Марс", "Mars", "sun", "planet"),
    ("phobos", 401, "Фобос", "Phobos", "mars", "moon"),
    ("deimos", 402, "Деймос", "Deimos", "mars", "moon"),
    ("jupiter", 599, "Юпитер", "Jupiter", "sun", "planet"),
    ("io", 501, "Ио", "Io", "jupiter", "moon"),
    ("europa", 502, "Европа", "Europa", "jupiter", "moon"),
    ("ganymede", 503, "Ганимед", "Ganymede", "jupiter", "moon"),
    ("callisto", 504, "Каллисто", "Callisto", "jupiter", "moon"),
    ("saturn", 699, "Сатурн", "Saturn", "sun", "planet"),
    ("mimas", 601, "Мимас", "Mimas", "saturn", "moon"),
    ("enceladus", 602, "Энцелад", "Enceladus", "saturn", "moon"),
    ("tethys", 603, "Тефия", "Tethys", "saturn", "moon"),
    ("dione", 604, "Диона", "Dione", "saturn", "moon"),
    ("rhea", 605, "Рея", "Rhea", "saturn", "moon"),
    ("titan", 606, "Титан", "Titan", "saturn", "moon"),
    ("iapetus", 608, "Япет", "Iapetus", "saturn", "moon"),
    ("uranus", 799, "Уран", "Uranus", "sun", "planet"),
    ("miranda", 705, "Миранда", "Miranda", "uranus", "moon"),
    ("ariel", 701, "Ариэль", "Ariel", "uranus", "moon"),
    ("umbriel", 702, "Умбриэль", "Umbriel", "uranus", "moon"),
    ("titania", 703, "Титания", "Titania", "uranus", "moon"),
    ("oberon", 704, "Оберон", "Oberon", "uranus", "moon"),
    ("neptune", 899, "Нептун", "Neptune", "sun", "planet"),
    ("triton", 801, "Тритон", "Triton", "neptune", "moon"),
    ("pluto", 999, "Плутон", "Pluto", "sun", "dwarf"),
    ("charon", 901, "Харон", "Charon", "pluto", "moon"),
    ("ceres", 2000001, "Церера", "Ceres", "sun", "dwarf"),
    ("eris", 2136199, "Эрида", "Eris", "sun", "dwarf"),
    ("haumea", 2136108, "Хаумеа", "Haumea", "sun", "dwarf"),
    ("makemake", 2136472, "Макемаке", "Makemake", "sun", "dwarf"),
    ("halley", 1000036, "Комета Галлея", "1P/Halley", "sun", "comet"),
]

# Невидимые возмущающие тела для N-body (только масса и орбита)
PERTURBERS = [
    ("pallas", 2000002, "Паллада", "Pallas"),
    ("vesta", 2000004, "Веста", "Vesta"),
    ("hygiea", 2000010, "Гигея", "Hygiea"),
]

# Барицентры систем: GM барицентра нужен для N-body по системам
BARYCENTERS = {1: "mercury", 2: "venus", 3: "earth", 4: "mars", 5: "jupiter",
               6: "saturn", 7: "uranus", 8: "neptune", 9: "pluto"}

# Тела, положения которых берутся из .eph (остальные — из SPK малых тел, отдельный шаг)
EPH_BODIES = {naif for _, naif, *_ in BODIES if naif < 1000}
