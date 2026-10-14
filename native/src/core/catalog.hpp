// Каталог тел для игры: bodies.json, spice_constants.json, dynamics.json (их собирает pipeline/).
// И начальное состояние «Симуляции» — перенос pipeline/tests/solar_system.py (system_bodies + build_sim) на C++.
#pragma once

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ephemeris.hpp"
#include "gravity_field.hpp"
#include "nbody.hpp"
#include "orientation.hpp"
#include "vec3.hpp"

namespace sc {

class Json;

struct RingBand {
    std::string name;
    double inner_km = 0, outer_km = 0;
    double tau_lo = NAN, tau_hi = NAN;        // оптическая толщина (диапазон со страницы NASA)
    double albedo_lo = NAN, albedo_hi = NAN;
};

struct BodyInfo {
    std::string key, name, name_en, kind;  // kind: star, planet, dwarf, moon, comet, perturber
    int naif = 0;
    int parent = -1;       // индекс в каталоге (у Солнца −1)
    bool visible = true;   // возмущающие астероиды (Паллада, Веста, Гигея) не рисуются
    double gm = 0;         // км³/с² по bodies.json; 0 — неизвестна
    V3 radii;              // полуоси эллипсоида, км (a, b, c по осям тела)
    double mean_radius = 0;
    double geometric_albedo = NAN;
    // со страниц NASA (этап 6: производные величины); NaN — нет данных
    double bond_albedo = NAN;
    double rotation_h = NAN;    // период вращения, ч (знак — обратное вращение)
    double mean_temp_k = NAN;   // средняя температура (у Солнца — эффективная)
    double blackbody_k = NAN;   // равновесная температура чёрного тела
    bool has_rotation = false;
    RotationModel rot;     // pck00011
    std::vector<RingBand> rings;
};

// Планетная система из dynamics.json: модель динамики, с которой JPL строила спутниковое ядро.
struct DynSystem {
    std::string name;
    int barycenter = 0, planet = 0;
    std::vector<int> moons;               // коды NAIF моделируемых спутников
    std::map<int, double> gm;             // код NAIF → GM
    double system_gm_in_file = 0, unmodeled_gm = 0, radius_km = 0;
    std::vector<double> zonal;            // zonal[n] = Jₙ
    double pole_epoch_days = 0;  // эпоха полюса: JD TDB − 2451545
    double pole_ra = 0, pole_dec = 0, pole_ra_rate = 0, pole_dec_rate = 0;  // град, град/век
    std::shared_ptr<const HarmonicField> field;  // полное поле (Марс)
};

// Тело N-body вместе с кодом NAIF и индексом в каталоге.
struct SimBody {
    int naif = 0;
    int catalog = -1;
    Body body;
};

class Catalog {
public:
    bool load(const std::string &bodies_json, const std::string &constants_json, const std::string &dynamics_json,
              std::string &error);

    std::vector<BodyInfo> bodies;
    std::vector<DynSystem> systems;
    std::vector<double> earth_zonal;
    double earth_radius_km = 0;

    int index_of_naif(int naif) const;
    int index_of_key(const std::string &key) const;
    const DynSystem *system_of_planet(int naif) const;
    const DynSystem *system_of_moon(int naif) const;

    // Начальное состояние «Симуляции» на эпоху et: все тела каталога (включая возмущающие астероиды),
    // спутники — с parent на свою планету. include[i] — брать ли тело i каталога (nullptr — все); если ни одного
    // спутника системы нет, планета ставится в барицентр системы с её полной массой.
    // false — нет эфемерид на эту эпоху (текст в error).
    bool initial_state(const Ephemeris &eph, double et, const std::vector<bool> *include, std::vector<SimBody> &out,
                       std::string &error) const;
};

}  // namespace sc
