/* vk_sm3.c - D3D9 vs_3_0 / ps_3_0 bytecode to SPIR-V 1.0 (the interface: vk_sm3.h).
 *
 * A direct translation, no optimisation (the driver's compiler does that): every D3D register is a vec4 variable
 * (temporaries and the address register Function-scope, outputs Private and copied to the real outputs at the end),
 * every instruction loads its sources (swizzle, modifiers), computes, and stores through its write mask. Flow control
 * (if / ifc / else / endif, rep / endrep, break / breakc, texkill) becomes SPIR-V's structured constructs.
 *
 * What FF13's shaders use is translated (tools: XRC_D3D_SURVEY dumps them); the rest (loop / aL, call / label,
 * predicates, most source modifiers, 1.x / 2.x shaders) is refused with its name, so it shows up rather than draws
 * wrongly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "vk_sm3.h"

typedef uint32_t u32;

/* ============================================================================================ SPIR-V */
enum {
	OpName = 5, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
	OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
	OpTypeImage = 25, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32,
	OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44,
	OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
	OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80,
	OpCompositeExtract = 81, OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88, OpConvertFToS = 110,
	OpConvertSToF = 111, OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136,
	OpVectorTimesScalar = 142, OpDot = 148, OpAny = 154, OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168,
	OpSelect = 169, OpIEqual = 170, OpINotEqual = 171, OpSLessThan = 177,
	OpFOrdEqual = 180, OpFUnordNotEqual = 183, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186,
	OpFOrdLessThanEqual = 188, OpFOrdGreaterThanEqual = 190, OpDPdx = 207, OpDPdy = 208, OpLoopMerge = 246,
	OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpKill = 252, OpReturn = 253,
};
enum { GFAbs = 4, GFloor = 8, GFract = 10, GSin = 13, GCos = 14, GPow = 26, GExp2 = 29, GLog2 = 30, GInverseSqrt = 32,
       GFMin = 37, GFMax = 40, GSClamp = 45, GFClamp = 43, GFMix = 46 };
enum { SC_UniformConstant = 0, SC_Input = 1, SC_Uniform = 2, SC_Output = 3, SC_Private = 6, SC_Function = 7, SC_PushConstant = 9 };
enum { D_Block = 2, D_ArrayStride = 6, D_BuiltIn = 11, D_Centroid = 16, D_Location = 30, D_Binding = 33, D_DescriptorSet = 34, D_Offset = 35 };
enum { B_Position = 0, B_FragCoord = 15, B_FrontFacing = 17, B_FragDepth = 22 };

typedef struct { u32 *w; u32 n, cap; } Buf;
static void put(Buf *b, u32 v) {
	if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 256; b->w = (u32 *)realloc(b->w, b->cap * 4); }
	b->w[b->n++] = v;
}
static void emitv(Buf *b, u32 op, const u32 *a, u32 n) {
	u32 k;
	put(b, (n + 1) << 16 | op);
	for (k = 0; k < n; k++) put(b, a[k]);
}
#define E(b, op, ...) emitv((b), (op), (const u32[]){__VA_ARGS__}, (u32)(sizeof((const u32[]){__VA_ARGS__}) / 4))
#define E0(b, op) emitv((b), (op), 0, 0)
static void emit_str(Buf *b, u32 op, u32 pre, const u32 *a, u32 na, const char *s, const u32 *post, u32 npost) {
	u32 len = (u32)strlen(s) + 1, nw = (len + 3) / 4, k;
	put(b, (1 + na + nw + npost) << 16 | op);
	(void)pre;
	for (k = 0; k < na; k++) put(b, a[k]);
	for (k = 0; k < nw; k++) {
		u32 v = 0, j;
		for (j = 0; j < 4; j++) if (k * 4 + j < len) v |= (u32)(unsigned char)s[k * 4 + j] << (8 * j);
		put(b, v);
	}
	for (k = 0; k < npost; k++) put(b, post[k]);
}

/* ============================================================================================ the translator */
typedef struct { int type; u32 merge, els, cont, header, counter; int has_else; } Flow;

typedef struct {
	Buf caps, ext, ent, modes, deco, types, fvars, init, code;
	u32 id;
	int ps, failed, no_alpha_test, no_depth_out;
	char *err;
	int errlen;
	u32 glsl;
	/* types */
	u32 t_void, t_bool, t_int, t_float, t_v2, t_v3, t_v4, t_iv4, t_bv4, t_fn;
	u32 ptr_cache[64][3]; int nptr;
	u32 c_cache[256][3]; int nc;   /* scalar constants: bits, type, id */
	u32 v_cache[128][5]; int nv;   /* vec4 float constants */
	/* registers */
	u32 r[32], a0, in[16], out[12], oc[4], odepth, fragcoord, frontfacing;
	uint8_t out_usage[12], out_index[12], out_dcl[12];
	u32 ubo, push, samp[20], samp_t[20], img_dim_t[5]; /* (image / sampled image types per kind: declared once) */
	u32 ubo_ptr_v4, ubo_ptr_iv4, push_ptr_v4, push_ptr_i, push_ptr_f;
	int def_f[256], def_i[16], def_b[16];
	float def_fv[256][4]; int def_iv[16][4]; u32 def_bv[16];
	int written_oc;
	Flow flow[32]; int nflow;
	Sm3Info *info;
	u32 iface[64]; int niface;
} T;

static void fail(T *t, const char *fmt, ...) {
	va_list ap;
	if (t->failed) return;
	t->failed = 1;
	va_start(ap, fmt);
	vsnprintf(t->err, (size_t)t->errlen, fmt, ap);
	va_end(ap);
}
static u32 nid(T *t) { return t->id++; }

static u32 ptr(T *t, u32 sc, u32 type) {
	int k;
	u32 id;
	for (k = 0; k < t->nptr; k++) if (t->ptr_cache[k][0] == sc && t->ptr_cache[k][1] == type) return t->ptr_cache[k][2];
	id = nid(t);
	E(&t->types, OpTypePointer, id, sc, type);
	if (t->nptr < 64) { t->ptr_cache[t->nptr][0] = sc; t->ptr_cache[t->nptr][1] = type; t->ptr_cache[t->nptr][2] = id; t->nptr++; }
	return id;
}
/* constants: cached by value and type */
static u32 cache_c(T *t, u32 type, u32 bits) {
	int k;
	u32 id;
	for (k = 0; k < t->nc; k++) if (t->c_cache[k][0] == bits && t->c_cache[k][1] == type) return t->c_cache[k][2];
	id = nid(t);
	E(&t->types, OpConstant, type, id, bits);
	if (t->nc < 256) { t->c_cache[t->nc][0] = bits; t->c_cache[t->nc][1] = type; t->c_cache[t->nc][2] = id; t->nc++; }
	return id;
}
static u32 cf(T *t, float f) { u32 b; memcpy(&b, &f, 4); return cache_c(t, t->t_float, b); }
static u32 ci(T *t, int i) { return cache_c(t, t->t_int, (u32)i); }
static u32 cv4(T *t, float x, float y, float z, float w) {
	float v[4] = {x, y, z, w};
	u32 b[4], id;
	int k;
	memcpy(b, v, 16);
	for (k = 0; k < t->nv; k++) if (!memcmp(t->v_cache[k], b, 16)) return t->v_cache[k][4];
	{
		u32 c0 = cf(t, x), c1 = cf(t, y), c2 = cf(t, z), c3 = cf(t, w);
		id = nid(t);
		E(&t->types, OpConstantComposite, t->t_v4, id, c0, c1, c2, c3);
	}
	if (t->nv < 128) { memcpy(t->v_cache[t->nv], b, 16); t->v_cache[t->nv][4] = id; t->nv++; }
	return id;
}

/* ---------------------------------------------------------------------------------------- value helpers */
static u32 load(T *t, u32 type, u32 var) { u32 id = nid(t); E(&t->code, OpLoad, type, id, var); return id; }
static u32 un(T *t, u32 op, u32 type, u32 a) { u32 id = nid(t); E(&t->code, op, type, id, a); return id; }
static u32 bin(T *t, u32 op, u32 type, u32 a, u32 b) { u32 id = nid(t); E(&t->code, op, type, id, a, b); return id; }
static u32 g1(T *t, u32 f, u32 type, u32 a) { u32 id = nid(t); E(&t->code, OpExtInst, type, id, t->glsl, f, a); return id; }
static u32 g2(T *t, u32 f, u32 type, u32 a, u32 b) { u32 id = nid(t); E(&t->code, OpExtInst, type, id, t->glsl, f, a, b); return id; }
static u32 g3(T *t, u32 f, u32 type, u32 a, u32 b, u32 c) { u32 id = nid(t); E(&t->code, OpExtInst, type, id, t->glsl, f, a, b, c); return id; }
static u32 shuf4(T *t, u32 type, u32 a, u32 b, u32 x, u32 y, u32 z, u32 w) { u32 id = nid(t); E(&t->code, OpVectorShuffle, type, id, a, b, x, y, z, w); return id; }
static u32 ext(T *t, u32 type, u32 v, u32 k) { u32 id = nid(t); E(&t->code, OpCompositeExtract, type, id, v, k); return id; }
static u32 splat(T *t, u32 s) { u32 id = nid(t); E(&t->code, OpCompositeConstruct, t->t_v4, id, s, s, s, s); return id; }
static u32 xyz(T *t, u32 v) { u32 id = nid(t); E(&t->code, OpVectorShuffle, t->t_v3, id, v, v, 0, 1, 2); return id; }
static u32 xy(T *t, u32 v) { u32 id = nid(t); E(&t->code, OpVectorShuffle, t->t_v2, id, v, v, 0, 1); return id; }
static void label(T *t, u32 l) { E(&t->code, OpLabel, l); }

static u32 fvar(T *t, u32 type) {
	u32 id = nid(t);
	E(&t->fvars, OpVariable, ptr(t, SC_Function, type), id, SC_Function);
	return id;
}
static u32 gvar(T *t, u32 sc, u32 type) {
	u32 id = nid(t);
	E(&t->types, OpVariable, ptr(t, sc, type), id, sc);
	if ((sc == SC_Input || sc == SC_Output) && t->niface < 64) t->iface[t->niface++] = id;
	return id;
}
static void deco(T *t, u32 id, u32 d, u32 v) { E(&t->deco, OpDecorate, id, d, v); }

/* ---------------------------------------------------------------------------------------- registers */
#define RTYPE(tok) ((((tok) >> 28) & 7) | (((tok) >> 8) & 0x18))
#define RNUM(tok) ((tok) & 0x7ff)
enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_ADDR = 3, R_TEXTURE = 3, R_OUTPUT = 6, R_CONSTINT = 7, R_COLOROUT = 8,
       R_DEPTHOUT = 9, R_SAMPLER = 10, R_CONSTBOOL = 14, R_LOOP = 15, R_MISCTYPE = 17, R_PREDICATE = 19 };

static u32 temp(T *t, u32 n) {
	if (n >= 32) { fail(t, "temporary r%u", n); n = 0; }
	if (!t->r[n]) {
		t->r[n] = fvar(t, t->t_v4);
		E(&t->init, OpStore, t->r[n], cv4(t, 0, 0, 0, 0));
	}
	return t->r[n];
}
static u32 addr_var(T *t) {
	if (!t->a0) {
		u32 z = ci(t, 0), zv = nid(t);
		E(&t->types, OpConstantComposite, t->t_iv4, zv, z, z, z, z);
		t->a0 = fvar(t, t->t_iv4);
		E(&t->init, OpStore, t->a0, zv);
	}
	return t->a0;
}
static u32 priv_v4(T *t) {
	u32 id = nid(t);
	E(&t->types, OpVariable, ptr(t, SC_Private, t->t_v4), id, SC_Private, cv4(t, 0, 0, 0, 0));
	return id;
}
static u32 out_var(T *t, u32 n) {
	if (n >= 12) { fail(t, "output o%u", n); n = 0; }
	if (!t->out[n]) t->out[n] = priv_v4(t);
	return t->out[n];
}
static u32 oc_var(T *t, u32 n) {
	if (n >= 4) { fail(t, "colour output oC%u", n); n = 0; }
	if (!t->oc[n]) t->oc[n] = priv_v4(t);
	t->written_oc |= 1 << n;
	return t->oc[n];
}
static u32 ubo_c(T *t, u32 index_id) {
	u32 p = nid(t);
	E(&t->code, OpAccessChain, t->ubo_ptr_v4, p, t->ubo, ci(t, 0), index_id);
	return load(t, t->t_v4, p);
}
static u32 int_reg(T *t, u32 n) { /* i# as ivec4 */
	if (n >= 16) { fail(t, "integer constant i%u", n); n = 0; }
	if (t->def_i[n]) {
		u32 id = nid(t), a = ci(t, t->def_iv[n][0]), b = ci(t, t->def_iv[n][1]), c = ci(t, t->def_iv[n][2]), d = ci(t, t->def_iv[n][3]);
		E(&t->code, OpCompositeConstruct, t->t_iv4, id, a, b, c, d);
		return id;
	} else {
		u32 p = nid(t);
		E(&t->code, OpAccessChain, t->ubo_ptr_iv4, p, t->ubo, ci(t, 1), ci(t, (int)n));
		return load(t, t->t_iv4, p);
	}
}
static u32 bool_reg(T *t, u32 n) { /* b# as bool */
	if (n >= 16) { fail(t, "boolean constant b%u", n); n = 0; }
	if (t->def_b[n]) {
		u32 id = nid(t);
		E(&t->types, t->def_bv[n] ? OpConstantTrue : OpConstantFalse, t->t_bool, id);
		return id;
	}
	{
		u32 p = nid(t), v, x;
		E(&t->code, OpAccessChain, t->ubo_ptr_iv4, p, t->ubo, ci(t, 2), ci(t, (int)n));
		v = load(t, t->t_iv4, p);
		x = ext(t, t->t_int, v, 0);
		return bin(t, OpINotEqual, t->t_bool, x, ci(t, 0));
	}
}

/* a source operand (and its relative-address token) as a vec4: register, swizzle, modifier */
static u32 src(T *t, u32 tok, u32 rel) {
	u32 type = RTYPE(tok), n = RNUM(tok), v = 0, sw = (tok >> 16) & 0xff, mod = (tok >> 24) & 0xf;
	switch (type) {
	case R_TEMP: v = load(t, t->t_v4, temp(t, n)); break;
	case R_INPUT:
		if (n >= 16 || !t->in[n]) { fail(t, "input v%u not declared", n); return cv4(t, 0, 0, 0, 0); }
		v = load(t, t->t_v4, t->in[n]);
		break;
	case R_CONST:
		if (tok & (1u << 13)) { /* c[a0.comp + n] */
			u32 a, comp, idx;
			if (RTYPE(rel) != R_ADDR || t->ps) { fail(t, "relative addressing with register type %u", RTYPE(rel)); return cv4(t, 0, 0, 0, 0); }
			comp = (rel >> 16) & 3;
			a = load(t, t->t_iv4, addr_var(t));
			idx = bin(t, OpIAdd, t->t_int, ext(t, t->t_int, a, comp), ci(t, (int)n));
			idx = g3(t, GSClamp, t->t_int, idx, ci(t, 0), ci(t, 255));
			v = ubo_c(t, idx);
			t->info->max_const = 256;
		} else if (n < 256 && t->def_f[n]) {
			v = cv4(t, t->def_fv[n][0], t->def_fv[n][1], t->def_fv[n][2], t->def_fv[n][3]);
		} else {
			if (n >= 256) { fail(t, "constant c%u", n); return cv4(t, 0, 0, 0, 0); }
			v = ubo_c(t, ci(t, (int)n));
			if (n + 1 > t->info->max_const) t->info->max_const = n + 1;
		}
		break;
	case R_ADDR:
		if (t->ps) { fail(t, "texture register t%u in a pixel shader", n); return cv4(t, 0, 0, 0, 0); }
		{ u32 a = load(t, t->t_iv4, addr_var(t)); v = un(t, OpConvertSToF, t->t_v4, a); }
		break;
	case R_CONSTINT: v = un(t, OpConvertSToF, t->t_v4, int_reg(t, n)); break;
	case R_MISCTYPE:
		if (!t->ps) { fail(t, "misc register in a vertex shader"); return cv4(t, 0, 0, 0, 0); }
		if (n == 0) { /* vPos: the pixel's top-left corner (D3D9: no half-pixel) */
			u32 fc = load(t, t->t_v4, t->fragcoord);
			v = bin(t, OpFSub, t->t_v4, fc, cv4(t, 0.5f, 0.5f, 0, 0));
		} else if (n == 1) { /* vFace: +1 front, -1 back */
			u32 ff = load(t, t->t_bool, t->frontfacing), s = nid(t);
			E(&t->code, OpSelect, t->t_float, s, ff, cf(t, 1.0f), cf(t, -1.0f));
			v = splat(t, s);
		} else { fail(t, "misc register %u", n); return cv4(t, 0, 0, 0, 0); }
		break;
	default: fail(t, "source register type %u", type); return cv4(t, 0, 0, 0, 0);
	}
	if (sw != 0xe4) v = shuf4(t, t->t_v4, v, v, sw & 3, (sw >> 2) & 3, (sw >> 4) & 3, (sw >> 6) & 3);
	switch (mod) {
	case 0: break;
	case 1: v = un(t, OpFNegate, t->t_v4, v); break;
	case 11: v = g1(t, GFAbs, t->t_v4, v); break;
	case 12: v = un(t, OpFNegate, t->t_v4, g1(t, GFAbs, t->t_v4, v)); break;
	default: fail(t, "source modifier %u", mod);
	}
	return v;
}
static u32 scalar(T *t, u32 v) { return ext(t, t->t_float, v, 0); }

/* the result to a destination operand: shift, saturate, write mask */
static void dst(T *t, u32 tok, u32 v) {
	u32 type = RTYPE(tok), n = RNUM(tok), mask = (tok >> 16) & 0xf, mod = (tok >> 20) & 0xf, shift = (tok >> 24) & 0xf, var = 0;
	if (shift) {
		float f = shift < 8 ? (float)(1 << shift) : 1.0f / (float)(1 << (16 - shift));
		v = bin(t, OpVectorTimesScalar, t->t_v4, v, cf(t, f));
	}
	if (mod & 1) v = g3(t, GFClamp, t->t_v4, v, cv4(t, 0, 0, 0, 0), cv4(t, 1, 1, 1, 1));
	if (mod & ~7u) fail(t, "destination modifier %x", mod);
	switch (type) {
	case R_TEMP: var = temp(t, n); break;
	case R_OUTPUT: if (t->ps) { fail(t, "output register in a pixel shader"); return; } var = out_var(t, n); break;
	case R_COLOROUT: var = oc_var(t, n); break;
	case R_DEPTHOUT:
		if (!t->odepth) t->odepth = priv_v4(t);
		var = t->odepth;
		t->info->writes_depth = 1;
		break;
	case R_ADDR:
		if (t->ps) { fail(t, "texture register write"); return; }
		{ /* (mova: here only through mova) */
			u32 r = g1(t, GFloor, t->t_v4, bin(t, OpFAdd, t->t_v4, v, cv4(t, 0.5f, 0.5f, 0.5f, 0.5f)));
			u32 iv = un(t, OpConvertFToS, t->t_iv4, r), a = addr_var(t);
			if (mask != 0xf) {
				u32 old = load(t, t->t_iv4, a);
				iv = shuf4(t, t->t_iv4, old, iv, mask & 1 ? 4 : 0, mask & 2 ? 5 : 1, mask & 4 ? 6 : 2, mask & 8 ? 7 : 3);
			}
			E(&t->code, OpStore, a, iv);
		}
		return;
	default: fail(t, "destination register type %u", type); return;
	}
	if (mask != 0xf) {
		u32 old = load(t, t->t_v4, var);
		v = shuf4(t, t->t_v4, old, v, mask & 1 ? 4 : 0, mask & 2 ? 5 : 1, mask & 4 ? 6 : 2, mask & 8 ? 7 : 3);
	}
	E(&t->code, OpStore, var, v);
}

/* comparisons of ifc / breakc (on .x): 1 gt 2 eq 3 ge 4 lt 5 ne 6 le */
static u32 compare(T *t, u32 ctrl, u32 a, u32 b) {
	u32 x = scalar(t, a), y = scalar(t, b), op;
	switch (ctrl) {
	case 1: op = OpFOrdGreaterThan; break;
	case 2: op = OpFOrdEqual; break;
	case 3: op = OpFOrdGreaterThanEqual; break;
	case 4: op = OpFOrdLessThan; break;
	case 5: op = OpFUnordNotEqual; break;
	case 6: op = OpFOrdLessThanEqual; break;
	default: fail(t, "comparison %u", ctrl); op = OpFOrdEqual;
	}
	return bin(t, op, t->t_bool, x, y);
}

static Flow *inner_loop(T *t) {
	int k;
	for (k = t->nflow - 1; k >= 0; k--) if (t->flow[k].type == 2) return &t->flow[k];
	return 0;
}
/* if (cond) { <kill or break to target> } */
static void cond_exit(T *t, u32 cond, u32 target) {
	u32 th = nid(t), m = nid(t);
	E(&t->code, OpSelectionMerge, m, 0);
	E(&t->code, OpBranchConditional, cond, th, m);
	label(t, th);
	if (target) E(&t->code, OpBranch, target);
	else E0(&t->code, OpKill);
	label(t, m);
}

/* the sampler of s# (declared) */
static u32 sampler_of(T *t, u32 tok, int *dimk) {
	u32 n = RNUM(tok), s = t->ps ? n : 16 + n;
	if (RTYPE(tok) != R_SAMPLER || s >= 20 || !t->samp[s]) { fail(t, "sampler s%u not declared", n); return 0; }
	*dimk = t->info->sampler[s];
	return load(t, t->samp_t[s], t->samp[s]);
}
static u32 tex_coord(T *t, u32 v, int dimk) { return dimk == 2 ? xy(t, v) : xyz(t, v); }

/* ---------------------------------------------------------------------------------------- declarations */
static void declare(T *t, u32 d, u32 r) {
	u32 type = RTYPE(r), n = RNUM(r), usage = d & 0x1f, index = (d >> 16) & 0xf;
	if (type == R_SAMPLER) {
		u32 kind = (d >> 27) & 0xf, s = t->ps ? n : 16 + n, dim, img;
		if (s >= 20) { fail(t, "sampler s%u", n); return; }
		dim = kind == 2 ? 1 : kind == 3 ? 3 : kind == 4 ? 2 : 99;
		if (dim == 99) { fail(t, "sampler type %u", kind); return; }
		if (!t->img_dim_t[kind]) {
			img = nid(t);
			E(&t->types, OpTypeImage, img, t->t_float, dim, 0, 0, 0, 1, 0);
			t->img_dim_t[kind] = nid(t);
			E(&t->types, OpTypeSampledImage, t->img_dim_t[kind], img);
		}
		t->samp_t[s] = t->img_dim_t[kind];
		t->samp[s] = gvar(t, SC_UniformConstant, t->samp_t[s]);
		deco(t, t->samp[s], D_DescriptorSet, 0);
		deco(t, t->samp[s], D_Binding, 2 + s);
		t->info->sampler[s] = (uint8_t)kind;
		return;
	}
	if (type == R_INPUT) {
		if (n >= 16) { fail(t, "input v%u", n); return; }
		t->in[n] = gvar(t, SC_Input, t->t_v4);
		t->info->in_mask |= 1u << n;
		t->info->in_usage[n] = (uint8_t)usage;
		t->info->in_index[n] = (uint8_t)index;
		if (t->ps) {
			int loc = sm3_location((int)usage, (int)index);
			if (loc < 0) { fail(t, "too many interpolated semantics"); return; }
			deco(t, t->in[n], D_Location, (u32)loc);
			if ((r >> 20) & 4) E(&t->deco, OpDecorate, t->in[n], D_Centroid);
		} else deco(t, t->in[n], D_Location, n);
		return;
	}
	if (type == R_OUTPUT && !t->ps) {
		if (n >= 12) { fail(t, "output o%u", n); return; }
		out_var(t, n);
		t->out_usage[n] = (uint8_t)usage;
		t->out_index[n] = (uint8_t)index;
		t->out_dcl[n] = 1;
		return;
	}
	if (type == R_MISCTYPE) return; /* vPos / vFace: their built-ins exist anyway */
	fail(t, "declaration of register type %u", type);
}

/* ============================================================================================ the shader */
int sm3_location(int usage, int index) {
	static int reg[32][2], nreg;
	int k;
	if (usage == 5 && index < 10) return index;       /* TEXCOORD0..9 */
	if (usage == 10 && index < 2) return 10 + index;  /* COLOR0..1 */
	if (usage == 11 && index == 0) return 12;         /* FOG */
	for (k = 0; k < nreg; k++) if (reg[k][0] == usage && reg[k][1] == index) return 13 + k;
	if (13 + nreg >= 32) return -1;
	reg[nreg][0] = usage; reg[nreg][1] = index;
	return 13 + nreg++;
}

int sm3_translate(const uint32_t *w, uint32_t nw, Sm3Info *info, char *err, int errlen) { return sm3_translate_ex(w, nw, info, err, errlen, 0); }
int sm3_translate_ex(const uint32_t *w, uint32_t nw, Sm3Info *info, char *err, int errlen, int flags) {
	T tt, *t = &tt;
	u32 i, fn, main_l, ver, ubo_t, push_t;
	int k;
	memset(t, 0, sizeof *t);
	t->no_alpha_test = (flags & SM3_NO_ALPHA_TEST) != 0;
	t->no_depth_out = (flags & SM3_NO_DEPTH_OUT) != 0;
	memset(info, 0, sizeof *info);
	t->err = err; t->errlen = errlen; t->info = info;
	if (err && errlen) err[0] = 0;
	if (nw < 2) { fail(t, "empty shader"); return 0; }
	ver = w[0];
	t->ps = info->ps = (ver >> 16) == 0xffff;
	if ((ver >> 16) != 0xfffe && (ver >> 16) != 0xffff) { fail(t, "not a shader (version %08x)", ver); return 0; }
	if (((ver >> 8) & 0xff) != 3) { fail(t, "%s_%u_%u: only shader model 3 is translated", t->ps ? "ps" : "vs", (ver >> 8) & 0xff, ver & 0xff); return 0; }
	t->id = 1;
	/* types */
	t->t_void = nid(t); E(&t->types, OpTypeVoid, t->t_void);
	t->t_bool = nid(t); E(&t->types, OpTypeBool, t->t_bool);
	t->t_int = nid(t); E(&t->types, OpTypeInt, t->t_int, 32, 1);
	t->t_float = nid(t); E(&t->types, OpTypeFloat, t->t_float, 32);
	t->t_v2 = nid(t); E(&t->types, OpTypeVector, t->t_v2, t->t_float, 2);
	t->t_v3 = nid(t); E(&t->types, OpTypeVector, t->t_v3, t->t_float, 3);
	t->t_v4 = nid(t); E(&t->types, OpTypeVector, t->t_v4, t->t_float, 4);
	t->t_iv4 = nid(t); E(&t->types, OpTypeVector, t->t_iv4, t->t_int, 4);
	t->t_bv4 = nid(t); E(&t->types, OpTypeVector, t->t_bv4, t->t_bool, 4);
	t->t_fn = nid(t); E(&t->types, OpTypeFunction, t->t_fn, t->t_void);
	t->glsl = nid(t);
	emit_str(&t->ext, OpExtInstImport, 0, &t->glsl, 1, "GLSL.std.450", 0, 0);
	/* the constant block: {vec4 c[256]; ivec4 i[16]; ivec4 b[16];} */
	{
		u32 a256 = nid(t), a16 = nid(t), a16b = nid(t);
		E(&t->types, OpTypeArray, a256, t->t_v4, ci(t, 256));
		E(&t->types, OpTypeArray, a16, t->t_iv4, ci(t, 16));
		E(&t->types, OpTypeArray, a16b, t->t_iv4, ci(t, 16));
		deco(t, a256, D_ArrayStride, 16); deco(t, a16, D_ArrayStride, 16); deco(t, a16b, D_ArrayStride, 16);
		ubo_t = nid(t);
		E(&t->types, OpTypeStruct, ubo_t, a256, a16, a16b);
		E(&t->deco, OpDecorate, ubo_t, D_Block);
		E(&t->deco, OpMemberDecorate, ubo_t, 0, D_Offset, 0);
		E(&t->deco, OpMemberDecorate, ubo_t, 1, D_Offset, 4096);
		E(&t->deco, OpMemberDecorate, ubo_t, 2, D_Offset, 4352);
		t->ubo = gvar(t, SC_Uniform, ubo_t);
		deco(t, t->ubo, D_DescriptorSet, 0);
		deco(t, t->ubo, D_Binding, t->ps ? 1 : 0);
		t->ubo_ptr_v4 = ptr(t, SC_Uniform, t->t_v4);
		t->ubo_ptr_iv4 = ptr(t, SC_Uniform, t->t_iv4);
	}
	/* push constants {vec4 posfix; int alpha_func; float alpha_ref;} */
	push_t = nid(t);
	E(&t->types, OpTypeStruct, push_t, t->t_v4, t->t_int, t->t_float);
	E(&t->deco, OpDecorate, push_t, D_Block);
	E(&t->deco, OpMemberDecorate, push_t, 0, D_Offset, 0);
	E(&t->deco, OpMemberDecorate, push_t, 1, D_Offset, 16);
	E(&t->deco, OpMemberDecorate, push_t, 2, D_Offset, 20);
	t->push = gvar(t, SC_PushConstant, push_t);
	if (t->ps) {
		t->fragcoord = gvar(t, SC_Input, t->t_v4); deco(t, t->fragcoord, D_BuiltIn, B_FragCoord);
		t->frontfacing = gvar(t, SC_Input, t->t_bool); deco(t, t->frontfacing, D_BuiltIn, B_FrontFacing);
	}
	/* pass 1: the constants the shader defines */
	for (i = 1; i < nw;) {
		u32 tok = w[i], op = tok & 0xffff, len = (tok >> 24) & 0xf;
		if (op == 0xffff) break;
		if (op == 0xfffe) { i += 1 + ((tok >> 16) & 0x7fff); continue; }
		if (i + len >= nw + 1) break;
		if (op == 81 && len >= 5) { u32 n = RNUM(w[i + 1]); if (n < 256) { t->def_f[n] = 1; memcpy(t->def_fv[n], &w[i + 2], 16); } }
		if (op == 48 && len >= 5) { u32 n = RNUM(w[i + 1]); if (n < 16) { t->def_i[n] = 1; memcpy(t->def_iv[n], &w[i + 2], 16); } }
		if (op == 47 && len >= 2) { u32 n = RNUM(w[i + 1]); if (n < 16) { t->def_b[n] = 1; t->def_bv[n] = w[i + 2]; } }
		i += 1 + len;
	}
	/* pass 2: the code */
	fn = nid(t);
	main_l = nid(t);
	for (i = 1; i < nw && !t->failed;) {
		u32 tok = w[i], op = tok & 0xffff, len = (tok >> 24) & 0xf, ctrl = (tok >> 16) & 0xff;
		const u32 *a = &w[i + 1];
		u32 s0 = 0, s1 = 0, s2 = 0, v = 0;
		if (op == 0xffff) break;
		if (op == 0xfffe) { i += 1 + ((tok >> 16) & 0x7fff); continue; }
		if (tok & (1u << 28)) { fail(t, "predicated instruction (opcode %u)", op); break; }
		i += 1 + len;
		switch (op) {
		case 0: break; /* nop */
		case 31: if (len >= 2) declare(t, a[0], a[1]); break;
		case 81: case 47: case 48: break; /* (pass 1) */
		default: {
			/* operand positions: tokens after the destination, skipping relative-address tokens */
			u32 pos[4], np = 0, k2;
			int has_dst = !(op == 40 || op == 41 || op == 42 || op == 43 || op == 38 || op == 39 || op == 44 || op == 45 || op == 65);
			u32 first = has_dst ? 1 : 0;
			for (k2 = first; k2 < len && np < 4; k2++) {
				pos[np++] = k2;
				if (a[k2] & (1u << 13) && k2 + 1 < len) k2++;
			}
#define S_(k) src(t, a[pos[(k)]], (a[pos[(k)]] & (1u << 13)) ? a[pos[(k)] + 1] : 0)
			switch (op) {
			case 1: v = S_(0); break;                                                       /* mov */
			case 2: v = bin(t, OpFAdd, t->t_v4, S_(0), S_(1)); break;                       /* add */
			case 3: v = bin(t, OpFSub, t->t_v4, S_(0), S_(1)); break;                       /* sub */
			case 4: s0 = S_(0); s1 = S_(1); s2 = S_(2); v = bin(t, OpFAdd, t->t_v4, bin(t, OpFMul, t->t_v4, s0, s1), s2); break; /* mad */
			case 5: v = bin(t, OpFMul, t->t_v4, S_(0), S_(1)); break;                       /* mul */
			case 6: v = splat(t, bin(t, OpFDiv, t->t_float, cf(t, 1.0f), scalar(t, S_(0)))); break; /* rcp */
			case 7: v = splat(t, g1(t, GInverseSqrt, t->t_float, g1(t, GFAbs, t->t_float, scalar(t, S_(0))))); break; /* rsq */
			case 8: s0 = S_(0); s1 = S_(1); v = splat(t, bin(t, OpDot, t->t_float, xyz(t, s0), xyz(t, s1))); break; /* dp3 */
			case 9: v = splat(t, bin(t, OpDot, t->t_float, S_(0), S_(1))); break;            /* dp4 */
			case 10: v = g2(t, GFMin, t->t_v4, S_(0), S_(1)); break;                         /* min */
			case 11: v = g2(t, GFMax, t->t_v4, S_(0), S_(1)); break;                         /* max */
			case 12: case 13: {                                                              /* slt / sge */
				u32 c = bin(t, op == 12 ? OpFOrdLessThan : OpFOrdGreaterThanEqual, t->t_bv4, S_(0), S_(1));
				v = nid(t);
				E(&t->code, OpSelect, t->t_v4, v, c, cv4(t, 1, 1, 1, 1), cv4(t, 0, 0, 0, 0));
				break;
			}
			case 14: v = splat(t, g1(t, GExp2, t->t_float, scalar(t, S_(0)))); break;       /* exp */
			case 15: v = splat(t, g1(t, GLog2, t->t_float, g1(t, GFAbs, t->t_float, scalar(t, S_(0))))); break; /* log */
			case 18: s0 = S_(0); s1 = S_(1); s2 = S_(2); v = g3(t, GFMix, t->t_v4, s2, s1, s0); break; /* lrp */
			case 19: v = g1(t, GFract, t->t_v4, S_(0)); break;                               /* frc */
			case 32: s0 = S_(0); s1 = S_(1);                                                 /* pow */
				v = splat(t, g2(t, GPow, t->t_float, g1(t, GFAbs, t->t_float, scalar(t, s0)), scalar(t, s1)));
				break;
			case 35: v = g1(t, GFAbs, t->t_v4, S_(0)); break;                                /* abs */
			case 36: s0 = S_(0);                                                             /* nrm */
				v = bin(t, OpVectorTimesScalar, t->t_v4, s0, g1(t, GInverseSqrt, t->t_float, bin(t, OpDot, t->t_float, xyz(t, s0), xyz(t, s0))));
				break;
			case 37: { u32 x = scalar(t, S_(0)), cs = g1(t, GCos, t->t_float, x), sn = g1(t, GSin, t->t_float, x); /* sincos */
				v = nid(t); E(&t->code, OpCompositeConstruct, t->t_v4, v, cs, sn, cf(t, 0), cf(t, 0)); break; }
			case 46: v = S_(0); break;                                                       /* mova (dst: a0) */
			case 88: {                                                                       /* cmp */
				u32 c;
				s0 = S_(0); s1 = S_(1); s2 = S_(2);
				c = bin(t, OpFOrdGreaterThanEqual, t->t_bv4, s0, cv4(t, 0, 0, 0, 0));
				v = nid(t);
				E(&t->code, OpSelect, t->t_v4, v, c, s1, s2);
				break;
			}
			case 90: s0 = S_(0); s1 = S_(1); s2 = S_(2);                                     /* dp2add */
				v = splat(t, bin(t, OpFAdd, t->t_float, bin(t, OpDot, t->t_float, xy(t, s0), xy(t, s1)), scalar(t, s2)));
				break;
			case 91: v = un(t, OpDPdx, t->t_v4, S_(0)); break;                               /* dsx */
			case 92: v = un(t, OpDPdy, t->t_v4, S_(0)); break;                               /* dsy */
			case 66: case 95: case 93: {                                                     /* texld(p/b) / texldl / texldd */
				int dimk = 0;
				u32 coord = S_(0), smp, sw = (a[pos[1]] >> 16) & 0xff, cd;
				smp = sampler_of(t, a[pos[1]], &dimk);
				if (!smp) break;
				if (op == 66 && ctrl == 1) { /* texldp: divided by .w */
					u32 wq = ext(t, t->t_float, coord, 3);
					coord = bin(t, OpVectorTimesScalar, t->t_v4, coord, bin(t, OpFDiv, t->t_float, cf(t, 1.0f), wq));
				}
				cd = tex_coord(t, coord, dimk);
				v = nid(t);
				if (op == 95 || !t->ps) /* explicit lod: .w */
					E(&t->code, OpImageSampleExplicitLod, t->t_v4, v, smp, cd, 2, op == 95 ? ext(t, t->t_float, coord, 3) : cf(t, 0));
				else if (op == 93) {
					u32 dx = S_(2), dy = S_(3);
					E(&t->code, OpImageSampleExplicitLod, t->t_v4, v, smp, cd, 4, tex_coord(t, dx, dimk), tex_coord(t, dy, dimk));
				} else if (ctrl == 2) E(&t->code, OpImageSampleImplicitLod, t->t_v4, v, smp, cd, 1, ext(t, t->t_float, coord, 3));
				else E(&t->code, OpImageSampleImplicitLod, t->t_v4, v, smp, cd);
				if (sw != 0xe4) v = shuf4(t, t->t_v4, v, v, sw & 3, (sw >> 2) & 3, (sw >> 4) & 3, (sw >> 6) & 3);
				break;
			}
			case 65: { /* texkill: any written component < 0 */
				u32 r = a[0], m = (r >> 16) & 0xf, lt, anyc = 0, k3;
				u32 val = RTYPE(r) == R_TEMP ? load(t, t->t_v4, temp(t, RNUM(r))) : RTYPE(r) == R_INPUT && RNUM(r) < 16 && t->in[RNUM(r)] ? load(t, t->t_v4, t->in[RNUM(r)]) : 0;
				if (!val) { fail(t, "texkill of register type %u", RTYPE(r)); break; }
				lt = bin(t, OpFOrdLessThan, t->t_bv4, val, cv4(t, 0, 0, 0, 0));
				for (k3 = 0; k3 < 4; k3++) if (m & (1u << k3)) {
					u32 c = ext(t, t->t_bool, lt, k3);
					anyc = anyc ? bin(t, OpLogicalOr, t->t_bool, anyc, c) : c;
				}
				if (anyc) cond_exit(t, anyc, 0);
				break;
			}
			case 40: case 41: { /* if b# / ifc */
				Flow *f;
				u32 c, th;
				if (op == 40) {
					if (RTYPE(a[0]) != R_CONSTBOOL) { fail(t, "if on register type %u", RTYPE(a[0])); break; }
					c = bool_reg(t, RNUM(a[0]));
				} else c = compare(t, ctrl, S_(0), S_(1));
				if (t->nflow == 32) { fail(t, "flow nested too deep"); break; }
				f = &t->flow[t->nflow++];
				memset(f, 0, sizeof *f);
				f->type = 1; f->merge = nid(t); f->els = nid(t);
				th = nid(t);
				E(&t->code, OpSelectionMerge, f->merge, 0);
				E(&t->code, OpBranchConditional, c, th, f->els);
				label(t, th);
				break;
			}
			case 42: { /* else */
				Flow *f = t->nflow ? &t->flow[t->nflow - 1] : 0;
				if (!f || f->type != 1 || f->has_else) { fail(t, "else without if"); break; }
				E(&t->code, OpBranch, f->merge);
				label(t, f->els);
				f->has_else = 1;
				break;
			}
			case 43: { /* endif */
				Flow *f = t->nflow ? &t->flow[t->nflow - 1] : 0;
				if (!f || f->type != 1) { fail(t, "endif without if"); break; }
				E(&t->code, OpBranch, f->merge);
				if (!f->has_else) { label(t, f->els); E(&t->code, OpBranch, f->merge); }
				label(t, f->merge);
				t->nflow--;
				break;
			}
			case 38: { /* rep i#: i.x times (0..255) */
				Flow *f;
				u32 cnt, body, c, cur;
				if (RTYPE(a[0]) != R_CONSTINT) { fail(t, "rep on register type %u", RTYPE(a[0])); break; }
				cnt = g3(t, GSClamp, t->t_int, ext(t, t->t_int, int_reg(t, RNUM(a[0])), 0), ci(t, 0), ci(t, 255));
				if (t->nflow == 32) { fail(t, "flow nested too deep"); break; }
				f = &t->flow[t->nflow++];
				memset(f, 0, sizeof *f);
				f->type = 2; f->merge = nid(t); f->cont = nid(t); f->header = nid(t);
				f->counter = fvar(t, t->t_int);
				E(&t->code, OpStore, f->counter, ci(t, 0));
				E(&t->code, OpBranch, f->header);
				label(t, f->header);
				cur = load(t, t->t_int, f->counter);
				c = bin(t, OpSLessThan, t->t_bool, cur, cnt);
				body = nid(t);
				E(&t->code, OpLoopMerge, f->merge, f->cont, 0);
				E(&t->code, OpBranchConditional, c, body, f->merge);
				label(t, body);
				break;
			}
			case 39: { /* endrep */
				Flow *f = t->nflow ? &t->flow[t->nflow - 1] : 0;
				u32 cur;
				if (!f || f->type != 2) { fail(t, "endrep without rep"); break; }
				E(&t->code, OpBranch, f->cont);
				label(t, f->cont);
				cur = load(t, t->t_int, f->counter);
				E(&t->code, OpStore, f->counter, bin(t, OpIAdd, t->t_int, cur, ci(t, 1)));
				E(&t->code, OpBranch, f->header);
				label(t, f->merge);
				t->nflow--;
				break;
			}
			case 44: { /* break */
				Flow *f = inner_loop(t);
				if (!f) { fail(t, "break outside a loop"); break; }
				E(&t->code, OpBranch, f->merge);
				label(t, nid(t)); /* (what follows is unreachable) */
				break;
			}
			case 45: { /* breakc */
				Flow *f = inner_loop(t);
				if (!f) { fail(t, "break outside a loop"); break; }
				cond_exit(t, compare(t, ctrl, S_(0), S_(1)), f->merge);
				break;
			}
			default: fail(t, "opcode %u", op);
			}
			if (has_dst && !t->failed && v) dst(t, a[0], v);
			else if (has_dst && !t->failed && !v && op != 65) fail(t, "opcode %u made no value", op);
#undef S_
		}
		}
	}
	if (t->nflow && !t->failed) fail(t, "unclosed flow control");
	/* the epilogue: outputs */
	if (!t->failed) {
		if (!t->ps) {
			int pos_done = 0;
			for (k = 0; k < 12; k++) {
				u32 v, o;
				if (!t->out[k]) continue;
				if (!t->out_dcl[k]) { fail(t, "output o%d not declared", k); break; }
				v = load(t, t->t_v4, t->out[k]);
				if (t->out_usage[k] == 0 && t->out_index[k] == 0 && !pos_done) { /* POSITION0: gl_Position, with the pixel-centre fix */
					u32 pp = nid(t), fix, wv;
					E(&t->code, OpAccessChain, ptr(t, SC_PushConstant, t->t_v4), pp, t->push, ci(t, 0));
					fix = load(t, t->t_v4, pp);
					wv = ext(t, t->t_float, v, 3);
					fix = bin(t, OpVectorTimesScalar, t->t_v4, shuf4(t, t->t_v4, fix, cv4(t, 0, 0, 0, 0), 0, 1, 4, 4), wv);
					v = bin(t, OpFAdd, t->t_v4, v, fix);
					o = gvar(t, SC_Output, t->t_v4);
					deco(t, o, D_BuiltIn, B_Position);
					pos_done = 1;
				} else {
					int loc = sm3_location(t->out_usage[k], t->out_index[k]);
					if (t->out_usage[k] == 4) continue; /* (PSIZE: no point sprites) */
					if (loc < 0) { fail(t, "too many interpolated semantics"); break; }
					o = gvar(t, SC_Output, t->t_v4);
					deco(t, o, D_Location, (u32)loc);
				}
				E(&t->code, OpStore, o, v);
			}
			if (!pos_done && !t->failed) fail(t, "no position output");
		} else {
			if (t->oc[0] && !t->no_alpha_test) { /* the alpha test (push constants: func, ref) */
				u32 pf = nid(t), pr = nid(t), fnc, ref, al, pass, c, k2;
				static const u32 ops[6][2] = {{2, OpFOrdLessThan}, {3, OpFOrdEqual}, {4, OpFOrdLessThanEqual}, {5, OpFOrdGreaterThan},
				                              {6, OpFUnordNotEqual}, {7, OpFOrdGreaterThanEqual}};
				E(&t->code, OpAccessChain, ptr(t, SC_PushConstant, t->t_int), pf, t->push, ci(t, 1));
				E(&t->code, OpAccessChain, ptr(t, SC_PushConstant, t->t_float), pr, t->push, ci(t, 2));
				fnc = load(t, t->t_int, pf);
				ref = load(t, t->t_float, pr);
				al = ext(t, t->t_float, load(t, t->t_v4, t->oc[0]), 3);
				/* pass = func >= 8 (ALWAYS) || (func == k && alpha OP ref) ...; NEVER (1) fails */
				pass = bin(t, OpSLessThan, t->t_bool, ci(t, 7), fnc);
				for (k2 = 0; k2 < 6; k2++) {
					c = bin(t, OpLogicalAnd, t->t_bool, bin(t, OpIEqual, t->t_bool, fnc, ci(t, (int)ops[k2][0])), bin(t, ops[k2][1], t->t_bool, al, ref));
					pass = bin(t, OpLogicalOr, t->t_bool, pass, c);
				}
				cond_exit(t, un(t, OpLogicalNot, t->t_bool, pass), 0);
			}
			for (k = 0; k < 4; k++) {
				u32 o;
				if (!t->oc[k]) continue;
				o = gvar(t, SC_Output, t->t_v4);
				deco(t, o, D_Location, (u32)k);
				E(&t->code, OpStore, o, load(t, t->t_v4, t->oc[k]));
			}
			if (t->odepth && !t->no_depth_out) {
				u32 o = gvar(t, SC_Output, t->t_float);
				deco(t, o, D_BuiltIn, B_FragDepth);
				E(&t->code, OpStore, o, ext(t, t->t_float, load(t, t->t_v4, t->odepth), 0));
			}
		}
		E0(&t->code, OpReturn);
	}
	/* the module */
	if (!t->failed) {
		Buf m = {0, 0, 0};
		u32 hdr[5] = {0x07230203u, 0x00010000u, 0, 0, 0}, k2;
		E(&t->caps, OpCapability, 1); /* Shader */
		{
			u32 pre[2] = {t->ps ? 4u : 0u, fn};
			emit_str(&t->ent, OpEntryPoint, 0, pre, 2, "main", t->iface, (u32)t->niface);
		}
		if (t->ps) {
			E(&t->modes, OpExecutionMode, fn, 7); /* OriginUpperLeft */
			if (t->odepth && !t->no_depth_out) E(&t->modes, OpExecutionMode, fn, 12); /* DepthReplacing */
		}
		hdr[3] = t->id;
		for (k2 = 0; k2 < 5; k2++) put(&m, hdr[k2]);
#define CAT(b) for (k2 = 0; k2 < (b).n; k2++) put(&m, (b).w[k2])
		CAT(t->caps);
		CAT(t->ext);
		E(&m, OpMemoryModel, 0, 1);
		CAT(t->ent);
		CAT(t->modes);
		CAT(t->deco);
		CAT(t->types);
		E(&m, OpFunction, t->t_void, fn, 0, t->t_fn);
		E(&m, OpLabel, main_l);
		CAT(t->fvars);
		CAT(t->init);
		CAT(t->code);
		E0(&m, OpFunctionEnd);
#undef CAT
		m.w[3] = t->id;
		info->spirv = m.w;
		info->nwords = m.n;
	}
	free(t->caps.w); free(t->ext.w); free(t->ent.w); free(t->modes.w); free(t->deco.w); free(t->types.w);
	free(t->fvars.w); free(t->init.w); free(t->code.w);
	return !t->failed;
}

void sm3_free(Sm3Info *s) {
	free(s->spirv);
	s->spirv = 0;
}
