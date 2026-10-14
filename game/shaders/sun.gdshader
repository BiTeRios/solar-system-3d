// Солнце: светящаяся поверхность с потемнением к краю и грануляцией.
// Потемнение: I(μ)/I(1) = μ^α(λ), α = a0 + a1/λ[мкм] (Hestroffer & Magnan 1998), λ каналов — 680, 550, 440 нм.
// Яркость центра диска в единицах «белая ламбертова поверхность у Земли = 1»: средняя яркость диска равна
// (1 а. е./R☉)² = π/Ω☉, а для закона μ^α средняя по диску = центр · 2/(α + 2).
// Грануляция — процедурный шум с ячейками порядка 1000 км; на мелком изображении гаснет (усредняется).
shader_type spatial;
render_mode unshaded, cull_back, fog_disabled, skip_vertex_transform;

#include "res://shaders/compress.gdshaderinc"

uniform vec3 radii = vec3(695700.0);
uniform vec3 cam_pos = vec3(0.0, 0.0, 1.5e8);  // км, оси Солнца
uniform float limb_a0 = -0.023;
uniform float limb_a1 = 0.292;
uniform vec3 lambda_um = vec3(0.680, 0.550, 0.440);
uniform float disk_mean = 46239.0;  // (1 а. е./R☉)²
uniform float granulation = 0.12;   // контраст грануляции (условный)
uniform float granule_km = 1000.0;
// Кадр Godot хранится в FP16 (не больше 65504), а центр диска с грануляцией — около 65000: переполнение даёт
// бесконечность и чёрные пиксели. Поэтому яркость Солнца и короны (corona.gdshader) умножается на один множитель
// sun_scale: соотношение диска и короны сохраняется, а при обычной экспозиции диск всё равно белый.
uniform float sun_scale = 0.25;
uniform bool flat_mode = false;
uniform vec3 base_color : source_color = vec3(1.0);

varying vec3 body_dir;
varying vec3 body_nrm;

void vertex() {
	body_dir = CUSTOM0.xyz;
	body_nrm = NORMAL;
	VERTEX = compress_view((MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz);
}

vec3 hash3(vec3 p) {
	p = vec3(dot(p, vec3(127.1, 311.7, 74.7)), dot(p, vec3(269.5, 183.3, 246.1)), dot(p, vec3(113.5, 271.9, 124.6)));
	return fract(sin(p) * 43758.5453123);
}

// Клеточный шум (расстояние до ближайшего узла минус до второго): яркие центры гранул, тёмные межгранульные полосы.
float cells(vec3 x) {
	vec3 i = floor(x), f = fract(x);
	float d1 = 8.0, d2 = 8.0;
	for (int a = -1; a <= 1; a++)
		for (int b = -1; b <= 1; b++)
			for (int c = -1; c <= 1; c++) {
				vec3 g = vec3(float(a), float(b), float(c));
				vec3 r = g + hash3(i + g) - f;
				float d = dot(r, r);
				if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) { d2 = d; }
			}
	return sqrt(d2) - sqrt(d1);
}

void fragment() {
	if (flat_mode) {
		ALBEDO = base_color;
	} else {
		vec3 d = normalize(body_dir);
		vec3 n = normalize(body_nrm);
		vec3 q = d / length(d / radii);  // точка поверхности, км
		float mu = clamp(dot(n, normalize(cam_pos - q)), 0.0, 1.0);
		vec3 alpha = vec3(limb_a0) + limb_a1 / lambda_um;
		vec3 center = disk_mean * (alpha + 2.0) * 0.5;
		vec3 c = center * pow(vec3(max(mu, 1e-4)), alpha);
		// грануляция гаснет, когда ячейка меньше пикселя
		vec3 x = q / granule_km;
		float fw = length(fwidth(x));
		float fade = 1.0 - smoothstep(0.25, 0.8, fw);
		if (fade > 0.0) {
			float g = cells(x) * 2.0 - 0.5;
			c *= 1.0 + granulation * fade * clamp(g, -1.0, 1.0);
		}
		ALBEDO = c * sun_scale;
	}
}
