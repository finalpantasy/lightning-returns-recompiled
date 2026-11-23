/* ovr_simd.c - "fast" overrides (overrides.tsv kind fast): hand-written versions of hot vector-math functions of the
 * game. The originals are unoptimized compiler output that stores every intermediate __m128 to its stack frame and
 * reads it back (~100 instructions for a matrix * vector); these compute the same values with the same operations in
 * the same order (so bit-identical, NaNs included), and write the same results. The translator calls xo_<addr>
 * directly; the dispatcher maps the address to it too (xrc_manual_simd). The stack below the caller's esp (the
 * original's dead locals) is not written.
 *
 * XRC_FAST_CHECK=1: each call also runs the translation and compares what it wrote with ours (a mismatch is
 * reported, the first few in detail). XRC_NO_FAST=1: the translations only (to compare speed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"
#include "guest_types.h"

static int fast_mode = -1; /* 0 ours, 1 ours checked against the translation, 2 the translation */
static int mode(void) {
	if (fast_mode < 0) fast_mode = getenv("XRC_NO_FAST") ? 2 : xrc_tool_env("XRC_FAST_CHECK") ? 1 : 0;
	return fast_mode;
}
static unsigned long long n_checked, n_bad;
/* per function: the mismatches and the largest difference of a float lane in ulps (its bits as an ordered integer) */
static struct { u32 addr; const char *what; unsigned long long bad; u32 ulp; } tally[32];
static int ntally;
static s32 ord_(u32 u) { return (u & 0x80000000u) ? (s32)(0x80000000u - (u & 0x7fffffffu)) : (s32)u; }
static void tally_bad(const char *what, u32 addr, const void *ours, u32 at, unsigned n) {
	int i;
	unsigned k;
	for (i = 0; i < ntally && tally[i].addr != addr; i++) {}
	if (i == ntally) { if (ntally == 32) return; tally[i].addr = addr; tally[i].what = what; ntally++; }
	tally[i].bad++;
	for (k = 0; k + 4 <= n; k += 4) {
		long long d = (long long)ord_(*(const u32 *)((const u8 *)ours + k)) - ord_(M32(at + k));
		u32 a = (u32)(d < 0 ? (d < -0x7fffffffll ? 0x7fffffff : -d) : (d > 0x7fffffff ? 0x7fffffff : d));
		if (a > tally[i].ulp) tally[i].ulp = a;
	}
}
static void tally_print(void) {
	int i;
	for (i = 0; i < ntally; i++) fprintf(stderr, "xrc:   %08x %s: %llu mismatches, up to %u ulp\n", tally[i].addr, tally[i].what, tally[i].bad, tally[i].ulp);
}
static void check(const char *what, u32 addr, const void *ours, u32 at, unsigned n) {
	n_checked++;
	if (memcmp(ours, xrc_mem + at, n)) {
		tally_bad(what, addr, ours, at, n);
		if (++n_bad <= 10) {
			unsigned k;
			fprintf(stderr, "xrc: FAST MISMATCH %08x (%s) at %08x: ours", addr, what, at);
			for (k = 0; k < n; k += 4) fprintf(stderr, " %08x", *(const u32 *)((const u8 *)ours + k));
			fprintf(stderr, ", original");
			for (k = 0; k < n; k += 4) fprintf(stderr, " %08x", M32(at + k));
			fprintf(stderr, "\n");
		}
	}
	if ((n_checked & 0xffffff) == 0) { fprintf(stderr, "xrc: fast check: %llu calls checked, %llu mismatches\n", n_checked, n_bad); tally_print(); }
}
#define ORIG(addr_) static XFn orig; if (!orig) orig = xrc_translation_of(addr_)
/* x86's own mulps / addps / minps / maxps, operands as the original has them. With -ffast-math (its finite-math-only)
 * the compiler may swap the operands of _mm_max_ps & co: on x86 a NaN makes maxps / minps return the second operand,
 * and of two NaNs mulps / addps return the first -- swapped, the result differs. Aabb::Extended came out
 * max(p, this.max) for the original's max(this.max, p) (906 mismatches in a battle, Matrix44::Mul 12). */
#if defined(XRC_VEC) && defined(__x86_64__) && defined(__AVX__)
#define EXOP(name_, ins_) static inline X128 name_(X128 a, X128 b) { X128 r; __asm__(ins_ " %2, %1, %0" : "=x"(r.v) : "x"(a.v), "x"(b.v)); return r; }
EXOP(ex_mulps, "vmulps") EXOP(ex_addps, "vaddps") EXOP(ex_minps, "vminps") EXOP(ex_maxps, "vmaxps")
#elif defined(XRC_VEC) && defined(__x86_64__)
#define EXOP(name_, ins_) static inline X128 name_(X128 a, X128 b) { __asm__(ins_ " %1, %0" : "+x"(a.v) : "x"(b.v)); return a; }
EXOP(ex_mulps, "mulps") EXOP(ex_addps, "addps") EXOP(ex_minps, "minps") EXOP(ex_maxps, "maxps")
#else
#define ex_mulps xv_mulps
#define ex_addps xv_addps
#define ex_minps xv_minps
#define ex_maxps xv_maxps
#endif
/* (Matrix44::Transform / Mul, Aabb::Extended and Quat::FromMatrix33 were off for a while: XRC_FAST_CHECK found them
 * off the translation -- the operand swaps above, and FromMatrix33 by 1-2 ulp: fast math had made the translation's
 * exact 1 / sqrt for rsqrtps an rsqrtps estimate, differently inlined in each (-mrecip=none since, tools/xrc_build.sh)
 * -- and with them on a character flickered out of sight in Chapter 1. Since: 0 mismatches in 470M checked calls.) */

/* 004559b0: thiscall Vec4 *Matrix44::Transform(Vec4 *out, const Vec4 *v) -- out = v.x * row0 + v.y * row1 +
 * v.z * row2 + v.w * row3 (this = 4 rows of 4 floats); ((row0 * x + row1 * y) + row2 * z) + row3 * w; returns out */
void XRC_CC xo_004559b0(XCtx *c) {
	u32 m = c->ecx, out = arg_u32(c, 1), v = arg_u32(c, 2);
	X128 vv, r0, r1, r2, r3, acc;
	int md = mode();
	ORIG(0x004559b0u);
	if (md == 2) { orig(c); return; }
	XLD(vv, v);
	XLD(r0, m); XLD(r1, m + 16); XLD(r2, m + 32); XLD(r3, m + 48);
	acc = ex_mulps(r0, XV_SHUFPS(vv, vv, 0x00));
	acc = ex_addps(acc, ex_mulps(r1, XV_SHUFPS(vv, vv, 0x55)));
	acc = ex_addps(acc, ex_mulps(r2, XV_SHUFPS(vv, vv, 0xaa)));
	acc = ex_addps(acc, ex_mulps(r3, XV_SHUFPS(vv, vv, 0xff)));
	if (md == 1) { orig(c); check("Matrix44::Transform", 0x004559b0u, &acc, out, 16); return; }
	XST(out, acc);
	c->eax = out;
	xrc_stdcall_ret(c, 2);
}

/* 00455840: thiscall Vec4 *Vec4::operator=(const Vec4 *v) -- 4 floats copied through the x87 (fld / fstp: a
 * signaling NaN comes out quiet); returns this */
static u32 x87_copy(u32 u) { return (u & 0x7f800000u) == 0x7f800000u && (u & 0x7fffffu) && !(u & 0x400000u) ? u | 0x400000u : u; }
void XRC_CC xo_00455840(XCtx *c) {
	u32 t = c->ecx, v = arg_u32(c, 1), r[4];
	int md = mode(), k;
	ORIG(0x00455840u);
	if (md == 2) { orig(c); return; }
	if (md == 1) {
		for (k = 0; k < 4; k++) r[k] = x87_copy(M32(v + 4 * k));
		orig(c); check("Vec4::operator=", 0x00455840u, r, t, 16); return;
	}
	for (k = 0; k < 4; k++) M32(t + 4 * k) = x87_copy(M32(v + 4 * k)); /* (each read just before its store, as the original) */
	c->eax = t;
	xrc_stdcall_ret(c, 1);
}

/* 00455880: thiscall Vec4 *Vec4::Get(Vec4 *out) -- out = this (16 bytes); returns out */
void XRC_CC xo_00455880(XCtx *c) {
	u32 t = c->ecx, out = arg_u32(c, 1);
	X128 x;
	int md = mode();
	ORIG(0x00455880u);
	if (md == 2) { orig(c); return; }
	XLD(x, t);
	if (md == 1) { orig(c); check("Vec4::Get", 0x00455880u, &x, out, 16); return; }
	XST(out, x);
	c->eax = out;
	xrc_stdcall_ret(c, 1);
}

/* 004586c0: thiscall Aabb *Aabb::Extended(Aabb *out, const Vec4 *p) -- out.min = minps(this.min, p), out.max =
 * maxps(this.max, p) (this.min at +0, max at +16); returns out */
void XRC_CC xo_004586c0(XCtx *c) {
	u32 t = c->ecx, out = arg_u32(c, 1), p = arg_u32(c, 2);
	X128 pv, r[2], a;
	int md = mode();
	ORIG(0x004586c0u);
	if (md == 2) { orig(c); return; }
	XLD(pv, p); XLD(a, t + 16); r[1] = ex_maxps(a, pv);
	XLD(a, t); r[0] = ex_minps(a, pv);
	if (md == 1) { orig(c); check("Aabb::Extended", 0x004586c0u, r, out, 32); return; }
	XST(out, r[0]); XST(out + 16, r[1]);
	c->eax = out;
	xrc_stdcall_ret(c, 2);
}

/* 00710860: thiscall Vec4 *Vec4::Dot4(Vec4 *out, const Vec4 *v) -- p = this * v; out = ((p + p.yyyy) + p.zzzz) +
 * p.wwww (lane 0: the dot product); returns out */
void XRC_CC xo_00710860(XCtx *c) {
	u32 t = c->ecx, out = arg_u32(c, 1), v = arg_u32(c, 2);
	X128 a, b, pr, acc;
	int md = mode();
	ORIG(0x00710860u);
	if (md == 2) { orig(c); return; }
	XLD(b, v); XLD(a, t);
	pr = xv_mulps(a, b);
	acc = xv_addps(pr, XV_SHUFPS(pr, pr, 0x55));
	acc = xv_addps(acc, XV_SHUFPS(pr, pr, 0xaa));
	acc = xv_addps(acc, XV_SHUFPS(pr, pr, 0xff));
	if (md == 1) { orig(c); check("Vec4::Dot4", 0x00710860u, &acc, out, 16); return; }
	XST(out, acc);
	c->eax = out;
	xrc_stdcall_ret(c, 2);
}

/* 00711f40: thiscall Vec4 *Matrix33::Transform(Vec4 *out, const Vec4 *v) -- out = (row0 * v.x + row1 * v.y) +
 * row2 * v.z (this = 3 rows of 4 floats); returns out */
void XRC_CC xo_00711f40(XCtx *c) {
	u32 m = c->ecx, out = arg_u32(c, 1), v = arg_u32(c, 2);
	X128 vv, r0, r1, r2, acc;
	int md = mode();
	ORIG(0x00711f40u);
	if (md == 2) { orig(c); return; }
	XLD(vv, v); XLD(r0, m); XLD(r1, m + 16); XLD(r2, m + 32);
	acc = xv_mulps(r0, XV_SHUFPS(vv, vv, 0x00));
	acc = xv_addps(acc, xv_mulps(r1, XV_SHUFPS(vv, vv, 0x55)));
	acc = xv_addps(acc, xv_mulps(r2, XV_SHUFPS(vv, vv, 0xaa)));
	if (md == 1) { orig(c); check("Matrix33::Transform", 0x00711f40u, &acc, out, 16); return; }
	XST(out, acc);
	c->eax = out;
	xrc_stdcall_ret(c, 2);
}

/* 00455f00: thiscall Vec4 *Matrix44::Transform(Vec4 *out, const __m128 *v) -- as 004559b0 (v read with movaps) */
void XRC_CC xo_00455f00(XCtx *c) {
	u32 m = c->ecx, out = arg_u32(c, 1), v = arg_u32(c, 2);
	X128 vv, r0, r1, r2, r3, acc;
	int md = mode();
	ORIG(0x00455f00u);
	if (md == 2) { orig(c); return; }
	XLD(vv, v);
	XLD(r0, m); XLD(r1, m + 16); XLD(r2, m + 32); XLD(r3, m + 48);
	acc = ex_mulps(r0, XV_SHUFPS(vv, vv, 0x00));
	acc = ex_addps(acc, ex_mulps(r1, XV_SHUFPS(vv, vv, 0x55)));
	acc = ex_addps(acc, ex_mulps(r2, XV_SHUFPS(vv, vv, 0xaa)));
	acc = ex_addps(acc, ex_mulps(r3, XV_SHUFPS(vv, vv, 0xff)));
	if (md == 1) { orig(c); check("Matrix44::Transform(m128)", 0x00455f00u, &acc, out, 16); return; }
	XST(out, acc);
	c->eax = out;
	xrc_stdcall_ret(c, 2);
}

/* 004584c0: thiscall Vec4 *Matrix44::TransformPoint(Vec4 *out, const __m128 *v) -- ((row0 * v.x + row1 * v.y) +
 * row2 * v.z) + row3; returns out */
void XRC_CC xo_004584c0(XCtx *c) {
	u32 m = c->ecx, out = arg_u32(c, 1), v = arg_u32(c, 2);
	X128 vv, r0, r1, r2, r3, acc;
	int md = mode();
	ORIG(0x004584c0u);
	if (md == 2) { orig(c); return; }
	XLD(vv, v);
	XLD(r0, m); XLD(r1, m + 16); XLD(r2, m + 32); XLD(r3, m + 48);
	acc = xv_mulps(r0, XV_SHUFPS(vv, vv, 0x00));
	acc = xv_addps(acc, xv_mulps(r1, XV_SHUFPS(vv, vv, 0x55)));
	acc = xv_addps(acc, xv_mulps(r2, XV_SHUFPS(vv, vv, 0xaa)));
	acc = xv_addps(acc, r3);
	if (md == 1) { orig(c); check("Matrix44::TransformPoint", 0x004584c0u, &acc, out, 16); return; }
	XST(out, acc);
	c->eax = out;
	xrc_stdcall_ret(c, 2);
}

/* 00455d80: thiscall Matrix44 *Matrix44::Get(Matrix44 *out) -- out = this (all 64 bytes read before any is
 * written); returns out */
void XRC_CC xo_00455d80(XCtx *c) {
	u32 t = c->ecx, out = arg_u32(c, 1);
	X128 r[4];
	int md = mode(), k;
	ORIG(0x00455d80u);
	if (md == 2) { orig(c); return; }
	for (k = 0; k < 4; k++) XLD(r[k], t + 16 * k);
	if (md == 1) { orig(c); check("Matrix44::Get", 0x00455d80u, r, out, 64); return; }
	for (k = 0; k < 4; k++) XST(out + 16 * k, r[k]);
	c->eax = out;
	xrc_stdcall_ret(c, 1);
}

/* 00462f80: thiscall Matrix44 *Matrix44::operator=(const Matrix44 *m) -- row by row (each read, then written);
 * returns this */
void XRC_CC xo_00462f80(XCtx *c) {
	u32 t = c->ecx, m = arg_u32(c, 1);
	X128 r[4];
	int md = mode(), k;
	ORIG(0x00462f80u);
	if (md == 2) { orig(c); return; }
	if (md == 1) {
		u8 sim[64];
		/* the result of the row-by-row copy, overlap included */
		memcpy(sim, xrc_mem + t, 64);
		for (k = 0; k < 4; k++) {
			X128 x; u32 sa = m + 16 * k; unsigned j;
			for (j = 0; j < 16; j++) {
				u32 b = sa + j; ((u8 *)&x)[j] = b - t < 64 ? sim[b - t] : M8(b);
			}
			memcpy(sim + 16 * k, &x, 16);
		}
		(void)r;
		orig(c); check("Matrix44::operator=", 0x00462f80u, sim, t, 64); return;
	}
	for (k = 0; k < 4; k++) { XLD(r[k], m + 16 * k); XST(t + 16 * k, r[k]); }
	c->eax = t;
	xrc_stdcall_ret(c, 1);
}

/* 00457e50: thiscall Aabb *Aabb::Transformed(Aabb *out, const Matrix44 *m) -- the box of the 8 transformed corners:
 * d = max - min; A = row0 * d.x, B = row1 * d.y, C = row2 * d.z; P = m->TransformPoint(min) (004584c0); a box at P
 * extended (004586c0: minps / maxps) by P+A, P+B, (P+A)+B, P+C, (P+A)+C, (P+B)+C, ((P+A)+B)+C in that order (the
 * sums by 00458460, this + v); returns out. The original's temporaries are below the caller's esp (not written). */
static void ext(X128 *b, X128 p) { b[0] = xv_minps(b[0], p); b[1] = xv_maxps(b[1], p); }
void XRC_CC xo_00457e50(XCtx *c) {
	u32 t = c->ecx, out = arg_u32(c, 1), m = arg_u32(c, 2);
	X128 mn, mx, d, r0, r1, r2, r3, A, B, C, P, PA, PB, PAB, b[2];
	int md = mode();
	ORIG(0x00457e50u);
	if (md == 2) { orig(c); return; }
	XLD(mn, t); XLD(mx, t + 16);
	XLD(r0, m); XLD(r1, m + 16); XLD(r2, m + 32); XLD(r3, m + 48);
	d = xv_subps(mx, mn);
	A = xv_mulps(r0, XV_SHUFPS(d, d, 0x00));
	B = xv_mulps(r1, XV_SHUFPS(d, d, 0x55));
	C = xv_mulps(r2, XV_SHUFPS(d, d, 0xaa));
	P = xv_mulps(r0, XV_SHUFPS(mn, mn, 0x00)); /* (TransformPoint, as xo_004584c0) */
	P = xv_addps(P, xv_mulps(r1, XV_SHUFPS(mn, mn, 0x55)));
	P = xv_addps(P, xv_mulps(r2, XV_SHUFPS(mn, mn, 0xaa)));
	P = xv_addps(P, r3);
	PA = xv_addps(P, A); PB = xv_addps(P, B); PAB = xv_addps(PA, B);
	b[0] = P; b[1] = P;
	ext(b, PA); ext(b, PB); ext(b, PAB); ext(b, xv_addps(P, C));
	ext(b, xv_addps(PA, C)); ext(b, xv_addps(PB, C)); ext(b, xv_addps(PAB, C));
	if (md == 1) { orig(c); check("Aabb::Transformed", 0x00457e50u, b, out, 32); return; }
	XST(out, b[0]); XST(out + 16, b[1]);
	c->eax = out;
	xrc_stdcall_ret(c, 2);
}

/* 00456970: thiscall Matrix44 *Matrix44::Mul(Matrix44 *out, const Matrix44 *n) -- out row i = this->Transform(n row i)
 * (004559b0), rows 3, 2, 1, 0 computed into temporaries before any is written (out may be this or n); returns out */
static X128 m44_transform(const X128 *r, X128 v) {
	X128 acc = ex_mulps(r[0], XV_SHUFPS(v, v, 0x00));
	acc = ex_addps(acc, ex_mulps(r[1], XV_SHUFPS(v, v, 0x55)));
	acc = ex_addps(acc, ex_mulps(r[2], XV_SHUFPS(v, v, 0xaa)));
	return ex_addps(acc, ex_mulps(r[3], XV_SHUFPS(v, v, 0xff)));
}
void XRC_CC xo_00456970(XCtx *c) {
	u32 t = c->ecx, out = arg_u32(c, 1), n = arg_u32(c, 2);
	X128 r[4], v, o[4];
	int md = mode(), k;
	ORIG(0x00456970u);
	if (md == 2) { orig(c); return; }
	XLD(r[0], t); XLD(r[1], t + 16); XLD(r[2], t + 32); XLD(r[3], t + 48);
	for (k = 3; k >= 0; k--) { XLD(v, n + 16u * (u32)k); o[k] = m44_transform(r, v); }
	if (md == 1) { orig(c); check("Matrix44::Mul", 0x00456970u, o, out, 64); return; }
	for (k = 0; k < 4; k++) XST(out + 16u * (u32)k, o[k]);
	c->eax = out;
	xrc_stdcall_ret(c, 2);
}

/* 00743c70: thiscall Quat *Quat::Mul(Quat *out, const Quat *b) -- a = this; r = ((Bw * a + Aw * b) + a.yzxw * b.zxyw)
 * - a.zxyw * b.yzxw (xyz: the cross-product form); w from s2 = (a * b - a.wxyz * b.wxyz) - (a.wxyz * b.wxyz +
 * a * b).zwxy; out = (r.x, r.y, r.z, s2.w); returns out */
void XRC_CC xo_00743c70(XCtx *c) {
	u32 t = c->ecx, out = arg_u32(c, 1), bp = arg_u32(c, 2);
	X128 a, b, r, t5, t6, s, u, h;
	int md = mode();
	ORIG(0x00743c70u);
	if (md == 2) { orig(c); return; }
	XLD(a, t); XLD(b, bp);
	r = xv_addps(xv_mulps(XV_SHUFPS(b, b, 0xff), a), xv_mulps(XV_SHUFPS(a, a, 0xff), b));
	r = xv_addps(xv_mulps(XV_SHUFPS(a, a, 0xc9), XV_SHUFPS(b, b, 0xd2)), r);
	r = xv_subps(r, xv_mulps(XV_SHUFPS(a, a, 0xd2), XV_SHUFPS(b, b, 0xc9)));
	t5 = xv_mulps(a, b);
	t6 = xv_mulps(XV_SHUFPS(a, a, 0x93), XV_SHUFPS(b, b, 0x93));
	s = xv_subps(t5, t6);
	u = xv_addps(t6, t5);
	s = xv_subps(s, XV_SHUFPS(u, u, 0x4e));
	h = xv_unpckhps(r, s);
	r = XV_SHUFPS(r, h, 0xc4);
	if (md == 1) { orig(c); check("Quat::Mul", 0x00743c70u, &r, out, 16); return; }
	XST(out, r);
	c->eax = out;
	xrc_stdcall_ret(c, 2);
}

/* 00744160: cdecl Matrix44 *QuatToMatrix(Matrix44 *out, const Quat *q) -- Q2 = q + q; t = (Q2.yzxw * q.wwww +
 * q.zxyw * Q2) ; a = (1 - q.yzxw * Q2.yzxw) - q.zxyw * Q2.zxyw ; b = q.yzxw * Q2 - Q2.zxyw * q.wwww; the rows mixed
 * from them (movss / unpckhps / shufps 0x94), w zeroed (00637d40), stored in reverse order; row 3 = (0, 0, 0, 1)
 * (0109f810); returns out (cdecl: no arguments popped) */
static X128 w0(X128 v) { X128 z; z.u[0] = z.u[1] = z.u[2] = z.u[3] = 0; return XV_SHUFPS(v, xv_unpckhps(v, z), 0xc4); }
static X128 movss_(X128 a, X128 b) { a.u[0] = b.u[0]; return a; }
/* the rows of 00744160 / 00517130 (before their w lanes are set) */
static void quat_rows(u32 qp, X128 *r) {
	X128 q, q2, qw, qc9, qd2, q2c9, q2d2, one, t, a, b, m180, m150, m120;
	float onef = 1.0f;
	XLD(q, qp);
	q2 = xv_addps(q, q);
	qw = XV_SHUFPS(q, q, 0xff); qc9 = XV_SHUFPS(q, q, 0xc9); qd2 = XV_SHUFPS(q, q, 0xd2);
	q2c9 = XV_SHUFPS(q2, q2, 0xc9); q2d2 = XV_SHUFPS(q2, q2, 0xd2);
	one.f[0] = one.f[1] = one.f[2] = one.f[3] = onef;
	t = xv_mulps(q2c9, qw);
	a = xv_subps(one, xv_mulps(qc9, q2c9));
	b = xv_mulps(qc9, q2);
	t = xv_addps(xv_mulps(qd2, q2), t);
	a = xv_subps(a, xv_mulps(qd2, q2d2));
	b = xv_subps(b, xv_mulps(q2d2, qw));
	m180 = movss_(t, a); m150 = movss_(a, b); m120 = movss_(b, t);
	r[0] = XV_SHUFPS(m180, xv_unpckhps(m180, b), 0x94);
	r[1] = XV_SHUFPS(m150, xv_unpckhps(m150, t), 0x94);
	r[2] = XV_SHUFPS(m120, xv_unpckhps(m120, a), 0x94);
}
void XRC_CC xo_00744160(XCtx *c) {
	u32 out = arg_u32(c, 1), qp = arg_u32(c, 2);
	X128 r[4];
	int md = mode(), k;
	ORIG(0x00744160u);
	if (md == 2) { orig(c); return; }
	quat_rows(qp, r);
	for (k = 0; k < 3; k++) r[k] = w0(r[k]);
	XLD(r[3], 0x0109f810u);
	if (md == 1) { orig(c); check("QuatToMatrix", 0x00744160u, r, out, 64); return; }
	XST(out, r[0]); XST(out + 16, r[1]); XST(out + 32, r[2]); XST(out + 48, r[3]);
	c->eax = out;
	xrc_cdecl_ret(c);
}

/* 00517130: cdecl Matrix44 *QuatToMatrix2(Matrix44 *out, const Quat *q) -- 00744160's rows with w = the .rdata
 * float at 025a3c20 (lanes copied one by one: exact), row 3 = (0, 0, 0, 1) through Vec4::operator= (00455840: the
 * x87 copy, the constant has no NaN); returns out */
void XRC_CC xo_00517130(XCtx *c) {
	u32 out = arg_u32(c, 1), qp = arg_u32(c, 2), w = M32(0x025a3c20u);
	X128 r[4];
	int md = mode(), k;
	ORIG(0x00517130u);
	if (md == 2) { orig(c); return; }
	quat_rows(qp, r);
	for (k = 0; k < 3; k++) r[k].u[3] = w;
	XLD(r[3], 0x0109f810u);
	if (md == 1) { orig(c); check("QuatToMatrix2", 0x00517130u, r, out, 64); return; }
	XST(out, r[0]); XST(out + 16, r[1]); XST(out + 32, r[2]); XST(out + 48, r[3]);
	c->eax = out;
	xrc_cdecl_ret(c);
}

/* 00515c00: thiscall Quat *Quat::FromMatrix33(const Matrix33 *m) -- the branch-free (select by compares) matrix to
 * quaternion of the game's math library: the four candidate quaternions (by the largest diagonal term), each scaled
 * by 0.5 * rsqrt'(1 + trace-like sums) (005148b0: rsqrtps and one Newton step), chosen with cmpltps masks; stored
 * into this; returns this (ret 4). Transcribed operation by operation from the original (same order). */
static X128 rsqrt_nr(X128 x) { /* 005148b0 */
	X128 half, one, y, a, b;
	half.f[0] = half.f[1] = half.f[2] = half.f[3] = 0.5f;
	one.f[0] = one.f[1] = one.f[2] = one.f[3] = 1.0f;
	y = xv_rsqrtps(x, x);
	a = xv_mulps(y, x);
	b = xv_mulps(y, half);
	return xv_addps(xv_mulps(xv_subps(one, xv_mulps(y, a)), b), y);
}
static X128 sel(X128 m, X128 t, X128 f) { return xv_orps(xv_andnps(m, f), xv_andps(m, t)); }
void XRC_CC xo_00515c00(XCtx *c) {
	u32 t = c->ecx, mp = arg_u32(c, 1);
	X128 r0, r1, r2, v25, v26, v27, v29, v31, v36, v37, half, one, zero, v41, v44, v45, v46, v49, v51, v53, v54, v55, v57, v59, v61,
	     v63, v65, v67, v69, v70, v71, v72, m, q;
	int md = mode();
	ORIG(0x00515c00u);
	if (md == 2) { orig(c); return; }
	half.f[0] = half.f[1] = half.f[2] = half.f[3] = 0.5f;
	one.f[0] = one.f[1] = one.f[2] = one.f[3] = 1.0f;
	zero.u[0] = zero.u[1] = zero.u[2] = zero.u[3] = 0;
	XLD(r0, mp); XLD(r1, mp + 16); XLD(r2, mp + 32);
	v25 = XV_SHUFPS(XV_SHUFPS(XV_SHUFPS(xv_unpcklps(r0, r1), r0, 0xec), r2, 0xe4), XV_SHUFPS(XV_SHUFPS(xv_unpcklps(r0, r1), r0, 0xec), r2, 0xe4), 0x24);
	v26 = XV_SHUFPS(v25, v25, 0x49);
	v27 = XV_SHUFPS(v25, v25, 0x92);
	v29 = xv_addps(xv_addps(v25, v26), v27);
	v31 = xv_subps(xv_subps(v25, v26), v27);
	v36 = xv_addps(XV_SHUFPS(v31, xv_unpckhps(v31, v29), 0xc4), one);
	v37 = rsqrt_nr(v36);
	v41 = XV_SHUFPS(XV_SHUFPS(XV_SHUFPS(r0, xv_unpckhps(r0, r1), 0x94), r2, 0xc6), XV_SHUFPS(XV_SHUFPS(r0, xv_unpckhps(r0, r1), 0x94), r2, 0xc6), 0xd8);
	v44 = XV_SHUFPS(movss_(r0, r1), r2, 0xd2);
	v44 = XV_SHUFPS(v44, v44, 0xd2);
	v45 = xv_addps(v41, v44);
	v46 = xv_subps(v41, v44);
	v49 = xv_mulps(v37, half);
	v51 = XV_SHUFPS(v45, v46, 0xc6); v51 = XV_SHUFPS(v51, v51, 0x93);
	v53 = XV_SHUFPS(v45, v46, 0xd2); v53 = XV_SHUFPS(v53, v53, 0x9c);
	v54 = XV_SHUFPS(v45, v46, 0xb1);
	v55 = movss_(v51, v36);
	v57 = XV_SHUFPS(xv_unpcklps(v53, v36), v53, 0xec);
	v59 = XV_SHUFPS(v54, xv_unpckhps(v54, v36), 0x94);
	v61 = XV_SHUFPS(v46, xv_unpckhps(v46, v36), 0xc4);
	v63 = xv_mulps(v55, XV_SHUFPS(v49, v49, 0x00));
	v65 = xv_mulps(v57, XV_SHUFPS(v49, v49, 0x55));
	v67 = xv_mulps(v59, XV_SHUFPS(v49, v49, 0xaa));
	v69 = xv_mulps(v61, XV_SHUFPS(v49, v49, 0xff));
	v70 = XV_SHUFPS(r0, r0, 0x00); v71 = XV_SHUFPS(r1, r1, 0x55); v72 = XV_SHUFPS(r2, r2, 0xaa);
	q = sel(xv_cmpps1(v70, v71), v65, v63);
	m = xv_andps(xv_cmpps1(v70, v72), xv_cmpps1(v71, v72));
	q = sel(m, v67, q);
	q = sel(xv_cmpps1(zero, XV_SHUFPS(v29, v29, 0x00)), v69, q);
	if (md == 1) { orig(c); check("Quat::FromMatrix33", 0x00515c00u, &q, t, 16); return; }
	XST(t, q);
	c->eax = t;
	xrc_stdcall_ret(c, 1);
}

void XRC_CC xo_00d785d0(XCtx *c); /* ovr_bvh.c */
void XRC_CC xo_00c77820(XCtx *c); /* ovr_unlocked.c */
void XRC_CC xo_00809600(XCtx *c); /* ovr_config_menu.c */
void XRC_CC xo_0080d0a0(XCtx *c);
void XRC_CC xo_007a4550(XCtx *c);
const XrcManual xrc_manual_simd[] = {
	{0x00809600u, xo_00809600, "the Settings menu's row list: + PC Graphics (ovr_config_menu.c)"},
	{0x0080d0a0u, xo_0080d0a0, "the Settings menu's confirm: PC Graphics opens the overlay (ovr_config_menu.c)"},
	{0x007a4550u, xo_007a4550, "the UI text lookup: the PC Graphics row's strings (ovr_config_menu.c)"},
	{0x00c77820u, xo_00c77820, "zlib inflate_fast, run without the GIL (ovr_unlocked.c)"},
	{0x00d785d0u, xo_00d785d0, "the collision BVH build step (fast, ovr_bvh.c)"},
	{0x00515c00u, xo_00515c00, "Quat::FromMatrix33 (fast)"},
	{0x00517130u, xo_00517130, "QuatToMatrix2 (fast)"},
	{0x00744160u, xo_00744160, "QuatToMatrix (fast)"},
	{0x00743c70u, xo_00743c70, "Quat::Mul (fast)"},
	{0x00456970u, xo_00456970, "Matrix44::Mul (fast)"},
	{0x00457e50u, xo_00457e50, "Aabb::Transformed (fast)"},
	{0x004559b0u, xo_004559b0, "Matrix44::Transform (fast)"},
	{0x00455840u, xo_00455840, "Vec4::operator= (fast)"},
	{0x00455880u, xo_00455880, "Vec4::Get (fast)"},
	{0x004586c0u, xo_004586c0, "Aabb::Extended (fast)"},
	{0x00710860u, xo_00710860, "Vec4::Dot4 (fast)"},
	{0x00711f40u, xo_00711f40, "Matrix33::Transform (fast)"},
	{0x00455f00u, xo_00455f00, "Matrix44::Transform(m128) (fast)"},
	{0x004584c0u, xo_004584c0, "Matrix44::TransformPoint (fast)"},
	{0x00455d80u, xo_00455d80, "Matrix44::Get (fast)"},
	{0x00462f80u, xo_00462f80, "Matrix44::operator= (fast)"},
};
const unsigned xrc_manual_simd_n = sizeof xrc_manual_simd / sizeof xrc_manual_simd[0];
