// Кольца Сатурна в плоскости экватора планеты (z = 0 в её осях). Цвет вершины: rgb — альбедо частиц,
// a — нормальная оптическая толщина τ/4 (NASA Saturnian Rings Fact Sheet, середина диапазона).
// Слой частиц в приближении однократного рассеяния (Chandrasekhar): μ0, μ — синусы высоты Солнца и камеры над
// плоскостью колец. Освещённая сторона: I/F = p·μ0/(μ + μ0)·(1 − e^(−τ(1/μ + 1/μ0))),
// тень (вид с неосвещённой стороны): I/F = p·μ0/(μ0 − μ)·(e^(−τ/μ0) − e^(−τ/μ)), где p = 2A/3 (ламбертовы частицы).
// Непрозрачность для фона: 1 − e^(−τ/μ). Тень планеты на кольцах — луч к Солнцу против эллипсоида Сатурна.
shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never, fog_disabled, skip_vertex_transform;

#include "res://shaders/compress.gdshaderinc"

uniform vec3 sun_dir = vec3(1.0, 0.0, 0.0);      // на Солнце, оси Сатурна
uniform vec3 cam_pos = vec3(0.0, 0.0, 1.0e6);    // км, оси Сатурна
uniform vec3 radii = vec3(60268.0, 60268.0, 54364.0);
uniform bool flat_mode = false;
uniform vec3 flat_color : source_color = vec3(1.0, 0.0, 0.0);
varying vec4 ring;
varying vec3 ring_pos;  // км, оси Сатурна

void vertex() {
	ring = COLOR;
	ring_pos = VERTEX * 0.001;
	VERTEX = compress_view((MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz);
}

// Луч из q к Солнцу пересекает Сатурн? Возвращает освещённость 0..1 (мягкий край шириной ~ угловой размер Солнца).
float planet_shadow(vec3 q, vec3 s) {
	vec3 o = q / radii, d = s / radii;
	float a = dot(d, d);
	float t = -dot(o, d) / a;
	if (t <= 0.0) return 1.0;
	vec3 c = o + d * t;
	float miss = length(c) - 1.0;  // > 0 — луч проходит мимо (в единицах радиуса)
	return smoothstep(-0.002, 0.002, miss);
}

void fragment() {
	if (flat_mode) {
		ALBEDO = flat_color;
		ALPHA = 1.0;
	} else {
		float tau = ring.a * 4.0;
		vec3 p = ring.rgb * (2.0 / 3.0);
		vec3 s = normalize(sun_dir);
		vec3 v = normalize(cam_pos - ring_pos);
		float mu0 = max(abs(s.z), 1e-3), mu = max(abs(v.z), 1e-3);
		float opacity = 1.0 - exp(-tau / mu);
		vec3 rad;
		if (s.z * v.z > 0.0) {
			rad = p * mu0 / (mu + mu0) * (1.0 - exp(-tau * (1.0 / mu + 1.0 / mu0)));
		} else {
			float dm = mu0 - mu;
			float k = abs(dm) > 1e-4 ? (exp(-tau / mu0) - exp(-tau / mu)) / dm : tau / (mu * mu) * exp(-tau / mu);
			rad = p * mu0 * k;
		}
		rad *= planet_shadow(ring_pos, s);
		ALPHA = clamp(opacity, 0.0, 1.0);
		ALBEDO = rad / max(ALPHA, 1e-4);
	}
}
