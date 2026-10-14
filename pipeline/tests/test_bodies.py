"""bodies.json и sources.json: полнота, ссылки на источники, совпадение с ядрами и таблицами NASA."""
import json

import pytest

from bodies import BODIES, PERTURBERS
from build_bodies import DATA, build

# Чего нет в разрешённых источниках (11.10.2026, 13.10.2026). Меняется только вместе с docs/sources.md.
# Сферическое (бондовское) альбедо спутников и малых тел в таблицах NASA не приводится.
EXPECTED_MISSING = {
    "eris": {"bond_albedo"},
    "haumea": {"bond_albedo"},
    "makemake": {"bond_albedo"},
    "halley": {"gm_km3s2", "bond_albedo"},
    "ceres": {"bond_albedo"},
    "phobos": {"bond_albedo", "geometric_albedo"},
    "deimos": {"bond_albedo", "geometric_albedo"},
    "charon": {"bond_albedo", "geometric_albedo"},
}
SATELLITE_BOND_MISSING = {"bond_albedo"}


@pytest.fixture(scope="module")
def built():
    bodies, srcs = build()
    spice = json.loads((DATA / "spice_constants.json").read_text(encoding="utf-8"))
    return bodies, srcs, spice


def _values(obj):
    if isinstance(obj, dict):
        if "v" in obj and "src" in obj:
            yield obj
        for v in obj.values():
            yield from _values(v)
    elif isinstance(obj, list):
        for v in obj:
            yield from _values(v)


def test_all_bodies(built):
    bodies, _, _ = built
    keys = [b["key"] for b in bodies["bodies"]]
    assert keys == [b[0] for b in BODIES] + [p[0] for p in PERTURBERS]
    assert sum(b["visible"] for b in bodies["bodies"]) == 36


def test_every_value_has_known_source(built):
    bodies, srcs, _ = built
    vals = list(_values(bodies["bodies"]))
    for b in bodies["bodies"]:
        if b["visible"]:
            assert "radii_km" in b or "radius_mean_km" in b["phys"], f"{b['key']}: нет радиуса"
    unknown = {v["src"] for v in vals} - set(srcs)
    assert not unknown, f"нет в sources.json: {unknown}"


def test_spice_values_exact(built):
    bodies, _, spice = built
    for b in bodies["bodies"]:
        sc = spice["bodies"].get(str(b["naif"]), {})
        if "gm" in sc:
            assert b["gm_km3s2"]["v"] == sc["gm"]
        if "radii" in sc:
            assert b["radii_km"]["v"] == sc["radii"]


def test_fact_sheet_values(built):
    """Значения как на страницах NASA (11.10.2026). Марс — проверка, что таблица спутников не затирает планету."""
    by = {b["key"]: b["phys"] for b in built[0]["bodies"]}
    assert by["earth"]["bond_albedo"]["v"] == 0.294
    assert by["earth"]["j2_1e6"]["v"] == 1082.63
    assert by["earth"]["mean_temp_K"]["v"] == 288.0
    assert by["earth"]["surface_pressure_bar"]["v"] == pytest.approx(1.014)
    assert by["mars"]["density_kgm3"]["v"] == 3934.0
    assert by["mars"]["geometric_albedo"]["v"] == 0.17
    assert by["sun"]["mean_temp_K"]["v"] == 5772.0
    assert by["sun"]["radius_mean_km"]["v"] == 695700.0
    assert by["venus"]["rotation_h"]["v"] < 0  # ретроградное вращение
    assert by["titan"]["radius_mean_km"]["v"] == 2574.76
    assert by["pluto"]["mean_temp_K"]["range"] == [24.0, 38.0]
    assert by["jupiter"]["surface_pressure_bar"]["v"] is None  # «>>1000 bars»: поверхности нет


def test_missing_is_exactly_documented(built):
    bodies = {b["key"]: b for b in built[0]["bodies"]}
    missing = {k: set(v) for k, v in built[0]["missing"].items()}
    for key, gaps in list(missing.items()):
        if bodies[key]["kind"] == "moon" and gaps == SATELLITE_BOND_MISSING:
            del missing[key]
    assert missing == EXPECTED_MISSING


def test_literature_values(built):
    """Значения из статей переписаны как в первоисточнике (docs/sources.md)."""
    by = {b["key"]: b for b in built[0]["bodies"]}
    assert by["eris"]["gm_km3s2"]["v"] == pytest.approx(1098.99, abs=0.01)
    assert by["eris"]["phys"]["radius_mean_km"]["v"] == 1163.0
    assert by["haumea"]["gm_km3s2"]["v"] == pytest.approx(267.37, abs=0.01)
    assert by["haumea"]["radii_km"]["v"] == [1161.0, 852.0, 513.0]
    mk = by["makemake"]["gm_km3s2"]
    assert mk["v"] == pytest.approx(179.3, abs=0.1) and built[1][mk["src"]]["preprint"] is True
    # плотность Хаумеа по этим числам — как в Ortiz et al. 2017 (1885 ± 80 кг/м³)
    import math
    a, b, c = by["haumea"]["radii_km"]["v"]
    rho = by["haumea"]["gm_km3s2"]["v"] / 6.67430e-20 / (4 / 3 * math.pi * a * b * c * 1e9)
    assert rho == pytest.approx(1885, abs=80)


def test_saturn_rings(built):
    """Кольца Сатурна со страницы NASA: края колец как на странице, деление Кассини между B и A."""
    rings = {r["name"]: r for r in next(b for b in built[0]["bodies"] if b["key"] == "saturn")["rings"]}
    assert list(rings) == ["D", "C", "B", "A", "G", "E"]
    assert (rings["C"]["inner_km"], rings["C"]["outer_km"]) == (74658.0, 91975.0)
    assert (rings["B"]["inner_km"], rings["B"]["outer_km"]) == (91975.0, 117507.0)
    assert (rings["A"]["inner_km"], rings["A"]["outer_km"]) == (122340.0, 136780.0)
    assert rings["B"]["optical_depth"] == [0.4, 2.5] and rings["A"]["albedo"] == [0.4, 0.6]
    assert all(r["src"] in built[1] for r in rings.values())


def test_satellite_albedo_from_nasa_tables(built):
    """Геометрическое альбедо спутников — из таблиц NASA по системам (*satfact.html), как на странице."""
    by = {b["key"]: b["phys"] for b in built[0]["bodies"]}
    expect = {"io": 0.62, "europa": 0.68, "ganymede": 0.44, "callisto": 0.19, "mimas": 0.6, "enceladus": 1.0,
              "tethys": 0.8, "dione": 0.7, "rhea": 0.7, "titan": 0.22, "miranda": 0.32, "ariel": 0.39,
              "umbriel": 0.21, "titania": 0.27, "oberon": 0.23, "triton": 0.72}
    for key, v in expect.items():
        g = by[key]["geometric_albedo"]
        assert g["v"] == v and g["src"].startswith("nasa_fact_") and g["src"] in built[1], key
    iap = by["iapetus"]["geometric_albedo"]
    assert iap["range"] == [0.05, 0.5] and iap["v"] == pytest.approx(0.275)


def test_dwarf_albedo_from_literature(built):
    by = {b["key"]: b["phys"] for b in built[0]["bodies"]}
    assert by["eris"]["geometric_albedo"]["v"] == 0.96 and by["eris"]["geometric_albedo"]["src"] == "lit_sicardy2011"
    assert by["haumea"]["geometric_albedo"]["v"] == 0.51 and by["haumea"]["geometric_albedo"]["src"] == "lit_ortiz2017"
    assert by["makemake"]["geometric_albedo"]["v"] == 0.77
    assert built[1]["lit_ortiz2012"]["url"].endswith("nature11597")
