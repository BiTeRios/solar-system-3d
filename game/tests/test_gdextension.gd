# Проверка, что GDExtension spacecore загружается в Godot 4.7 и читает эфемериды.
# Запуск: Godot --headless --path game -s tests/test_gdextension.gd -- <эпоха ET> <код тела>...
# Печатает строки «STATE <код> <hex>»: шесть double (x, y, z, vx, vy, vz) байтами; pytest сверяет их с эталоном.
extends SceneTree


func _initialize() -> void:
	var code := 0
	if not ClassDB.class_exists("SpaceCore"):
		printerr("ОШИБКА: класс SpaceCore не зарегистрирован, расширение не загрузилось")
		quit(1)
		return
	var core: RefCounted = ClassDB.instantiate("SpaceCore")
	print("VERSION ", core.version())
	if not core.open_ephemeris("res://data/ephemeris.eph"):
		printerr("ОШИБКА: ", core.last_error())
		quit(1)
		return
	var args := OS.get_cmdline_user_args()
	var et := float(args[0]) if args.size() > 0 else 0.0
	for i in range(1, args.size()):
		var target := int(args[i])
		var s: PackedFloat64Array = core.ephemeris_state(target, et, true)
		if s.is_empty():
			printerr("ОШИБКА: нет данных для ", target)
			code = 1
			continue
		# байты double в hex: сверка с эталоном до бита
		print("STATE ", target, " ", s.to_byte_array().hex_encode())
	quit(code)
