# Интерфейс: верхняя панель (время и режим), слева — тела, слои, качество, закладки, справа — вкладки,
# снизу — камера. Строится кодом. Состояние берётся из SolarSystem каждый кадр (refresh).
extends CanvasLayer

const YEAR_S := 365.25 * 86400.0
const RATE_MAX := 1000.0 * YEAR_S
const KIND_RU := {"star": "звезда", "planet": "планета", "dwarf": "карликовая планета", "moon": "спутник",
	"comet": "комета", "perturber": "астероид (только в расчёте)"}
const CAM_MODE_RU := ["полёт", "облёт", "перелёт"]

var main: Node
var sys: Node
var infos: Array = []

var date_edit: LineEdit
var tdb_label: Label
var pause_btn: Button
var back_btn: Button
var rate_slider: HSlider
var rate_label: Label
var mode_opt: OptionButton
var actual_label: Label
var status_label: Label
var search: LineEdit
var tree: Tree
var fixed_check: CheckBox
var orbits_check: CheckBox
var labels_check: CheckBox
var grid_check: CheckBox
var quality_opt: OptionButton
var bookmarks: ItemList
var info_grid: GridContainer
var dist_grid: GridContainer
var cam_label: Label
var speed_slider: HSlider
var speed_label: Label
var fps_label: Label
var help_panel: PanelContainer
var sources_panel: PanelContainer
var toast: Label
var _toast_time := 0.0
var _items := {}  # индекс тела → TreeItem
var _bookmarks: Array = []
var _rate_lock := false
var _speed_lock := false
var root: Control
# правки (этап 6)
var tabs: TabContainer
var param_title: Label
var param_grid: GridContainer
var undo_btn: Button
var redo_btn: Button
var reset_btn: Button
var hist_list: ItemList
var warn_label: Label
var file_dialog: FileDialog
var _param_key := ""
var _param_rows: Array = []  # [{before: Label, after: Control, param: int}]
var _param_time := 0.0
var _warn_time := 0.0
var _saving := false


func setup(main_node: Node, system: Node, body_infos: Array) -> void:
	main = main_node
	sys = system
	infos = body_infos
	root = Control.new()
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(root)
	_build_top()
	_build_left()
	_build_right()
	_build_bottom()
	_build_help()
	_build_sources()
	toast = Label.new()
	toast.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	toast.position = Vector2(-300, -90)
	toast.size = Vector2(600, 30)
	toast.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	toast.add_theme_color_override("font_outline_color", Color.BLACK)
	toast.add_theme_constant_override("outline_size", 6)
	root.add_child(toast)
	_fill_tree("")


func _panel(preset: int) -> PanelContainer:
	var p := PanelContainer.new()
	p.set_anchors_preset(preset)
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.05, 0.06, 0.09, 0.82)
	sb.set_content_margin_all(6)
	p.add_theme_stylebox_override("panel", sb)
	root.add_child(p)
	# привязанные к правому и нижнему краю панели растут внутрь экрана
	if preset == Control.PRESET_RIGHT_WIDE:
		p.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	if preset == Control.PRESET_BOTTOM_WIDE:
		p.grow_vertical = Control.GROW_DIRECTION_BEGIN
	return p


func _label(text: String, parent: Node) -> Label:
	var l := Label.new()
	l.text = text
	parent.add_child(l)
	return l


func _build_top() -> void:
	var p := _panel(Control.PRESET_TOP_WIDE)
	var rows := VBoxContainer.new()
	p.add_child(rows)
	var h := HBoxContainer.new()
	h.add_theme_constant_override("separation", 8)
	rows.add_child(h)
	_label("UTC", h)
	date_edit = LineEdit.new()
	date_edit.custom_minimum_size.x = 215
	date_edit.tooltip_text = "Дата и время UTC: ГГГГ-ММ-ДД чч:мм:сс. Enter — перейти."
	date_edit.text_submitted.connect(_on_date)
	h.add_child(date_edit)
	tdb_label = _label("", h)
	tdb_label.modulate = Color(0.7, 0.7, 0.75)
	pause_btn = Button.new()
	pause_btn.toggle_mode = true
	pause_btn.text = "Пауза"
	pause_btn.tooltip_text = "Остановить время (Пробел)"
	pause_btn.toggled.connect(func(on): sys.set_paused(on))
	h.add_child(pause_btn)
	back_btn = Button.new()
	back_btn.toggle_mode = true
	back_btn.text = "Назад"
	back_btn.tooltip_text = "Время идёт назад"
	back_btn.toggled.connect(func(_on): _apply_rate())
	h.add_child(back_btn)
	var now := Button.new()
	now.text = "Сейчас"
	now.tooltip_text = "Текущий момент (Home)"
	now.pressed.connect(func(): main.go_now())
	h.add_child(now)
	_label("Скорость времени", h)
	rate_slider = HSlider.new()
	rate_slider.min_value = 0.0
	rate_slider.max_value = 1.0
	rate_slider.step = 0.0005
	rate_slider.custom_minimum_size = Vector2(200, 20)
	rate_slider.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	rate_slider.tooltip_text = "От 1 с/с до 1000 лет/с, логарифмическая шкала"
	rate_slider.value_changed.connect(func(_v): _apply_rate())
	h.add_child(rate_slider)
	rate_label = _label("", h)
	rate_label.custom_minimum_size.x = 95
	h = HBoxContainer.new()
	h.add_theme_constant_override("separation", 8)
	rows.add_child(h)
	_label("Режим", h)
	mode_opt = OptionButton.new()
	mode_opt.add_item("Эфемериды", 0)
	mode_opt.add_item("Симуляция", 1)
	mode_opt.tooltip_text = "«Эфемериды» — положения JPL; «Симуляция» — N-body от текущего состояния"
	mode_opt.item_selected.connect(func(i): sys.set_mode(i))
	h.add_child(mode_opt)
	actual_label = _label("", h)
	actual_label.custom_minimum_size.x = 150
	status_label = _label("", h)
	status_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	status_label.clip_text = true
	status_label.modulate = Color(1.0, 0.85, 0.5)


func _build_left() -> void:
	var p := _panel(Control.PRESET_LEFT_WIDE)
	p.offset_top = 84
	p.offset_bottom = -42
	p.custom_minimum_size.x = 270
	var v := VBoxContainer.new()
	p.add_child(v)
	search = LineEdit.new()
	search.placeholder_text = "Поиск тела"
	search.clear_button_enabled = true
	search.text_changed.connect(_fill_tree)
	v.add_child(search)
	tree = Tree.new()
	tree.hide_root = true
	tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	tree.item_selected.connect(func(): main.select_body(tree.get_selected().get_metadata(0)))
	tree.item_activated.connect(func(): main.fly_to(tree.get_selected().get_metadata(0)))
	v.add_child(tree)
	var hb := HBoxContainer.new()
	v.add_child(hb)
	var fly := Button.new()
	fly.text = "Перелёт"
	fly.tooltip_text = "Перелететь к выбранному телу (G)"
	fly.pressed.connect(func(): main.fly_to(main.selected))
	hb.add_child(fly)
	var orb := Button.new()
	orb.text = "Облёт"
	orb.tooltip_text = "Облетать выбранное тело с текущего расстояния (O)"
	orb.pressed.connect(func(): main.orbit(main.selected))
	hb.add_child(orb)
	fixed_check = CheckBox.new()
	fixed_check.text = "Вращаться вместе с телом"
	fixed_check.tooltip_text = "Камера неподвижна относительно поверхности опорного тела (B)"
	fixed_check.toggled.connect(func(on): sys.camera_set_body_fixed(on))
	v.add_child(fixed_check)
	v.add_child(HSeparator.new())
	_label("Слои", v)
	orbits_check = _check("Орбиты", true, v, func(on): sys.set_orbits_visible(on))
	labels_check = _check("Подписи", true, v, func(on): main.set_labels(on))
	grid_check = _check("Сетка координат", false, v, func(on): sys.set_grid_visible(on))
	_check("Атмосфера", true, v, func(on): sys.set_effect("atmosphere", on))
	_check("Облака Земли", true, v, func(on): sys.set_effect("clouds", on))
	_check("Огни городов", true, v, func(on): sys.set_effect("night", on))
	_check("Звёзды", true, v, func(on): sys.set_effect("stars", on))
	_check("Млечный Путь", true, v, func(on): sys.set_effect("milkyway", on))
	var eh := HBoxContainer.new()
	v.add_child(eh)
	_label("Экспозиция", eh)
	var ev := HSlider.new()
	ev.min_value = -16.0
	ev.max_value = 6.0
	ev.step = 0.5
	ev.value = 0.0
	ev.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	ev.tooltip_text = "Ступени экспозиции (EV). Солнце вблизи видно при −13…−15, корона при затмении — при +2"
	var ev_label := _label("0 EV", eh)
	ev.value_changed.connect(func(x):
		main.set_exposure(x)
		ev_label.text = "%+.1f EV" % x if x != 0.0 else "0 EV")
	eh.add_child(ev)
	eh.move_child(ev_label, 2)
	var later := _label("Пояс астероидов — после MVP", v)
	later.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	later.modulate = Color(0.6, 0.6, 0.65)
	var src_btn := Button.new()
	src_btn.text = "Источники и лицензии"
	src_btn.pressed.connect(toggle_sources)
	v.add_child(src_btn)
	v.add_child(HSeparator.new())
	var qh := HBoxContainer.new()
	v.add_child(qh)
	_label("Качество", qh)
	quality_opt = OptionButton.new()
	for q in ["Низкое", "Среднее", "Высокое"]:
		quality_opt.add_item(q)
	quality_opt.select(1)
	quality_opt.item_selected.connect(func(i): sys.set_lod_split([3.0, 1.5, 0.9][i]))
	qh.add_child(quality_opt)
	v.add_child(HSeparator.new())
	var bh := HBoxContainer.new()
	v.add_child(bh)
	_label("Закладки", bh)
	var save := Button.new()
	save.text = "Запомнить вид"
	save.pressed.connect(_save_bookmark)
	bh.add_child(save)
	bookmarks = ItemList.new()
	bookmarks.custom_minimum_size.y = 56
	bookmarks.item_activated.connect(func(i): sys.camera_set_state(_bookmarks[i]))
	bookmarks.item_clicked.connect(func(i, _p, _b): sys.camera_set_state(_bookmarks[i]))
	v.add_child(bookmarks)


func _check(text: String, on: bool, parent: Node, cb: Callable) -> CheckBox:
	var c := CheckBox.new()
	c.text = text
	c.button_pressed = on
	c.toggled.connect(cb)
	parent.add_child(c)
	return c


func _build_right() -> void:
	var p := _panel(Control.PRESET_RIGHT_WIDE)
	p.offset_top = 84
	p.offset_bottom = -42
	p.custom_minimum_size.x = 390
	tabs = TabContainer.new()
	p.add_child(tabs)
	info_grid = _grid_tab(tabs, "Характеристики")
	_build_params_tab()
	dist_grid = _grid_tab(tabs, "Расстояния")
	var sc := ScrollContainer.new()
	sc.name = "Устойчивость"
	sc.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	tabs.add_child(sc)
	warn_label = Label.new()
	warn_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	warn_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sc.add_child(warn_label)


## Вкладка «Параметры»: таблица «было → стало» с полями правки, отмена и повтор, история, сценарии.
func _build_params_tab() -> void:
	var sc := ScrollContainer.new()
	sc.name = "Параметры"
	sc.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	tabs.add_child(sc)
	var v := VBoxContainer.new()
	v.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sc.add_child(v)
	param_title = _label("", v)
	param_grid = GridContainer.new()
	param_grid.columns = 3
	param_grid.add_theme_constant_override("h_separation", 8)
	param_grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	v.add_child(param_grid)
	var hint := _label("Новое значение — в поле «стало», затем Enter. Масса — в кг (можно 5,97e24), скорость и "
		+ "расстояние — множители к реальным. Правки массы, скорости и расстояния включают «Симуляцию».", v)
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.modulate = Color(0.7, 0.75, 0.85)
	var h := HBoxContainer.new()
	v.add_child(h)
	undo_btn = Button.new()
	undo_btn.text = "Отменить"
	undo_btn.tooltip_text = "Ctrl+Z"
	undo_btn.pressed.connect(edit_undo)
	h.add_child(undo_btn)
	redo_btn = Button.new()
	redo_btn.text = "Повторить"
	redo_btn.tooltip_text = "Ctrl+Y"
	redo_btn.pressed.connect(edit_redo)
	h.add_child(redo_btn)
	reset_btn = Button.new()
	reset_btn.text = "Сброс"
	reset_btn.tooltip_text = "Вернуть реальные значения всем телам (отменяется)"
	reset_btn.pressed.connect(func():
		if sys.edit_reset():
			show_toast("Все правки сброшены"))
	h.add_child(reset_btn)
	_label("История правок", v)
	hist_list = ItemList.new()
	hist_list.custom_minimum_size.y = 90
	v.add_child(hist_list)
	var h2 := HBoxContainer.new()
	v.add_child(h2)
	var save := Button.new()
	save.text = "Сохранить сценарий…"
	save.pressed.connect(func(): _open_file(true))
	h2.add_child(save)
	var load := Button.new()
	load.text = "Загрузить…"
	load.pressed.connect(func(): _open_file(false))
	h2.add_child(load)
	file_dialog = FileDialog.new()
	file_dialog.access = FileDialog.ACCESS_USERDATA
	file_dialog.filters = PackedStringArray(["*.json ; Сценарий"])
	file_dialog.size = Vector2i(700, 450)
	file_dialog.file_selected.connect(_on_file)
	add_child(file_dialog)


func edit_undo() -> void:
	if sys.edit_undo():
		show_toast("Отменено")


func edit_redo() -> void:
	if sys.edit_redo():
		show_toast("Повторено")


func _open_file(saving: bool) -> void:
	_saving = saving
	DirAccess.make_dir_recursive_absolute("user://scenarios")
	file_dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE if saving else FileDialog.FILE_MODE_OPEN_FILE
	file_dialog.title = "Сохранить сценарий" if saving else "Загрузить сценарий"
	file_dialog.current_dir = "user://scenarios"
	if saving:
		file_dialog.current_file = "сценарий.json"
	file_dialog.popup_centered()


func _on_file(path: String) -> void:
	if _saving:
		var f := FileAccess.open(path, FileAccess.WRITE)
		if f == null:
			show_toast("Не удалось записать " + path)
			return
		f.store_string(sys.scenario_to_json(_bookmarks))
		show_toast("Сценарий сохранён: " + path.get_file())
		return
	var text := FileAccess.get_file_as_string(path)
	var err: String = sys.scenario_from_json(text)
	if err != "":
		show_toast("Сценарий не загружен: " + err)
		return
	_bookmarks = sys.scenario_bookmarks()
	bookmarks.clear()
	for k in _bookmarks.size():
		var a := int(_bookmarks[k][0])
		bookmarks.add_item("%d. %s" % [k + 1, infos[a]["name"] if a >= 0 else "барицентр"])
	show_toast("Сценарий загружен: " + path.get_file())


static func _parse_number(text: String) -> float:
	var t := text.strip_edges().replace(" ", "").replace("\u2009", "").replace("\u00a0", "").replace(",", ".")
	t = t.replace("−", "-").replace("×", "").replace("·10^", "e")
	return t.to_float() if t.is_valid_float() else NAN


static func _value_text(x: float, unit: String) -> String:
	if is_nan(x):
		return "—"
	if unit == "×":
		return "×" + Fmt.sig(x, 4)
	return Fmt.sci(x, 4) + (" " + unit if unit != "" else "")


func _refresh_params(delta: float) -> void:
	var i: int = main.selected
	_param_time -= delta
	var key := "%d:%d" % [i, sys.edit_revision()]
	if key == _param_key and _param_time > 0.0:
		return
	_param_time = 0.5
	undo_btn.disabled = not sys.edit_can_undo()
	redo_btn.disabled = not sys.edit_can_redo()
	if key != _param_key:
		var hist: PackedStringArray = sys.edit_history()
		var pos: int = sys.edit_history_position()
		hist_list.clear()
		for k in hist.size():
			hist_list.add_item(("%d. " % (k + 1)) + hist[k])
			if k >= pos:
				hist_list.set_item_custom_fg_color(k, Color(0.5, 0.5, 0.55))
	if i < 0:
		param_title.text = "Выберите тело слева"
		_clear_grid(param_grid)
		_param_rows = []
		_param_key = key
		return
	var rows: Array = sys.body_params(i)
	if key != _param_key or rows.size() != _param_rows.size():
		_param_key = key
		param_title.text = "%s: было → стало" % infos[i]["name"]
		_clear_grid(param_grid)
		_param_rows = []
		for t in ["Величина", "Было", "Стало"]:
			var l := _label(t, param_grid)
			l.modulate = Color(0.7, 0.75, 0.85)
		for r in rows:
			var name := Label.new()
			name.text = r["name"]
			name.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			name.custom_minimum_size.x = 150
			name.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			param_grid.add_child(name)
			var before := Label.new()
			param_grid.add_child(before)
			var after: Control
			var prm: int = r["param"]
			if prm >= 0:
				var e := LineEdit.new()
				e.custom_minimum_size.x = 110
				e.text_submitted.connect(func(text: String): _apply_edit(i, prm, text))
				after = e
			else:
				after = Label.new()
			param_grid.add_child(after)
			_param_rows.append({"before": before, "after": after, "param": prm})
	for k in rows.size():
		var r: Dictionary = rows[k]
		var unit: String = r["unit"]
		var row: Dictionary = _param_rows[k]
		(row["before"] as Label).text = _value_text(r["before"], unit)
		var txt := _value_text(r["after"], unit)
		var b: float = r["before"]
		var a: float = r["after"]
		var changed: bool = not is_nan(a) and not is_nan(b) and absf(a - b) > 1e-3 * maxf(absf(a), absf(b))
		if row["after"] is LineEdit:
			var e: LineEdit = row["after"]
			if not e.has_focus():
				e.text = txt
			e.modulate = Color(1.0, 0.85, 0.4) if r["edited"] else Color.WHITE
		else:
			(row["after"] as Label).text = txt
			(row["after"] as Label).modulate = Color(1.0, 0.85, 0.4) if changed else Color.WHITE


func _apply_edit(i: int, prm: int, text: String) -> void:
	var v := _parse_number(text)
	if is_nan(v):
		show_toast("Не число: " + text)
		return
	var err: String = sys.edit_param(i, prm, v)
	if err != "":
		show_toast(err)
	_param_key = ""


func _refresh_warnings(delta: float) -> void:
	_warn_time -= delta
	if _warn_time > 0.0:
		return
	_warn_time = 1.0
	var w: Array = sys.stability_warnings()
	var lines := []
	for x in w:
		lines.append("• " + x["text"])
	if lines.is_empty():
		warn_label.text = ("Нарушений нет. Проверяются: плотность 0,01–25 г/см³, вращение медленнее критического, "
			+ "радиус больше радиуса Шварцшильда, предел Роша, уход с орбиты (e ≥ 1), спутник в половине сферы Хилла, "
			+ "разнос орбит не меньше 2√3 взаимных радиусов Хилла, сближение орбит не ближе трёх радиусов Хилла, "
			+ "столкновения.")
	else:
		warn_label.text = "\n".join(lines)
	tabs.set_tab_title(3, "Устойчивость" + (" (%d)" % w.size() if w.size() > 0 else ""))


func _clear_grid(g: GridContainer) -> void:
	for c in g.get_children():
		g.remove_child(c)
		c.queue_free()


func _grid_tab(tc: TabContainer, title: String) -> GridContainer:
	var sc := ScrollContainer.new()
	sc.name = title
	sc.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	tc.add_child(sc)
	var g := GridContainer.new()
	g.columns = 2
	g.add_theme_constant_override("h_separation", 10)
	g.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sc.add_child(g)
	return g


func _build_bottom() -> void:
	var p := _panel(Control.PRESET_BOTTOM_WIDE)
	var h := HBoxContainer.new()
	h.add_theme_constant_override("separation", 10)
	p.add_child(h)
	cam_label = _label("", h)
	cam_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	cam_label.clip_text = true
	_label("Скорость полёта", h)
	speed_slider = HSlider.new()
	speed_slider.min_value = 0.0
	speed_slider.max_value = 1.0
	speed_slider.step = 0.0005
	speed_slider.custom_minimum_size = Vector2(220, 20)
	speed_slider.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	speed_slider.tooltip_text = "От 1 м/с до 1000 c, логарифмическая шкала (колесо мыши в полёте, [ и ])"
	speed_slider.value_changed.connect(_on_speed_slider)
	h.add_child(speed_slider)
	speed_label = _label("", h)
	speed_label.custom_minimum_size.x = 170
	fps_label = _label("", h)
	fps_label.custom_minimum_size.x = 150


func _build_help() -> void:
	help_panel = PanelContainer.new()
	help_panel.set_anchors_preset(Control.PRESET_CENTER)
	help_panel.custom_minimum_size = Vector2(560, 420)
	help_panel.position = Vector2(-280, -210)
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.03, 0.04, 0.07, 0.95)
	sb.set_content_margin_all(14)
	help_panel.add_theme_stylebox_override("panel", sb)
	var t := RichTextLabel.new()
	t.bbcode_enabled = true
	t.fit_content = true
	t.text = """[b]Клавиши[/b]
W / S — вперёд / назад, A / D — влево / вправо, R / F — вверх / вниз, Q / E — крен
Правая кнопка мыши (или левая на пустом месте) — повернуть взгляд; в облёте — облететь тело
Колесо — в облёте приблизиться / отдалиться, в полёте — скорость полёта; [ и ] — тоже скорость
G — перелёт к выбранному телу, O — облёт, B — вращаться вместе с телом
Щелчок по подписи — выбрать тело, двойной щелчок — перелёт
Пробел — пауза, Home — текущий момент
Ctrl+Z / Ctrl+Y — отменить / повторить правку параметров
F12 — снимок экрана в user://screenshots, F1 — эта подсказка, Esc — закрыть"""
	help_panel.add_child(t)
	help_panel.visible = false
	root.add_child(help_panel)


func _fill_tree(filter: String) -> void:
	tree.clear()
	_items.clear()
	var r := tree.create_item()
	var groups := {"planet": "Планеты", "dwarf": "Карликовые планеты", "comet": "Кометы"}
	var group_items := {}
	var f := filter.strip_edges().to_lower()
	for i in infos.size():
		var inf: Dictionary = infos[i]
		if not inf["visible"]:
			continue
		var name: String = inf["name"]
		var match_self := f.is_empty() or name.to_lower().contains(f) or (inf["name_en"] as String).to_lower().contains(f)
		var kind: String = inf["kind"]
		var parent_item: TreeItem = r
		if kind == "moon":
			if not match_self:
				continue
			parent_item = _items.get(inf["parent"], r)
		elif kind != "star":
			if not match_self and not _has_matching_moon(i, f):
				continue
			if not group_items.has(kind):
				var g := tree.create_item(r)
				g.set_text(0, groups.get(kind, kind))
				g.set_selectable(0, false)
				group_items[kind] = g
			parent_item = group_items[kind]
		elif not match_self:
			continue
		var it := tree.create_item(parent_item)
		it.set_text(0, name)
		it.set_metadata(0, i)
		_items[i] = it
		if kind == "planet" or kind == "dwarf":
			it.collapsed = f.is_empty()


func _has_matching_moon(i: int, f: String) -> bool:
	for inf in infos:
		if inf["parent"] == i and (inf["name"] as String).to_lower().contains(f):
			return true
	return false


func _on_date(text: String) -> void:
	var et: float = sys.utc_to_et(text)
	if is_nan(et):
		show_toast("Не понял дату: нужно ГГГГ-ММ-ДД чч:мм:сс (UTC)")
		return
	sys.set_et(et)
	date_edit.release_focus()


func _apply_rate() -> void:
	if _rate_lock:
		return
	var r := pow(RATE_MAX, rate_slider.value)
	sys.set_rate(-r if back_btn.button_pressed else r)


func set_rate_slider(rate: float) -> void:
	_rate_lock = true
	rate_slider.value = log(maxf(absf(rate), 1.0)) / log(RATE_MAX)
	back_btn.button_pressed = rate < 0
	_rate_lock = false
	_apply_rate()


func _on_speed_slider(v: float) -> void:
	if _speed_lock:
		return
	var kms := 1e-3 * pow(1000.0 * Fmt.C_KMS / 1e-3, v)
	sys.camera_set_speed(kms)


func _save_bookmark() -> void:
	var s: PackedFloat64Array = sys.camera_get_state()
	_bookmarks.append(s)
	var a := int(s[0])
	var name: String = infos[a]["name"] if a >= 0 else "барицентр"
	bookmarks.add_item("%d. %s, %s" % [_bookmarks.size(), name, sys.et_to_utc(sys.get_et()).substr(0, 16)])


func show_toast(text: String) -> void:
	toast.text = text
	_toast_time = 4.0


func toggle_help() -> void:
	help_panel.visible = not help_panel.visible


func toggle_sources() -> void:
	sources_panel.visible = not sources_panel.visible


## Окно «Источники»: все источники данных и карт с версиями, MD5 и лицензиями — из файлов данных игры
## (sources.json — эфемериды и константы, appearance.json — карты и внешний вид, sources_extra.json — этапы 6–7).
func _build_sources() -> void:
	sources_panel = PanelContainer.new()
	sources_panel.set_anchors_preset(Control.PRESET_CENTER)
	sources_panel.custom_minimum_size = Vector2(900, 620)
	sources_panel.position = Vector2(-450, -310)
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.03, 0.04, 0.07, 0.97)
	sb.set_content_margin_all(14)
	sources_panel.add_theme_stylebox_override("panel", sb)
	var v := VBoxContainer.new()
	sources_panel.add_child(v)
	var h := HBoxContainer.new()
	v.add_child(h)
	var title := _label("Источники данных, версии и лицензии", h)
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var close := Button.new()
	close.text = "Закрыть"
	close.pressed.connect(toggle_sources)
	h.add_child(close)
	var t := RichTextLabel.new()
	t.bbcode_enabled = true
	t.selection_enabled = true
	t.size_flags_vertical = Control.SIZE_EXPAND_FILL
	var parts := [["Эфемериды, ориентация, константы и физические параметры", "res://data/sources.json", ""],
		["Карты поверхностей, рельеф и внешний вид", "res://data/appearance.json", "sources"],
		["Звёзды, Млечный Путь и константы правки параметров", "res://data/sources_extra.json", "sources"]]
	var text := ""
	for part in parts:
		var data = JSON.parse_string(FileAccess.get_file_as_string(part[1]))
		if data == null:
			continue
		var d: Dictionary = data[part[2]] if part[2] != "" else data
		text += "[b]%s[/b]\n" % part[0]
		for key in d:
			var e: Dictionary = d[key]
			var line := "• " + str(e.get("title", key))
			for f in ["license", "doi", "md5", "fetched_utc", "note"]:
				if e.has(f) and str(e[f]) != "":
					line += " · %s: %s" % [f, e[f]]
			if e.has("url"):
				line += "\n   [color=#8fb4ff]%s[/color]" % e["url"]
			text += line + "\n"
		text += "\n"
	t.text = text
	v.add_child(t)
	sources_panel.visible = false
	root.add_child(sources_panel)


func select_in_tree(i: int) -> void:
	if _items.has(i):
		var it: TreeItem = _items[i]
		var p := it.get_parent()
		while p:
			p.collapsed = false
			p = p.get_parent()
		if tree.get_selected() != it:
			it.select(0)
		tree.scroll_to_item(it)


func refresh(delta: float) -> void:
	var et: float = sys.get_et()
	if not date_edit.has_focus():
		date_edit.text = sys.et_to_utc(et)
	tdb_label.text = "TDB " + sys.et_to_tdb(et).substr(0, 19)
	rate_label.text = Fmt.rate(sys.get_rate())
	if mode_opt.selected != sys.get_mode():
		mode_opt.select(sys.get_mode())
	var act: float = sys.actual_rate()
	actual_label.text = "факт: " + (Fmt.rate(act) if absf(act) > 1e-6 else "стоит")
	var st: String = sys.status()
	if sys.get_mode() == 1 and sys.mode_is_auto():
		st = "авто: " + st
	status_label.text = st
	status_label.tooltip_text = st
	pause_btn.button_pressed = sys.is_paused()
	# камера
	var a: int = sys.camera_anchor()
	var anchor_name: String = infos[a]["name"] if a >= 0 else "барицентр"
	var mode: int = sys.camera_mode()
	var t := "%s · опора: %s%s · высота %s · %s" % [CAM_MODE_RU[mode], anchor_name,
		" (вращается)" if sys.camera_body_fixed() else "", Fmt.dist(sys.camera_altitude()),
		Fmt.speed(sys.camera_actual_speed())]
	cam_label.text = t
	speed_label.text = "задано " + Fmt.speed(sys.camera_speed())
	_speed_lock = true
	speed_slider.value = log(sys.camera_speed() / 1e-3) / log(1000.0 * Fmt.C_KMS / 1e-3)
	_speed_lock = false
	if fixed_check.button_pressed != sys.camera_body_fixed():
		fixed_check.set_pressed_no_signal(sys.camera_body_fixed())
	fps_label.text = "%d кадр/с · чанков %d" % [Engine.get_frames_per_second(), sys.visible_chunk_count()]
	_refresh_info()
	_refresh_params(delta)
	_refresh_warnings(delta)
	if _toast_time > 0:
		_toast_time -= delta
		toast.modulate.a = clampf(_toast_time, 0.0, 1.0)


func _set_rows(g: GridContainer, rows: Array) -> void:
	while g.get_child_count() < rows.size() * 2:
		var l := Label.new()
		if g.get_child_count() % 2 == 1:  # значения переносятся по словам и занимают остаток ширины
			l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			l.custom_minimum_size.x = 170
		else:
			l.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
		g.add_child(l)
	while g.get_child_count() > rows.size() * 2:
		var c := g.get_child(g.get_child_count() - 1)
		g.remove_child(c)
		c.queue_free()
	for k in rows.size():
		var a: Label = g.get_child(2 * k)
		var b: Label = g.get_child(2 * k + 1)
		a.text = rows[k][0]
		a.modulate = Color(0.7, 0.75, 0.85)
		b.text = rows[k][1]


func _refresh_info() -> void:
	var i: int = main.selected
	if i < 0:
		_set_rows(info_grid, [["", "Выберите тело слева или щелчком по подписи"]])
		_set_rows(dist_grid, [])
		return
	var inf: Dictionary = infos[i]
	var rows := []
	rows.append(["Тело", inf["name"]])
	rows.append(["Тип", KIND_RU.get(inf["kind"], inf["kind"])])
	if inf["parent"] >= 0:
		rows.append(["Обращается вокруг", infos[inf["parent"]]["name"]])
	rows.append(["Код NAIF", str(inf["naif"])])
	var gm: float = inf["gm"]
	rows.append(["GM", (Fmt.sci(gm, 6) + " км³/с²") if gm > 0 else "нет данных"])
	if gm > 0:
		rows.append(["Масса", Fmt.sci(gm / 6.67430e-20, 5) + " кг"])
	var radii: PackedFloat64Array = inf["radii_km"]
	rows.append(["Средний радиус", Fmt.num(inf["radius_km"], 1) + " км"])
	if radii[0] != radii[2] or radii[0] != radii[1]:
		rows.append(["Полуоси, км", "%s × %s × %s" % [Fmt.num(radii[0], 1), Fmt.num(radii[1], 1), Fmt.num(radii[2], 1)]])
	var alb: float = inf["geometric_albedo"]
	rows.append(["Геометрическое альбедо", Fmt.sig(alb, 3) if not is_nan(alb) else "нет данных"])
	var ap: Dictionary = sys.appearance_info(i)
	if ap.has("map_source"):
		var w: int = ap.get("texture_width", 0)
		var mw: int = ap.get("max_width", w)
		rows.append(["Карта", "%s, сейчас %d×%d (до %d×%d)" % [ap["map_source"], w, w / 2, mw, mw / 2]])
		rows.append(["Лицензия карты", ap.get("map_license", "")])
		if ap.get("coverage", 1.0) < 0.999:
			rows.append(["Снято", "%d %% поверхности; %s" % [roundi(ap["coverage"] * 100.0), ap.get("fill_note", "")]])
	if ap.has("no_map"):
		rows.append(["Карта", "нет: " + ap["no_map"]])
	if ap.has("relief_source"):
		rows.append(["Рельеф", ap["relief_source"]])
	for k in ["note", "no_albedo", "atmosphere_note"]:
		if ap.has(k):
			rows.append(["Примечание", ap[k]])
	if ap.has("atmosphere"):
		rows.append(["Атмосфера", ap["atmosphere"]])
	var vis: float = sys.sun_visibility(i)
	if vis < 0.999:
		rows.append(["Затмение", "видно %d %% диска Солнца" % roundi(vis * 100.0)])
	var dev: float = sys.body_ephemeris_deviation(i)
	if not is_nan(dev):
		rows.append(["Расхождение с эфемеридами", Fmt.dist(dev)])
	var d: float = sys.distance_to_body(i)
	rows.append(["До камеры", Fmt.dist(d)])
	rows.append(["Свет идёт", Fmt.light_time(d)])
	if inf["parent"] >= 0:
		var s: PackedFloat64Array = sys.body_state_icrf(i)
		var ps: PackedFloat64Array = sys.body_state_icrf(inf["parent"])
		var dv := Vector3(s[3] - ps[3], s[4] - ps[4], s[5] - ps[5])
		var dr := sqrt(pow(s[0] - ps[0], 2) + pow(s[1] - ps[1], 2) + pow(s[2] - ps[2], 2))
		rows.append(["Расстояние до " + infos[inf["parent"]]["name"], Fmt.dist(dr)])
		rows.append(["Скорость относительно него", Fmt.speed(dv.length())])
	_set_rows(info_grid, rows)
	# расстояния
	var drows := []
	var targets := [["камеры", -2], ["Солнца", 0]]
	var earth := -1
	for k in infos.size():
		if infos[k]["naif"] == 399:
			earth = k
	if earth >= 0 and earth != i:
		targets.append(["Земли", earth])
	for tgt in targets:
		var dd: float
		if tgt[1] == -2:
			dd = d
		else:
			if tgt[1] == i:
				continue
			var a1: PackedFloat64Array = sys.body_position_icrf(i)
			var b1: PackedFloat64Array = sys.body_position_icrf(tgt[1])
			dd = sqrt(pow(a1[0] - b1[0], 2) + pow(a1[1] - b1[1], 2) + pow(a1[2] - b1[2], 2))
		drows.append(["От " + tgt[0], ""])
		drows.append(["   км", Fmt.num(dd, 0)])
		drows.append(["   а.е.", Fmt.sig(dd / Fmt.AU_KM, 6)])
		drows.append(["   время света", Fmt.light_time(dd)])
	_set_rows(dist_grid, drows)
