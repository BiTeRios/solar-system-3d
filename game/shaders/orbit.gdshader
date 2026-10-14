// Линия орбиты — лента постоянной ширины в пикселях с мягкими краями.
// Точки хранятся в ICRF (м) относительно центра родителя как две части float (hi + lo), сдвиг «родитель − камера»
// тоже: rel = (origin_hi + p_hi) + (origin_lo + p_lo). Большие части сокращаются точно, и линия у тела не дрожит
// даже на орбите Нептуна (≈4,5·10¹² м, у float шаг там ≈ 0,5 км). Затем поворот в оси камеры (view_rot).
// VERTEX — hi этой точки; CUSTOM0 = (lo этой точки, сторона ±1); CUSTOM1 = (hi соседней, 1 — эта точка конец отрезка);
// CUSTOM2 = (lo соседней, яркость вдоль орбиты).
shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never, skip_vertex_transform, fog_disabled;

#include "res://shaders/compress.gdshaderinc"

instance uniform vec3 origin_hi;
instance uniform vec3 origin_lo;
instance uniform vec4 line_color : source_color = vec4(0.4, 0.6, 1.0, 0.6);
uniform mat3 view_rot;  // ICRF → оси камеры
uniform float width_px = 1.6;
uniform float near_clip = 0.05;

varying float v_side;
varying float v_fade;

void vertex() {
	vec3 a = (origin_hi + VERTEX) + (origin_lo + CUSTOM0.xyz);
	vec3 b = (origin_hi + CUSTOM1.xyz) + (origin_lo + CUSTOM2.xyz);
	vec4 va = vec4(compress_view(view_rot * a), 1.0);
	vec4 vb = vec4(compress_view(view_rot * b), 1.0);
	float zn = -near_clip;
	if (va.z > zn && vb.z > zn) {
		POSITION = vec4(2.0, 2.0, 2.0, 1.0);  // отрезок целиком позади камеры
	} else {
		if (va.z > zn) va = mix(va, vb, (zn - va.z) / (vb.z - va.z));
		if (vb.z > zn) vb = mix(vb, va, (zn - vb.z) / (va.z - vb.z));
		vec4 ca = PROJECTION_MATRIX * va;
		vec4 cb = PROJECTION_MATRIX * vb;
		vec2 half_px = VIEWPORT_SIZE * 0.5;
		vec2 sa = ca.xy / ca.w * half_px;
		vec2 sb = cb.xy / cb.w * half_px;
		vec2 d = sb - sa;
		if (CUSTOM1.w > 0.5) d = -d;  // направление всегда от начала отрезка к концу
		d = length(d) > 1e-6 ? normalize(d) : vec2(1.0, 0.0);
		vec2 n = vec2(-d.y, d.x);
		ca.xy += n * CUSTOM0.w * width_px / half_px * ca.w;
		POSITION = ca;
	}
	v_side = CUSTOM0.w;
	v_fade = CUSTOM2.w;
}

void fragment() {
	float edge = 1.0 - smoothstep(0.35, 1.0, abs(v_side));
	ALBEDO = line_color.rgb;
	ALPHA = line_color.a * edge * v_fade;
}
