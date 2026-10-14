// Однократное рассеяние света в атмосфере: Рэлей (молекулы) + аэрозоль (Хеньи — Гринстейн). Всё — в осях тела, км.
// Плотности падают экспонентой с высотой: ρ = exp(−h/H). Коэффициенты у поверхности, шкалы высот и g задаёт C++
// из game/data/appearance.json (Земля: Рэлей — Bodhaine et al. 1999 по давлению и температуре из NASA,
// аэрозоль — Kinne 2019 и Bruneton & Neyret 2008). Единицы яркости: белая ламбертова поверхность под Солнцем = 1,
// то есть освещённость от Солнца равна π.
// Перед включением должны быть объявлены uniform vec3 radii (полуоси тела, км).

uniform bool atm_on = false;
uniform vec3 atm_beta_r = vec3(0.0);  // рэлеевское рассеяние у поверхности по каналам R, G, B, 1/км
uniform float atm_h_r = 8.5;          // км
uniform float atm_beta_m_sca = 0.0;   // аэрозоль: рассеяние у поверхности, 1/км
uniform float atm_beta_m_ext = 0.0;   // аэрозоль: ослабление у поверхности, 1/км
uniform float atm_h_m = 1.2;          // км
uniform float atm_g = 0.7;            // асимметрия рассеяния аэрозоля
uniform float atm_top = 100.0;        // верх атмосферы над наибольшей полуосью, км

const int ATM_STEPS = 24;
const int ATM_SUN_STEPS = 6;

float atm_surface_r(vec3 u) {
	vec3 q = u / radii;
	return 1.0 / sqrt(dot(q, q));
}

float atm_altitude(vec3 x) {
	float r = length(x);
	return r - atm_surface_r(x / max(r, 1e-6));
}

float atm_r_top() {
	return max(radii.x, max(radii.y, radii.z)) + atm_top;
}

// Пересечение луча o + t·d (|d| = 1) со сферой радиуса R: (t входа, t выхода); промах — x > y.
vec2 atm_sphere(vec3 o, vec3 d, float R) {
	float b = dot(o, d);
	float c = dot(o, o) - R * R;
	float h = b * b - c;
	if (h < 0.0) return vec2(1e30, -1e30);
	h = sqrt(h);
	return vec2(-b - h, -b + h);
}

// Оптическая толщина от точки x до верха атмосферы в сторону Солнца s.
vec3 atm_tau_sun(vec3 x, vec3 s) {
	float len = atm_sphere(x, s, atm_r_top()).y;
	if (len <= 0.0) return vec3(0.0);
	float ds = len / float(ATM_SUN_STEPS);
	float dr = 0.0, dm = 0.0;
	for (int i = 0; i < ATM_SUN_STEPS; i++) {
		float h = max(atm_altitude(x + s * (ds * (float(i) + 0.5))), 0.0);
		dr += exp(-h / atm_h_r);
		dm += exp(-h / atm_h_m);
	}
	return (atm_beta_r * dr + vec3(atm_beta_m_ext * dm)) * ds;
}

// Тело заслоняет Солнце от точки x (сфера среднего радиуса).
bool atm_in_shadow(vec3 x, vec3 s) {
	float R = (radii.x + radii.y + radii.z) / 3.0;
	float b = dot(x, s);
	return b < 0.0 && dot(x, x) - b * b < R * R;
}

// Свет, рассеянный на отрезке луча o + t·d, t ∈ [t0, t1], и пропускание отрезка. s — направление на Солнце.
void atm_integrate(vec3 o, vec3 d, float t0, float t1, vec3 s, out vec3 inscatter, out vec3 trans) {
	inscatter = vec3(0.0);
	trans = vec3(1.0);
	vec2 top = atm_sphere(o, d, atm_r_top());
	t0 = max(t0, top.x);
	t1 = min(t1, top.y);
	if (!atm_on || t1 <= t0) return;
	float mu = dot(d, s);
	float pr = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
	float g = atm_g;
	float pm = (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * mu, 1e-6), 1.5));
	float ds = (t1 - t0) / float(ATM_STEPS);
	for (int i = 0; i < ATM_STEPS; i++) {
		vec3 x = o + d * (t0 + ds * (float(i) + 0.5));
		float h = max(atm_altitude(x), 0.0);
		float rr = exp(-h / atm_h_r), rm = exp(-h / atm_h_m);
		vec3 ext = (atm_beta_r * rr + vec3(atm_beta_m_ext * rm)) * ds;
		vec3 t_half = trans * exp(-0.5 * ext);
		if (!atm_in_shadow(x, s)) {
			vec3 t_sun = exp(-atm_tau_sun(x, s));
			inscatter += t_half * t_sun * (atm_beta_r * rr * pr + vec3(atm_beta_m_sca * rm * pm)) * ds;
		}
		trans *= exp(-ext);
	}
	inscatter *= PI;
}
