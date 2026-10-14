# Числа и величины для интерфейса: запятая в дробях, тонкий пробел между разрядами, русские единицы.
class_name Fmt
extends RefCounted

const AU_KM := 149597870.7  # IAU 2012
const C_KMS := 299792.458
const YEAR_S := 365.25 * 86400.0


## Число с n знаками после запятой и пробелами между разрядами.
static func num(x: float, digits: int = 0) -> String:
	if is_nan(x):
		return "—"
	var neg := x < 0
	var s := String.num(absf(x), digits)
	if digits > 0 and s.find(".") < 0:
		s += "." + "0".repeat(digits)
	var parts := s.split(".")
	var ip: String = parts[0]
	var out := ""
	var k := 0
	for i in range(ip.length() - 1, -1, -1):
		out = ip[i] + out
		k += 1
		if k % 3 == 0 and i > 0:
			out = " " + out
	if parts.size() > 1 and digits > 0:
		out += "," + parts[1]
	return ("−" if neg else "") + out


## Значащие цифры: 3 → «1,23», «12,3», «123».
static func sig(x: float, n: int = 3) -> String:
	if x == 0.0 or is_nan(x) or is_inf(x):
		return num(x)
	var e := floori(log(absf(x)) / log(10.0))
	return num(x, clampi(n - 1 - e, 0, 12))


## Научная запись «1,23·10⁸».
static func sci(x: float, n: int = 3) -> String:
	if x == 0.0 or is_nan(x):
		return num(x)
	var e := floori(log(absf(x)) / log(10.0))
	if e >= -2 and e < 6:
		return sig(x, n)
	var m := x / pow(10.0, e)
	return sig(m, n) + "·10" + sup(str(e))


static func sup(s: String) -> String:
	var t := {"-": "⁻", "0": "⁰", "1": "¹", "2": "²", "3": "³", "4": "⁴", "5": "⁵", "6": "⁶", "7": "⁷", "8": "⁸", "9": "⁹"}
	var out := ""
	for ch in s:
		out += t.get(ch, ch)
	return out


## Расстояние в км → «850 м», «12 345 км», «1,52 а.е.».
static func dist(km: float) -> String:
	var a := absf(km)
	if a < 1.0:
		return sig(km * 1000.0, 3) + " м"
	if a < 1.0e7:
		return num(km, 0 if a >= 100.0 else 1) + " км"
	if a < 1000.0 * AU_KM:
		return sig(km / AU_KM, 4) + " а.е."
	return sci(km / AU_KM, 3) + " а.е."


## Скорость в км/с → «1,0 м/с», «12,3 км/с», «3,2 c».
static func speed(kms: float) -> String:
	var a := absf(kms)
	if a < 1.0:
		return sig(kms * 1000.0, 3) + " м/с"
	if a < 0.1 * C_KMS:
		return sig(kms, 3) + " км/с"
	return sig(kms / C_KMS, 3) + " c"


## Скорость времени в с/с → «1 с/с», «2,5 мин/с», «1 сут/с», «12 лет/с».
static func rate(r: float) -> String:
	var a := absf(r)
	var sign := "−" if r < 0 else ""
	if a < 1e-9:
		return "0"
	if a < 60.0:
		return sign + sig(a, 3) + " с/с"
	if a < 3600.0:
		return sign + sig(a / 60.0, 3) + " мин/с"
	if a < 86400.0:
		return sign + sig(a / 3600.0, 3) + " ч/с"
	if a < YEAR_S:
		return sign + sig(a / 86400.0, 3) + " сут/с"
	return sign + sig(a / YEAR_S, 3) + " " + years_word(a / YEAR_S) + "/с"


static func years_word(y: float) -> String:
	var n := int(round(y))
	if absf(y - n) > 1e-6:
		return "года"
	var d := n % 10
	var dd := n % 100
	if d == 1 and dd != 11:
		return "год"
	if d >= 2 and d <= 4 and (dd < 12 or dd > 14):
		return "года"
	return "лет"


## Время света: секунды → «1,28 с», «8 мин 19 с», «4 ч 10 мин».
static func light_time(km: float) -> String:
	var s := absf(km) / C_KMS
	if s < 60.0:
		return sig(s, 3) + " с"
	if s < 3600.0:
		return "%d мин %d с" % [int(s / 60.0), int(fmod(s, 60.0))]
	if s < 86400.0 * 2:
		return "%d ч %d мин" % [int(s / 3600.0), int(fmod(s, 3600.0) / 60.0)]
	return sig(s / 86400.0, 3) + " сут"
