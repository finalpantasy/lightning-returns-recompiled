/* ovr_math.c - native replacements for the hottest translated math helpers of LRFF13.exe (white::math):
 *
 *   00408210 Matrix4x4::TransformVector4   0040acb0 Matrix4x4::Multiply   0040ab00 Vector4::Normalize
 *   0040b1e0 matrix copy (64 bytes)        00416140 / 004164f0 / 00416250 / 00416020 vector and quaternion math
 *   00cf1a20 _aulldiv (the CRT's unsigned 64-bit divide)
 *   005a44e0 Transpose, 004bb0b0 Aabb::ExpandByPoint, 004bada0 TransformPoint, 00437270 MakeTranslation, 00408320 Inverse,
 *   004bb1c0 Aabb::TransformByMatrix, 00416a30 GetRotationQuat (second batch; the ones with calls run their callees as cores)
 *   00b09d50 (a quantized AABB-tree query loop, hand-written: SSE2 integer compares), 00b8edb0 (a 3-row dequantize with table gathers,
 *   generated) (third batch, math3)
 *
 * They are `fast` rows in translator/lr/overrides.tsv (translator: ps4star's XRC, Unlicense). The generated pages call the
 * xo_<addr> defined here directly (XCALLMDK / XCALLDK) and keep the translation as xf_<addr>, the reference these bodies are
 * checked against and fall back to. The dispatcher maps the addresses to xo_<addr> too (xrc_manual_math at the end of this
 * file, registered in xrc_rt.c), so indirect calls reach them as before. No page renaming is needed.
 *
 * The straight-line SSE bodies (ovr_math_gen.inc) are generated one statement per guest instruction from the
 * translator's own page source (tools/ovr_math/gen_ovr_math.py): the same operations in the same order, packed
 * arithmetic through ex_mulps / ex_addps / ex_subps (x86's own operand order for NaN payloads: -ffast-math may
 * swap the operands of a plain _mm_mul_ps), rsqrtps as the translation has it (xv_rsqrtps: an exact 1 / sqrt, so
 * the results do not depend on the CPU vendor's estimate table). The guest stack frame is a local array, and the guest xmm registers the
 * function writes are stored back exactly as the translation leaves them, so even the dead state is the same.
 *
 * Speed notes (math3): the generated bodies keep the guest xmm registers in an X128 X[8] array, and at -O1 that array stays in
 * memory (every statement a store) unless nothing indexes it dynamically: the generator now emits the register copies unrolled, the
 * movss / movq / movaps statements as intrinsics on the whole vector (not the u[] / q[] union lanes), and the return as inline stores.
 * cvtps2dq is _mm_cvtps_epi32 when the guest's and the host's rounding mode are both nearest, paddd _mm_add_epi32.
 *
 *   XRC_OVR_MATH=0         the translations only (to compare speed / behavior)
 *   XRC_OVR_MATH_CHECK=1   every call also runs the translation and compares the written memory, eax and the
 *                          preserved registers (the translation's result is what the game gets)
 *   XRC_OVR_MATH_TEST=1    at the first call: >= XRC_OVR_MATH_N (default 200000) random inputs per function (zero,
 *                          denormals, NaN of both kinds with payloads, inf, random bit patterns), ours against the
 *                          translation, bitwise; prints the result and ends the process (exit code 1 on a mismatch) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"
#include "guest_types.h"

#if !defined(XRC_VEC)
#error "ovr_math.c needs the SSE host path (XRC_VEC)"
#endif

/* x86's own mulps / addps / subps, operands as the guest has them (see ovr_simd.c) */
#if defined(__x86_64__) && defined(__AVX__)
#define EXOP(name_, ins_) static inline X128 name_(X128 a, X128 b) { X128 r; __asm__(ins_ " %2, %1, %0" : "=x"(r.v) : "x"(a.v), "x"(b.v)); return r; }
EXOP(ex_mulps, "vmulps") EXOP(ex_addps, "vaddps") EXOP(ex_subps, "vsubps") EXOP(ex_maxps, "vmaxps") EXOP(ex_minps, "vminps")
#else
#define EXOP(name_, ins_) static inline X128 name_(X128 a, X128 b) { __asm__(ins_ " %1, %0" : "+x"(a.v) : "x"(b.v)); return a; }
EXOP(ex_mulps, "mulps") EXOP(ex_addps, "addps") EXOP(ex_subps, "subps") EXOP(ex_maxps, "maxps") EXOP(ex_minps, "minps")
#endif

static inline u32 ld16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 ld32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 ld64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static inline void st32(void *p, u32 v) { memcpy(p, &v, 4); }
static inline void st64(void *p, u64 v) { memcpy(p, &v, 8); }

/* xrc_stdcall_ret / xrc_cdecl_ret inline (they are two stores; the call was a measurable part of a leaf's cost) */
#define XRC_STDCALL_RET(c_, n_) do { (c_)->eip = ret_addr(c_); (c_)->esp += 4u + 4u * (u32)(n_); } while (0)
#define XRC_CDECL_RET(c_) do { (c_)->eip = ret_addr(c_); (c_)->esp += 4u; } while (0)

#include "ovr_math_gen.inc"

/* the translations (the reference the pages keep) */
void XRC_CC xf_00408210(XCtx *c);
void XRC_CC xf_0040acb0(XCtx *c);
void XRC_CC xf_0040ab00(XCtx *c);
void XRC_CC xf_0040b1e0(XCtx *c);
void XRC_CC xf_00416140(XCtx *c);
void XRC_CC xf_004164f0(XCtx *c);
void XRC_CC xf_00416020(XCtx *c);
void XRC_CC xf_00416250(XCtx *c);
void XRC_CC xf_00cf1a20(XCtx *c);
void XRC_CC xf_005a44e0(XCtx *c);
void XRC_CC xf_004bb0b0(XCtx *c);
void XRC_CC xf_004bada0(XCtx *c);
void XRC_CC xf_00437270(XCtx *c);
void XRC_CC xf_00408320(XCtx *c);
void XRC_CC xf_004bb1c0(XCtx *c);
void XRC_CC xf_00416a30(XCtx *c);
void XRC_CC xf_00aac130(XCtx *c);
void XRC_CC xf_00b09d50(XCtx *c);
void XRC_CC xf_00b8edb0(XCtx *c);
void XRC_CC xf_0040abc0(XCtx *c);

/* 0040acb0 Multiply: thiscall (out, rhs), ret 8. out.row[r] = this.TransformVector4(rhs.row[r]) for r = 3, 2, 1, 0 (in that
 * order, each into a temporary), then the four results are copied to out: the temporaries and the arguments of the
 * TransformVector4 calls live in the dead stack below esp, as the original's own frame does. Returns out. */
static void XRC_CC gen_Multiply(XCtx *c) {
	u8 *const xm = XM;
	X128 X[8];
	u32 s = c->esp - 0x100u, out = arg_u32(c, 1), rhs = arg_u32(c, 2), tmp = s + 0x20u;
	u64 hi;
	int r;
	X[0] = c->x[0]; X[1] = c->x[1]; X[2] = c->x[2]; X[3] = c->x[3]; X[4] = c->x[4]; X[5] = c->x[5]; X[6] = c->x[6]; X[7] = c->x[7];
	_Pragma("clang loop unroll(full)")
	for (r = 3; r >= 0; r--) {
		FRAME_ARG(s, 1) = tmp + 16u * (u32)r;     /* the callee's [esp + 4]: out */
		FRAME_ARG(s, 2) = rhs + 16u * (u32)r;     /* [esp + 8]: v */
		(void)core_TransformVector4(c, X, c->ecx, s);
	}
	_Pragma("clang loop unroll(full)")
	for (r = 0; r < 4; r++) {
		u64 lo_ = ld64(xm + tmp + 16u * (u32)r), hi_ = ld64(xm + tmp + 16u * (u32)r + 8);
		st64(xm + out + 16u * (u32)r, lo_);
		st64(xm + out + 16u * (u32)r + 8, hi_);
	}
	hi = ld64(xm + tmp + 0x30u + 8);
	X[0].v = _mm_castsi128_ps(_mm_cvtsi64_si128((long long)hi));
	c->x[0] = X[0]; c->x[1] = X[1]; c->x[2] = X[2]; c->x[3] = X[3]; c->x[4] = X[4]; c->x[5] = X[5];
	c->eax = out;
	XRC_STDCALL_RET(c, 2);
}

/* 00cf1a20 _aulldiv: stdcall-like (ret 0x10) (dividend lo, hi, divisor lo, hi) -> edx:eax = the quotient. A zero divisor
 * is left to the translation (the CPU's divide fault) */
static void XRC_CC gen_aulldiv(XCtx *c) {
	u32 sp = c->esp;
	u64 n = (u64)FRAME_ARG(sp, 1) | (u64)FRAME_ARG(sp, 2) << 32, d = (u64)FRAME_ARG(sp, 3) | (u64)FRAME_ARG(sp, 4) << 32, q;
	if (!d) { xf_00cf1a20(c); return; }
	q = n / d;
	c->eax = (u32)q;
	c->edx = (u32)(q >> 32);
	XRC_STDCALL_RET(c, 4);
}

/* 00b09d50 (a quantized AABB-tree query; thiscall, ret 8): this + 0x18 = the node base, this + 0x1c = the end, args (start,
 * key). A node is 16 bytes of 8 words; the key's 8 words are tested against it: ((node -us key) & m4) | ((key -us node) & m3) == 0
 * in every word -> a hit when word 7 != 0xffff (returns that node), word 7 == 0xffff -> the next node (+16); a miss -> the node at
 * base + (word 3 << 4). Stops (returns the node) at the end. m4, m3 and the zero vector are function-local statics the code
 * initializes lazily under the guard bits 1, 2, 4 of the dword at 0x51157b0 (copied from 0x219c920 / 0x219c930, stored at
 * 0x51157a0 / 0x5115790 / 0x5115780): done here the same way, a call at a time. xmm0..6 end as the translation leaves them
 * (the last node, the key, the zero vector, m3, m4, the last compare mask and the last masked difference). */
#define B09D50_FLAGS GF(Fn00b09d50Statics, G_B09D50_STATICS, flags)
#define B09D50_STATIC_ADDR(f) GADDR(Fn00b09d50Statics, G_B09D50_STATICS, f)
#define B09D50_M4_SRC 0x219c920u /* the initial m4 / m3 (const data) */
#define B09D50_M3_SRC 0x219c930u
static void XRC_CC gen_Fn00b09d50(XCtx *c) {
	u8 *const xm = XM;
	u32 start = arg_u32(c, 1), key = arg_u32(c, 2), self = c->ecx, base, end, f, p;
	X128 X0 = c->x[0], X1, X2, X3, X4, X5 = c->x[5], X6 = c->x[6];
	__m128i q, s4, s3, s2;
	q = _mm_loadu_si128((const __m128i *)(xm + key));
	X1.vi = q;
	f = B09D50_FLAGS;
	base = GF(QuantAabbTree, self, nodes); end = GF(QuantAabbTree, self, nodes_end);
	if (!(f & B09D50_M4_INIT)) { s4 = _mm_loadu_si128((const __m128i *)(xm + B09D50_M4_SRC)); f |= B09D50_M4_INIT; B09D50_FLAGS = f; _mm_storeu_si128((__m128i *)(xm + B09D50_STATIC_ADDR(m4)), s4); }
	else s4 = _mm_loadu_si128((const __m128i *)(xm + B09D50_STATIC_ADDR(m4)));
	if (!(f & B09D50_M3_INIT)) { s3 = _mm_loadu_si128((const __m128i *)(xm + B09D50_M3_SRC)); f |= B09D50_M3_INIT; B09D50_FLAGS = f; _mm_storeu_si128((__m128i *)(xm + B09D50_STATIC_ADDR(m3)), s3); }
	else s3 = _mm_loadu_si128((const __m128i *)(xm + B09D50_STATIC_ADDR(m3)));
	if (!(f & B09D50_ZERO_INIT)) { s2 = _mm_setzero_si128(); f |= B09D50_ZERO_INIT; B09D50_FLAGS = f; _mm_storeu_si128((__m128i *)(xm + B09D50_STATIC_ADDR(zero)), s2); }
	else s2 = _mm_loadu_si128((const __m128i *)(xm + B09D50_STATIC_ADDR(zero)));
	X4.vi = s4; X3.vi = s3; X2.vi = s2;
	p = start;
	if (p != end) {
		__m128i n = _mm_setzero_si128(), a = n, b = n;
		for (;;) {
			n = _mm_loadu_si128((const __m128i *)(xm + p));
			a = _mm_subs_epu16(n, q);
			b = _mm_subs_epu16(q, n);
			a = _mm_and_si128(a, s4);
			b = _mm_and_si128(b, s3);
			a = _mm_or_si128(a, b);
			a = _mm_cmpeq_epi16(a, s2);
			if (_mm_movemask_epi8(a) != 0xffff) {
				p = ((u32)(u16)_mm_extract_epi16(n, 3) << 4) + base;
			} else {
				if ((u16)_mm_extract_epi16(n, 7) != 0xffffu) break;
				p += 0x10u;
			}
			if (p == end) break;
		}
		X0.vi = n; X5.vi = a; X6.vi = b;
	}
	c->x[0] = X0; c->x[1] = X1; c->x[2] = X2; c->x[3] = X3; c->x[4] = X4; c->x[5] = X5; c->x[6] = X6;
	c->eax = p; c->ecx = end; c->edx = base;
	XRC_STDCALL_RET(c, 2);
}

/* 0040abc0 Vector4::ToPackedColor8888 (thiscall, ret): four floats at this -> the low bytes of their truncated (fistp with the
 * control word forced to round-toward-zero) integer values, or-ed together shifted 24 / 16 / 8 / 0 (no masking: a value over 255
 * bleeds into the next byte, as the code does it). The translation runs four fnstcw / fldcw / fistp.q round trips on the x87
 * emulation; this is four cvttss2si. fistp.q's "integer indefinite" (NaN, out of range) is cvttss2si's, low dword 0. edx ends as
 * the shifted partial (the code's last `or eax, edx` leaves it), eax the result, ecx the this. The x87 control word ends as it
 * began (the code restores it), the stack is balanced. A control word whose rounding field would not be 3 after the `or`
 * (high garbage bits) is left to the translation. */
static void XRC_CC gen_ToPackedColor8888(XCtx *c) {
	u8 *const xm = XM;
	u32 p = c->ecx, i0, i1, i2, i3, e;
	if (__builtin_expect((((u32)(u16)c->fcw | 0xc00u) >> 10) != 3, 0)) { xf_0040abc0(c); return; }
	i0 = (u32)_mm_cvttss_si64(_mm_load_ss((const float *)(xm + p)));
	i1 = (u32)_mm_cvttss_si64(_mm_load_ss((const float *)(xm + (u32)(p + 4u))));
	i2 = (u32)_mm_cvttss_si64(_mm_load_ss((const float *)(xm + (u32)(p + 8u))));
	i3 = (u32)_mm_cvttss_si64(_mm_load_ss((const float *)(xm + (u32)(p + 12u))));
	e = ((((i0 << 8) | i1) << 8) | i2) << 8;
	c->eax = e | i3; c->edx = e;
	XRC_CDECL_RET(c);
}

/* ------------------------------------------------------------------------------------------------ checking */

static int g_mode = -1; /* 0 translations, 1 ours, 2 ours checked against the translation */
static int g_inited;

typedef struct Spec Spec;
struct Spec {
	u32 addr;
	const char *name;
	XFn impl, orig;
	int nreg;                               /* regions of memory the function writes: at entry (out pointer, size) */
	void (*region)(XCtx *c, u32 *at, u32 *len);
	void (*setup)(XCtx *c, u32 base, unsigned long long *rng, unsigned it); /* the self-test's random inputs */
	int cmp_edx, cmp_x;                     /* compare edx (bit 0) and ecx (bit 1) / the xmm registers (a mask) too */
	u32 gat, glen;                          /* a fixed region of guest globals the function writes (compared and restored like the others) */
	unsigned long long calls, bad, nanonly;
};

static void rg_out4(XCtx *c, u32 *at, u32 *len) { at[0] = arg_u32(c, 1); len[0] = 16; }
static void rg_out16(XCtx *c, u32 *at, u32 *len) { at[0] = arg_u32(c, 1); len[0] = 64; }
static void rg_sincos(XCtx *c, u32 *at, u32 *len) { at[0] = c->eax; len[0] = 16; at[1] = c->ecx; len[1] = 16; }
static void rg_out32(XCtx *c, u32 *at, u32 *len) { at[0] = arg_u32(c, 1); len[0] = 32; }
static void rg_compose(XCtx *c, u32 *at, u32 *len) { at[0] = arg_u32(c, 2); len[0] = 0xa0; }
static void rg_out48(XCtx *c, u32 *at, u32 *len) { at[0] = arg_u32(c, 1); len[0] = 48; }
static void rg_none(XCtx *c, u32 *at, u32 *len) { (void)c; at[0] = 0; len[0] = 0; }

static void su_tv(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_mul(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_norm(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_copy(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_2vec(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_in4(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_sincos(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_div(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_aabb(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_tbm(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_mk(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_grq(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_compose(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_b09d50(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_b8edb0(XCtx *c, u32 base, unsigned long long *rng, unsigned it);
static void su_color(XCtx *c, u32 base, unsigned long long *rng, unsigned it);

#define XM6 0x3f
static Spec specs[] = {
	{0x00408210u, "Matrix4x4::TransformVector4", gen_TransformVector4, xf_00408210, 1, rg_out4, su_tv, 0, 0xff},
	{0x0040acb0u, "Matrix4x4::Multiply", gen_Multiply, xf_0040acb0, 1, rg_out16, su_mul, 0, 0x01 | 0x3e},
	{0x0040ab00u, "Vector4::Normalize", gen_Normalize, xf_0040ab00, 1, rg_out4, su_norm, 0, 0xff},
	{0x0040b1e0u, "Matrix4x4 copy", gen_Copy4x4, xf_0040b1e0, 1, rg_out16, su_copy, 0, 0xff},
	{0x00416140u, "Vector4 op (00416140)", gen_Fn00416140, xf_00416140, 1, rg_out4, su_2vec, 0, 0xff},
	{0x004164f0u, "Matrix from quaternion (004164f0)", gen_Fn004164f0, xf_004164f0, 1, rg_out16, su_in4, 0, 0xff},
	{0x00416020u, "sincos x4 (00416020)", gen_SinCos4, xf_00416020, 2, rg_sincos, su_sincos, 0, 0xff},
	{0x00416250u, "Vector4 op (00416250)", gen_Fn00416250, xf_00416250, 1, rg_out4, su_in4, 0, 0xff},
	{0x00cf1a20u, "_aulldiv", gen_aulldiv, xf_00cf1a20, 0, rg_none, su_div, 1, 0},
	{0x005a44e0u, "Matrix4x4::Transpose", gen_Transpose, xf_005a44e0, 1, rg_out16, su_copy, 0, 0xff},
	{0x004bb0b0u, "Aabb::ExpandByPoint", gen_ExpandByPoint, xf_004bb0b0, 1, rg_out32, su_aabb, 0, 0xff},
	{0x004bada0u, "Matrix4x4::TransformPoint", gen_TransformPoint, xf_004bada0, 1, rg_out4, su_tv, 0, 0xff},
	{0x00437270u, "Matrix4x4::MakeTranslation", gen_MakeTranslation, xf_00437270, 1, rg_out16, su_mk, 0, 0xff},
	{0x00408320u, "Matrix4x4::Inverse", gen_Inverse, xf_00408320, 1, rg_out16, su_copy, 0, 0xff},
	{0x004bb1c0u, "Aabb::TransformByMatrix", gen_TransformByMatrix, xf_004bb1c0, 1, rg_out32, su_tbm, 0, 0xff},
	{0x00416a30u, "JointExpression::GetRotationQuat", gen_GetRotationQuat, xf_00416a30, 1, rg_out4, su_grq, 0, 0xff},
	{0x00aac130u, "Transform::ComposeLocalToWorld", gen_ComposeLocalToWorld, xf_00aac130, 1, rg_compose, su_compose, 0, 0xff},
	{0x00b09d50u, "AabbTree query (00b09d50)", gen_Fn00b09d50, xf_00b09d50, 0, rg_none, su_b09d50, 3, 0x7f, 0x5115780u, 0x40u},
	{0x00b8edb0u, "Dequantize 3 rows (00b8edb0)", gen_Fn00b8edb0, xf_00b8edb0, 1, rg_out48, su_b8edb0, 3, 0xff},
	{0x0040abc0u, "Vector4::ToPackedColor8888", gen_ToPackedColor8888, xf_0040abc0, 0, rg_none, su_color, 1, 0},
};
enum { S_TV, S_MUL, S_NORM, S_COPY, S_F140, S_F4F0, S_SINCOS, S_F250, S_DIV, S_TRANSPOSE, S_EXPAND, S_TPOINT, S_MKTRANS, S_INVERSE, S_TBM, S_GRQ, S_COMPOSE, S_B09D50, S_B8EDB0, S_COLOR };


static int is_nan_u(u32 u) { return (u & 0x7f800000u) == 0x7f800000u && (u & 0x007fffffu); }
/* 0 identical, 1 differ only in lanes where both are NaN (which of two NaN operands' payloads a packed op returns:
 * x86 takes the first, and a compiler is free to swap the operands of the translation's plain _mm_mul_ps / _mm_add_ps
 * under -ffast-math), 2 anything else */
static int cmp_lanes(const void *p, const void *q, u32 n) {
	int r = 0;
	u32 j;
	for (j = 0; j + 4 <= n; j += 4) {
		u32 a = ld32((const u8 *)p + j), b = ld32((const u8 *)q + j);
		if (a != b) { if (is_nan_u(a) && is_nan_u(b)) r = r ? r : 1; else return 2; }
	}
	return r;
}
/* runs ours and the translation on the same inputs and compares. Memory ends as the translation leaves it (the game
 * gets the translation's result); *c too. Returns the number of mismatching items (0: identical). */
static int run_pair(Spec *s, XCtx *c, const u32 *at, const u32 *len, int nreg, char *why, size_t whysz) {
	XCtx a = *c, b = *c;
	u8 save[2][1024], r1[2][1024], gsave[0x40], gr1[0x40];
	int k, bad = 0, nanonly = 0, d;
	for (k = 0; k < nreg; k++) memcpy(save[k], XM + at[k], len[k]);
	if (s->glen) memcpy(gsave, XM + s->gat, s->glen);
	s->impl(&a);
	if (s->glen) { memcpy(gr1, XM + s->gat, s->glen); memcpy(XM + s->gat, gsave, s->glen); }
	for (k = 0; k < nreg; k++) { memcpy(r1[k], XM + at[k], len[k]); memcpy(XM + at[k], save[k], len[k]); }
	{ int sv = g_mode; g_mode = 0; s->orig(&b); g_mode = sv; } /* (calls the translation makes go to the translations too) */
	why[0] = 0;
	if (s->glen && memcmp(gr1, XM + s->gat, s->glen)) { u32 j; for (j = 0; gr1[j] == XM[s->gat + j]; j++) {} bad++; snprintf(why + strlen(why), whysz - strlen(why), " globals@%08x+%x ours %02x orig %02x", s->gat, j, gr1[j], XM[s->gat + j]); }
	for (k = 0; k < nreg; k++)
		if ((d = cmp_lanes(r1[k], XM + at[k], len[k])) != 0) {
			u32 j;
			if (d == 1) nanonly = 1; else bad++;
			for (j = 0; j < len[k] && r1[k][j] == XM[at[k] + j]; j++) {}
			j &= ~3u;
			snprintf(why + strlen(why), whysz - strlen(why), " mem[%d]@%08x+%x ours %08x orig %08x", k, at[k], j, ld32(&r1[k][j]), ld32(XM + at[k] + j));
		}
	if (a.eax != b.eax || a.esp != b.esp || a.eip != b.eip || a.ebx != b.ebx || a.ebp != b.ebp || a.esi != b.esi || a.edi != b.edi) {
		bad++; snprintf(why + strlen(why), whysz - strlen(why), " regs(eax %08x/%08x esp %08x/%08x eip %08x/%08x)", a.eax, b.eax, a.esp, b.esp, a.eip, b.eip);
	}
	if (a.fcw != b.fcw) { bad++; snprintf(why + strlen(why), whysz - strlen(why), " fcw %04x/%04x", a.fcw, b.fcw); }
	if ((s->cmp_edx & 2) && a.ecx != b.ecx) { bad++; snprintf(why + strlen(why), whysz - strlen(why), " ecx %08x/%08x", a.ecx, b.ecx); }
	if ((s->cmp_edx & 1) && a.edx != b.edx) { bad++; snprintf(why + strlen(why), whysz - strlen(why), " edx %08x/%08x", a.edx, b.edx); }
	for (k = 0; k < 8; k++)
		if ((s->cmp_x >> k & 1) && (d = cmp_lanes(&a.x[k], &b.x[k], 16)) != 0) { if (d == 1) nanonly = 1; else bad++; snprintf(why + strlen(why), whysz - strlen(why), " xmm%d(%08x %08x %08x %08x / %08x %08x %08x %08x)", k, a.x[k].u[0], a.x[k].u[1], a.x[k].u[2], a.x[k].u[3], b.x[k].u[0], b.x[k].u[1], b.x[k].u[2], b.x[k].u[3]); }
	*c = b;
	return bad ? 2 : nanonly;
}

/* ---- random inputs */
static u32 rnd(unsigned long long *s) {
	unsigned long long x = *s;
	x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
	*s = x;
	return (u32)((x * 0x2545F4914F6CDD1Dull) >> 32);
}
static int nan1; /* XRC_OVR_MATH_NAN1=1: every NaN input is the default quiet NaN, so no two operands ever differ in payload */
static u32 fbits(unsigned long long *r, int mixed) {
	u32 v = rnd(r), sign = (v & 1u) << 31, k = (v >> 1) % 20u, m = rnd(r), f;
	if (!mixed) k = 12 + k % 8; /* moderate values only */
	if (nan1 && (k == 4 || k == 5)) return 0x7fc00000u;
	if (nan1 && (k == 6 || k == 7 || k == 11)) { u32 q = rnd(r); return is_nan_u(q) ? 0x7fc00000u : q; }
	switch (k) {
	case 0: return 0;
	case 1: return 0x80000000u;
	case 2: return sign | (m & 0x007fffffu);                          /* denormal (or zero) */
	case 3: return sign | 0x7f800000u;                                /* inf */
	case 4: return sign | 0x7fc00000u | (m & 0x003fffffu);            /* quiet NaN, payload */
	case 5: return sign | 0x7f800000u | ((m & 0x003fffffu) ? (m & 0x003fffffu) : 1u); /* signaling NaN */
	case 6: return rnd(r);                                            /* any bits */
	case 7: return rnd(r);
	case 8: return sign | 0x00800000u | (m & 0x00ffffffu);            /* tiny normals */
	case 9: return sign | 0x7f000000u | (m & 0x007fffffu);            /* huge */
	case 10: { float t = (float)((int)(m % 17u) - 8); memcpy(&f, &t, 4); return f; } /* small integers */
	case 11: return rnd(r);
	default: { float t = ((float)(m >> 8) / 8388608.0f - 1.0f) * (float)(1 << (rnd(r) % 12)); memcpy(&f, &t, 4); return f; }
	}
}
static void fillf(u32 at, int n, unsigned long long *r, int mixed) {
	int i;
	for (i = 0; i < n; i++) GARR32(at, i) = fbits(r, mixed);
}
/* out sometimes overlaps an input (in place; shifted by a lane or a row) */
static u32 pick_out(unsigned long long *r, u32 plain, u32 a, u32 b) {
	u32 v = rnd(r) % 12u;
	switch (v) {
	case 0: return a;
	case 1: return b;
	case 2: return a + 4u;
	case 3: return b + 8u;
	case 4: return a + 16u;
	case 5: return b + 16u;
	default: return plain;
	}
}
static void push_args(XCtx *c, u32 base, int n, const u32 *a) {
	int i;
	c->esp = base + 0x1000u;
	set_arg_u32(c, 0, 0x12345678u);
	for (i = 0; i < n; i++) set_arg_u32(c, 1u + (u32)i, a[i]);
}
static void su_tv(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 m = base, v = base + 0x40, o, a[2];
	int mx = (it & 3) != 0;
	fillf(m, 16, r, mx); fillf(v, 4, r, mx); fillf(base + 0x80, 4, r, mx);
	o = pick_out(r, base + 0x80, m + 0x30, v);
	a[0] = o; a[1] = v; push_args(c, base, 2, a); c->ecx = m;
}
static void su_mul(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 m = base, rh = base + 0x40, o, a[2];
	int mx = (it & 3) != 0;
	fillf(m, 16, r, mx); fillf(rh, 16, r, mx); fillf(base + 0x80, 16, r, mx);
	o = pick_out(r, base + 0x80, m, rh);
	a[0] = o; a[1] = rh; push_args(c, base, 2, a); c->ecx = m;
}
static void su_norm(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 m = base, o, a[1];
	int mx = (it & 3) != 0;
	fillf(m, 4, r, mx); fillf(base + 0x40, 4, r, mx);
	o = pick_out(r, base + 0x40, m, m + 4u);
	a[0] = o; push_args(c, base, 1, a); c->ecx = m;
}
static void su_copy(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 m = base, o, a[1];
	(void)it;
	fillf(m, 16, r, 1); fillf(base + 0x80, 16, r, 1);
	o = pick_out(r, base + 0x80, m, m + 0x30);
	a[0] = o; push_args(c, base, 1, a); c->ecx = m;
}
static void su_2vec(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 m = base, b = base + 0x20, o, a[2];
	int mx = (it & 3) != 0;
	fillf(m, 4, r, mx); fillf(b, 4, r, mx); fillf(base + 0x40, 4, r, mx);
	o = pick_out(r, base + 0x40, m, b);
	a[0] = o; a[1] = b; push_args(c, base, 2, a); c->ecx = m;
}
static void su_in4(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 in = base, o, a[2];
	int mx = (it & 3) != 0;
	fillf(in, 4, r, mx); fillf(base + 0x80, 16, r, mx);
	o = pick_out(r, base + 0x80, in, in + 8u);
	a[0] = o; a[1] = in; push_args(c, base, 2, a);
}
static void su_sincos(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	int mx = (it & 3) != 0, k;
	c->esp = base + 0x1000u;
	set_arg_u32(c, 0, 0x12345678u);
	for (k = 0; k < 4; k++) c->x[0].u[k] = fbits(r, mx);
	for (k = 1; k < 8; k++) { int j; for (j = 0; j < 4; j++) c->x[k].u[j] = rnd(r); }
	c->eax = base; c->ecx = base + 0x40;
	fillf(base, 4, r, 1); fillf(base + 0x40, 4, r, 1);
}
static void su_aabb(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 m = base, p = base + 0x40, o, a[2];
	int mx = (it & 3) != 0;
	fillf(m, 8, r, mx); fillf(p, 4, r, mx); fillf(base + 0x80, 8, r, mx);
	o = pick_out(r, base + 0x80, m, p);
	a[0] = o; a[1] = p; push_args(c, base, 2, a); c->ecx = m;
}
static void su_tbm(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 m = base, mat = base + 0x40, o, a[2];
	int mx = (it & 3) != 0;
	fillf(m, 8, r, mx); fillf(mat, 16, r, mx); fillf(base + 0x100, 8, r, mx);
	o = pick_out(r, base + 0x100, m, mat);
	a[0] = o; a[1] = mat; push_args(c, base, 2, a); c->ecx = m;
}
static void su_mk(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 v = base + 0x40, o, a[2];
	int mx = (it & 3) != 0;
	fillf(v, 4, r, mx); fillf(base + 0x80, 16, r, mx);
	o = pick_out(r, base + 0x80, v, v + 8u);
	a[0] = o; a[1] = v; push_args(c, base, 2, a);
}
static void su_grq(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 m = base, o, a[1], k;
	int mx = (it & 3) != 0;
	fillf(m, 4, r, mx); fillf(m + 0xa0, 4, r, mx); fillf(m + 0xe0, 4, r, mx); fillf(base + 0x180, 4, r, mx);
	for (k = 0xf2; k <= 0xf4; k += 2) { u32 v = rnd(r) % 4u; XM[m + k] = v < 2 ? (u8)v : (u8)rnd(r); }
	o = pick_out(r, base + 0x180, m, m + 0xa0);
	a[0] = o; push_args(c, base, 1, a); c->ecx = m;
}
static void su_compose(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 node = base, par = base + 0x100, out = base + 0x200, a[3], k;
	int mx = (it & 3) != 0;
	fillf(node, 4, r, mx); fillf(node + 0x20, 4, r, mx); fillf(node + 0x30, 4, r, mx); fillf(node + 0x40, 16, r, mx);
	fillf(node + 0x80, 4, r, mx); fillf(node + 0xa0, 4, r, mx); fillf(node + 0xc0, 4, r, mx); fillf(node + 0xe0, 4, r, mx);
	fillf(par + 0x80, 4, r, mx); fillf(par + 0x90, 4, r, mx); fillf(out, 40, r, mx);
	for (k = 0xf2; k <= 0xf4; k += 2) { u32 v = rnd(r) % 4u; XM[node + k] = v < 2 ? (u8)v : (u8)rnd(r); }
	a[0] = node; a[1] = out; a[2] = par; push_args(c, base, 3, a);
}
static void su_div(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u64 n, d;
	u32 k = rnd(r) % 8u, a[4];
	n = (u64)rnd(r) << 32 | rnd(r);
	d = (u64)rnd(r) << 32 | rnd(r);
	switch (k) {
	case 0: d &= 0xffffffffull; break;               /* 32-bit divisor (the two `div`s path) */
	case 1: d >>= 1 + (rnd(r) % 63u); break;
	case 2: n >>= rnd(r) % 64u; d >>= rnd(r) % 64u; break;
	case 3: d = n; break;
	case 4: d = n + (rnd(r) & 3u) - 1u; break;
	case 5: d = (d & 0xffffffffull) | 0x100000000ull; break;
	case 6: n = ~0ull; d >>= rnd(r) % 64u; break;
	default: break;
	}
	if (!d) d = 1;
	a[0] = (u32)n; a[1] = (u32)(n >> 32); a[2] = (u32)d; a[3] = (u32)(d >> 32);
	(void)it;
	push_args(c, base, 4, a);
}

static void su_b09d50(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 nb = base + 0x100u, key = base + 0x80u, a[2], n = 2u + rnd(r) % 7u, i, k, start, f = rnd(r);
	u32 q[8];
	int mx = (it & 3) != 0;
	(void)mx;
	for (i = 0; i < 8; i++) q[i] = (rnd(r) % 3u == 0) ? 0xffffu : (rnd(r) % 4u == 0) ? 0u : rnd(r) & 0xffffu;
	for (i = 0; i < 8; i++) GFI(QuantAabbNode, key, w, i) = (u16)q[i];
	GF(QuantAabbTree, base, nodes) = nb; GF(QuantAabbTree, base, nodes_end) = nb + 16u * n;
	for (k = 0; k < n; k++)
		for (i = 0; i < 8; i++) {
			u32 v = rnd(r) % 8u, w;
			switch (v) {
			case 0: case 1: case 2: w = q[i]; break;
			case 3: w = (q[i] + 1u) & 0xffffu; break;
			case 4: w = (q[i] - 1u) & 0xffffu; break;
			case 5: w = 0xffffu; break;
			default: w = rnd(r) & 0xffffu; break;
			}
			if (i == 3) w = k + 1u + rnd(r) % (n - k);                 /* forward only: no cycle */
			if (i == 7 && rnd(r) % 2u) w = 0xffffu;
			GFI(QuantAabbNode, nb + 16u * k, w, i) = (u16)w;
		}
	/* the guard bits (any of the 8 combinations, and a stray high bit) and the statics: all-ones / zero / random words */
	B09D50_FLAGS = f & 0x87u;
	for (i = 0; i < 3; i++) {
		u32 j, m = rnd(r) % 4u;
		for (j = 0; j < 8; j++) GFI(QuantAabbNode, G_B09D50_STATICS + 16u * i, w, j) = (u16)(m == 0 ? 0xffffu : m == 1 ? 0u : rnd(r) & 0xffffu);
	}
	start = nb + 16u * (rnd(r) % (n + 1u));
	a[0] = start; a[1] = key; push_args(c, base, 2, a); c->ecx = base;
}

/* 00b8edb0 (thiscall, ret 0x14): this -> a u16 index A; args (out, tab, scale, ent, vec): a table of 8-byte entries ent[A] = 4 u16 (the
 * first two and the fifth word, +4, are indices into the float table tab after * scale), 3 rows of 4 floats gathered and blended with
 * the vec's lanes by compare masks, three rows stored at out. The test keeps every index small so the gathers stay in the scratch. */
static void su_b8edb0(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	u32 t1 = base + 0x10u, vec = base + 0x40u, tab = base + 0x100u, o, a[5], i, k;
	int mx = (it & 3) != 0;
	GF(TableSelector, base, index) = (u16)(rnd(r) % 4u);
	for (k = 0; k < 4; k++) {
		GF(TableEntry8, t1 + 8u * k, idx0) = (u16)(rnd(r) % 13u); GF(TableEntry8, t1 + 8u * k, idx1) = (u16)(rnd(r) % 13u);
		GF(TableEntry8, t1 + 8u * k, idx2) = (u16)(rnd(r) % 13u); GF(TableEntry8, t1 + 8u * k, unk_06) = (u16)rnd(r);
	}
	fillf(vec, 4, r, mx); fillf(tab, 0xc0, r, mx); fillf(base + 0x80, 12, r, mx);
	o = pick_out(r, base + 0x80, vec, vec + 8u);
	(void)i;
	a[0] = o; a[1] = tab; a[2] = rnd(r) % 8u; a[3] = t1; a[4] = vec;
	push_args(c, base, 5, a); c->ecx = base;
}

static void su_color(XCtx *c, u32 base, unsigned long long *r, unsigned it) {
	static const u32 fcws[] = {0x27fu, 0x37fu, 0x7fu, 0xf7fu, 0x1f7fu, 0x27fu};
	u32 i;
	int mx = (it & 3) != 0;
	for (i = 0; i < 4; i++) {
		u32 v = rnd(r) % 6u;
		/* mostly byte-range values (what the game feeds it), sometimes anything (negative, > 255, huge, NaN, inf) */
		if (v < 3) { float t = (float)(rnd(r) % 2560u) / 10.0f; if (rnd(r) & 1u) t = (float)(int)t; memcpy(&v, &t, 4); GARR32(base, i) = v; }
		else GARR32(base, i) = fbits(r, mx);
	}
	c->fcw = fcws[rnd(r) % 6u];
	push_args(c, base, 0, (const u32 *)0); c->ecx = base;
}

static unsigned long long n_pair, n_bad_pair;
static void report_line(Spec *s) {
	fprintf(stderr, "xrc: ovr_math %08x %-36s %llu calls, %llu mismatches, %llu NaN-payload-only\n", s->addr, s->name, s->calls, s->bad, s->nanonly);
}
static void live_check(Spec *s, XCtx *c) {
	u32 at[2] = {0, 0}, len[2] = {0, 0};
	char why[256];
	int bad;
	s->region(c, at, len);
	bad = run_pair(s, c, at, len, s->nreg, why, sizeof why);
	s->calls++; n_pair++;
	if (bad == 1) s->nanonly++;
	if (bad == 2) {
		s->bad++; n_bad_pair++;
		if (n_bad_pair <= 10) fprintf(stderr, "xrc: OVR_MATH MISMATCH %08x (%s):%s\n", s->addr, s->name, why);
	}
	if ((n_pair & 0xfffff) == 0) {
		unsigned i;
		fprintf(stderr, "xrc: ovr_math check: %llu calls, %llu mismatches\n", n_pair, n_bad_pair);
		for (i = 0; i < sizeof specs / sizeof specs[0]; i++) report_line(&specs[i]);
	}
}

/* per-call speed (part of the self-test): ours against the translation, on the same random moderate inputs; the cost of
 * generating the inputs and copying the register context is measured once and subtracted */
int __stdcall QueryPerformanceCounter(long long *);
int __stdcall QueryPerformanceFrequency(long long *);
static double bench_once(Spec *s, XCtx *c0, u32 base, int which, unsigned n) {
	unsigned long long rng = 0x1234567ull;
	long long f, t0, t1;
	unsigned i;
	static u8 snap[0x400];
	XCtx cs = *c0;
	memset(XM + base, 0, 0x400);
	s->setup(&cs, base, &rng, 0);
	memcpy(snap, XM + base, 0x400);
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	for (i = 0; i < n; i++) {
		XCtx c = cs;
		memcpy(XM + base, snap, 0x400);
		if (which == 1) s->impl(&c);
		else if (which == 2) s->orig(&c);
		else __asm__ __volatile__("" : : "r"(&c) : "memory");
	}
	QueryPerformanceCounter(&t1);
	return (double)(t1 - t0) * 1e9 / (double)f / (double)n;
}
/* the three loops (empty, translation, ours) are timed interleaved, XRC_OVR_MATH_BENCH_R times (default 21), and each one's minimum is
 * taken, so a slow patch of the machine hits all three alike */
static void bench_all(XCtx *c0, u32 base) {
	unsigned n = getenv("XRC_OVR_MATH_BENCH_N") ? (unsigned)atoi(getenv("XRC_OVR_MATH_BENCH_N")) : 200000u;
	int reps = getenv("XRC_OVR_MATH_BENCH_R") ? atoi(getenv("XRC_OVR_MATH_BENCH_R")) : 21, r;
	unsigned k;
	for (k = 0; k < sizeof specs / sizeof specs[0]; k++) {
		Spec *s = &specs[k];
		double b = 1e30, o = 1e30, v = 1e30, t;
		for (r = 0; r < reps; r++) {
			g_mode = 0; t = bench_once(s, c0, base, 0, n); if (t < b) b = t;
			t = bench_once(s, c0, base, 2, n); if (t < o) o = t;
			g_mode = 1; t = bench_once(s, c0, base, 1, n); if (t < v) v = t;
		}
		o -= b; v -= b;
		fprintf(stderr, "xrc: ovr_math bench %08x %-36s translation %7.1f ns/call, native %7.1f ns/call (%.2fx)\n", s->addr, s->name, o, v, v > 0.01 ? o / v : 0.0);
	}
}

/* the self-test (XRC_OVR_MATH_TEST=1): scratch memory in the dead guest stack under the calling thread's esp */
static void selftest(XCtx *c0) {
	unsigned long long rng = 0x9e3779b97f4a7c15ull;
	int total_bad = 0;
	nan1 = getenv("XRC_OVR_MATH_NAN1") && getenv("XRC_OVR_MATH_NAN1")[0] == '1';
	unsigned i, n = getenv("XRC_OVR_MATH_N") ? (unsigned)atoi(getenv("XRC_OVR_MATH_N")) : 200000u;
	u32 base = (c0->esp - 0x8000u) & ~0xffu;
	unsigned k;
	fprintf(stderr, "xrc: ovr_math self-test: %u random inputs per function, scratch at %08x\n", n, base);
	if (base < xrc_stack_lo + 0x1000u) { fprintf(stderr, "xrc: ovr_math self-test: not enough stack here, skipped\n"); return; }
	for (k = 0; k < sizeof specs / sizeof specs[0]; k++) {
		Spec *s = &specs[k];
		unsigned long long bad = 0, nanonly = 0;
		char why[256], first[256], firstn[256];
		first[0] = firstn[0] = 0;
		for (i = 0; i < n; i++) {
			XCtx c = *c0;
			u32 at[1] = {base}, len[1] = {0x400};
			memset(XM + base, 0, 0x400);
			s->setup(&c, base, &rng, i);
			int d = run_pair(s, &c, at, len, 1, why, sizeof why);
			if (d == 2) {
				if (!bad) snprintf(first, sizeof first, "iter %u:%s", i, why);
				bad++;
			} else if (d == 1) {
				if (!nanonly) snprintf(firstn, sizeof firstn, "iter %u:%s", i, why);
				nanonly++;
			}
		}
		s->calls = n; s->bad = bad; s->nanonly = nanonly;
		fprintf(stderr, "xrc: ovr_math test %08x %-36s %u inputs: %llu exact-match failures, %llu NaN-payload-only differences, %llu identical\n",
			s->addr, s->name, n, bad, nanonly, (unsigned long long)n - bad - nanonly);
		if (bad) { fprintf(stderr, "xrc:   %llu mismatches\n", bad); total_bad++; }
	}
	fprintf(stderr, "xrc: ovr_math self-test: %s\n", total_bad ? "FAILED" : "all PASS");
	if (!(getenv("XRC_OVR_MATH_BENCH") && getenv("XRC_OVR_MATH_BENCH")[0] == '0')) bench_all(c0, base);
	fflush(stderr);
	_exit(total_bad ? 1 : 0);
}

static void init_mode(XCtx *c) {
	const char *e = getenv("XRC_OVR_MATH");
	g_mode = (e && e[0] == '0') ? 0 : 1;
	if (g_mode && getenv("XRC_OVR_MATH_CHECK") && getenv("XRC_OVR_MATH_CHECK")[0] == '1') g_mode = 2;
	g_inited = 1;
	fprintf(stderr, "xrc: ovr_math: %s\n", g_mode == 0 ? "off (translations)" : g_mode == 2 ? "on, checked against the translations" : "on");
	if (getenv("XRC_OVR_MATH_TEST") && getenv("XRC_OVR_MATH_TEST")[0] == '1') selftest(c);
}

static void slow(Spec *s, XCtx *c) {
	if (!g_inited) init_mode(c);
	if (g_mode == 1) s->impl(c);
	else if (g_mode == 2) live_check(s, c);
	else s->orig(c);
}

/* the symbols every caller reaches */
#define OVR(addr_, idx_, gen_) void XRC_CC xo_##addr_(XCtx *c) { if (__builtin_expect(g_mode == 1, 1)) gen_(c); else if (g_mode == 0) xf_##addr_(c); else slow(&specs[idx_], c); }
OVR(00408210, S_TV, gen_TransformVector4)
OVR(0040acb0, S_MUL, gen_Multiply)
OVR(0040ab00, S_NORM, gen_Normalize)
OVR(0040b1e0, S_COPY, gen_Copy4x4)
OVR(00416140, S_F140, gen_Fn00416140)
OVR(004164f0, S_F4F0, gen_Fn004164f0)
OVR(00416020, S_SINCOS, gen_SinCos4)
OVR(00416250, S_F250, gen_Fn00416250)
OVR(00cf1a20, S_DIV, gen_aulldiv)
OVR(005a44e0, S_TRANSPOSE, gen_Transpose)
OVR(004bb0b0, S_EXPAND, gen_ExpandByPoint)
OVR(004bada0, S_TPOINT, gen_TransformPoint)
OVR(00437270, S_MKTRANS, gen_MakeTranslation)
OVR(00408320, S_INVERSE, gen_Inverse)
OVR(004bb1c0, S_TBM, gen_TransformByMatrix)
OVR(00416a30, S_GRQ, gen_GetRotationQuat)
OVR(00aac130, S_COMPOSE, gen_ComposeLocalToWorld)
OVR(00b09d50, S_B09D50, gen_Fn00b09d50)
OVR(00b8edb0, S_B8EDB0, gen_Fn00b8edb0)
OVR(0040abc0, S_COLOR, gen_ToPackedColor8888)

/* the dispatcher's view of the same overrides (indirect calls, inline caches): the xo_ entries */
#define MAN(addr_, nm_) {0x##addr_##u, xo_##addr_, nm_}
const XrcManual xrc_manual_math[] = {
	MAN(00408210, "Matrix4x4::TransformVector4"), MAN(0040acb0, "Matrix4x4::Multiply"), MAN(0040ab00, "Vector4::Normalize"),
	MAN(0040b1e0, "Matrix4x4 copy"), MAN(00416140, "Vector4 op"), MAN(004164f0, "Matrix from quaternion"),
	MAN(00416020, "sincos x4"), MAN(00416250, "Vector4 op"), MAN(00cf1a20, "_aulldiv"), MAN(005a44e0, "Matrix4x4::Transpose"),
	MAN(004bb0b0, "Aabb::ExpandByPoint"), MAN(004bada0, "Matrix4x4::TransformPoint"), MAN(00437270, "Matrix4x4::MakeTranslation"),
	MAN(00408320, "Matrix4x4::Inverse"), MAN(004bb1c0, "Aabb::TransformByMatrix"), MAN(00416a30, "JointExpression::GetRotationQuat"),
	MAN(00aac130, "Transform::ComposeLocalToWorld"), MAN(00b09d50, "AabbTree query"), MAN(00b8edb0, "Dequantize 3 rows"),
	MAN(0040abc0, "Vector4::ToPackedColor8888"),
};
const unsigned xrc_manual_math_n = sizeof xrc_manual_math / sizeof xrc_manual_math[0];
