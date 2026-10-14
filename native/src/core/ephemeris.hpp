// Чтение эфемерид в формате .eph (описание формата — pipeline/eph_format.py).
// Положения в км, скорости в км/с, время — секунды TDB от J2000, система ICRF.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "vec3.hpp"

namespace sc {

// Источник байтов: обычный файл (тесты, редактор) или FileAccess Godot (файл внутри .pck).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual bool read(uint64_t offset, size_t size, void *dst) = 0;
};

std::unique_ptr<ByteSource> open_file_source(const std::string &path);

struct EphSegment {
    int32_t target = 0, center = 0, degree = 0;
    double start_et = 0, end_et = 0, init = 0, intlen = 0;
    uint64_t n_records = 0, data_offset = 0;
    uint32_t rsize = 0, source = 0;
};

class Ephemeris {
public:
    // Открывает .eph. При ошибке возвращает false, текст — в error().
    bool open(std::unique_ptr<ByteSource> src);
    bool open(const std::string &path) { return open(open_file_source(path)); }

    const std::string &header_json() const { return header_; }
    const std::string &error() const { return error_; }
    const std::vector<EphSegment> &segments() const { return segs_; }

    bool has(int target) const { return by_target_.count(target) != 0; }
    int center_of(int target) const;
    // Покрытие цели: [min start, max end] по всем её сегментам.
    bool coverage(int target, double &start, double &end) const;

    // Цель относительно своего центра. false — нет данных на эту эпоху.
    bool state_rel(int target, double et, V3 &pos, V3 &vel) const;
    // Цель относительно барицентра (0) по цепочке центров.
    bool state_ssb(int target, double et, V3 &pos, V3 &vel) const;

private:
    const EphSegment *find(int target, double et) const;
    bool record(const EphSegment &s, uint64_t idx, std::vector<double> &out) const;

    std::unique_ptr<ByteSource> src_;
    std::string header_, error_;
    std::vector<EphSegment> segs_;
    std::map<int, std::vector<int>> by_target_;  // индексы сегментов в порядке файла

    // кэш последних записей: (сегмент, номер записи) → коэффициенты
    mutable std::mutex mu_;
    struct CacheEntry { const EphSegment *seg = nullptr; uint64_t idx = 0; std::vector<double> data; };
    mutable std::vector<CacheEntry> cache_;
    mutable size_t cache_next_ = 0;
};

// Схема Кленшоу в порядке операций CHBINT из SPICE: результат совпадает со SPICE до бита.
void cheb_eval(const double *coef, int ncoef, double s, double &p, double &dp);

}  // namespace sc
