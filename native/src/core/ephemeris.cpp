#include "ephemeris.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sc {

namespace {

constexpr char kMagic[8] = {'S', 'S', 'E', 'P', 'H', 0, 0, 1};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeadSize = 24;
constexpr size_t kSegSize = 72;
constexpr size_t kCacheSize = 64;

class StdFileSource : public ByteSource {
public:
    explicit StdFileSource(std::FILE *f) : f_(f) {}
    ~StdFileSource() override { std::fclose(f_); }
    bool read(uint64_t offset, size_t size, void *dst) override {
        if (_fseeki64(f_, static_cast<long long>(offset), SEEK_SET) != 0) return false;
        return std::fread(dst, 1, size, f_) == size;
    }

private:
    std::FILE *f_;
};

template <typename T>
T get(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof(T));  // файл little-endian, как и x86-64
    return v;
}

}  // namespace

std::unique_ptr<ByteSource> open_file_source(const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    return std::make_unique<StdFileSource>(f);
}

void cheb_eval(const double *coef, int ncoef, double s, double &p, double &dp) {
    // CHBINT (SPICE): W — значения, DW — производные, обход от старшего коэффициента
    const double s2 = 2.0 * s;
    double w1 = 0, w2 = 0, w3 = 0, d1 = 0, d2 = 0, d3 = 0;
    for (int j = ncoef - 1; j >= 1; --j) {
        w3 = w2;
        w2 = w1;
        w1 = coef[j] + (s2 * w2 - w3);
        d3 = d2;
        d2 = d1;
        d1 = w2 * 2.0 + d2 * s2 - d3;
    }
    p = coef[0] + (s * w1 - w2);
    dp = w1 + s * d1 - d2;
}

bool Ephemeris::open(std::unique_ptr<ByteSource> src) {
    src_ = std::move(src);
    segs_.clear();
    by_target_.clear();
    cache_.assign(kCacheSize, CacheEntry{});
    if (!src_) {
        error_ = "файл не открывается";
        return false;
    }
    uint8_t head[kHeadSize];
    if (!src_->read(0, kHeadSize, head) || std::memcmp(head, kMagic, 8) != 0) {
        error_ = "не файл .eph";
        return false;
    }
    const uint32_t version = get<uint32_t>(head + 8);
    const uint32_t js_len = get<uint32_t>(head + 12);
    const uint32_t n_seg = get<uint32_t>(head + 16);
    if (version != kVersion) {
        error_ = "версия формата " + std::to_string(version) + ", ожидается 1";
        return false;
    }
    header_.resize(js_len);
    std::vector<uint8_t> table(static_cast<size_t>(n_seg) * kSegSize);
    if (!src_->read(kHeadSize, js_len, header_.data()) ||
        !src_->read(kHeadSize + js_len, table.size(), table.data())) {
        error_ = "файл обрезан";
        return false;
    }
    segs_.resize(n_seg);
    for (uint32_t i = 0; i < n_seg; ++i) {
        const uint8_t *p = table.data() + static_cast<size_t>(i) * kSegSize;
        EphSegment &s = segs_[i];
        s.target = get<int32_t>(p + 0);
        s.center = get<int32_t>(p + 4);
        const int32_t type = get<int32_t>(p + 8);
        s.degree = get<int32_t>(p + 12);
        s.start_et = get<double>(p + 16);
        s.end_et = get<double>(p + 24);
        s.init = get<double>(p + 32);
        s.intlen = get<double>(p + 40);
        s.n_records = get<uint64_t>(p + 48);
        s.data_offset = get<uint64_t>(p + 56);
        s.rsize = get<uint32_t>(p + 64);
        s.source = get<uint32_t>(p + 68);
        if (type != 2 || s.rsize != static_cast<uint32_t>(2 + 3 * (s.degree + 1))) {
            error_ = "сегмент " + std::to_string(i) + ": ожидается SPK тип 2";
            return false;
        }
        by_target_[s.target].push_back(static_cast<int>(i));
    }
    for (auto &kv : by_target_) {
        const int c = segs_[kv.second.front()].center;
        for (int i : kv.second)
            if (segs_[i].center != c) {
                error_ = "цель " + std::to_string(kv.first) + ": разные центры";
                return false;
            }
    }
    error_.clear();
    return true;
}

int Ephemeris::center_of(int target) const {
    auto it = by_target_.find(target);
    return it == by_target_.end() ? -1 : segs_[it->second.front()].center;
}

bool Ephemeris::coverage(int target, double &start, double &end) const {
    auto it = by_target_.find(target);
    if (it == by_target_.end()) return false;
    start = HUGE_VAL;
    end = -HUGE_VAL;
    for (int i : it->second) {
        start = std::min(start, segs_[i].start_et);
        end = std::max(end, segs_[i].end_et);
    }
    return true;
}

const EphSegment *Ephemeris::find(int target, double et) const {
    auto it = by_target_.find(target);
    if (it == by_target_.end()) return nullptr;
    // как в SPICE: при перекрытии побеждает сегмент, записанный позже
    const auto &idx = it->second;
    for (auto r = idx.rbegin(); r != idx.rend(); ++r) {
        const EphSegment &s = segs_[*r];
        if (et >= s.start_et && et <= s.end_et) return &s;
    }
    return nullptr;
}

bool Ephemeris::record(const EphSegment &s, uint64_t idx, std::vector<double> &out) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const CacheEntry &c : cache_)
        if (c.seg == &s && c.idx == idx) {
            out = c.data;
            return true;
        }
    CacheEntry &c = cache_[cache_next_];
    cache_next_ = (cache_next_ + 1) % cache_.size();
    c.seg = &s;
    c.idx = idx;
    c.data.resize(s.rsize);
    if (!src_->read(s.data_offset + idx * s.rsize * sizeof(double), s.rsize * sizeof(double), c.data.data())) {
        c.seg = nullptr;
        return false;
    }
    out = c.data;
    return true;
}

bool Ephemeris::state_rel(int target, double et, V3 &pos, V3 &vel) const {
    const EphSegment *s = find(target, et);
    if (!s) return false;
    // номер записи — как в SPKR02: (et − init) / intlen с отсечением по краям
    double k = std::floor((et - s->init) / s->intlen);
    k = std::max(0.0, std::min(k, static_cast<double>(s->n_records - 1)));
    thread_local std::vector<double> rec;
    if (!record(*s, static_cast<uint64_t>(k), rec)) return false;
    const double mid = rec[0], radius = rec[1];
    const double sarg = (et - mid) / radius;
    const int n = s->degree + 1;
    for (int c = 0; c < 3; ++c) {
        double p, dp;
        cheb_eval(rec.data() + 2 + c * n, n, sarg, p, dp);
        pos[c] = p;
        vel[c] = dp / radius;
    }
    return true;
}

bool Ephemeris::state_ssb(int target, double et, V3 &pos, V3 &vel) const {
    if (!state_rel(target, et, pos, vel)) return false;
    int c = center_of(target);
    while (c != 0) {
        V3 p, v;
        if (!state_rel(c, et, p, v)) return false;
        pos += p;
        vel += v;
        c = center_of(c);
    }
    return true;
}

}  // namespace sc
