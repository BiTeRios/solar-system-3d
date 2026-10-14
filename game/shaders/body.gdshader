// Поверхность тела: чанк кубосферы (вершины в осях тела относительно центра чанка, см. native/src/core/cubesphere.hpp).
// Освещение считаем сами, в осях тела (км): Солнце, затмения другими телами, тень колец, атмосфера.
// CUSTOM0.xyz — направление из центра тела, NORMAL — нормаль эллипсоида (оба в осях тела).
// Карты — в общей раскладке (pipeline/build_textures.py): u = (долгота + 180°)/360°, v = (90° − широта)/180°.
// Единицы яркости: белая ламбертова поверхность, на которую Солнце светит отвесно, = 1.
shader_type spatial;
render_mode unshaded, cull_back, fog_disabled, skip_vertex_transform;

#include "res://shaders/compress.gdshaderinc"

uniform vec3 radii = vec3(1.0);            // полуоси тела, км
#include "res://shaders/atmosphere.gdshaderinc"

// ---- карты (задаёт C++ по game/data/appearance.json)
uniform bool has_map = false;
uniform bool map_linear = false;           // серые мозаики USGS: значения линейны (I/F), иначе sRGB
uniform bool map_grey = false;
uniform bool geodetic = false;             // широта карт геодезическая (Земля)
uniform sampler2D albedo_srgb : source_color, repeat_enable, filter_linear_mipmap_anisotropic;
uniform sampler2D albedo_lin : repeat_enable, filter_linear_mipmap_anisotropic;
uniform float albedo_scale = 1.0;          // приведение средней яркости карты к альбедо тела
uniform vec3 base_color : source_color = vec3(0.6);  // тела без карты
uniform bool has_normal = false;
uniform sampler2D normal_tex : hint_normal, repeat_enable, filter_linear_mipmap_anisotropic;
uniform bool has_night = false;
uniform sampler2D night_tex : source_color, repeat_enable, filter_linear_mipmap_anisotropic;
uniform float night_gain = 1.0;
uniform bool has_clouds = false;
uniform sampler2D clouds_tex : repeat_enable, filter_linear_mipmap_anisotropic;
uniform bool has_ocean = false;
uniform sampler2D ocean_tex : repeat_enable, filter_linear_mipmap_anisotropic;
uniform float ocean_slope_var = 0.0388;    // дисперсия наклонов волн (Cox & Munk 1954 при ветре 7 м/с)

// ---- освещение (каждый кадр)
uniform vec3 sun_dir = vec3(1.0, 0.0, 0.0);      // на Солнце, оси тела
uniform vec3 sun_pos = vec3(1.5e8, 0.0, 0.0);    // центр Солнца от центра тела, км, оси тела
uniform float sun_radius = 695700.0;
uniform vec3 cam_pos = vec3(0.0, 0.0, 1.0e5);    // камера от центра тела, км, оси тела
uniform vec4 occluders[4];                       // xyz — центр тела-заслонителя (км, оси тела), w — радиус
uniform int occluder_count = 0;
uniform bool ring_shadow = false;                // тень колец (Сатурн): кольца в плоскости экватора z = 0
uniform vec4 ring_edges = vec4(0.0);             // внутр. край C, граница C/B, внешн. край B, внутр. край A, км
uniform float ring_outer = 0.0;                  // внешний край A, км
uniform vec3 ring_tau = vec3(0.0);               // оптическая толщина колец C, B, A
uniform float ambient = 0.0;
uniform bool show_atmosphere = true;
uniform bool show_clouds = true;
uniform bool show_night = true;

// ---- режимы
uniform bool grid = false;       // сетка координат через 30°: экватор и нулевой меридиан ярче
uniform bool flat_mode = false;  // тесты: ровный цвет без освещения
uniform int debug_view = 0;      // тесты: 1 — Ламберт без карт и атмосферы (с затмениями), 2 — код u, 3 — код v

varying vec3 body_dir;
varying vec3 body_nrm;

void vertex() {
	body_dir = CUSTOM0.xyz;
	body_nrm = NORMAL;
	VERTEX = compress_view((MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz);
	NORMAL = normalize((MODELVIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

// Антиалиасная линия на значении v с шагом step (в тех же единицах), w — производная v на пиксель.
float grid_line(float v, float step, float w) {
	float x = v / step;
	float d = abs(fract(x + 0.5) - 0.5);
	return 1.0 - smoothstep(0.0, 1.5 * w / step, d);
}

vec3 srgb_to_linear(vec3 c) {
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// Выборка из карты с правильным мип-уровнем на шве ±180°: производную u берём у копии со швом на 0°.
vec4 sample_map(sampler2D t, vec2 uv, vec2 dx, vec2 dy) {
	return textureGrad(t, uv, dx, dy);
}

// Доля площади диска Солнца (угловой радиус a), закрытая диском радиуса b на угловом расстоянии d.
float disk_overlap(float a, float b, float d) {
	if (d >= a + b) return 0.0;
	if (d <= abs(a - b)) return min(1.0, (b * b) / (a * a));
	float a2 = a * a, b2 = b * b, d2 = d * d;
	float x = a2 * acos(clamp((d2 + a2 - b2) / (2.0 * d * a), -1.0, 1.0)) +
			  b2 * acos(clamp((d2 + b2 - a2) / (2.0 * d * b), -1.0, 1.0)) -
			  0.5 * sqrt(max((-d + a + b) * (d + a - b) * (d - a + b) * (d + a + b), 0.0));
	return clamp(x / (PI * a2), 0.0, 1.0);
}

// Доля света Солнца, дошедшая до точки p: затмения телами-заслонителями и тень колец.
float sun_visibility(vec3 p, vec3 s) {
	float vis = 1.0;
	vec3 vs = sun_pos - p;
	float ds = length(vs);
	float as = asin(clamp(sun_radius / ds, 0.0, 1.0));
	for (int k = 0; k < 4; k++) {
		if (k >= occluder_count) break;
		vec3 vo = occluders[k].xyz - p;
		float d_o = length(vo);
		if (d_o <= occluders[k].w || dot(vo, vs) <= 0.0) continue;
		float ao = asin(occluders[k].w / d_o);
		float sep = acos(clamp(dot(vs, vo) / (ds * d_o), -1.0, 1.0));
		vis *= 1.0 - disk_overlap(as, ao, sep);
	}
	if (ring_shadow && abs(s.z) > 1e-4) {
		float t = -p.z / s.z;
		if (t > 0.0) {
			float r = length((p + s * t).xy);
			float tau = 0.0;
			if (r >= ring_edges.x && r < ring_edges.y) tau = ring_tau.x;
			else if (r >= ring_edges.y && r < ring_edges.z) tau = ring_tau.y;
			else if (r >= ring_edges.w && r < ring_outer) tau = ring_tau.z;
			vis *= exp(-tau / abs(s.z));
		}
	}
	return vis;
}

// Блеск воды: микрограни с распределением Бекмана (дисперсия наклонов — Cox & Munk), Френель по Шлику (вода, F0 = 0,02).
float glint(vec3 n, vec3 l, vec3 v) {
	float nl = dot(n, l), nv = dot(n, v);
	if (nl <= 0.0 || nv <= 0.0) return 0.0;
	vec3 h = normalize(l + v);
	float nh = max(dot(n, h), 1e-4);
	float t2 = (1.0 - nh * nh) / (nh * nh);
	float D = exp(-t2 / ocean_slope_var) / (PI * ocean_slope_var * nh * nh * nh * nh);
	float F = 0.02 + 0.98 * pow(1.0 - max(dot(h, v), 0.0), 5.0);
	return F * D * PI / (4.0 * nv);  // яркость при освещённости π, уже умноженная на cos падения
}

void fragment() {
	vec3 d = normalize(body_dir);
	vec3 up = normalize(body_nrm);
	vec3 p = d * atm_surface_r(d);  // точка поверхности, км, оси тела
	float lon = atan(d.y, d.x);
	float lat = geodetic ? asin(clamp(up.z, -1.0, 1.0)) : asin(clamp(d.z, -1.0, 1.0));
	vec2 uv = vec2(lon / (2.0 * PI) + 0.5, 0.5 - lat / PI);
	vec2 dx = dFdx(uv), dy = dFdy(uv);
	float u2 = fract(uv.x + 0.5);
	vec2 dx2 = vec2(dFdx(u2), dx.y), dy2 = vec2(dFdy(u2), dy.y);
	if (abs(dx2.x) + abs(dy2.x) < abs(dx.x) + abs(dy.x)) {
		dx = dx2;
		dy = dy2;
	}

	if (flat_mode) {
		ALBEDO = base_color;
	} else if (debug_view == 2 || debug_view == 3) {
		// код координаты карты: старшие 8 бит в R, младшие в G; на экран попадает ровно это значение (sRGB)
		float x = debug_view == 2 ? fract(uv.x) : clamp(uv.y, 0.0, 1.0 - 1e-7);
		float hi = floor(x * 255.0) / 255.0;
		float lo = fract(x * 255.0);
		ALBEDO = srgb_to_linear(vec3(hi, lo, 0.0));
	} else {
		vec3 l = normalize(sun_dir);
		vec3 v = normalize(cam_pos - p);
		vec3 albedo = base_color;
		if (has_map && debug_view == 0) {
			if (map_linear) {
				albedo = sample_map(albedo_lin, uv, dx, dy).rgb;
			} else {
				albedo = sample_map(albedo_srgb, uv, dx, dy).rgb;
			}
			if (map_grey) albedo = albedo.rrr;
			albedo = min(albedo * albedo_scale, vec3(1.0));
		}
		if (debug_view == 1) albedo = vec3(1.0);
		// нормаль с рельефом: карта нормалей в местном базисе (восток, север, вверх)
		vec3 n = up;
		if (has_normal && debug_view == 0) {
			vec2 t = sample_map(normal_tex, uv, dx, dy).rg * 2.0 - 1.0;
			vec3 east = normalize(cross(vec3(0.0, 0.0, 1.0), up) + vec3(1e-9, 0.0, 0.0));
			vec3 north = cross(up, east);
			n = normalize(east * t.x + north * t.y + up * sqrt(max(1.0 - dot(t, t), 0.0)));
		}
		float horizon = smoothstep(-0.02, 0.02, dot(up, l));  // рельеф не светится за терминатором
		float vis = sun_visibility(p, l);  // затмения есть и в режиме «Ламберт» (тест тени Луны на Земле)
		vec3 c = albedo * max(dot(n, l), 0.0) * horizon * vis;
		if (debug_view == 0) {
			float cloud = 0.0;
			if (has_clouds && show_clouds) cloud = sample_map(clouds_tex, uv, dx, dy).r;
			if (has_ocean) {
				float water = sample_map(ocean_tex, uv, dx, dy).r;
				c += vec3(water * glint(up, l, v) * horizon * vis) * (1.0 - cloud);
			}
			if (has_clouds && show_clouds) c = mix(c, vec3(max(dot(up, l), 0.0) * vis), cloud);
			c += albedo * ambient;
			if (has_night && show_night) {
				float dark = 1.0 - smoothstep(-0.12, 0.02, dot(up, l));
				c += sample_map(night_tex, uv, dx, dy).rgb * night_gain * dark * (1.0 - cloud);
			}
			if (atm_on && show_atmosphere) {
				vec3 dv = p - cam_pos;
				float len = length(dv);
				vec3 ins, tr;
				atm_integrate(cam_pos, dv / len, 0.0, len, l, ins, tr);
				c = c * tr + ins;
			}
		}
		if (grid) {
			float glon = lon, glat = asin(clamp(d.z, -1.0, 1.0));
			float lon2 = atan(-d.y, -d.x);  // та же долгота со сдвигом на π: без скачка на ±180°
			float wl = min(fwidth(glon), fwidth(lon2));
			float step = radians(30.0);
			float g = max(grid_line(glat, step, fwidth(glat)), grid_line(glon, step, wl));
			float main = max(grid_line(glat, 2.0 * PI, fwidth(glat)), grid_line(glon, 2.0 * PI, wl));
			c = mix(c, vec3(0.9, 0.75, 0.3), 0.5 * g);
			c = mix(c, vec3(1.0, 0.35, 0.2), 0.7 * main);
		}
		ALBEDO = c;
	}
}
