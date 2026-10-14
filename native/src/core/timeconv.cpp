#include "timeconv.hpp"

#include <algorithm>
#include <cmath>

#include "json.hpp"

namespace sc {

namespace {
constexpr int64_t kJ2000DaysFrom1970 = 10957;

int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
}  // namespace

int64_t days_from_civil(int64_t y, int m, int d) {
    // алгоритм Хиннанта, как в pipeline/timeconv.py
    y -= m <= 2;
    const int64_t era = floor_div(y, 400);
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int64_t &y, int &m, int &d) {
    z += 719468;
    const int64_t era = floor_div(z, 146097);
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = yoe + era * 400 + (m <= 2);
}

double TimeModel::delta_at(double utc_formal) const {
    // до первой записи (1972-01-01) SPICE берёт первое значение минус 1 с
    auto it = std::upper_bound(leap_epochs.begin(), leap_epochs.end(), utc_formal);
    if (it == leap_epochs.begin()) return leap_values.front() - 1.0;
    return leap_values[static_cast<size_t>(it - leap_epochs.begin()) - 1];
}

double TimeModel::tdb_minus_tt(double t) const {
    const double m = m0 + m1 * t;
    return k * std::sin(m + eb * std::sin(m));
}

double TimeModel::utc_to_tdb(const UtcDate &dt) const {
    const double day0 =
        static_cast<double>(days_from_civil(dt.year, dt.month, dt.day) - kJ2000DaysFrom1970) * 86400.0 - 43200.0;
    const double tai = day0 + delta_at(day0) + (dt.hour * 3600 + dt.minute * 60) + dt.second;
    const double tt = tai + delta_t_a;
    return tt + tdb_minus_tt(tt);
}

UtcDate TimeModel::tdb_to_utc(double et) const {
    const double tai = et - tdb_minus_tt(et) - delta_t_a;
    int64_t day = static_cast<int64_t>(std::floor((tai - delta_at(tai) + 43200.0) / 86400.0));
    double sod = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double day0 = static_cast<double>(day) * 86400.0 - 43200.0;
        sod = tai - (day0 + delta_at(day0));
        if (sod < 0.0) {
            --day;
            continue;
        }
        const double day_len = 86400.0 + delta_at(day0 + 86400.0) - delta_at(day0);
        if (sod >= day_len) {
            ++day;
            continue;
        }
        break;
    }
    UtcDate out;
    civil_from_days(day + kJ2000DaysFrom1970, out.year, out.month, out.day);
    if (sod >= 86400.0) {  // внутри високосной секунды
        out.hour = 23;
        out.minute = 59;
        out.second = sod - 86340.0;
        return out;
    }
    out.hour = static_cast<int>(std::floor(sod / 3600.0));
    out.minute = static_cast<int>(std::floor((sod - out.hour * 3600) / 60.0));
    out.second = sod - out.hour * 3600 - out.minute * 60;
    return out;
}

double TimeModel::delta_et_utc(double et) const {
    const double tai = et - tdb_minus_tt(et) - delta_t_a;
    double dat = leap_values.front() - 1.0;
    for (size_t i = 0; i < leap_epochs.size(); ++i)
        if (tai >= leap_epochs[i] + leap_values[i]) dat = leap_values[i];
    return delta_t_a + dat + tdb_minus_tt(et);
}

bool TimeModel::load_json(const std::string &text, std::string &error) {
    std::string e;
    const Json j = Json::parse(text, &e);
    if (!e.empty()) {
        error = "time.json: " + e;
        return false;
    }
    if (!j["DELTA_T_A"].is_number() || j["M"].size() != 2 || j["leap_seconds"].size() == 0) {
        error = "time.json: нет констант или таблицы високосных секунд";
        return false;
    }
    delta_t_a = j["DELTA_T_A"].num();
    k = j["K"].num();
    eb = j["EB"].num();
    m0 = j["M"][0].num();
    m1 = j["M"][1].num();
    leap_epochs.clear();
    leap_values.clear();
    for (const Json &r : j["leap_seconds"].elements()) {
        leap_epochs.push_back(r["utc_formal_s"].num());
        leap_values.push_back(r["delta_at"].num());
    }
    error.clear();
    return true;
}

}  // namespace sc
