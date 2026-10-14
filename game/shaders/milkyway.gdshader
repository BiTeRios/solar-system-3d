// Млечный Путь (этап 7): карта NASA SVS «Deep Star Maps 2020» (фон без ярких звёзд) на весь экран у дальней плоскости.
// Карта — plate carrée в ICRF: в центре α = 0h, α растёт влево, сверху δ = +90°.
shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled, fog_disabled, skip_vertex_transform;

uniform sampler2D milkyway : filter_linear_mipmap, repeat_enable;
uniform mat3 view_to_icrf;   // оси камеры → ICRF
uniform float scale = 2.5;   // условная яркость: полоса Млечного Пути ~0,05–0,12 при 0 EV
uniform int debug_view = 0;  // тесты: 1 — код α, 2 — код δ (старшие 8 бит в R, младшие в G)

varying vec3 ray;

vec3 srgb_to_linear(vec3 c) {
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void vertex() {
	POSITION = vec4(VERTEX.xy, 1e-7, 1.0);
	vec4 p = INV_PROJECTION_MATRIX * vec4(VERTEX.xy, 1.0, 1.0);
	ray = p.xyz / p.w;
}

void fragment() {
	vec3 d = view_to_icrf * normalize(ray);
	float ra = atan(d.y, d.x);                      // −π…π
	float dec = asin(clamp(d.z, -1.0, 1.0));
	vec2 uv = vec2(fract(0.5 - ra / TAU), 0.5 - dec / PI);
	if (debug_view != 0) {
		float a = debug_view == 1 ? fract(ra / TAU + 1.0) : clamp(dec / PI + 0.5, 0.0, 1.0 - 1e-7);
		ALBEDO = srgb_to_linear(vec3(floor(a * 255.0) / 255.0, fract(a * 255.0), 0.0));
	} else {
		// производные — по координате со швом на α = 0h, а не на 12h, где шов у uv: без полосы на шве
		vec2 uv2 = vec2(fract(uv.x + 0.5), uv.y);
		vec2 dx = dFdx(uv), dy = dFdy(uv), dx2 = dFdx(uv2), dy2 = dFdy(uv2);
		if (abs(dx2.x) + abs(dy2.x) < abs(dx.x) + abs(dy.x)) {
			dx = dx2;
			dy = dy2;
		}
		ALBEDO = textureGrad(milkyway, uv, dx, dy).rgb * scale;
	}
}
