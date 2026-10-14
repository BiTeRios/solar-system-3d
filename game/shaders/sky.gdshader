// Оболочка атмосферы (сфера радиусом «верх атмосферы»). Рисуются задние грани: так оболочка видна и снаружи,
// и изнутри. Лучи, упирающиеся в поверхность, закрыты самой поверхностью (тест глубины), а дымку над поверхностью
// считает body.gdshader. Здесь — свет, рассеянный вдоль луча мимо тела (ореол у края, небо изнутри).
// Смешивание: цвет + фон·(1 − α), где α = 1 − пропускание.
shader_type spatial;
render_mode unshaded, cull_front, blend_premul_alpha, depth_draw_never, fog_disabled, skip_vertex_transform;

#include "res://shaders/compress.gdshaderinc"

uniform vec3 radii = vec3(1.0);
#include "res://shaders/atmosphere.gdshaderinc"

uniform vec3 sun_dir = vec3(1.0, 0.0, 0.0);  // оси тела
uniform vec3 cam_pos = vec3(0.0, 0.0, 1.0e5);  // км, оси тела

varying vec3 shell_pos;  // км, оси тела

void vertex() {
	shell_pos = VERTEX * 0.001;
	VERTEX = compress_view((MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz);
}

void fragment() {
	vec3 o = cam_pos;
	vec3 dv = shell_pos - o;
	vec3 d = normalize(dv);
	float t1 = length(dv);
	// луч упирается в тело — до поверхности (такие пиксели обычно закрыты поверхностью)
	float R = (radii.x + radii.y + radii.z) / 3.0;
	vec2 hit = atm_sphere(o, d, R);
	if (hit.x > 0.0 && hit.x < hit.y) t1 = min(t1, hit.x);
	vec3 ins, tr;
	atm_integrate(o, d, 0.0, t1, normalize(sun_dir), ins, tr);
	ALBEDO = ins;
	ALPHA = clamp(1.0 - dot(tr, vec3(1.0 / 3.0)), 0.0, 1.0);
}
