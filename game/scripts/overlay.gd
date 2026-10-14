# Подписи и метки тел поверх 3D: кружок в точке тела и имя. Мелкие тела без меток не найти глазами,
# поэтому метка рисуется всегда, когда тело перед камерой и не заслонено.
extends Control

signal body_clicked(index: int, double: bool)

var sys: Node
var camera: Camera3D
var infos: Array = []  # body_info по индексам (неизменны после загрузки)
var labels_on := true
var selected := -1
var _hits: Array = []  # [позиция, индекс]
var _font: Font


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_preset(Control.PRESET_FULL_RECT)
	_font = get_theme_default_font()


func _process(_delta: float) -> void:
	queue_redraw()


func _draw() -> void:
	_hits.clear()
	if sys == null or camera == null or not labels_on:
		return
	var px_per_rad: float = get_viewport_rect().size.y / deg_to_rad(camera.fov)
	var screen := {}
	for i in infos.size():
		var info: Dictionary = infos[i]
		if not info["visible"]:
			continue
		var p: Vector3 = sys.body_render_position(i)
		if camera.is_position_behind(p) or sys.body_hidden(i):
			continue
		screen[i] = camera.unproject_position(p)
	for i in screen:
		var info: Dictionary = infos[i]
		var sp: Vector2 = screen[i]
		# спутник рядом со своей планетой на экране — без подписи, чтобы не было каши
		var par: int = info["parent"]
		if info["kind"] == "moon" and screen.has(par) and sp.distance_to(screen[par]) < 14.0 and i != selected:
			continue
		var r_px: float = sys.body_angular_radius(i) * px_per_rad
		var col := Color(1.0, 0.85, 0.3) if i == selected else Color(0.8, 0.88, 1.0, 0.85)
		if r_px < 6.0:
			draw_arc(sp, 5.0, 0.0, TAU, 20, col, 1.2, true)
		var name: String = info["name"]
		var off := Vector2(maxf(r_px, 6.0) * 0.72 + 3.0, -maxf(r_px, 6.0) * 0.72 - 3.0)
		draw_string(_font, sp + off, name, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, col)
		_hits.append([sp, i, maxf(r_px, 8.0)])


## Тело под курсором (метка или диск), −1 — нет.
func pick(pos: Vector2) -> int:
	var best := -1
	var best_d := 1e9
	for h in _hits:
		var d: float = (h[0] as Vector2).distance_to(pos)
		if d < maxf(14.0, h[2]) and d < best_d:
			best = h[1]
			best_d = d
	return best
