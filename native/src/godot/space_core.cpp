#include "space_core.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "file_source.hpp"

namespace godot {


bool SpaceCore::open_ephemeris(const String &path) {
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
    if (f.is_null()) {
        error_ = "не открывается " + path;
        return false;
    }
    if (!eph_.open(std::make_unique<GodotFileSource>(f))) {
        error_ = String::utf8(eph_.error().c_str());
        return false;
    }
    error_ = "";
    return true;
}

String SpaceCore::last_error() const { return error_; }

PackedFloat64Array SpaceCore::ephemeris_state(int target, double et, bool ssb) const {
    PackedFloat64Array out;
    sc::V3 p, v;
    const bool ok = ssb ? eph_.state_ssb(target, et, p, v) : eph_.state_rel(target, et, p, v);
    if (!ok) return out;
    out.resize(6);
    for (int c = 0; c < 3; ++c) {
        out.set(c, p[c]);
        out.set(3 + c, v[c]);
    }
    return out;
}

String SpaceCore::version() const { return String::utf8("spacecore 0.2 (этап 4)"); }

void SpaceCore::_bind_methods() {
    ClassDB::bind_method(D_METHOD("open_ephemeris", "path"), &SpaceCore::open_ephemeris);
    ClassDB::bind_method(D_METHOD("last_error"), &SpaceCore::last_error);
    ClassDB::bind_method(D_METHOD("ephemeris_state", "target", "et", "ssb"), &SpaceCore::ephemeris_state);
    ClassDB::bind_method(D_METHOD("version"), &SpaceCore::version);
}

}  // namespace godot
