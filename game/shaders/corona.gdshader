// Корона Солнца: квадрат, повёрнутый к камере, складывается с кадром. Яркость по формуле Баумбаха (1937):
// I(r)/I_центра = 10⁻⁶·Σ c_k·r^(−p_k), r — прицельное расстояние луча от центра Солнца в радиусах Солнца.
// Корона в миллион раз тусклее диска: видна у самого края или когда диск закрыт (затмение).
shader_type spatial;
render_mode unshaded, cull_disabled, blend_add, depth_draw_never, fog_disabled, skip_vertex_transform;

#include "res://shaders/compress.gdshaderinc"

uniform vec3 sun_view = vec3(0.0, 0.0, -1.5e8);  // центр Солнца, км, оси камеры
uniform float sun_radius = 695700.0;
uniform float half_size = 4.17e9;                 // полуразмер квадрата, м (в осях камеры)
uniform vec3 center_radiance = vec3(58000.0);     // яркость центра диска
uniform vec4 terms = vec4(2.565, 17.0, 1.425, 7.0);  // c1, p1, c2, p2
uniform vec2 term3 = vec2(0.0532, 2.5);              // c3, p3
uniform float unit = 1e-6;
uniform float sun_scale = 0.25;  // тот же множитель, что у диска (sun.gdshader)

void vertex() {
	// квадрат 2×2 в плоскости, перпендикулярной лучу на центр Солнца
	vec3 c = sun_view * 1000.0;
	vec3 z = normalize(-c);
	vec3 x = normalize(cross(abs(z.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), z));
	vec3 y = cross(z, x);
	vec3 v = c + (x * VERTEX.x + y * VERTEX.y) * half_size;
	VERTEX = compress_view(v);
}

void fragment() {
	vec3 d = normalize(VERTEX);
	float b = length(cross(d, sun_view)) / sun_radius;  // прицельное расстояние, R☉
	if (b <= 1.0 || dot(d, sun_view) <= 0.0) discard;
	float k = terms.x * pow(b, -terms.y) + terms.z * pow(b, -terms.w) + term3.x * pow(b, -term3.y);
	ALBEDO = center_radiance * unit * k * sun_scale;
}
