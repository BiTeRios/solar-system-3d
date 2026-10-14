// Этап 6: правки параметров тел с отменой и повтором, производные величины, проверки устойчивости, сценарий JSON.
//
// Модель правок — «начальные условия»: все правки относятся к моменту epoch. «Симуляция» стартует из эфемерид
// на epoch, к состоянию применяются правки (масса, скорость и расстояние до родителя), дальше считает N-body.
// Состояние на любой момент — функция (epoch, правки), поэтому отмена и повтор восстанавливают его точно,
// а сценарий JSON хранит только epoch, правки, режим и закладки камеры.
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "catalog.hpp"

namespace sc {

constexpr double kLightKms = 299792.458;  // c, км/с (точное значение SI)
constexpr double kGravity = 6.67430e-20;  // G, км³/(кг·с²) (CODATA 2018, NIST)

enum Param { kMass, kRadius, kAlbedo, kRotation, kSpeed, kDistance, kTemperature, kParamCount };

struct ParamDef {
    const char *id, *name, *unit;
};
extern const ParamDef kParams[kParamCount];

// Производные величины тела. NaN — не определено (нет данных или нет родителя).
struct Derived {
    double mass = NAN, radius = NAN, radius_eq = NAN, albedo = NAN, rotation_h = NAN, temperature = NAN;
    double density = NAN;        // г/см³
    double gravity = NAN;        // м/с², на экваторе
    double escape = NAN;         // км/с, вторая космическая
    double t_eq = NAN;           // К, равновесная по облучению, усреднённому по орбите
    double t_surface = NAN;      // К, T_eq × парниковый множитель (по данным NASA)
    double critical_h = NAN;     // период вращения, при котором экватор отрывается, ч
    double schwarzschild = NAN;  // км
    double hill = NAN, roche = NAN;           // км: сфера Хилла и предел Роша (твёрдое тело) у родителя
    double distance = NAN, speed = NAN;       // км и км/с относительно родителя
    double a = NAN, e = NAN;                  // оскулирующая орбита вокруг родителя
};

struct TableRow {
    std::string name, unit;
    double before = NAN, after = NAN;
    int param = -1;  // строка правится этим параметром; −1 — производная величина
};

struct Warning {
    enum Kind { Density, Spin, Horizon, Roche, Hill, Escape, Spacing, Crossing, Collision };
    Kind kind;
    int body = -1, other = -1;
    std::string text;
};

class Scenario {
public:
    using Values = std::map<std::pair<int, int>, double>;  // (тело, параметр) → значение

    void attach(const Catalog *cat);

    double real(int body, int p) const;
    double value(int body, int p) const;
    bool editable(int body, int p) const;
    bool edited(int body, int p) const { return values_.count({body, p}) != 0; }
    bool empty() const { return values_.empty(); }
    bool dynamic() const;  // есть правки массы, скорости или расстояния — нужна «Симуляция»
    Values dynamic_edits() const;  // только они

    // Команды. set: false — значение вне пределов (текст в error).
    bool set(int body, int p, double v, std::string &error);
    bool reset();
    bool undo();
    bool redo();
    bool can_undo() const { return pos_ > 0; }
    bool can_redo() const { return pos_ < history_.size(); }
    std::vector<std::string> history() const;
    size_t position() const { return pos_; }  // выполнено команд из history()
    uint64_t revision() const { return rev_; }

    double epoch = 0;  // момент, к которому относятся правки, с TDB от J2000
    int mode = 0;      // режим при сохранении сценария
    std::vector<std::vector<double>> bookmarks;  // виды камеры (SolarSystem::camera_get_state)

    // Правки массы, скорости и расстояния — к начальному состоянию «Симуляции» (тела каталога, барицентрические).
    void apply(std::vector<SimBody> &bodies) const;
    V3 radii(int body) const;                 // полуоси эллипсоида с правкой радиуса
    RotationModel rotation(int body) const;   // вращение с правкой периода; угол W непрерывен на epoch

    // x, v — состояния тел каталога; real_values — реальные параметры вместо правок.
    Derived derive(int body, const std::vector<V3> &x, const std::vector<V3> &v, bool real_values) const;
    // «Было → стало»: реальные параметры и состояние (x0, v0) против текущих (x, v).
    std::vector<TableRow> table(int body, const std::vector<V3> &x0, const std::vector<V3> &v0,
                                const std::vector<V3> &x, const std::vector<V3> &v) const;
    // Пределы и устойчивость на текущее состояние (только видимые тела).
    std::vector<Warning> check(const std::vector<V3> &x, const std::vector<V3> &v) const;

    std::string to_json() const;
    // Загрузка заменяет правки, epoch, режим и закладки и очищает историю.
    bool from_json(const std::string &text, std::string &error);

private:
    struct Command {
        std::string label;
        Values before, after;
    };
    void commit(const std::string &label, Values after);
    double param(int body, int p, bool real_values) const;
    bool valid(int body, int p, double v, std::string &error) const;

    const Catalog *cat_ = nullptr;
    int star_ = 0;
    Values values_;
    std::vector<Command> history_;
    size_t pos_ = 0;
    uint64_t rev_ = 0;
};

// Минимальное расстояние между двумя эллиптическими орбитами (MOID), км. Перебор по эксцентрическим аномалиям
// с уточнением; точность — доли процента расстояния, для предупреждений достаточно.
double orbit_distance(const V3 &r1, const V3 &v1, double mu1, const V3 &r2, const V3 &v2, double mu2);

}  // namespace sc
