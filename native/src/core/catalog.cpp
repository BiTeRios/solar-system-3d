#include "catalog.hpp"

#include <cstdlib>

#include "json.hpp"

namespace sc {

namespace {

V3 pole_vector(double ra_deg, double dec_deg) {
    const double ra = ra_deg * kDeg, dec = dec_deg * kDeg;
    return {std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec)};
}

std::vector<double> zonal_from(const Json &j) {
    std::vector<double> out;
    for (const std::string &k : j.keys()) {
        const int n = std::atoi(k.c_str());
        if (n < 2) continue;
        if (out.size() < static_cast<size_t>(n) + 1) out.resize(static_cast<size_t>(n) + 1, 0.0);
        out[static_cast<size_t>(n)] = j[k].num();
    }
    return out;
}

std::shared_ptr<const HarmonicField> field_from(const Json &f) {
    auto h = std::make_shared<HarmonicField>();
    h->resize(f["nmax"].integer());
    h->r_ref = f["radius_km"].num();
    for (int pass = 0; pass < 2; ++pass) {
        const Json &tab = f[pass == 0 ? "C" : "S"];
        for (const std::string &k : tab.keys()) {
            const size_t comma = k.find(',');
            const int n = std::atoi(k.substr(0, comma).c_str()), m = std::atoi(k.substr(comma + 1).c_str());
            (pass == 0 ? h->c : h->s)[static_cast<size_t>(HarmonicField::idx(n, m))] = tab[k].num();
        }
    }
    const Json &r = f["rotation"];
    h->ra0 = r["ra_deg"].num();
    h->dec0 = r["dec_deg"].num();
    h->ra_rate = r["ra_rate_deg_cy"].num();
    h->dec_rate = r["dec_rate_deg_cy"].num();
    h->w0 = r["w_deg"].num();
    h->wdot = r["w_rate_deg_day"].num();
    h->epoch_et = (r["epoch_jd_tdb"].num() - 2451545.0) * 86400.0;
    return h;
}

}  // namespace

bool Catalog::load(const std::string &bodies_json, const std::string &constants_json,
                   const std::string &dynamics_json, std::string &error) {
    std::string e;
    const Json bj = Json::parse(bodies_json, &e);
    if (!e.empty()) {
        error = "bodies.json: " + e;
        return false;
    }
    const Json cj = Json::parse(constants_json, &e);
    if (!e.empty()) {
        error = "spice_constants.json: " + e;
        return false;
    }
    const Json dj = Json::parse(dynamics_json, &e);
    if (!e.empty()) {
        error = "dynamics.json: " + e;
        return false;
    }
    bodies.clear();
    systems.clear();
    std::vector<std::string> parent_keys;
    for (const Json &b : bj["bodies"].elements()) {
        BodyInfo info;
        info.key = b["key"].str();
        info.name = b["name"].str();
        info.name_en = b["name_en"].str();
        info.kind = b["kind"].str();
        info.naif = b["naif"].integer();
        info.visible = b["visible"].boolean(true);
        info.gm = b["gm_km3s2"]["v"].num(0.0);
        const Json &phys = b["phys"];
        const double rmean = phys["radius_mean_km"]["v"].num(0.0);
        const std::vector<double> radii = b["radii_km"]["v"].numbers();
        if (radii.size() == 3) {
            info.radii = {radii[0], radii[1], radii[2]};
        } else {
            info.radii = {rmean, rmean, rmean};
        }
        info.mean_radius = rmean > 0 ? rmean : std::cbrt(info.radii.x * info.radii.y * info.radii.z);
        info.geometric_albedo = phys["geometric_albedo"]["v"].num(NAN);
        info.bond_albedo = phys["bond_albedo"]["v"].num(NAN);
        info.rotation_h = phys["rotation_h"]["v"].num(NAN);
        info.mean_temp_k = phys["mean_temp_K"]["v"].num(NAN);
        info.blackbody_k = phys["blackbody_K"]["v"].num(NAN);
        const Json &c = cj["bodies"][std::to_string(info.naif)];
        if (c.has("pole_ra") && c.has("pm")) {
            info.has_rotation = true;
            info.rot.pole_ra = c["pole_ra"].numbers();
            info.rot.pole_dec = c["pole_dec"].numbers();
            info.rot.pm = c["pm"].numbers();
            info.rot.nut_ra = c["nut_prec_ra"].numbers();
            info.rot.nut_dec = c["nut_prec_dec"].numbers();
            info.rot.nut_pm = c["nut_prec_pm"].numbers();
            if (info.naif > 100 && info.naif < 1000) {  // углы системы барицентра, как в SPICE: 501 → 5
                const Json &s = cj["systems"][std::to_string(info.naif / 100)];
                info.rot.system_angles = s["nut_prec_angles"].numbers();
                info.rot.max_phase_degree = s["max_phase_degree"].integer(1);
            }
        }
        for (const Json &r : b["rings"].elements()) {
            RingBand rb;
            rb.name = r["name"].str();
            rb.inner_km = r["inner_km"].num();
            rb.outer_km = r["outer_km"].num();
            rb.tau_lo = r["optical_depth"][0].num(NAN);
            rb.tau_hi = r["optical_depth"][1].num(NAN);
            rb.albedo_lo = r["albedo"][0].num(NAN);
            rb.albedo_hi = r["albedo"][1].num(NAN);
            info.rings.push_back(rb);
        }
        parent_keys.push_back(b["parent"].is_string() ? b["parent"].str() : std::string());
        bodies.push_back(std::move(info));
    }
    for (size_t i = 0; i < bodies.size(); ++i) bodies[i].parent = parent_keys[i].empty() ? -1 : index_of_key(parent_keys[i]);

    for (const std::string &name : dj["systems"].keys()) {
        const Json &s = dj["systems"][name];
        DynSystem d;
        d.name = name;
        d.barycenter = s["barycenter"].integer();
        d.planet = s["planet"].integer();
        for (const Json &m : s["moons"].elements()) d.moons.push_back(m.integer());
        for (const std::string &k : s["gm"].keys()) d.gm[std::atoi(k.c_str())] = s["gm"][k].num();
        d.system_gm_in_file = s["system_gm_in_file"].num();
        d.unmodeled_gm = s["unmodeled_gm"].num();
        d.radius_km = s["radius_km"].num();
        d.zonal = zonal_from(s["zonal"]);
        const Json &p = s["pole"];
        d.pole_epoch_days = p["epoch_jd_tdb"].num() - 2451545.0;
        d.pole_ra = p["ra_deg"].num();
        d.pole_dec = p["dec_deg"].num();
        d.pole_ra_rate = p["ra_rate_deg_cy"].num();
        d.pole_dec_rate = p["dec_rate_deg_cy"].num();
        if (s["field"].is_object()) d.field = field_from(s["field"]);
        systems.push_back(std::move(d));
    }
    earth_zonal = zonal_from(dj["earth"]["zonal"]);
    earth_radius_km = dj["earth"]["radius_km"].num();
    if (bodies.empty()) {
        error = "bodies.json: нет тел";
        return false;
    }
    error.clear();
    return true;
}

int Catalog::index_of_naif(int naif) const {
    for (size_t i = 0; i < bodies.size(); ++i)
        if (bodies[i].naif == naif) return static_cast<int>(i);
    return -1;
}

int Catalog::index_of_key(const std::string &key) const {
    for (size_t i = 0; i < bodies.size(); ++i)
        if (bodies[i].key == key) return static_cast<int>(i);
    return -1;
}

const DynSystem *Catalog::system_of_planet(int naif) const {
    for (const DynSystem &s : systems)
        if (s.planet == naif) return &s;
    return nullptr;
}

const DynSystem *Catalog::system_of_moon(int naif) const {
    for (const DynSystem &s : systems)
        for (int m : s.moons)
            if (m == naif) return &s;
    return nullptr;
}

bool Catalog::initial_state(const Ephemeris &eph, double et, const std::vector<bool> *include,
                            std::vector<SimBody> &out, std::string &error) const {
    // Порядок операций — как в pipeline/tests/solar_system.py: тест сверяет результат до бита.
    auto chosen = [&](size_t i) { return include == nullptr || (i < include->size() && (*include)[i]); };
    std::map<int, double> by_naif;
    for (const DynSystem &s : systems)
        for (const auto &kv : s.gm) by_naif[kv.first] = kv.second;
    // Если ни одного спутника системы нет, планета ставится в барицентр системы с её полной массой.
    std::map<int, const DynSystem *> whole;
    for (const DynSystem &s : systems) {
        bool any = false;
        for (int m : s.moons) {
            const int k = index_of_naif(m);
            if (k >= 0 && chosen(static_cast<size_t>(k))) any = true;
        }
        if (!any) whole[s.planet] = &s;
    }
    out.clear();
    std::vector<bool> as_system;
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (!chosen(i)) continue;
        const BodyInfo &b = bodies[i];
        SimBody r;
        r.naif = b.naif;
        r.catalog = static_cast<int>(i);
        auto g = by_naif.find(b.naif);
        r.body.gm = g != by_naif.end() ? g->second : b.gm;
        const auto w = whole.find(b.naif);
        const int target = w != whole.end() ? w->second->barycenter : b.naif;
        if (!eph.state_ssb(target, et, r.body.x, r.body.v)) {
            error = "нет эфемерид для " + b.key + " на эту эпоху";
            return false;
        }
        if (w != whole.end()) r.body.gm = w->second->system_gm_in_file;
        as_system.push_back(w != whole.end());
        out.push_back(std::move(r));
    }
    auto rec = [&](int naif) -> SimBody * {
        for (SimBody &r : out)
            if (r.naif == naif) return &r;
        return nullptr;
    };
    auto is_system = [&](const SimBody *r) { return as_system[static_cast<size_t>(r - out.data())]; };
    for (const DynSystem &s : systems) {
        SimBody *r = rec(s.planet);
        if (r == nullptr || s.zonal.empty() || is_system(r)) continue;
        const double t_cy = (et / 86400.0 - s.pole_epoch_days) / 36525.0;
        r->body.zonal = s.zonal;
        r->body.r_ref = s.radius_km;
        r->body.pole = pole_vector(s.pole_ra + s.pole_ra_rate * t_cy, s.pole_dec + s.pole_dec_rate * t_cy);
        if (s.field) {
            r->body.field = s.field;
            r->body.zonal.clear();
        }
    }
    if (SimBody *e = rec(399)) {
        const int k = index_of_naif(399);
        if (k >= 0 && bodies[static_cast<size_t>(k)].has_rotation) {
            const M3 m = bodies[static_cast<size_t>(k)].rot.j2000_to_body(et);
            e->body.zonal = earth_zonal;
            e->body.r_ref = earth_radius_km;
            e->body.pole = {m.m[2][0], m.m[2][1], m.m[2][2]};
        }
    }
    // Масса мелких и отсутствующих спутников — на планету; затем вся система сдвигается так,
    // чтобы её центр масс совпал с барицентром из эфемерид.
    for (const DynSystem &s : systems) {
        SimBody *r = rec(s.planet);
        if (r == nullptr || is_system(r)) continue;
        double missing = 0.0;
        for (const auto &kv : s.gm) {
            bool is_moon = false;
            for (int m : s.moons) is_moon |= (m == kv.first);
            if (is_moon && rec(kv.first) == nullptr) missing += kv.second;
        }
        r->body.gm += s.unmodeled_gm + missing;
        std::vector<SimBody *> members{r};
        for (int m : s.moons)
            if (SimBody *mm = rec(m)) members.push_back(mm);
        V3 pb, vb;
        if (!eph.state_ssb(s.barycenter, et, pb, vb)) {
            error = "нет эфемерид барицентра системы " + s.name;
            return false;
        }
        // сумма масс — как sum() в Python 3.12+ (компенсированное суммирование Ноймайера): совпадение до бита
        double total = members[0]->body.gm, comp = 0.0;
        for (size_t k = 1; k < members.size(); ++k) {
            const double x = members[k]->body.gm, t = total + x;
            comp += std::fabs(total) >= std::fabs(x) ? (total - t) + x : (x - t) + total;
            total = t;
        }
        if (comp != 0.0 && std::isfinite(comp)) total += comp;
        V3 cx, cv;
        for (SimBody *m : members) {
            cx += m->body.x * m->body.gm;
            cv += m->body.v * m->body.gm;
        }
        cx = cx / total;
        cv = cv / total;
        const V3 sx = pb - cx, sv = vb - cv;
        for (SimBody *m : members) {
            m->body.x = m->body.x + sx;
            m->body.v = m->body.v + sv;
        }
    }
    // спутники (NNN, кроме NN99) — относительно своей планеты
    for (SimBody &r : out) {
        r.body.parent = -1;
        if (r.naif > 100 && r.naif < 1000 && r.naif % 100 != 99) {
            const int pn = (r.naif / 100) * 100 + 99;
            for (size_t k = 0; k < out.size(); ++k)
                if (out[k].naif == pn) r.body.parent = static_cast<int>(k);
        }
    }
    error.clear();
    return true;
}

}  // namespace sc
