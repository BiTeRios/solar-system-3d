// Время UTC ↔ TDB по алгоритму SPICE. Эталон — pipeline/timeconv.py, константы — game/data/time.json.
// Календарь пролептический григорианский; секунды TDB отсчитываются от J2000 (2000-01-01 12:00:00 TDB).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

struct UtcDate {
    int64_t year = 2000;
    int month = 1, day = 1, hour = 12, minute = 0;
    double second = 0.0;  // внутри високосной секунды — от 60 до 61
};

int64_t days_from_civil(int64_t y, int m, int d);
void civil_from_days(int64_t z, int64_t &y, int &m, int &d);

class TimeModel {
public:
    double delta_t_a = 32.184;  // TT − TAI, с
    double k = 1.657e-3;        // амплитуда TDB − TT, с
    double eb = 1.671e-2;
    double m0 = 6.239996;       // рад
    double m1 = 1.99096871e-7;  // рад/с
    std::vector<double> leap_epochs;  // формальные секунды UTC от J2000 (начало суток)
    std::vector<double> leap_values;  // ΔAT = TAI − UTC, с

    // Константы и таблица високосных секунд из game/data/time.json. false — ошибка (текст в error).
    bool load_json(const std::string &text, std::string &error);

    double delta_at(double utc_formal) const;
    double utc_to_tdb(const UtcDate &d) const;
    UtcDate tdb_to_utc(double et) const;
    double delta_et_utc(double et) const;  // аналог deltet_c(et, "ET")

private:
    double tdb_minus_tt(double t) const;
};

}  // namespace sc
