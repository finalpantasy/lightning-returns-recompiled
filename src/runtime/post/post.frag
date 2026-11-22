#version 450
// vk_post.c's pass over the final picture. Mode bits (pc.p.x):
//   1 resample: Lanczos-2 from the source rectangle (its footprint widened when shrinking: a real downscale, no aliasing)
//   2 sharpen:  contrast-adaptive sharpening (the method of AMD FidelityFX CAS, MIT) on the source at 1:1
//   4 deband:   flat areas replaced by the mean of four far samples (when all are within a threshold)
//   8 dither:   triangular noise of one 8-bit step, so the 8-bit write does not band
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
	vec4 rect; // source position of the target's (0,0) corner; source pixels per target pixel (x, y)
	vec4 lim;  // the source rectangle: x0, y0, x1, y1 (pixels; reads are clamped into it)
	vec4 p;    // mode, sharpness 0..1, deband threshold (0..1 units), deband radius (pixels)
	vec4 q;    // frame seed, dither amplitude (8-bit steps), 0, 0
} pc;
layout(location = 0) out vec4 o;

vec3 fetch(ivec2 c) {
	c = clamp(c, ivec2(pc.lim.xy), ivec2(pc.lim.zw) - 1);
	return texelFetch(src, c, 0).rgb;
}
float lz(float x) {
	x = abs(x);
	if (x < 1e-4) return 1.0;
	if (x >= 2.0) return 0.0;
	float a = 3.14159265 * x;
	return 2.0 * sin(a) * sin(a * 0.5) / (a * a);
}
#define MAXT 24
vec3 resample(vec2 fc) {
	vec2 s = pc.rect.zw;
	vec2 p = pc.rect.xy + fc * s; // (fc: the pixel centre, so p is in the same pixel-centre space)
	vec2 k = max(s, vec2(1.0));
	vec2 r = 2.0 * k;
	ivec2 a = ivec2(ceil(p - r - 0.5)), b = ivec2(floor(p + r - 0.5));
	int nx = min(b.x - a.x + 1, MAXT), ny = min(b.y - a.y + 1, MAXT);
	float wx[MAXT];
	float sx = 0.0, sy = 0.0;
	for (int i = 0; i < nx; i++) { wx[i] = lz((float(a.x + i) + 0.5 - p.x) / k.x); sx += wx[i]; }
	vec3 acc = vec3(0.0);
	for (int j = 0; j < ny; j++) {
		float wy = lz((float(a.y + j) + 0.5 - p.y) / k.y);
		if (wy == 0.0) continue;
		vec3 row = vec3(0.0);
		for (int i = 0; i < nx; i++) row += wx[i] * fetch(ivec2(a.x + i, a.y + j));
		acc += wy * row;
		sy += wy;
	}
	vec3 c = acc / (sx * sy);
	if (s.x < 1.0 || s.y < 1.0) { // (enlarging: no ringing past the four nearest source pixels)
		ivec2 n = ivec2(floor(p - 0.5));
		vec3 t0 = fetch(n), t1 = fetch(n + ivec2(1, 0)), t2 = fetch(n + ivec2(0, 1)), t3 = fetch(n + ivec2(1, 1));
		c = clamp(c, min(min(t0, t1), min(t2, t3)), max(max(t0, t1), max(t2, t3)));
	}
	return clamp(c, 0.0, 1.0);
}
vec3 sharpen(ivec2 c, float sharp) {
	vec3 a = fetch(c + ivec2(-1, -1)), b = fetch(c + ivec2(0, -1)), d0 = fetch(c + ivec2(1, -1));
	vec3 d = fetch(c + ivec2(-1, 0)), e = fetch(c), f = fetch(c + ivec2(1, 0));
	vec3 g = fetch(c + ivec2(-1, 1)), h = fetch(c + ivec2(0, 1)), i = fetch(c + ivec2(1, 1));
	vec3 mnc = min(min(min(b, d), min(f, h)), e), mxc = max(max(max(b, d), max(f, h)), e);
	vec3 mn = mnc + min(mnc, min(min(a, d0), min(g, i)));
	vec3 mx = mxc + max(mxc, max(max(a, d0), max(g, i)));
	vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0));
	vec3 w = amp * (-1.0 / mix(8.0, 5.0, sharp));
	return clamp(((b + d + f + h) * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
}
uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
float rnd(inout uint s) { s = hash(s); return float(s >> 8) * (1.0 / 16777216.0); }

void main() {
	int mode = int(pc.p.x);
	vec2 fc = gl_FragCoord.xy;
	ivec2 ic = ivec2(pc.rect.xy + floor(fc)); // (1:1 modes: the source pixel under this one)
	uint seed = hash(uint(fc.x) * 1973u + uint(fc.y) * 9277u + uint(pc.q.x) * 26699u);
	vec3 c;
	if ((mode & 1) != 0) c = resample(fc);
	else if ((mode & 2) != 0) c = sharpen(ic, pc.p.y);
	else c = fetch(ic);
	if ((mode & 4) != 0) {
		vec3 e = (mode & 2) != 0 ? fetch(ic) : c;
		float ang = 6.2831853 * rnd(seed), rad = pc.p.w * (0.35 + 0.65 * rnd(seed));
		ivec2 v = ivec2(round(vec2(cos(ang), sin(ang)) * rad)), u = ivec2(-v.y, v.x);
		vec3 s0 = fetch(ic + v), s1 = fetch(ic - v), s2 = fetch(ic + u), s3 = fetch(ic - u);
		vec3 m = (s0 + s1 + s2 + s3) * 0.25;
		vec3 dm = max(max(abs(s0 - e), abs(s1 - e)), max(abs(s2 - e), abs(s3 - e)));
		if (max(max(dm.r, dm.g), dm.b) < pc.p.z) c = m;
	}
	if ((mode & 8) != 0) c += (rnd(seed) + rnd(seed) - 1.0) * (pc.q.y / 255.0) * clamp(min(c, 1.0 - c) * 255.0, 0.0, 1.0); // (none at black / white: no lift)
	o = vec4(c, 1.0);
}
