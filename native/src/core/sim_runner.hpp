// «Симуляция» в отдельном потоке. Поток интегрирует N-body (IAS15) и складывает снимки состояний; рендер берёт
// положения на любой момент между снимками интерполяцией Эрмита 5-й степени (положение, скорость, ускорение).
//
// Уровни упрощения — когда полная модель не успевает за скоростью времени:
//   0 — полная модель: все спутники, ОТО, поля планет;
//   1 — спутники Марса на прецессирующих элементах с J2 (их шаг ограничивает всю систему);
//   2 — спутники всех планетных систем из dynamics.json на элементах (Луна интегрируется всегда: без неё центр Земли
//       уходит на тысячи км — больше допуска 100 км).
// Спутник на элементах снимается с интегрирования, а планета заменяется барицентром своей системы с полной массой.
// Центр планеты восстанавливается как барицентр минус Σ GMₖ·rₖ / GM системы — так он остаётся на своём месте.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catalog.hpp"
#include "kepler.hpp"
#include "nbody.hpp"

namespace sc {

// Состав «Симуляции» на одном уровне.
struct SimSetup {
    int level = 0;
    std::vector<int> sim_to_cat;  // тело Simulation → индекс каталога (у объединённой системы — планета)
    std::vector<int> cat_to_sim;  // каталог → тело Simulation; −1 — спутник на элементах
    std::vector<double> merged_gm;  // по телам Simulation: GM объединённой системы (0 — не объединено)
    struct Kin {
        int cat = -1;         // спутник
        int planet_sim = -1;  // объединённое тело в Simulation
        double gm = 0;
        KinematicOrbit orbit;
    };
    std::vector<Kin> kin;

    // Положения и скорости всех тел каталога по телам Simulation на момент t.
    void expand(double t, const std::vector<V3> &x, const std::vector<V3> &v, std::vector<V3> &cx,
                std::vector<V3> &cv) const;
};

struct SimSnapshot {
    double t = 0;
    std::shared_ptr<const SimSetup> setup;
    std::vector<V3> x, v, a;  // тела Simulation, барицентрические
};

class SimRunner {
public:
    static constexpr int kLevels = 3;

    SimRunner();
    ~SimRunner();
    SimRunner(const SimRunner &) = delete;
    SimRunner &operator=(const SimRunner &) = delete;

    // Каталог и эфемериды должны жить дольше SimRunner.
    void attach(const Catalog *catalog, const Ephemeris *eph);

    // Начать заново из эфемерид на эпоху et (синхронно, в вызывающем потоке). false — нет эфемерид (текст в error).
    bool seed(double et, std::string &error);
    // Остановить поток и забыть снимки.
    void stop();
    bool running() const { return running_; }

    // Рендер сообщает, какой момент ему нужен и с какой скоростью идёт время (с/с, знак — направление).
    void request(double t, double rate);
    // Положения и скорости всех тел каталога на момент t. false — момент ещё не посчитан.
    bool state_at(double t, std::vector<V3> &x, std::vector<V3> &v) const;
    // Посчитанный отрезок времени.
    bool range(double &lo, double &hi) const;
    // Моменты смены уровня внутри посчитанного отрезка (для тестов непрерывности).
    std::vector<double> level_switches() const;

    int level() const { return level_; }
    int min_level = 0;  // наименьший разрешённый уровень (тесты)
    int max_level = kLevels - 1;
    // Оценка скорости счёта уровня, с модельного времени за секунду.
    double throughput(int level) const;
    std::string error() const;
    NBodyOptions options;
    // Правки начального состояния (этап 6): вызывается в seed() после состояния из эфемерид.
    std::function<void(std::vector<SimBody> &)> adjust;

    // Для тестов: шаги интегратора всего.
    int64_t steps() const { return steps_; }

private:
    void loop();
    std::shared_ptr<SimSetup> make_setup(int level, double t, const std::vector<V3> &cx, const std::vector<V3> &cv,
                                         std::vector<V3> &sx, std::vector<V3> &sv) const;
    void load_sim(const SimSnapshot &s);
    void fill_accel(SimSnapshot &s);
    int choose_level(double rate) const;
    bool interpolate(const SimSnapshot &a, const SimSnapshot &b, double t, std::vector<V3> &x,
                     std::vector<V3> &v) const;
    int kinematic_from(int level, int cat) const;  // уровень, с которого спутник cat идёт на элементах (99 — никогда)

    const Catalog *cat_ = nullptr;
    const Ephemeris *eph_ = nullptr;
    std::vector<SimBody> templates_;  // полная модель (индекс = каталог)

    Simulation sim_;                  // только поток расчёта
    std::shared_ptr<const SimSetup> sim_setup_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<SimSnapshot> snaps_;
    double req_t_ = 0, req_rate_ = 0;
    double tp_[kLevels];
    std::string error_;
    std::thread thread_;
    std::atomic<bool> quit_{false}, running_{false};
    std::atomic<int> level_{0};
    std::atomic<int64_t> steps_{0};
};

}  // namespace sc
