# Оконные тест-сценарии этапов 4–7. Запуск — из pipeline/tests/test_render.py:
#   Godot --path game --fixed-fps 60 --resolution 1280x720 -- --scenario=<имя> --out=<папка>
# --fixed-fps делает шаг кадра ровно 1/60 с независимо от скорости машины.
# Сценарий собирает данные (кадры, вырезки, числа) в папку и печатает «RESULT <имя> <json>».
# Пороги проверяет pytest: здесь только сбор данных.
extends Node

const C_KMS := 299792.458
const CROP := 64

var app: Node
var sys: Node
var cam: Camera3D
var out_dir := ""
var name_ := ""
var _drive := false
var _move := Vector3.ZERO
var meta := {}


func drives_input() -> bool:
	return _drive


func after_frame(_delta: float) -> void:
	if _drive:
		sys.camera_free_input(_move, Vector3.ZERO)


func start(main: Node, scenario: String) -> void:
	app = main
	sys = main.sys
	cam = main.camera
	name_ = scenario
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--out="):
			out_dir = a.substr(6)
	if out_dir.is_empty():
		out_dir = ProjectSettings.globalize_path("user://test_out/" + scenario)
	DirAccess.make_dir_recursive_absolute(out_dir)
	app.ui.root.visible = false  # интерфейс и подписи не попадают в кадр
	app.overlay.visible = false
	app.set_cinematic(false)  # линейная тональная кривая без свечения: в кадре ровно цвет из шейдеров
	sys.set_effect("stars", false)  # фон кадра — чёрный (звёзды и небо включает только сценарий stars)
	sys.set_effect("milkyway", false)
	if OS.get_cmdline_user_args().has("--cinematic"):
		app.set_cinematic(true)  # для просмотра глазами
	call_deferred("_run")


func _run() -> void:
	meta["viewport"] = [get_viewport().get_visible_rect().size.x, get_viewport().get_visible_rect().size.y]
	match name_:
		"smoke":
			await _smoke()
		"jitter":
			await _jitter()
		"zfight":
			await _zfight()
		"speed":
			await _speed()
		"flyto":
			await _flyto()
		"attach":
			await _attach()
		"modes":
			await _modes()
		"perf":
			await _perf()
		"maps":
			await _maps()
		"showcase":
			await _showcase()
		"lod":
			await _lod()
		"geometry":
			await _geometry()
		"eclipse":
			await _eclipse()
		"edit":
			await _edit()
		"sky":
			await _sky()
		_:
			meta["error"] = "нет сценария " + name_
	_finish()


func _finish() -> void:
	var f := FileAccess.open(out_dir.path_join("meta.json"), FileAccess.WRITE)
	f.store_string(JSON.stringify(meta, " "))
	f.close()
	print("RESULT ", name_, " ", JSON.stringify(meta).left(2000))
	get_tree().quit(0 if not meta.has("error") else 1)


func _frames(n: int) -> void:
	for k in n:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw


func _image() -> Image:
	var img := get_viewport().get_texture().get_image()
	img.convert(Image.FORMAT_RGB8)
	return img


func _save_bytes(file: String, data: PackedByteArray) -> void:
	var f := FileAccess.open(out_dir.path_join(file), FileAccess.WRITE)
	f.store_buffer(data)
	f.close()


func _flat_colors(target: int) -> void:
	sys.set_flat_mode(true)
	sys.set_orbits_visible(false)
	for i in sys.body_count():
		sys.set_body_color(i, Color(0, 1, 0))  # прочие тела — зелёные
	sys.set_body_color(target, Color(0, 0, 1))  # цель — синяя, её кольца — жёлтые (1 − цвет)


func _vp_center() -> Vector2:
	return get_viewport().get_visible_rect().size * 0.5


# --------------------------------------------------------------- пробный запуск (глазами)
func _smoke() -> void:
	app.ui.root.visible = true
	app.overlay.visible = true
	sys.set_rate(0.0)
	await _frames(30)
	_image().save_png(out_dir.path_join("earth.png"))
	var sat: int = sys.body_index("saturn")
	sys.camera_look_from(sat, Vector3(0.6, -0.5, 0.35), 400000.0)
	await _frames(30)
	_image().save_png(out_dir.path_join("saturn.png"))
	var earth: int = sys.body_index("earth")
	sys.camera_place(earth, 45.0, 30.0, 300.0, 90.0, 10.0, true)
	await _frames(30)
	_image().save_png(out_dir.path_join("surface.png"))
	sys.set_grid_visible(true)
	sys.camera_look_from(earth, Vector3(0.3, 0.8, 0.5), 20000.0)
	await _frames(30)
	_image().save_png(out_dir.path_join("earth_grid.png"))
	meta["fps"] = Engine.get_frames_per_second()
	meta["chunks"] = sys.visible_chunk_count()


# --------------------------------------------------------------- дрожание у поверхности
# Камера в 1 м над Землёй, неподвижна относительно поверхности (вращается с Землёй), метка — красный шарик 20 см
# в 3 м к северу. Время идёт 1 с/с, потом 1 сут/с. Пишутся вырезки 64×64 вокруг метки каждого кадра.
func _jitter() -> void:
	var earth: int = sys.body_index("earth")
	_flat_colors(earth)
	sys.set_body_color(earth, Color(0.3, 0.3, 0.3))
	var lat := 45.0
	var lon := 30.0
	var dlat := rad_to_deg(3.0 / 6367000.0)  # 3 м по меридиану
	var mk: int = sys.add_surface_marker(earth, lat + dlat, lon, 0.1, 0.2, Color(1, 0, 0))
	sys.camera_place(earth, lat, lon, 1.0, 0.0, -18.0, true)
	sys.set_rate(1.0)
	await _frames(20)
	var c: Vector2 = cam.unproject_position(sys.marker_render_position(mk))
	var origin := Vector2i(int(c.x) - CROP / 2, int(c.y) - CROP / 2)
	meta["crop_origin"] = [origin.x, origin.y]
	meta["crop"] = CROP
	meta["altitude_km"] = sys.camera_altitude()
	meta["near"] = cam.near
	var phases := [["1s", 1.0, 180], ["1d", 86400.0, 180]]
	for ph in phases:
		sys.set_rate(ph[1])
		await _frames(3)
		var buf := PackedByteArray()
		var proj := []
		for k in ph[2]:
			await get_tree().process_frame
			await RenderingServer.frame_post_draw
			var img := _image()
			buf.append_array(img.get_region(Rect2i(origin, Vector2i(CROP, CROP))).get_data())
			var q: Vector2 = cam.unproject_position(sys.marker_render_position(mk))
			proj.append([q.x, q.y])
		_save_bytes("jitter_%s.rgb" % ph[0], buf)
		meta["frames_" + ph[0]] = ph[2]
		meta["projected_" + ph[0]] = proj
	meta["et_end"] = sys.get_et()


# --------------------------------------------------------------- z-fighting
# Сатурн с кольцами (и Юпитер с Ио на фоне диска) с разных расстояний вплоть до 10¹¹ км, увеличение так, чтобы
# система занимала кадр. Кадр сравнивается с эталоном: лучи в double через эллипсоиды и кольца (reference_classes).
func _zview(label: String, body: int, dir: Vector3, dist_km: float, extent_km: float, body_frame: bool) -> void:
	sys.set_zoom(1.0)
	sys.camera_look_from(body, dir, dist_km, body_frame)
	var half := atan(extent_km / dist_km) / 0.85
	sys.set_zoom(maxf(1.0, tan(deg_to_rad(sys.get_view_fov()) / 2.0) / tan(half)))
	await _frames(4)
	var img := _image()
	img.save_png(out_dir.path_join("zf_%s.png" % label))
	var w := img.get_width()
	var h := img.get_height()
	_save_bytes("zf_%s.cls" % label, sys.reference_classes(w, h))
	meta["views"].append({"label": label, "body": body, "dist_km": dist_km, "zoom": sys.get_zoom(), "w": w, "h": h,
		"near": cam.near, "far": cam.far})


func _zfight() -> void:
	sys.set_rate(0.0)
	sys.set_et(sys.utc_to_et("2026-01-01 00:00:00"))  # фиксированный момент: положения спутников повторяемы
	meta["views"] = []
	var sat: int = sys.body_index("saturn")
	_flat_colors(sat)
	meta["target_saturn"] = sat
	var r_out := 136780.0
	for d in [1.5e6, 1.3e9, 1.0e10, 1.0e11]:
		for el in [25.0, 4.0, -18.0]:
			var e := deg_to_rad(el)
			var dir := Vector3(cos(e) * cos(0.6), cos(e) * sin(0.6), sin(e))
			await _zview("saturn_%d_%s%d" % [int(round(log(d) / log(10.0))), "m" if el < 0 else "p", int(absf(el))],
				sat, dir, d, r_out, true)
	# Ио перед диском Юпитера: камера на продолжении линии Юпитер → Ио
	var jup: int = sys.body_index("jupiter")
	var io: int = sys.body_index("io")
	_flat_colors(jup)
	meta["target_jupiter"] = jup
	meta["io"] = io
	var pj: PackedFloat64Array = sys.body_position_icrf(jup)
	var pio: PackedFloat64Array = sys.body_position_icrf(io)
	var u := Vector3(pio[0] - pj[0], pio[1] - pj[1], pio[2] - pj[2]).normalized()
	for d in [1.0e8, 1.0e10, 1.0e11]:
		await _zview("jupiter_io_%d" % int(round(log(d) / log(10.0))), jup, u, d, 12000.0, false)
	sys.set_zoom(1.0)


# --------------------------------------------------------------- скорость полёта
func _speed() -> void:
	sys.set_rate(0.0)
	var earth: int = sys.body_index("earth")
	var runs := []
	for v in [1.0e-3, 1.0, C_KMS, 1000.0 * C_KMS]:
		sys.camera_look_from(earth, Vector3(0.2, 0.9, 0.3), 30000.0)
		await _frames(2)
		sys.camera_set_speed(v)
		_move = Vector3(0, 0, 1)  # назад, от Земли: камера смотрит на неё
		_drive = true
		var speeds := []
		var anchors := []
		for k in 180:
			await get_tree().process_frame
			speeds.append(sys.camera_actual_speed())
			anchors.append(sys.camera_anchor())
		_drive = false
		_move = Vector3.ZERO
		await _frames(2)
		runs.append({"set_kms": v, "speeds": speeds, "anchors": anchors})
	meta["runs"] = runs
	# шкала скорости в интерфейсе: концы — 1 м/с и 1000 c
	app.ui.speed_slider.value = 0.0
	meta["slider_min_kms"] = sys.camera_speed()
	app.ui.speed_slider.value = 1.0
	meta["slider_max_kms"] = sys.camera_speed()
	meta["fixed_delta"] = 1.0 / 60.0


# --------------------------------------------------------------- перелёт
func _flyto_one(label: String, target: int) -> void:
	var t0 := Time.get_ticks_msec()
	sys.camera_fly_to(target)
	var frames := 0
	var dist := []
	while sys.camera_mode() == 2 and frames < 600:
		await get_tree().process_frame
		frames += 1
		dist.append(sys.distance_to_body(target))
	await _frames(5)
	var info: Dictionary = sys.body_info(target)
	var c: Vector2 = cam.unproject_position(sys.body_render_position(target))
	var img := _image()
	img.save_png(out_dir.path_join("flyto_%s.png" % label))
	_save_bytes("flyto_%s.cls" % label, sys.reference_classes(img.get_width(), img.get_height()))
	meta["flights"].append({"label": label, "body": target, "w": img.get_width(), "h": img.get_height(), "frames": frames, "seconds": frames / 60.0,
		"final_dist_km": sys.distance_to_body(target), "radius_km": info["radius_km"], "mode": sys.camera_mode(),
		"center_px": [c.x, c.y], "viewport_center": [_vp_center().x, _vp_center().y], "dist": dist,
		"wall_ms": Time.get_ticks_msec() - t0})


func _flyto() -> void:
	meta["flights"] = []
	sys.set_rate(1.0)
	var earth: int = sys.body_index("earth")
	sys.camera_look_from(earth, Vector3(0.2, 0.9, 0.3), 30000.0)
	await _frames(3)
	var jup: int = sys.body_index("jupiter")
	_flat_colors(jup)
	await _flyto_one("jupiter", jup)
	var pho: int = sys.body_index("phobos")
	_flat_colors(pho)
	await _flyto_one("phobos", pho)
	var sun: int = sys.body_index("sun")
	_flat_colors(sun)
	await _flyto_one("sun", sun)


# --------------------------------------------------------------- облёт при 1 сут/с
# Облёт Юпитера в «Эфемеридах» и в «Симуляции»: диск должен стоять в центре кадра. Облёт Луны с вращением вместе
# с ней: метка на поверхности стоит на месте. Пишутся вырезки вокруг центра кадра (диск) и вокруг метки.
func _attach_run(label: String, crop_center: Vector2, size: int, frames: int, probe: Callable) -> void:
	var origin := Vector2i(int(crop_center.x) - size / 2, int(crop_center.y) - size / 2)
	var buf := PackedByteArray()
	var proj := []
	for k in frames:
		await get_tree().process_frame
		await RenderingServer.frame_post_draw
		buf.append_array(_image().get_region(Rect2i(origin, Vector2i(size, size))).get_data())
		proj.append(probe.call())
	_save_bytes("attach_%s.rgb" % label, buf)
	meta["runs"].append({"label": label, "crop_origin": [origin.x, origin.y], "crop": size, "frames": frames,
		"projected": proj})


func _attach() -> void:
	meta["runs"] = []
	var jup: int = sys.body_index("jupiter")
	_flat_colors(jup)
	sys.set_rate(0.0)
	sys.camera_look_from(jup, Vector3(0.3, 0.8, 0.4), 10.0 * 69911.0)
	sys.set_rate(86400.0)
	await _frames(10)
	var probe := func():
		var q: Vector2 = cam.unproject_position(sys.body_render_position(jup))
		return [q.x, q.y, sys.distance_to_body(jup), sys.get_et()]
	await _attach_run("jupiter_eph", _vp_center(), 192, 240, probe)
	# то же в «Симуляции» (интегратор в потоке, интерполяция Эрмита)
	sys.set_mode(1)
	await _frames(30)
	meta["sim_level"] = sys.sim_level()
	await _attach_run("jupiter_sim", _vp_center(), 192, 240, probe)
	meta["sim_status"] = sys.status()
	sys.set_mode(0)
	# Луна: вращаемся вместе с ней, метка на поверхности
	var moon: int = sys.body_index("moon")
	_flat_colors(moon)
	var mk: int = sys.add_surface_marker(moon, 10.0, 20.0, 0.0, 30000.0, Color(1, 0, 0))
	sys.set_rate(0.0)
	var la := deg_to_rad(10.0)
	var lo := deg_to_rad(20.0)
	sys.camera_look_from(moon, Vector3(cos(la) * cos(lo) + 0.3, cos(la) * sin(lo) - 0.2, sin(la)), 4000.0)
	sys.camera_set_body_fixed(true)
	sys.set_rate(86400.0)
	await _frames(10)
	var mprobe := func():
		var q: Vector2 = cam.unproject_position(sys.marker_render_position(mk))
		return [q.x, q.y, sys.distance_to_body(moon), sys.get_et()]
	var mc: Vector2 = cam.unproject_position(sys.marker_render_position(mk))
	await _attach_run("moon_fixed", mc, 96, 240, mprobe)


# --------------------------------------------------------------- смена режима на краю эфемерид
# Время идёт за край покрытия спутниковых ядер (2100 год) и обратно: «Эфемериды» → «Симуляция» (авто) → «Эфемериды».
func _modes() -> void:
	var r: PackedFloat64Array = sys.ephemeris_range()
	meta["eph_range"] = [r[0], r[1]]
	var earth: int = sys.body_index("earth")
	var moon: int = sys.body_index("moon")
	sys.camera_look_from(earth, Vector3(0.2, 0.9, 0.3), 30000.0)
	var rows := []
	for phase in [[r[1] - 5.0 * 86400.0, 10.0 * 86400.0, 120], [-1.0, -10.0 * 86400.0, 150]]:
		if phase[0] > 0:
			sys.set_et(phase[0])
		sys.set_rate(phase[1])
		for k in phase[2]:
			await get_tree().process_frame
			var e: PackedFloat64Array = sys.body_state_icrf(earth)
			var m: PackedFloat64Array = sys.body_state_icrf(moon)
			rows.append([sys.get_et(), sys.get_mode(), sys.mode_is_auto(), e[0], e[1], e[2], e[3], e[4], e[5],
				m[0], m[1], m[2], sys.sim_level()])
	meta["rows"] = rows
	meta["status_end"] = sys.status()


# --------------------------------------------------------------- частота кадров (с интерфейсом)
func _perf() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)  # иначе упрёмся в частоту монитора
	app.ui.root.visible = true
	app.overlay.visible = true
	app.set_cinematic(true)  # как в игре: тональная кривая AgX и свечение
	sys.set_rate(3600.0)
	var views := []
	var earth: int = sys.body_index("earth")
	var sat: int = sys.body_index("saturn")
	var jup: int = sys.body_index("jupiter")
	var setups := [
		["earth_orbit", func(): sys.camera_look_from(earth, Vector3(0.4, 0.8, 0.3), 20000.0)],
		["saturn_rings", func(): sys.camera_look_from(sat, Vector3(0.6, -0.5, 0.35), 400000.0)],
		["earth_surface", func(): sys.camera_place(earth, 45.0, 30.0, 2.0, 90.0, 5.0, true)],
		["jupiter_system", func(): sys.camera_look_from(jup, Vector3(0.3, 0.8, 0.4), 4000000.0)],
		["solar_system", func(): sys.camera_look_from(sys.body_index("sun"), Vector3(0.1, 0.3, 0.95), 8.0e9)],
	]
	for st in setups:
		st[1].call()
		await _frames(60)  # прогрев: чанки строятся
		var t0 := Time.get_ticks_usec()
		var n := 240
		var worst := 0.0
		var prev := t0
		for k in n:
			await get_tree().process_frame
			var now := Time.get_ticks_usec()
			worst = maxf(worst, (now - prev) / 1000.0)
			prev = now
		var ms := (Time.get_ticks_usec() - t0) / 1000.0 / n
		_image().save_png(out_dir.path_join("perf_%s.png" % st[0]))
		views.append({"view": st[0], "ms": ms, "fps": 1000.0 / ms, "worst_ms": worst, "chunks": sys.visible_chunk_count()})
	meta["views"] = views


# --------------------------------------------------------------- карты тел (этап 5)
# Каждое видимое тело снимается вблизи со стороны Солнца (фаза ~25°), чтобы диск занимал ~60 % высоты кадра.
# В кадр пишутся вырезка и сведения о карте; pytest проверяет, что снимок не однотонный и нужного оттенка.
func _sun_dir_icrf(i: int) -> Vector3:
	var b: PackedFloat64Array = sys.body_position_icrf(i)
	var s: PackedFloat64Array = sys.body_position_icrf(0)
	return Vector3(s[0] - b[0], s[1] - b[1], s[2] - b[2]).normalized()


func _view_lit(i: int, fill: float) -> float:
	var s := _sun_dir_icrf(i) if i != 0 else Vector3(0.3, 0.8, 0.5).normalized()
	var perp := s.cross(Vector3(0, 0, 1)).normalized()
	var dir := (s + perp * 0.45).normalized()
	var r: float = sys.body_info(i)["radius_km"]
	var dist := r / tan(deg_to_rad(sys.get_view_fov() * 0.5) * fill)
	sys.camera_look_from(i, dir, dist, false)
	return dist


func _maps() -> void:
	sys.set_rate(0.0)
	sys.set_orbits_visible(false)
	sys.set_et(sys.utc_to_et("2026-01-01 00:00:00"))  # фиксированный момент: тесты повторяемы
	var only := []  # --keys=io,sun — только эти тела (для отладки)
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--keys="):
			only = a.substr(7).split(",")
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--debug-view="):
			sys.set_debug_view(int(a.substr(13)))
	var bodies := []
	for i in sys.body_count():
		var inf: Dictionary = sys.body_info(i)
		if not inf["visible"] or (not only.is_empty() and not only.has(inf["key"])):
			continue
		var dist := _view_lit(i, 0.6)
		if i == 0:
			app.set_exposure(-14.0)  # Солнце: без пересвета, видно потемнение к краю
		await _frames(25)
		var img := _image()
		var file := "body_%s.png" % inf["key"]
		img.save_png(out_dir.path_join(file))
		app.set_exposure(0.0)
		var vp0 := get_viewport().get_visible_rect().size
		_save_bytes("ref_%s.bin" % inf["key"], sys.reference_classes(int(vp0.x), int(vp0.y)))
		var ap: Dictionary = sys.appearance_info(i)
		var c: Vector3 = sys.body_render_position(i)
		var vp := get_viewport().get_visible_rect().size
		var proj := cam.unproject_position(c) if c.z < 0 else Vector2(-1, -1)
		bodies.append({"key": inf["key"], "file": file, "center_px": [proj.x, proj.y],
			"radius_px": sys.body_angular_radius(i) * 0.5 * vp.y / tan(deg_to_rad(sys.get_view_fov() * 0.5)),
			"texture_width": ap.get("texture_width", 0), "appearance": ap, "sun_visibility": sys.sun_visibility(i),
			"index": i, "distance_km": dist, "radii_km": inf["radii_km"], "exposure_ev": -14.0 if i == 0 else 0.0})
	meta["bodies"] = bodies
	meta["hi_bytes"] = sys.hi_texture_bytes()
	meta["et"] = sys.get_et()



# --------------------------------------------------------------- показательные виды (глазами и для документации)
func _icrf(i: int) -> Vector3:
	var a: PackedFloat64Array = sys.body_position_icrf(i)
	return Vector3(a[0], a[1], a[2])


func _shot(file: String, ev: float = 0.0) -> void:
	app.set_exposure(ev)
	await _frames(40)
	_image().save_png(out_dir.path_join(file))
	app.set_exposure(0.0)


func _showcase() -> void:
	sys.set_rate(0.0)
	sys.set_orbits_visible(false)
	var earth: int = sys.body_index("earth")
	var moon: int = sys.body_index("moon")
	var sat: int = sys.body_index("saturn")
	var sun: int = sys.body_index("sun")
	sys.set_et(sys.utc_to_et("2026-06-21 12:00:00"))
	await _frames(5)
	# Земля: терминатор сбоку (ночные огни, ореол атмосферы, блеск океана)
	var s := _sun_dir_icrf(earth)
	var side := s.cross(Vector3(0, 0, 1)).normalized()
	sys.camera_look_from(earth, (s * 0.5 + side).normalized(), 22000.0, false)
	await _shot("earth_terminator.png")
	# Земля с орбиты МКС: горизонт и небо
	sys.camera_place(earth, 20.0, -40.0, 400000.0, 80.0, -12.0, false)
	await _shot("earth_horizon.png")
	# Луна у терминатора: рельеф по LOLA
	s = _sun_dir_icrf(moon)
	side = s.cross(Vector3(0, 0, 1)).normalized()
	sys.camera_look_from(moon, (s * 0.25 + side).normalized(), 5000.0, false)
	await _shot("moon_terminator.png")
	# Сатурн со стороны Солнца, выше плоскости колец
	s = _sun_dir_icrf(sat)
	sys.camera_look_from(sat, (s + Vector3(0, 0, 0.6)).normalized(), 350000.0, false)
	await _shot("saturn.png")
	# Солнце вблизи: потемнение к краю и грануляция (экспозиция −15 EV)
	sys.camera_look_from(sun, Vector3(0.3, 0.8, 0.5), 2.2e6)
	await _shot("sun_close.png", -13.0)
	sys.camera_look_from(sun, Vector3(0.3, 0.8, 0.5), 7.5e5)
	sys.set_zoom(30.0)
	await _shot("sun_granulation.png", -13.0)
	sys.set_zoom(1.0)
	# ночная сторона Земли: огни городов (Европа)
	sys.set_et(sys.utc_to_et("2026-01-15 23:00:00"))
	await _frames(5)
	sys.camera_place(earth, 50.0, 10.0, 2500000.0, 0.0, -89.0, false)
	await _shot("earth_night.png")
	# Полное солнечное затмение 12.08.2026: камера на оси Солнце — Луна в 360 тыс. км за Луной
	sys.set_et(sys.utc_to_et("2026-08-12 17:46:00"))
	await _frames(5)
	var axis := (_icrf(moon) - _icrf(sun)).normalized()
	sys.camera_look_from(moon, axis, 360000.0, false)
	sys.set_zoom(40.0)
	await _shot("eclipse_corona.png", 2.0)
	sys.set_zoom(1.0)
	meta["sun_visibility_moon_axis"] = sys.sun_visibility(moon)


# --------------------------------------------------------------- карты 8K: подкачка и выгрузка
# Земля во весь кадр (диаметр больше 600 px) — должна подгрузиться карта 8K; потом Земля мелкая (меньше 300 px) —
# через 3 с карта 8K выгружается, видеопамять возвращается.
func _vram() -> float:
	return Performance.get_monitor(Performance.RENDER_TEXTURE_MEM_USED)


func _lod() -> void:
	sys.set_rate(0.0)
	sys.set_et(sys.utc_to_et("2026-01-01 00:00:00"))
	var earth: int = sys.body_index("earth")
	_view_lit(earth, 0.15)
	await _frames(240)  # всё мелкое: крупных карт нет
	meta["start"] = {"width": sys.body_texture_width(earth), "hi_bytes": sys.hi_texture_bytes(), "vram": _vram()}
	_view_lit(earth, 0.95)
	var n := 0
	var t0 := Time.get_ticks_msec()
	while sys.body_texture_width(earth) < 8192 and n < 1200:
		await get_tree().process_frame
		n += 1
	await _frames(10)
	meta["near"] = {"width": sys.body_texture_width(earth), "frames": n, "ms": Time.get_ticks_msec() - t0,
		"hi_bytes": sys.hi_texture_bytes(), "vram": _vram(), "px": sys.appearance_info(earth)["px"]}
	_image().save_png(out_dir.path_join("near.png"))
	_view_lit(earth, 0.15)
	n = 0
	while sys.body_texture_width(earth) > 2048 and n < 1200:
		await get_tree().process_frame
		n += 1
	await _frames(30)
	meta["far"] = {"width": sys.body_texture_width(earth), "frames": n, "hi_bytes": sys.hi_texture_bytes(),
		"vram": _vram(), "px": sys.appearance_info(earth)["px"]}


# --------------------------------------------------------------- геометрия: подсолнечная точка и координаты карты
# Для Земли, Марса, Луны и Юпитера (1 января 2026, 00:00 UTC): кадр «Ламберт без карт» (яркость = cos угла к Солнцу),
# кадры с кодом u и v карты и эталон по лучам (широта и долгота каждого пикселя). Сравнение со SPICE — в pytest.
func _geometry() -> void:
	sys.set_rate(0.0)
	sys.set_et(sys.utc_to_et("2026-01-01 00:00:00"))
	var shots := []
	var vp := get_viewport().get_visible_rect().size
	for key in ["earth", "mars", "moon", "jupiter"]:
		var i: int = sys.body_index(key)
		_view_lit(i, 0.8)
		var msaa := get_viewport().msaa_3d
		for mode in [1, 2, 3]:
			sys.set_debug_view(mode)
			# код u, v: без MSAA — сглаживание усреднило бы два фрагмента на стыке треугольников и испортило код
			get_viewport().msaa_3d = Viewport.MSAA_DISABLED if mode >= 2 else msaa
			await _frames(20)
			_image().save_png(out_dir.path_join("%s_v%d.png" % [key, mode]))
		sys.set_debug_view(0)
		get_viewport().msaa_3d = msaa
		_save_bytes("%s_ref.bin" % key, sys.reference_surface(int(vp.x), int(vp.y)).to_byte_array())
		shots.append({"key": key, "index": i, "visibility": sys.sun_visibility(i)})
	meta["shots"] = shots
	meta["et"] = sys.get_et()
	meta["size"] = [int(vp.x), int(vp.y)]


# --------------------------------------------------------------- затмение: тень Луны на Земле 12.08.2026
func _eclipse() -> void:
	sys.set_rate(0.0)
	sys.set_orbits_visible(false)
	var earth: int = sys.body_index("earth")
	var moon: int = sys.body_index("moon")
	var vp := get_viewport().get_visible_rect().size
	var shots := []
	for utc in ["2026-08-12 17:30:00", "2026-08-12 18:00:00"]:
		sys.set_et(sys.utc_to_et(utc))
		await _frames(3)
		# камера над точкой, где ось Солнце → Луна пересекает Землю (сфера среднего радиуса): тень — в середине кадра
		var r_e: float = sys.body_info(earth)["radius_km"]
		var dist := r_e / tan(deg_to_rad(sys.get_view_fov() * 0.5) * 0.8)
		var ax := (_icrf(moon) - _icrf(0)).normalized()
		var o := _icrf(moon) - _icrf(earth)
		var b := o.dot(ax)
		var hit := o + ax * (-b - sqrt(maxf(b * b - (o.dot(o) - r_e * r_e), 0.0)))
		sys.camera_look_from(earth, hit.normalized(), dist, false)
		sys.set_debug_view(1)
		await _frames(20)
		var tag: String = str(utc).substr(11, 5).replace(":", "")
		_image().save_png(out_dir.path_join("earth_%s.png" % tag))
		_save_bytes("earth_%s_ref.bin" % tag, sys.reference_surface(int(vp.x), int(vp.y)).to_byte_array())
		sys.set_debug_view(0)
		shots.append({"utc": utc, "tag": tag, "et": sys.get_et(), "moon_visibility": sys.sun_visibility(moon),
			"earth_visibility": sys.sun_visibility(earth), "casters": sys.shadow_casters(earth),
			"sun": sys.body_position_icrf(0), "earth": sys.body_position_icrf(earth), "moon": sys.body_position_icrf(moon)})
	# Ио в тени Юпитера: 13.10.2026 15:00 UTC (по SPICE — затмение)
	var io: int = sys.body_index("io")
	sys.set_et(sys.utc_to_et("2026-10-13 15:00:00"))
	await _frames(3)
	meta["io_visibility"] = sys.sun_visibility(io)
	meta["io_et"] = sys.get_et()
	meta["shots"] = shots
	meta["size"] = [int(vp.x), int(vp.y)]


# --------------------------------------------------------------- этап 6: правки через узел SolarSystem и интерфейс
func _wait_recompute() -> int:
	var n := 0
	while sys.status().contains("пересчёт") and n < 1800:
		await get_tree().process_frame
		n += 1
	return n


func _edit() -> void:
	sys.set_et(sys.utc_to_et("2026-01-01 00:00:00"))
	sys.set_rate(86400.0)
	var earth: int = sys.body_index("earth")
	var moon: int = sys.body_index("moon")
	sys.camera_look_from(earth, Vector3(0.3, 0.8, 0.4), 60000.0)
	await _frames(5)
	var steps := []
	var step := func(label: String) -> void:
		steps.append({"label": label, "et": sys.get_et(), "mode": sys.get_mode(), "status": sys.status(),
			"revision": sys.edit_revision(), "earth": Array(sys.body_state_icrf(earth)),
			"warnings": sys.stability_warnings().map(func(w): return w["text"])})
	# скорость Луны × 1,05: правка включает «Симуляцию» с текущего момента
	meta["err_moon"] = sys.edit_param(moon, 4, 1.05)
	step.call("moon")
	await _frames(60)
	# масса Земли × 2 через сутки: пересчёт от момента первой правки до текущего
	var before_et: float = sys.get_et()
	var mass: float = sys.body_params(earth)[0]["before"]
	meta["err_mass"] = sys.edit_param(earth, 0, 2.0 * mass)
	meta["recompute_frames"] = await _wait_recompute()
	meta["et_gap_after_recompute"] = sys.get_et() - before_et
	step.call("mass")
	# период вращения Земли 0,5 ч — быстрее критического (предупреждение, «Симуляция» не перезапускается)
	meta["err_spin"] = sys.edit_param(earth, 3, 0.5)
	step.call("spin")
	meta["rows"] = sys.body_params(earth)
	meta["err_bad"] = sys.edit_param(earth, 2, 1.5)  # альбедо вне 0–1
	# отмена до конца — снова эфемериды; повтор — снова «Симуляция»
	sys.edit_undo()
	sys.edit_undo()
	await _wait_recompute()
	step.call("undo2")
	sys.edit_undo()
	await _frames(2)
	step.call("undo3")
	for k in 3:
		sys.edit_redo()
	await _wait_recompute()
	step.call("redo3")
	meta["history"] = Array(sys.edit_history())
	# сценарий JSON через узел: сохранить → загрузить → сохранить
	var bm := [sys.camera_get_state()]
	var j1: String = sys.scenario_to_json(bm)
	meta["load_error"] = sys.scenario_from_json(j1)
	await _wait_recompute()
	var j2: String = sys.scenario_to_json(sys.scenario_bookmarks())
	meta["json_same"] = j1 == j2
	meta["json"] = j1
	step.call("loaded")
	meta["steps"] = steps
	# снимок вкладки «Параметры» для документации
	sys.set_paused(true)
	app.ui.root.visible = true
	app.overlay.visible = true
	app.set_cinematic(true)
	app.select_body(earth)
	app.ui.tabs.current_tab = 1
	await _frames(40)
	_image().save_png(out_dir.path_join("edit_ui.png"))
	app.ui.tabs.current_tab = 3
	await _frames(70)
	_image().save_png(out_dir.path_join("edit_warnings.png"))


# --------------------------------------------------------------- этап 7: звёзды и Млечный Путь
const HIP := {"sirius": 32349, "betelgeuse": 27989, "polaris": 11767}


func _look_icrf(d: Vector3) -> void:
	# камера над барицентром (1·10⁹ км к северному полюсу мира, тела далеко от этих направлений), взгляд вдоль d
	var up := Vector3(0, 0, 1) if absf(d.z) < 0.9 else Vector3(1, 0, 0)
	var q := Basis.looking_at(d, up).get_rotation_quaternion()
	sys.camera_set_state(PackedFloat64Array([-1, 0, 0, 0.0, 0.0, 1e9, q.w, q.x, q.y, q.z, 1e6, 0, 0, 1.0]))


func _radec(ra_deg: float, dec_deg: float) -> Vector3:
	var a := deg_to_rad(ra_deg)
	var d := deg_to_rad(dec_deg)
	return Vector3(cos(d) * cos(a), cos(d) * sin(a), sin(d))


func _sky() -> void:
	sys.set_et(sys.utc_to_et("2026-01-01 00:00:00"))
	sys.set_paused(true)
	sys.set_orbits_visible(false)
	sys.set_view_fov(20.0)
	sys.set_sky_param("star_flux0", 0.5)  # Сириус без насыщения 8-битного кадра
	var size := get_viewport().get_visible_rect().size
	meta["rad_per_px"] = 2.0 * tan(deg_to_rad(10.0)) / size.y
	meta["star_count"] = sys.star_count()
	# звёзды: камера смотрит точно на звезду, звезда должна быть в центре кадра
	sys.set_effect("stars", true)
	var c := Vector2i(size / 2.0)
	for key in HIP:
		var d: PackedFloat64Array = sys.star_direction(HIP[key])
		meta["dir_" + key] = Array(d)
		_look_icrf(Vector3(d[0], d[1], d[2]))
		await _frames(4)
		_image().get_region(Rect2i(c - Vector2i(20, 20), Vector2i(41, 41))).save_png(out_dir.path_join("star_%s.png" % key))
	# поток Сириуса при повороте камеры на доли пикселя
	var d0: PackedFloat64Array = sys.star_direction(HIP["sirius"])
	var base := Vector3(d0[0], d0[1], d0[2])
	var axis := base.cross(Vector3(0, 0, 1)).normalized()
	for k in 9:
		_look_icrf(base.rotated(axis, k * 0.125 * meta["rad_per_px"]))
		await _frames(3)
		_image().get_region(Rect2i(c - Vector2i(20, 20), Vector2i(41, 41))).save_png(out_dir.path_join("shift_%d.png" % k))
	sys.set_effect("stars", false)
	# Млечный Путь: код α, δ в центре кадра при взгляде на центр Галактики; яркость к центру и к полюсу Галактики
	sys.set_effect("milkyway", true)
	_look_icrf(_radec(266.405, -28.936))
	for m in [1, 2]:
		sys.set_sky_param("milkyway_debug", m)
		await _frames(4)
		_image().get_region(Rect2i(c - Vector2i(2, 2), Vector2i(5, 5))).save_png(out_dir.path_join("gc_code%d.png" % m))
	sys.set_sky_param("milkyway_debug", 0)
	for v in [["gc", 266.405, -28.936], ["ngp", 192.85948, 27.12825]]:
		_look_icrf(_radec(v[1], v[2]))
		await _frames(4)
		_image().save_png(out_dir.path_join("mw_%s.png" % v[0]))
	# вид для глаз: Орион и Млечный Путь в обычном режиме
	sys.set_effect("stars", true)
	sys.set_sky_param("star_flux0", 14.0)
	sys.set_view_fov(60.0)
	app.set_cinematic(true)
	_look_icrf(_radec(84.0, 0.0))
	await _frames(6)
	_image().save_png(out_dir.path_join("orion.png"))
	_look_icrf(_radec(266.405, -28.936))
	await _frames(6)
	_image().save_png(out_dir.path_join("galactic_center.png"))
	# окно «Источники» (этап 8)
	app.ui.root.visible = true
	app.ui.toggle_sources()
	await _frames(10)
	_image().save_png(out_dir.path_join("sources.png"))
	app.ui.toggle_sources()
