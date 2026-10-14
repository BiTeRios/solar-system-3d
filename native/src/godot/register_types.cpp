// Точка входа GDExtension.
#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "solar_system.hpp"
#include "space_core.hpp"

using namespace godot;

static void initialize_spacecore(ModuleInitializationLevel level) {
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    GDREGISTER_CLASS(SpaceCore);
    GDREGISTER_CLASS(SolarSystem);
}

static void uninitialize_spacecore(ModuleInitializationLevel level) { (void)level; }

extern "C" {
GDExtensionBool GDE_EXPORT spacecore_init(GDExtensionInterfaceGetProcAddress get_proc_address,
                                          GDExtensionClassLibraryPtr library,
                                          GDExtensionInitialization *initialization) {
    GDExtensionBinding::InitObject init_obj(get_proc_address, library, initialization);
    init_obj.register_initializer(initialize_spacecore);
    init_obj.register_terminator(uninitialize_spacecore);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}
