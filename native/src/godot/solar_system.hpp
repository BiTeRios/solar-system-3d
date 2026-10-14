// Узел SolarSystem: время и режимы («Эфемериды» / «Симуляция»), камера в double и всё, что рисуется в 3D.
// Floating origin: Camera3D всегда в нуле сцены, тела и орбиты ставятся относительно неё каждый кадр.
// Интерфейс (GDScript) только задаёт ввод и читает состояние; порядок кадра — в update_frame().
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>

#include "core/camera_rig.hpp"
#include "core/catalog.hpp"
#include "core/cubesphere.hpp"
#include "core/ephemeris.hpp"
#include "core/kepler.hpp"
#include "core/scenario.hpp"
#include "core/sim_runner.hpp"
#include "core/timeconv.hpp"

namespace godot {

class SolarSystem : public Node3D {
    GDCLASS(SolarSystem, Node3D)

public:
    SolarSystem();
    ~SolarSystem() override;

    // ---- данные ----
    bool load_data(const String &dir);
    String last_error() const { return error_; }
    int body_count() const { return static_cast<int>(cat_.bodies.size()); }
    Dictionary body_info(int i) const;
    int body_index(const String &key) const;

    // ---- время ----
    double get_et() const { return et_; }
    void set_et(double et);  // переход на момент (в «Симуляции» — пересчёт)
    void set_rate(double r) { rate_ = r; }
    double get_rate() const { return rate_; }
    void set_paused(bool p) { paused_ = p; }
    bool is_paused() const { return paused_; }
    double actual_rate() const { return actual_rate_; }
    double now_et() const;
    String et_to_utc(double et) const;      // «2026-10-11 12:34:56.789»
    String et_to_tdb(double et) const;      // то же, но шкала TDB
    double utc_to_et(const String &s) const;  // NaN — не разобрано
    // ---- режим ----
    int get_mode() const { return mode_; }  // 0 — «Эфемериды», 1 — «Симуляция»
    void set_mode(int m);
    bool mode_is_auto() const { return auto_sim_; }
    int sim_level() const { return mode_ == 1 ? runner_.level() : -1; }
    PackedFloat64Array ephemeris_range() const;  // [начало, конец] общего покрытия эфемерид, ET
    String status() const;                       // строка для интерфейса: что упрощено, ошибки

    // ---- камера ----
    void set_camera(Camera3D *cam) { camera_ = cam; }
    Camera3D *get_camera() const { return camera_; }
    void camera_free_input(const Vector3 &move, const Vector3 &rot);
    void camera_orbit_input(double dyaw, double dpitch, double zoom_steps);
    void camera_set_speed(double kms) { rig_.speed = std::clamp(kms, sc::CameraRig::kMinSpeed, sc::CameraRig::kMaxSpeed); }
    double camera_speed() const { return rig_.speed; }
    double camera_actual_speed() const { return cam_speed_; }  // км/с относительно опорного тела
    void camera_fly_to(int body);
    void camera_orbit(int body);
    void camera_set_body_fixed(bool on);
    bool camera_body_fixed() const { return rig_.body_fixed; }
    // Поставить камеру над точкой тела: широта и долгота (планетоцентрические, град), высота над эллипсоидом (м),
    // азимут взгляда от севера (град) и наклон над горизонтом (град).
    void camera_place(int body, double lat, double lon, double alt_m, double azimuth, double elevation, bool body_fixed);
    int camera_anchor() const { return rig_.anchor; }
    int camera_mode() const { return static_cast<int>(rig_.mode); }
    double camera_altitude() const;  // км над поверхностью опорного тела
    double camera_flyto_progress() const;
    PackedFloat64Array camera_position_icrf() const;  // км от барицентра
    PackedFloat64Array body_position_icrf(int i) const;
    double distance_to_body(int i) const;  // км от камеры до центра тела
    PackedFloat64Array body_state_icrf(int i) const;  // [x, y, z, vx, vy, vz], км и км/с от барицентра
    // «Симуляция»: насколько тело ушло от эфемерид на текущий момент, км. NaN — режим «Эфемериды» или нет эфемерид.
    double body_ephemeris_deviation(int i) const;
    // Вид камеры для закладок: [опора, вращение с телом, режим, pos×3, кватернион×4, дальность облёта, азимут, наклон, скорость].
    PackedFloat64Array camera_get_state() const;
    void camera_set_state(const PackedFloat64Array &s);

    // ---- кадр ----
    void update_frame(double delta);
    Vector3 body_render_position(int i) const;  // метры в осях камеры (Camera3D стоит в нуле без поворота)
    double body_angular_radius(int i) const;    // рад
    // Тело заслонено другим (сферы по средним радиусам) или позади камеры.
    bool body_hidden(int i) const;

    // ---- слои и отладка ----
    void set_orbits_visible(bool v);
    bool get_orbits_visible() const { return orbits_visible_; }
    void set_grid_visible(bool v);
    bool get_grid_visible() const { return grid_visible_; }
    void set_lod_split(double s) { lod_split_ = s; }
    double get_lod_split() const { return lod_split_; }
    void set_flat_mode(bool on);  // тесты: тела ровным цветом без освещения
    // Слои внешнего вида: "atmosphere", "clouds", "night" (ночные огни Земли).
    void set_effect(const String &name, bool on);
    bool get_effect(const String &name) const;
    // Тесты: 0 — обычный вид, 1 — Ламберт без карт и атмосферы, 2 — код u карты, 3 — код v карты.
    void set_debug_view(int mode);
    // Внешний вид тела по appearance.json: источник карты, пометки, альбедо, ширина карты на экране сейчас.
    Dictionary appearance_info(int i) const;
    int body_texture_width(int i) const;  // ширина карты цвета, стоящей в материале; 0 — карты нет
    int64_t hi_texture_bytes() const;     // оценка видеопамяти под крупные карты, байт
    // Доля диска Солнца, видимая из центра тела (затмения другими телами; кольца не учитываются).
    double sun_visibility(int i) const;
    PackedInt32Array shadow_casters(int i) const;  // тела, чьи тени сейчас учитываются на теле i
    void set_hi_texture_budget(int64_t bytes) { hi_budget_ = bytes; }
    void set_body_color(int i, const Color &c);
    int add_surface_marker(int body, double lat, double lon, double alt_m, double size_m, const Color &c);
    // Увеличение (телескоп): поле зрения делится на zoom. 1 — обычный вид.
    void set_zoom(double z) { zoom_ = std::max(1.0, z); }
    double get_zoom() const { return zoom_; }
    void set_view_fov(double deg) { fov_deg_ = std::clamp(deg, 1.0, 179.0); }
    double get_view_fov() const { return fov_deg_; }
    // Камера на расстоянии dist_km от центра тела в направлении dir (оси тела или ICRF); облёт, взгляд на центр.
    void camera_look_from(int body, const Vector3 &dir, double dist_km, bool body_frame = true);
    Vector3 marker_render_position(int id) const;
    // Тесты: что должно быть видно в каждом пикселе кадра w×h — лучи в double по эллипсоидам тел и кольцам.
    // 0 — фон, i + 1 — тело i, 255 — кольцо. Пиксели по строкам сверху вниз.
    PackedByteArray reference_classes(int w, int h) const;
    // Тесты: эталон по лучам в double для кадра w×h — по 4 числа на пиксель: номер тела + 1 (0 — фон),
    // планетоцентрическая широта, восточная долгота, геодезическая широта (градусы).
    PackedFloat32Array reference_surface(int w, int h) const;
    int visible_chunk_count() const { return visible_chunks_; }

    // ---- правки (этап 6, solar_system_edit.cpp) ----
    // Правка параметра p (sc::Param) тела i. Пустая строка — принято, иначе — почему нет.
    String edit_param(int i, int p, double value);
    bool edit_undo();
    bool edit_redo();
    bool edit_reset();
    bool edit_can_undo() const { return scen_.can_undo(); }
    bool edit_can_redo() const { return scen_.can_redo(); }
    PackedStringArray edit_history() const;
    int edit_history_position() const { return static_cast<int>(scen_.position()); }
    int edit_revision() const { return static_cast<int>(scen_.revision()); }
    // «Было → стало» для тела i: [{name, unit, before, after, param, edited}], param — номер параметра или −1.
    Array body_params(int i) const;
    // Пределы и устойчивость сейчас: [{kind, body, other, text}].
    Array stability_warnings() const;
    // Сценарий: epoch, режим, правки и закладки камеры (массивы camera_get_state).
    String scenario_to_json(const Array &bookmarks);
    String scenario_from_json(const String &text);  // пустая строка — загружено
    Array scenario_bookmarks() const;

    // ---- звёзды и Млечный Путь (этап 7, solar_system_sky.cpp) ----
    // Направление на звезду HIP сейчас (ICRF, с собственным движением); пусто — нет в каталоге.
    PackedFloat64Array star_direction(int hip) const;
    int star_count() const { return static_cast<int>(star_hip_.size()); }
    // "star_flux0" — условная яркость звезды 0m, "milkyway_scale" — яркость неба, "milkyway_debug" — 1: код α, δ.
    void set_sky_param(const String &name, double value);
    int chunk_level_max(int body) const;

protected:
    static void _bind_methods();
    static void bind_edit_methods();

private:
    struct Chunk {
        RID mesh, instance;
        sc::V3 center;   // км, оси тела
        double radius = 0;
        uint64_t used = 0;
        bool visible = false;
    };
    // Слой карты: уровни детализации (ширина → путь), нижний загружен всегда, верхний — пока тело крупно на экране.
    struct TexSlot {
        String param;
        std::vector<std::pair<int, String>> levels;  // по возрастанию ширины
        bool normal = false;
        Ref<Texture2D> base, hi;
        int shown = -1, pending = -1;  // индексы уровней
    };
    struct BodyView {
        sc::CubeSphere sphere;
        Ref<ShaderMaterial> material;
        std::vector<TexSlot> slots;
        double px = 0;                // диаметр на экране, пикселей
        double small_since = -1;      // с какого момента тело мелкое (для выгрузки крупных карт)
        bool want_hi = false;
        bool has_atmosphere = false, has_rings = false;
        RID sky_instance;
        Ref<Mesh> sky_ref;
        Ref<ShaderMaterial> sky_material;
        Dictionary info;              // appearance_info
        std::unordered_map<uint64_t, Chunk> chunks;
        std::vector<uint64_t> shown;  // чанки, показанные сейчас
        std::vector<RID> ring_meshes, ring_instances;
        std::vector<sc::V3> ring_centers;  // км, оси тела
        bool drawn = false;
        int max_level_shown = 0;
    };
    struct OrbitView {
        RID mesh, instance;
        int parent = -1;
        bool built = false;
        bool barycentric = false;  // орбита барицентра системы (у планет со спутниками)
        double gen_et = 0;
    };
    struct Marker {
        int body = -1;
        sc::V3 pos;  // км, оси тела
        RID mesh, instance;
    };

    // время и состояния
    void compute_states();
    bool eph_covers(double et) const;
    void enter_sim(double et, bool automatic);
    // камера
    sc::M3 frame_to_icrf(int anchor, bool body_fixed) const;
    sc::CamFrame cam_frame() const;
    void rebase(int anchor, bool body_fixed);
    void auto_anchor();
    sc::V3 rel_to_camera(int i) const;  // км, ICRF: центр тела − камера (точно у опорного тела)
    // отрисовка
    void update_bodies();
    void update_body(int i, BodyView &bv);
    void build_rings(int i, BodyView &bv);
    void update_orbits();
    void build_orbit(int i);
    void update_markers();
    void update_lighting(int i, BodyView &bv);
    // Тела, которые могут заслонять Солнце хоть от какой-то точки тела i, по убыванию углового размера.
    void find_occluders(size_t i, std::vector<size_t> &out) const;
    void update_textures(double now);
    void load_appearance(const String &dir);
    void apply_slot(BodyView &bv, TexSlot &slot, int level);
    void free_render();
    // правки: пересчёт после любой команды; вид тел по правкам; реальные состояния на et_
    void on_edits(bool fresh);
    void load_sky(const String &dir);
    void update_sky();
    void free_sky();
    void apply_edit_view();
    void real_states(std::vector<sc::V3> &x, std::vector<sc::V3> &v) const;
    sc::M3 body_rotation(int i) const;  // ICRF → оси тела на et_
    Transform3D scene_transform(const sc::M3 &basis_icrf, const sc::V3 &origin_icrf_km) const;

    // данные
    sc::Ephemeris eph_;
    sc::Catalog cat_;
    sc::TimeModel time_;
    sc::SimRunner runner_;
    sc::Catalog real_cat_;  // каталог без правок (cat_ несёт радиусы и вращение с правками)
    sc::Scenario scen_;
    double catch_up_ = NAN;  // «Симуляция» пересчитывается от epoch правок до этого момента
    bool edit_sim_ = false;  // «Симуляция» включена правками
    sc::Scenario::Values last_dyn_;  // правки, с которыми запущена «Симуляция»
    std::string error_s_;
    String error_;
    bool loaded_ = false;
    double eph_lo_ = 0, eph_hi_ = 0;

    // время
    double et_ = 0, rate_ = 1.0, actual_rate_ = 0;
    bool paused_ = false;
    int mode_ = 0;
    bool auto_sim_ = false;
    std::vector<sc::V3> x_, v_;  // состояния тел каталога на et_, ICRF, км и км/с
    std::vector<sc::M3> rot_;    // ICRF → оси тела на et_

    // камера
    Camera3D *camera_ = nullptr;
    sc::CameraRig rig_;
    sc::V3 in_move_, in_rot_;
    double in_dyaw_ = 0, in_dpitch_ = 0, in_zoom_ = 0;
    bool in_free_ = false, in_orbit_ = false;
    sc::V3 cam_abs_;          // км от барицентра
    sc::M3 cam_frame_;        // система отсчёта камеры → ICRF
    sc::V3 prev_cam_rel_;     // для замера скорости
    int prev_anchor_ = -2;
    double cam_speed_ = 0;
    double frame_dt_ = 0;

    // отрисовка
    Ref<ShaderMaterial> ring_material_, orbit_material_, marker_material_, corona_material_;
    RID corona_mesh_, corona_instance_;
    Ref<Mesh> corona_ref_;
    bool fx_atmosphere_ = true, fx_clouds_ = true, fx_night_ = true, fx_stars_ = true, fx_milkyway_ = true;
    RID stars_mesh_, stars_instance_, milkyway_mesh_, milkyway_instance_;
    Ref<ShaderMaterial> stars_material_, milkyway_material_;
    std::unordered_map<int, size_t> star_hip_;  // HIP → номер звезды
    std::vector<float> star_data_;              // направление и собственное движение по звёздам
    double star_epoch_ = 1991.25;
    int sky_debug_ = 0;
    int debug_view_ = 0;
    int64_t hi_budget_ = 1500LL * 1024 * 1024;
    double clock_ = 0;
    std::vector<BodyView> views_;
    std::vector<OrbitView> orbits_;
    std::vector<Marker> markers_;
    std::vector<Ref<Mesh>> marker_refs_;
    bool orbits_visible_ = true, grid_visible_ = false, flat_ = false;
    double lod_split_ = 1.5;
    double zoom_ = 1.0, fov_deg_ = 60.0;
    double px_per_rad_ = 1000.0;  // пикселей экрана на радиан в центре кадра
    double view_half_ = 1.0;      // половина диагонального поля зрения, рад
    double log_d0_ = 1.0e6;       // начало логарифмического сжатия расстояний, м
    sc::M3 view_;                 // ICRF → оси камеры (Godot: −Z вперёд, Y вверх)
    uint64_t frame_ = 0;
    size_t orbit_rr_ = 0;
    int visible_chunks_ = 0;
    std::vector<sc::V3> render_pos_;  // км, ICRF относительно камеры
};

}  // namespace godot
