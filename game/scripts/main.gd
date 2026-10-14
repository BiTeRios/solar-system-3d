# Главная сцена: SolarSystem (C++) + камера + ввод + интерфейс.
# Порядок кадра: ввод → SolarSystem.update_frame (время, камера, floating origin, отрисовка) → интерфейс.
# Оконные тест-сценарии запускаются аргументом: Godot --path game -- --scenario=<имя> (tests/scenarios.gd).
extends Node3D

const UI := preload("res://scripts/ui.gd")
const Overlay := preload("res://scripts/overlay.gd")

const LOOK_SENS := 0.0035   # рад на пиксель мыши
const ORBIT_SENS := 0.006
const ROLL_SPEED := 1.2     # рад/с

var sys: Node
var camera: Camera3D
var env: Environment
var ui: CanvasLayer
var overlay: Control
var infos: Array = []
var selected := -1
var scenario: Node = null
var _mouse_look := Vector2.ZERO
var _dragging := false
var _drag_moved := false


func _ready() -> void:
	env = Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	set_cinematic(true)
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	camera = Camera3D.new()
	camera.fov = 60.0
	add_child(camera)
	camera.make_current()
	sys = ClassDB.instantiate("SolarSystem")
	add_child(sys)
	if not sys.load_data("res://data"):
		push_error("Данные не загрузились: " + sys.last_error())
		var l := Label.new()
		l.text = "Данные не загрузились: " + sys.last_error()
		add_child(l)
		return
	sys.set_camera(camera)
	for i in sys.body_count():
		infos.append(sys.body_info(i))
	var layer := CanvasLayer.new()
	layer.layer = 0
	add_child(layer)
	overlay = Overlay.new()
	overlay.sys = sys
	overlay.camera = camera
	overlay.infos = infos
	layer.add_child(overlay)
	ui = UI.new()
	add_child(ui)
	ui.setup(self, sys, infos)
	ui.set_rate_slider(1.0)
	select_body(sys.body_index("earth"))
	var sc := _scenario_arg()
	if not sc.is_empty():
		var S: GDScript = load("res://tests/scenarios.gd")
		scenario = S.new()
		add_child(scenario)
		scenario.start(self, sc)


# Кадр «как в камере»: тональная кривая AgX сжимает яркое (лёд с альбедо ~1, Солнце), свечение вокруг Солнца.
# Тесты выключают это (линейная кривая без свечения): им нужен ровно цвет из шейдеров.
func set_cinematic(on: bool) -> void:
	env.tonemap_mode = Environment.TONE_MAPPER_AGX if on else Environment.TONE_MAPPER_LINEAR
	env.glow_enabled = on
	env.glow_hdr_threshold = 1.5
	env.glow_hdr_scale = 2.0
	env.glow_intensity = 0.6
	env.glow_bloom = 0.0
	env.glow_blend_mode = Environment.GLOW_BLEND_MODE_ADDITIVE
	for lv in 7:
		env.set_glow_level(lv, 1.0 if lv >= 2 else 0.0)


func set_exposure(ev: float) -> void:
	env.tonemap_exposure = pow(2.0, ev)


func _scenario_arg() -> String:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--scenario="):
			return a.substr(11)
	return ""


func _process(delta: float) -> void:
	if sys == null or infos.is_empty():
		return
	if scenario == null or not scenario.drives_input():
		_handle_keys(delta)
	sys.update_frame(delta)
	if scenario != null:
		scenario.after_frame(delta)
	ui.refresh(delta)


func _typing() -> bool:
	var f := get_viewport().gui_get_focus_owner()
	return f is LineEdit


func _handle_keys(delta: float) -> void:
	var move := Vector3.ZERO
	var rot := Vector3(-_mouse_look.y * LOOK_SENS, -_mouse_look.x * LOOK_SENS, 0.0)
	_mouse_look = Vector2.ZERO
	if not _typing():
		if Input.is_physical_key_pressed(KEY_W): move.z -= 1.0
		if Input.is_physical_key_pressed(KEY_S): move.z += 1.0
		if Input.is_physical_key_pressed(KEY_A): move.x -= 1.0
		if Input.is_physical_key_pressed(KEY_D): move.x += 1.0
		if Input.is_physical_key_pressed(KEY_R): move.y += 1.0
		if Input.is_physical_key_pressed(KEY_F): move.y -= 1.0
		if Input.is_physical_key_pressed(KEY_Q): rot.z += ROLL_SPEED * delta
		if Input.is_physical_key_pressed(KEY_E): rot.z -= ROLL_SPEED * delta
	if sys.camera_mode() == 1 and move == Vector3.ZERO and rot.z == 0.0:
		return  # облёт: повороты мышью идут через camera_orbit_input
	if move != Vector3.ZERO or rot != Vector3.ZERO:
		sys.camera_free_input(move, rot)


func _unhandled_input(event: InputEvent) -> void:
	if sys == null or infos.is_empty():
		return
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_WHEEL_UP or mb.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			if mb.pressed:
				var s := -1.0 if mb.button_index == MOUSE_BUTTON_WHEEL_UP else 1.0
				if sys.camera_mode() == 1:
					sys.camera_orbit_input(0.0, 0.0, s)
				else:
					sys.camera_set_speed(sys.camera_speed() * pow(1.5, -s))
			return
		if mb.button_index == MOUSE_BUTTON_LEFT:
			if mb.pressed:
				var hit: int = overlay.pick(mb.position)
				if hit >= 0:
					select_body(hit)
					if mb.double_click:
						fly_to(hit)
					_dragging = false
				else:
					_dragging = true
					_drag_moved = false
			else:
				_dragging = false
		if mb.button_index == MOUSE_BUTTON_RIGHT:
			_dragging = mb.pressed
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _dragging or (mm.button_mask & MOUSE_BUTTON_MASK_RIGHT):
			if sys.camera_mode() == 1:
				sys.camera_orbit_input(-mm.relative.x * ORBIT_SENS, mm.relative.y * ORBIT_SENS, 0.0)
			else:
				_mouse_look += mm.relative
	elif event is InputEventKey and event.pressed and not event.echo and not _typing():
		var k := (event as InputEventKey).physical_keycode
		if (event as InputEventKey).ctrl_pressed:
			if k == KEY_Z and not (event as InputEventKey).shift_pressed:
				ui.edit_undo()
			elif k == KEY_Y or k == KEY_Z:
				ui.edit_redo()
			return
		match k:
			KEY_F1:
				ui.toggle_help()
			KEY_ESCAPE:
				if ui.help_panel.visible:
					ui.toggle_help()
				if ui.sources_panel.visible:
					ui.toggle_sources()
			KEY_F12:
				screenshot()
			KEY_SPACE:
				sys.set_paused(not sys.is_paused())
			KEY_HOME:
				go_now()
			KEY_G:
				fly_to(selected)
			KEY_O:
				orbit(selected)
			KEY_B:
				sys.camera_set_body_fixed(not sys.camera_body_fixed())
			KEY_BRACKETLEFT:
				sys.camera_set_speed(sys.camera_speed() / 1.5)
			KEY_BRACKETRIGHT:
				sys.camera_set_speed(sys.camera_speed() * 1.5)


func select_body(i: int) -> void:
	if i < 0:
		return
	selected = i
	if overlay:
		overlay.selected = i
	if ui:
		ui.select_in_tree(i)


func fly_to(i: int) -> void:
	if i < 0:
		return
	select_body(i)
	sys.camera_fly_to(i)


func orbit(i: int) -> void:
	if i < 0:
		return
	sys.camera_orbit(i)


func set_labels(on: bool) -> void:
	overlay.labels_on = on


func go_now() -> void:
	sys.set_et(sys.now_et())


func screenshot() -> String:
	var img := get_viewport().get_texture().get_image()
	DirAccess.make_dir_recursive_absolute("user://screenshots")
	var name := "user://screenshots/%s.png" % Time.get_datetime_string_from_system().replace(":", "-")
	img.save_png(name)
	ui.show_toast("Снимок: " + ProjectSettings.globalize_path(name))
	return name
