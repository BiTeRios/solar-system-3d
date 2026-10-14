// Звёзды Hipparcos (этап 7): квадрат в несколько пикселей с гауссовым профилем у дальней плоскости.
// Размер звезды на экране постоянный (σ в пикселях), слабые звёзды тусклее по потоку, а не меньше; у ярких, чей пик
// превысил бы peak_max, σ растёт — поток тот же, звезда шире. Сумма пикселей звезды = flux0 · 10^(−0,4·V) (канал G),
// поэтому поток не зависит от того, куда звезда попала внутри пикселя.
// Вершина: VERTEX — направление ICRS на эпоху каталога, CUSTOM0 — собственное движение (рад/год) и V,
// CUSTOM1 — цвет, UV — угол квадрата.
shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled, fog_disabled, skip_vertex_transform;

uniform mat3 icrf_to_view;      // ICRF → оси камеры
uniform float years = 35.0;     // юлианские годы от эпохи каталога
uniform float flux0 = 14.0;     // условная яркость: сумма пикселей звезды 0m (единица — белая поверхность под Солнцем)
uniform float sigma_px = 0.75;
uniform float peak_max = 1.0;

varying float sigma;
varying vec3 star_rgb;
varying vec2 offs;

void vertex() {
	vec3 d = normalize(VERTEX + CUSTOM0.xyz * years);
	vec3 v = icrf_to_view * d;
	vec4 c = PROJECTION_MATRIX * vec4(v, 1.0);
	float flux = flux0 * pow(10.0, -0.4 * CUSTOM0.w);
	float peak = flux * max(CUSTOM1.r, max(CUSTOM1.g, CUSTOM1.b));
	sigma = max(sigma_px, sqrt(peak / (6.2831853 * peak_max)));
	float r = 3.5 * sigma;
	vec2 corner = UV * 2.0 - 1.0;
	c.xy += corner * (2.0 * r / VIEWPORT_SIZE) * c.w;
	c.z = 1e-7 * c.w;  // у дальней плоскости (обратный Z): тела закрывают звёзды
	if (v.z > -1e-6) c = vec4(0.0, 0.0, -1.0, 1.0);  // позади камеры — вне кадра
	POSITION = c;
	offs = corner * r;
	star_rgb = CUSTOM1.rgb * flux / (6.2831853 * sigma * sigma);
}

void fragment() {
	ALBEDO = star_rgb * exp(-dot(offs, offs) / (2.0 * sigma * sigma));
}
