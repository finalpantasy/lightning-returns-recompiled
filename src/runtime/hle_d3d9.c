/* hle_d3d9.c -- Direct3D 9 (d3d9.dll), headless: a device that keeps the state the game sets and the memory of the
 * resources it creates and locks, and draws nothing yet (a rendering backend goes under the same objects later).
 *
 * Objects are guest memory {vtable, id} like the other COM objects (hle_com.c); id indexes the host-side object
 * table. Every method of the interfaces is in the vtables: the ones here run, a setter / draw without its own
 * function is accepted (D3D_OK, its arguments popped: the generic handler knows each method's argument count), and
 * anything else stops the run with Interface::Method. Reference counts are kept; a resource's memory is freed when it
 * reaches 0. The adapter is one fixed GPU (NVIDIA-like, shader model 3, 1 GB). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32 /* (windows.h clashes with names here) */
__declspec(dllimport) void *__stdcall GetCurrentProcess(void);
__declspec(dllimport) int __stdcall GetProcessTimes(void *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *);
__declspec(dllimport) void *__stdcall GetCurrentThread(void);
__declspec(dllimport) int __stdcall GetThreadTimes(void *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *);
#endif
#include "hle_d3d9.h"

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define S(a) ((char *)(xrc_mem + (u32)(a)))

#define D3D_OK 0u
#define D3DERR_NOTAVAILABLE 0x8876086au
#define D3DERR_INVALIDCALL 0x8876086cu
#define E_NOINTERFACE 0x80004002u
#define E_OUTOFMEMORY 0x8007000eu

static int disp_w(void) { const char *e = getenv("XRC_WIDTH"); return e && atoi(e) > 0 ? atoi(e) : 1280; }
static int disp_h(void) { const char *e = getenv("XRC_HEIGHT"); return e && atoi(e) > 0 ? atoi(e) : 720; }
static int trace = -1;
static int tracing(void) {
	if (trace < 0) trace = getenv("XRC_TRACE_D3D") != 0;
	return trace;
}

#define NAME(m, a) #m,
#define NARG(m, a) a,
#define DEFI(id, name, LIST) \
	static const char *const id##_m[] = {LIST(NAME)}; \
	static const int id##_a[] = {LIST(NARG)}; \
	static Iface id = {name, id##_m, id##_a, (int)(sizeof id##_a / sizeof id##_a[0]), 0};
DEFI(if_d3d9, "IDirect3D9", I_D3D9)
DEFI(if_dev, "IDirect3DDevice9", I_DEVICE)
DEFI(if_tex, "IDirect3DTexture9", I_TEXTURE)
DEFI(if_cube, "IDirect3DCubeTexture9", I_CUBE)
DEFI(if_vtex, "IDirect3DVolumeTexture9", I_VOLTEX)
DEFI(if_surf, "IDirect3DSurface9", I_SURFACE)
DEFI(if_vol, "IDirect3DVolume9", I_VOLUME)
DEFI(if_vb, "IDirect3DVertexBuffer9", I_VB)
DEFI(if_ib, "IDirect3DIndexBuffer9", I_IB)
DEFI(if_decl, "IDirect3DVertexDeclaration9", I_DECL)
DEFI(if_vs, "IDirect3DVertexShader9", I_SHADER)
DEFI(if_ps, "IDirect3DPixelShader9", I_SHADER)
DEFI(if_query, "IDirect3DQuery9", I_QUERY)
DEFI(if_sb, "IDirect3DStateBlock9", I_SB)
DEFI(if_swap, "IDirect3DSwapChain9", I_SWAP)
DEFI(if_ctab, "ID3DXConstantTable", I_CTAB)
Iface *const d3_ifaces[] = {&if_d3d9, &if_dev, &if_tex, &if_cube, &if_vtex, &if_surf, &if_vol, &if_vb,
                                &if_ib, &if_decl, &if_vs, &if_ps, &if_query, &if_sb, &if_swap, &if_ctab};
#define NIFACES (int)(sizeof d3_ifaces / sizeof d3_ifaces[0])
const int d3_nifaces = NIFACES;
#define ifaces d3_ifaces

static u32 vtable_of(Iface *f) {
	if (!f->vtable) {
		char nm[96];
		int k;
		f->vtable = xrc_heap_alloc(4u * (u32)f->n, 1);
		for (k = 0; k < f->n; k++) {
			snprintf(nm, sizeof nm, "%s::%s", f->iface, f->m[k]);
			M32(f->vtable + 4u * (u32)k) = xrc_import_slot("d3d9", nm);
		}
	}
	return f->vtable;
}

/* ============================================================================================ objects */
/* the table is allocated in chunks that never move: Obj pointers stay valid while new objects are created */
#define OCHUNK 1024
#define MAXCHUNKS 1024
Obj *d3_ochunks[MAXCHUNKS];
u32 d3_nobjs;
#define ochunks d3_ochunks
#define nobjs d3_nobjs
static u32 cap_objs;
static u32 free_hint = 1; /* no free object below it */
#define OBJ(i) (ochunks[(u32)(i) / OCHUNK][(u32)(i) % OCHUNK])
/* (o is about to be freed: the next new_obj looks from it) */
static void obj_freed(Obj *o) { u32 i = M32(o->guest + 4); if (i && i < free_hint) free_hint = i; }

static u32 new_obj(int kind, Iface *f) {
	u32 i;
	for (i = free_hint > 1 ? free_hint : 1; i < nobjs; i++)
		if (OBJ(i).kind == K_FREE) break;
	free_hint = i + 1;
	if (i >= nobjs) {
		if (nobjs == 0) nobjs = 1; /* id 0: none */
		if (nobjs >= cap_objs) {
			if (cap_objs / OCHUNK == MAXCHUNKS) xrc_fatal(0, "d3d9: too many objects");
			ochunks[cap_objs / OCHUNK] = (Obj *)calloc(OCHUNK, sizeof(Obj));
			cap_objs += OCHUNK;
		}
		i = nobjs++;
	}
	memset(&OBJ(i), 0, sizeof(Obj));
	OBJ(i).kind = kind;
	OBJ(i).refs = 1;
	OBJ(i).guest = xrc_heap_alloc(16, 1);
	M32(OBJ(i).guest) = vtable_of(f);
	M32(OBJ(i).guest + 4) = i;
	M32(OBJ(i).guest + 8) = 0xd3d90b1eu;
	return i;
}

static XCtx *cur_ctx;
static Obj *self(XCtx *c) {
	Obj *o = obj_of(A(0));
	cur_ctx = c;
	if (!o) xrc_fatal(c, "d3d9: %s on a bad object %08x", xrc_cur_import, A(0));
	return o;
}

/* formats: bytes per pixel, or per 4x4 block for the DXT ones */
#define FOURCC(a, b, c_, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c_) << 16 | (u32)(d) << 24)
int is_dxt(u32 f) { return f == FOURCC('D', 'X', 'T', '1') || (f >= FOURCC('D', 'X', 'T', '2') && f <= FOURCC('D', 'X', 'T', '5')) || f == FOURCC('A', 'T', 'I', '1') || f == FOURCC('A', 'T', 'I', '2'); }
u32 fmt_bytes(u32 f) {
	if (is_dxt(f)) return f == FOURCC('D', 'X', 'T', '1') || f == FOURCC('A', 'T', 'I', '1') ? 8 : 16;
	switch (f) {
	case 28: case 50: case 41: return 1;                       /* A8 L8 P8 */
	case 23: case 24: case 25: case 26: case 51: case 80: case 81: case 111: case 60: case 70: return 2;
	case 36: case 113: case 115: case 64: return 8;            /* A16B16G16R16 (F) G32R32F Q16W16V16U16 */
	case 116: return 16;                                        /* A32B32G32R32F */
	default: return 4;
	}
}
u32 pitch_of(u32 f, u32 w) { return is_dxt(f) ? ((w + 3) / 4 ? (w + 3) / 4 : 1) * fmt_bytes(f) : w * fmt_bytes(f); }
u32 rows_of(u32 f, u32 h) { return is_dxt(f) ? ((h + 3) / 4 ? (h + 3) / 4 : 1) : h; }
u32 dim(u32 x, u32 l) { x >>= l; return x ? x : 1; }
u32 level_size(Obj *o, u32 l) { return pitch_of(o->fmt, dim(o->w, l)) * rows_of(o->fmt, dim(o->h, l)) * (o->kind == K_VTEX ? dim(o->d, l) : 1); }
static u32 nlevels(u32 w, u32 h, u32 d, u32 want) {
	u32 n = 1, m = w > h ? w : h;
	if (d > m) m = d;
	while (m > 1) { m >>= 1; n++; }
	if (want && want < n) n = want;
	return n > MAXLEVELS ? MAXLEVELS : n;
}

static void release_obj(Obj *o);
static void dev_released(void);
/* private data (SetPrivateData): see r_SetPrivateData */
typedef struct { u32 obj, guid[4], data, size, iunk; } PData;
static PData *pdata;
static unsigned npdata, cap_pdata;
static void pd_free(PData *p);
static void xrc_call_guest_addref(u32 i);
static void xrc_call_guest_release(u32 i);
/* a level surface's references are its texture's (as in D3D9: the surface lives as long as the container) */
static Obj *counted(u32 g) {
	Obj *o = obj_of(g);
	if (o && o->parent) o = obj_of(o->parent);
	return o;
}
static void addref_g(u32 g) { Obj *o = counted(g); if (o) o->refs++; }
static void release_g(u32 g) {
	Obj *o = counted(g);
	if (o && o->refs && --o->refs == 0) release_obj(o);
}
static void release_obj(Obj *o) {
	int f, l;
	if (o->parent) { /* a level surface: it lives as long as its texture */
		o->refs = 0;
		return;
	}
	for (f = 0; f < 6; f++)
		for (l = 0; l < MAXLEVELS; l++) {
			if (o->lvl[f][l]) xrc_heap_free(o->lvl[f][l]);
			if (o->sub[f][l]) {
				Obj *s = obj_of(o->sub[f][l]);
				unsigned k = 0;
				while (k < npdata) { if (pdata[k].obj == o->sub[f][l]) pd_free(&pdata[k]); else k++; }
				if (s) { ref_d3d_release(s); obj_freed(s); xrc_heap_free(s->guest); s->kind = K_FREE; }
			}
		}
	if (o->mem) xrc_heap_free(o->mem);
	if (o->kind == K_DEV) dev_released();
	{
		unsigned k = 0;
		while (k < npdata) { if (pdata[k].obj == o->guest) pd_free(&pdata[k]); else k++; }
	}
	ref_d3d_release(o);
	obj_freed(o);
	xrc_heap_free(o->guest);
	o->kind = K_FREE;
}

/* cur_ctx: the XCtx of the running method (for calls into guest interfaces) */
static void xrc_call_guest_addref(u32 i) {
	if (obj_of(i)) { addref_g(i); return; }
	if (cur_ctx) xrc_call_guest(cur_ctx, M32(M32(i) + 4), 1, &i);
}
static void xrc_call_guest_release(u32 i) {
	if (obj_of(i)) { release_g(i); return; }
	if (cur_ctx) xrc_call_guest(cur_ctx, M32(M32(i) + 8), 1, &i);
}

/* ============================================================================================ IUnknown */
static void m_AddRef(XCtx *c) { Obj *o = counted(A(0)); RET(o ? ++o->refs : 0, 1); }
static void m_Release(XCtx *c) {
	Obj *o = self(c);
	u32 r;
	if (o->parent) { /* level surfaces count on the texture */
		Obj *p = obj_of(o->parent);
		r = p && p->refs ? p->refs - 1 : 0;
		release_g(o->parent);
		RET(r, 1);
	}
	r = o->refs ? o->refs - 1 : 0;
	release_g(A(0));
	RET(r, 1);
}
static void m_QueryInterface(XCtx *c) { /* the object itself for any interface it could be asked for */
	addref_g(A(0));
	M32(A(2)) = A(0);
	RET(D3D_OK, 3);
}
u32 the_dev;
static void m_GetDevice(XCtx *c) { M32(A(1)) = the_dev; addref_g(the_dev); RET(D3D_OK, 2); }

/* the generic method: setters / draws are accepted; anything else is not implemented */
static int nargs_of(const char *name) {
	const char *m = strstr(name, "::");
	int k, i;
	char iface[64];
	if (!m || (size_t)(m - name) >= sizeof iface) return -1;
	memcpy(iface, name, (size_t)(m - name));
	iface[m - name] = 0;
	for (k = 0; k < NIFACES; k++) {
		if (strcmp(ifaces[k]->iface, iface)) continue;
		for (i = 0; i < ifaces[k]->n; i++)
			if (strcmp(ifaces[k]->m[i], m + 2) == 0) return ifaces[k]->nargs[i];
	}
	return -1;
}
static unsigned long long ndraws;
const D3Backend *d3_be;
#ifndef XRC_VULKAN
const D3Backend *d3_be_init(void) { return 0; }
#endif
/* XRC_D3D_SURVEY=DIR: what the game draws with (for the rendering backend): every shader's bytecode to DIR/<id>.vs|ps,
 * and DIR/survey.txt (rewritten every 600 frames): per draw method / primitive type, shader pairs, fixed-function
 * draws, each render / sampler state's values at draws, the formats of the textures and targets drawn with */
static const char *survey_dir(void) { static const char *d; static int k; if (!k) { d = xrc_tool_env("XRC_D3D_SURVEY"); k = 1; } return d; }
#define SV_MAXV 12
static u32 sv_rs[256][SV_MAXV], sv_ss[16][SV_MAXV], sv_tfmt[64], sv_rtfmt[16], sv_dsfmt[16], sv_decl_t[32], sv_decl_u[32];
static unsigned sv_nrs[256], sv_nss[16];
static unsigned long long sv_draw[4][8], sv_ffp_vs, sv_ffp_ps, sv_fvf_draws, sv_inst, sv_ntex[17], sv_mrt[5];
static u32 sv_pair[4096][2]; static unsigned long long sv_pairn[4096]; static unsigned sv_npair;
static void sv_add(u32 *set, unsigned *n, u32 v, unsigned max) { unsigned k; for (k = 0; k < *n; k++) if (set[k] == v) return; if (*n < max) set[(*n)++] = v; }
static void sv_add0(u32 *set, u32 v, unsigned max) { unsigned k; for (k = 0; k < max && set[k]; k++) if (set[k] == v) return; if (k < max) set[k] = v; }
static void survey_draw(int kind, u32 prim) {
	unsigned k, nt = 0, nrt = 0;
	if (!survey_dir()) return;
	sv_draw[kind][prim & 7]++;
	if (!dev.vs) sv_ffp_vs++;
	if (!dev.ps) sv_ffp_ps++;
	if (!dev.decl) sv_fvf_draws++;
	if (dev.freq[0] & 0x40000000u) sv_inst++;
	for (k = 0; k < sv_npair; k++) if (sv_pair[k][0] == dev.vs && sv_pair[k][1] == dev.ps) break;
	if (k == sv_npair && k < 4096) { sv_pair[k][0] = dev.vs; sv_pair[k][1] = dev.ps; sv_npair++; }
	if (k < 4096) sv_pairn[k]++;
	for (k = 0; k < 256; k++) sv_add(sv_rs[k], &sv_nrs[k], dev.rs[k], SV_MAXV);
	for (k = 0; k < 20; k++) {
		Obj *t = obj_of(dev.tex[k]);
		if (!t) continue;
		nt++;
		sv_add0(sv_tfmt, t->fmt | (u32)t->kind << 24, 64);
		if (k < 16) { unsigned j; for (j = 0; j < 14; j++) sv_add(sv_ss[j], &sv_nss[j], dev.ss[k][j], SV_MAXV); }
	}
	sv_ntex[nt > 16 ? 16 : nt]++;
	for (k = 0; k < 4; k++) { Obj *r = obj_of(dev.rt[k]); if (r) { nrt++; sv_add0(sv_rtfmt, r->fmt, 16); } }
	sv_mrt[nrt]++;
	{ Obj *d = obj_of(dev.ds); if (d) sv_add0(sv_dsfmt, d->fmt, 16); }
	{
		Obj *d = obj_of(dev.decl);
		if (d) for (k = 0; k + 1 < d->size; k++) {
			sv_add0(sv_decl_t, 1 + M8(d->mem + 8 * k + 4), 32);
			sv_add0(sv_decl_u, 1 + (M8(d->mem + 8 * k + 6) | M8(d->mem + 8 * k + 7) << 8), 32);
		}
	}
}
static void survey_write(void) {
	char p[1024];
	FILE *f;
	unsigned k, j;
	static const char *const dn[4] = {"DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP"};
	if (!survey_dir()) return;
	snprintf(p, sizeof p, "%s/survey.txt", survey_dir());
	if (!(f = fopen(p, "w"))) return;
	fprintf(f, "frames %llu\n", dev.frames);
	for (k = 0; k < 4; k++) for (j = 0; j < 8; j++) if (sv_draw[k][j]) fprintf(f, "draw %s prim %u: %llu\n", dn[k], j, sv_draw[k][j]);
	fprintf(f, "no vs %llu, no ps %llu, fvf (no decl) %llu, instanced %llu\n", sv_ffp_vs, sv_ffp_ps, sv_fvf_draws, sv_inst);
	fprintf(f, "textures bound per draw:"); for (k = 0; k < 17; k++) if (sv_ntex[k]) fprintf(f, " %u:%llu", k, sv_ntex[k]); fprintf(f, "\n");
	fprintf(f, "render targets per draw:"); for (k = 0; k < 5; k++) if (sv_mrt[k]) fprintf(f, " %u:%llu", k, sv_mrt[k]); fprintf(f, "\n");
	fprintf(f, "texture formats (kind<<24|fmt):"); for (k = 0; k < 64 && sv_tfmt[k]; k++) fprintf(f, " %08x", sv_tfmt[k]); fprintf(f, "\n");
	fprintf(f, "rt formats:"); for (k = 0; k < 16 && sv_rtfmt[k]; k++) fprintf(f, " %u", sv_rtfmt[k]); fprintf(f, "\n");
	fprintf(f, "ds formats:"); for (k = 0; k < 16 && sv_dsfmt[k]; k++) fprintf(f, " %u", sv_dsfmt[k]); fprintf(f, "\n");
	fprintf(f, "decl types+1:"); for (k = 0; k < 32 && sv_decl_t[k]; k++) fprintf(f, " %u", sv_decl_t[k]); fprintf(f, "\n");
	fprintf(f, "decl usage+1|idx<<8:"); for (k = 0; k < 32 && sv_decl_u[k]; k++) fprintf(f, " %x", sv_decl_u[k]); fprintf(f, "\n");
	for (k = 0; k < 256; k++) if (sv_nrs[k] > 1 || (sv_nrs[k] == 1 && sv_rs[k][0])) {
		fprintf(f, "rs %u:", k); for (j = 0; j < sv_nrs[k]; j++) fprintf(f, " %x", sv_rs[k][j]); fprintf(f, "%s\n", sv_nrs[k] == SV_MAXV ? " ..." : "");
	}
	for (k = 0; k < 14; k++) if (sv_nss[k]) { fprintf(f, "ss %u:", k); for (j = 0; j < sv_nss[k]; j++) fprintf(f, " %x", sv_ss[k][j]); fprintf(f, "\n"); }
	fprintf(f, "shader pairs %u\n", sv_npair);
	for (k = 0; k < sv_npair; k++) {
		Obj *v = obj_of(sv_pair[k][0]), *q = obj_of(sv_pair[k][1]);
		fprintf(f, "pair vs %u ps %u: %llu\n", v ? M32(sv_pair[k][0] + 4) : 0, q ? M32(sv_pair[k][1] + 4) : 0, sv_pairn[k]);
	}
	fclose(f);
}
static void survey_shader(u32 g, u32 mem, u32 n, int ps) {
	char p[1024];
	FILE *f;
	if (!survey_dir()) return;
	snprintf(p, sizeof p, "%s/%u.%s", survey_dir(), M32(g + 4), ps ? "ps" : "vs");
	if ((f = fopen(p, "wb"))) { fwrite(xrc_mem + mem, 1, n, f); fclose(f); }
}
static void m_generic(XCtx *c) {
	const char *name = xrc_cur_import, *m = strstr(name, "::");
	int n = nargs_of(name);
	static const char *const accept[] = {"Set", "Draw", "Begin", "End", "Clear", "LightEnable", "Multiply", "Evict",
	                                     "Update", "Stretch", "ColorFill", "Free", "PreLoad", "GenerateMip", "AddDirty",
	                                     "Delete", "Apply", "Capture", "Issue", "ProcessVertices", "ShowCursor"};
	unsigned k;
	if (n < 0 || !m) xrc_fatal(c, "d3d9: unknown method %s", name);
	for (k = 0; k < sizeof accept / sizeof accept[0]; k++)
		if (strncmp(m + 2, accept[k], strlen(accept[k])) == 0) {
			if (m[2] == 'D' && m[3] == 'r') ndraws++;
			RET(D3D_OK, n);
		}
	xrc_fatal(c, "d3d9: %s is not implemented yet", name);
}

/* ============================================================================================ IDirect3D9 */
#define NMODES 7
static const u32 modes[NMODES][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1600, 900}, {1920, 1080}};
static void d_RegisterSoftwareDevice(XCtx *c) { RET(D3DERR_NOTAVAILABLE, 2); }
static void d_GetAdapterCount(XCtx *c) { RET(1, 1); }
/* D3DADAPTER_IDENTIFIER9: Driver[512] Description[512] DeviceName[32] DriverVersion(1056) VendorId DeviceId SubSysId
 * Revision DeviceIdentifier(1080, GUID) WHQLLevel(1096) */
static void d_GetAdapterIdentifier(XCtx *c) {
	u32 p = A(3);
	if (A(1) != 0) RET(D3DERR_INVALIDCALL, 4);
	memset(xrc_mem + p, 0, 1100);
	snprintf(S(p), 512, "nvd3dum.dll");
	snprintf(S(p + 512), 512, "NVIDIA GeForce GTX 560");
	snprintf(S(p + 1024), 32, "\\\\.\\DISPLAY1");
	M32(p + 1056) = 0x00000fd4u | (0x000dU << 16); M32(p + 1060) = 0x00090012u; /* 9.18.13.4052 */
	M32(p + 1064) = 0x10de; M32(p + 1068) = 0x1201; M32(p + 1072) = 0; M32(p + 1076) = 0xa1;
	M32(p + 1080) = 0xd7b71e3eu; M32(p + 1084) = 0x41c111e9u; M32(p + 1088) = 0x6800cd70u; M32(p + 1092) = 0x35c3a6c9u;
	M32(p + 1096) = 1;
	RET(D3D_OK, 4);
}
static int display_format(u32 f) { return f == 22 || f == 23; } /* X8R8G8B8, R5G6B5 */
static void d_GetAdapterModeCount(XCtx *c) { RET(display_format(A(2)) ? NMODES : 0, 3); }
static void put_mode(u32 p, u32 w, u32 h, u32 f) { M32(p) = w; M32(p + 4) = h; M32(p + 8) = 60; M32(p + 12) = f; }
static void d_EnumAdapterModes(XCtx *c) {
	if (!display_format(A(2)) || A(3) >= NMODES) RET(D3DERR_INVALIDCALL, 5);
	put_mode(A(4), modes[A(3)][0], modes[A(3)][1], A(2));
	RET(D3D_OK, 5);
}
static void d_GetAdapterDisplayMode(XCtx *c) { put_mode(A(2), (u32)disp_w(), (u32)disp_h(), 22); RET(D3D_OK, 3); }
static void d_CheckDeviceType(XCtx *c) { RET(D3D_OK, 6); }
/* CheckDeviceFormat(adapter, type, adapterFmt, usage, rtype, fmt) */
static void d_CheckDeviceFormat(XCtx *c) {
	u32 f = A(6), usage = A(4);
	int ok;
	switch (f) {
	case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 31: case 32: case 33: case 34: case 35:
	case 36: case 50: case 51: case 71: case 75: case 77: case 80: case 81: case 111: case 112: case 113: case 114:
	case 115: case 116: case 60: case 63: case 64:
		ok = 1;
		break;
	default:
		ok = is_dxt(f) || f == FOURCC('I', 'N', 'T', 'Z') || f == FOURCC('N', 'U', 'L', 'L') || f == FOURCC('D', 'F', '2', '4') ||
		     f == FOURCC('A', 'T', 'O', 'C');
	}
	if (ok && is_dxt(f) && (usage & 1 /* RENDERTARGET */)) ok = 0;
	if (tracing() || !ok) fprintf(stderr, "xrc: d3d9 CheckDeviceFormat(usage %x, rtype %u, fmt %08x) -> %s\n", usage, A(5), f, ok ? "ok" : "not available");
	RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}
static void d_CheckDeviceMultiSampleType(XCtx *c) {
	u32 t = A(5);
	if (A(6)) M32(A(6)) = t == 0 ? 1 : 4;
	RET(t == 0 || t == 1 || t == 2 || t == 4 || t == 8 ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}
static void d_CheckDepthStencilMatch(XCtx *c) { RET(D3D_OK, 6); }
static void d_CheckDeviceFormatConversion(XCtx *c) { RET(D3D_OK, 5); }
static u32 f32bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static void fill_caps(u32 p) {
	memset(xrc_mem + p, 0, 304);
	M32(p + 0) = 1;            /* D3DDEVTYPE_HAL */
	M32(p + 8) = 0x00020000u;  /* Caps: READ_SCANLINE */
	M32(p + 12) = 0xe0020000u; /* Caps2: FULLSCREENGAMMA CANAUTOGENMIPMAP DYNAMICTEXTURES CANSHARERESOURCE */
	M32(p + 16) = 0x000003a0u; /* Caps3 */
	M32(p + 20) = 0x8000000fu; /* PresentationIntervals */
	M32(p + 24) = 1;
	M32(p + 28) = 0x001bbef0u; /* DevCaps: HWTRANSFORMANDLIGHT PUREDEVICE HWRASTERIZATION ... */
	M32(p + 32) = 0x002ecff2u; /* PrimitiveMiscCaps (incl. SEPARATEALPHABLEND, MRTINDEPENDENTBITDEPTHS) */
	M32(p + 36) = 0x07732191u; /* RasterCaps (incl. SCISSORTEST, DEPTHBIAS, SLOPESCALEDEPTHBIAS, ANISOTROPY) */
	M32(p + 40) = 0xff; M32(p + 44) = 0x3fff; M32(p + 48) = 0x3fff; M32(p + 52) = 0xff;
	M32(p + 56) = 0x00084208u;
	M32(p + 60) = 0x0001ec45u; /* TextureCaps: PERSPECTIVE ALPHA PROJECTED CUBEMAP VOLUMEMAP MIPMAP MIPVOLUMEMAP MIPCUBEMAP */
	M32(p + 64) = 0x03030700u; M32(p + 68) = 0x03030300u; M32(p + 72) = 0x03030300u;
	M32(p + 76) = 0x3f; M32(p + 80) = 0x3f; M32(p + 84) = 0x1f;
	M32(p + 88) = 8192; M32(p + 92) = 8192; M32(p + 96) = 2048; M32(p + 100) = 8192; M32(p + 104) = 8192;
	M32(p + 108) = 16;
	M32(p + 112) = f32bits(1e10f);
	M32(p + 116) = f32bits(-1e8f); M32(p + 120) = f32bits(-1e8f); M32(p + 124) = f32bits(1e8f); M32(p + 128) = f32bits(1e8f);
	M32(p + 136) = 0x1ff;      /* StencilCaps */
	M32(p + 140) = 0x00180008u;
	M32(p + 144) = 0x03feffffu;
	M32(p + 148) = 8; M32(p + 152) = 8;
	M32(p + 156) = 0x0000017bu;
	M32(p + 160) = 8; M32(p + 164) = 8; M32(p + 168) = 4; M32(p + 172) = 0;
	M32(p + 176) = f32bits(8192.0f);
	M32(p + 180) = 0x00ffffffu; M32(p + 184) = 0x00ffffffu;
	M32(p + 188) = 16; M32(p + 192) = 255;
	M32(p + 196) = 0xfffe0300u; M32(p + 200) = 256;
	M32(p + 204) = 0xffff0300u; M32(p + 208) = f32bits(3.4e38f);
	M32(p + 212) = 0x51u;      /* DevCaps2: STREAMOFFSET VERTEXELEMENTSCANSHARESTREAMOFFSET CAN_STRETCHRECT_FROM_TEXTURES */
	M32(p + 216) = f32bits(1.0f);
	M32(p + 224) = 0; M32(p + 228) = 0; M32(p + 232) = 1;
	M32(p + 236) = 0x3ff;      /* DeclTypes */
	M32(p + 240) = 4;          /* NumSimultaneousRTs */
	M32(p + 244) = 0x03000300u;
	M32(p + 248) = 1; M32(p + 252) = 24; M32(p + 256) = 32; M32(p + 260) = 4;                   /* VS20Caps */
	M32(p + 264) = 0x1f; M32(p + 268) = 24; M32(p + 272) = 32; M32(p + 276) = 4; M32(p + 280) = 512; /* PS20Caps */
	M32(p + 284) = 0x03000300u;
	M32(p + 288) = 0xffff; M32(p + 292) = 0xffff; M32(p + 296) = 32768; M32(p + 300) = 32768;
}
static void d_GetDeviceCaps(XCtx *c) { fill_caps(A(3)); RET(D3D_OK, 4); }
static void d_GetAdapterMonitor(XCtx *c) { RET(0x00012340u, 2); }

/* ============================================================================================ the device */
Dev dev;

static u32 new_surface(u32 w, u32 h, u32 fmt, u32 usage, u32 pool, u32 ms, u32 msq) {
	u32 i = new_obj(K_SURF, &if_surf);
	Obj *o = &OBJ(i);
	o->w = w; o->h = h; o->d = 1; o->fmt = fmt; o->usage = usage; o->pool = pool; o->ms = ms; o->msq = msq; o->levels = 1;
	if (ms) fprintf(stderr, "xrc: d3d9 a multisampled surface %08x: %ux%u fmt %08x, %s, %u samples (quality %u)\n", o->guest, w, h, fmt, usage == 2 ? "depth" : "render target", ms, msq);
	return o->guest;
}
static void default_states(void) {
	int k, s;
	memset(dev.rs, 0, sizeof dev.rs);
	dev.rs[7] = 1;   /* ZENABLE */
	dev.rs[8] = 3;   /* FILLMODE solid */
	dev.rs[9] = 2;   /* SHADEMODE gouraud */
	dev.rs[14] = 1;  /* ZWRITEENABLE */
	dev.rs[19] = 2;  /* SRCBLEND one */
	dev.rs[20] = 1;  /* DESTBLEND zero */
	dev.rs[22] = 3;  /* CULLMODE ccw */
	dev.rs[23] = 4;  /* ZFUNC lessequal */
	dev.rs[25] = 8;  /* ALPHAFUNC always */
	dev.rs[29] = 1;  /* DITHERENABLE off -> 0? keep */
	dev.rs[52] = 8;  /* STENCILFUNC always */
	dev.rs[57] = 0xffffffffu; dev.rs[58] = 0xffffffffu; /* STENCILMASK / WRITEMASK */
	dev.rs[59] = 0xffffffffu; /* TEXTUREFACTOR */
	dev.rs[168] = 0xf;        /* COLORWRITEENABLE */
	dev.rs[171] = 1;          /* BLENDOP add */
	for (s = 0; s < 32; s++) {
		dev.ss[s][1] = dev.ss[s][2] = dev.ss[s][3] = 1; /* ADDRESSU/V/W wrap */
		dev.ss[s][5] = dev.ss[s][6] = 1;                /* MAG/MINFILTER point */
		dev.ss[s][10] = 1;                              /* MAXANISOTROPY */
	}
	for (k = 0; k < 8; k++) {
		dev.tss[k][1] = k ? 1 : 4; dev.tss[k][2] = 2; dev.tss[k][3] = 1;
		dev.tss[k][4] = k ? 1 : 2; dev.tss[k][5] = 2; dev.tss[k][6] = 1;
		dev.tss[k][11] = (u32)k;
	}
}
static void setup_buffers(void) {
	u32 w = dev.pp[0], h = dev.pp[1];
	if (!w) dev.pp[0] = w = (u32)disp_w();
	if (!h) dev.pp[1] = h = (u32)disp_h();
	if (!dev.pp[2]) dev.pp[2] = 22;
	if (!dev.pp[3]) dev.pp[3] = 1;
	if (dev.backbuf) release_g(dev.backbuf);
	if (dev.depth) release_g(dev.depth);
	dev.backbuf = new_surface(w, h, dev.pp[2], 1, 0, dev.pp[4], dev.pp[5]);
	dev.depth = dev.pp[9] ? new_surface(w, h, dev.pp[10], 2, 0, dev.pp[4], dev.pp[5]) : 0;
	memset(dev.rt, 0, sizeof dev.rt);
	dev.rt[0] = dev.backbuf; /* (the slots hold references) */
	addref_g(dev.backbuf);
	dev.ds = dev.depth;
	if (dev.depth) addref_g(dev.depth);
	dev.vp[0] = 0; dev.vp[1] = 0; dev.vp[2] = w; dev.vp[3] = h; dev.vp[4] = 0; dev.vp[5] = f32bits(1.0f);
	dev.scissor[0] = 0; dev.scissor[1] = 0; dev.scissor[2] = w; dev.scissor[3] = h;
	fprintf(stderr, "xrc: d3d9 back buffer %ux%u fmt %u x%u, %s, depth fmt %u, interval %x\n", w, h, dev.pp[2], dev.pp[3],
	        dev.pp[8] ? "windowed" : "fullscreen", dev.pp[9] ? dev.pp[10] : 0, dev.pp[13]);
}
/* CreateDevice(adapter, type, hwnd, flags, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **) */
static void d_CreateDevice(XCtx *c) {
	u32 i;
	if (the_dev) xrc_fatal(c, "d3d9: a second device");
	memcpy(dev.pp, xrc_mem + A(5), sizeof dev.pp);
	dev.devtype = A(2); dev.hwnd = A(3); dev.flags = A(4); dev.d3d = A(0);
	i = new_obj(K_DEV, &if_dev);
	the_dev = OBJ(i).guest;
	fprintf(stderr, "xrc: d3d9 CreateDevice(hwnd %08x, flags %x; back buffer %ux%u format %u x%u, multisample %u, swap effect %u, windowed %u, flags %x, interval %x)\n", A(3), A(4),
	        dev.pp[0], dev.pp[1], dev.pp[2], dev.pp[3], dev.pp[4], dev.pp[6], dev.pp[8], dev.pp[11], dev.pp[13]);
	default_states();
	setup_buffers();
#ifdef XRC_REFERENCE
	if (xrc_use_vulkan()) d3_be = d3_be_init(); /* (XRC_VK: Vulkan draws instead of the real d3d9.dll: ref_d3d9.c) */
#else
	if (!getenv("XRC_NO_GPU")) d3_be = d3_be_init();
#endif
	memcpy(xrc_mem + A(5), dev.pp, sizeof dev.pp);
	M32(A(6)) = the_dev;
	RET(D3D_OK, 7);
}

/* the device's last reference: what it holds goes, and a new device may be created */
static void dev_released(void) {
	u32 *held[] = {&dev.backbuf, &dev.depth, &dev.ib, &dev.decl, &dev.vs, &dev.ps, &dev.swap};
	unsigned k;
	fprintf(stderr, "xrc: d3d9 device released\n");
	for (k = 0; k < 4; k++) if (dev.rt[k]) release_g(dev.rt[k]);
	if (dev.ds) release_g(dev.ds);
	for (k = 0; k < 32; k++) if (dev.tex[k]) release_g(dev.tex[k]);
	for (k = 0; k < 16; k++) if (dev.stream[k][0]) release_g(dev.stream[k][0]);
	for (k = 0; k < sizeof held / sizeof held[0]; k++) if (*held[k]) release_g(*held[k]);
	memset(&dev, 0, sizeof dev);
	the_dev = 0;
}
static void v_TestCooperativeLevel(XCtx *c) { RET(D3D_OK, 1); }
static void v_GetAvailableTextureMem(XCtx *c) { RET(0x3ff00000u, 1); }
static void v_GetDirect3D(XCtx *c) { M32(A(1)) = dev.d3d; addref_g(dev.d3d); RET(D3D_OK, 2); }
static void v_GetDeviceCaps(XCtx *c) { fill_caps(A(1)); RET(D3D_OK, 2); }
static void v_GetDisplayMode(XCtx *c) { put_mode(A(2), (u32)disp_w(), (u32)disp_h(), 22); RET(D3D_OK, 3); }
/* D3DDEVICE_CREATION_PARAMETERS: AdapterOrdinal DeviceType hFocusWindow BehaviorFlags */
static void v_GetCreationParameters(XCtx *c) {
	M32(A(1)) = 0; M32(A(1) + 4) = dev.devtype; M32(A(1) + 8) = dev.hwnd; M32(A(1) + 12) = dev.flags;
	RET(D3D_OK, 2);
}
static void v_GetNumberOfSwapChains(XCtx *c) { RET(1, 1); }
static void v_GetSwapChain(XCtx *c) {
	if (A(1)) RET(D3DERR_INVALIDCALL, 3);
	if (!dev.swap) dev.swap = OBJ(new_obj(K_SWAP, &if_swap)).guest;
	addref_g(dev.swap);
	M32(A(2)) = dev.swap;
	RET(D3D_OK, 3);
}
/* Reset: the device's state goes back to the defaults, and what was bound is unbound (released) */
static void unbind_all(void) {
	u32 *held[] = {&dev.ib, &dev.decl, &dev.vs, &dev.ps};
	unsigned k;
	for (k = 0; k < 4; k++) if (dev.rt[k]) { release_g(dev.rt[k]); dev.rt[k] = 0; }
	if (dev.ds) { release_g(dev.ds); dev.ds = 0; }
	for (k = 0; k < 32; k++) if (dev.tex[k]) { release_g(dev.tex[k]); dev.tex[k] = 0; }
	for (k = 0; k < 16; k++) if (dev.stream[k][0]) { release_g(dev.stream[k][0]); dev.stream[k][0] = 0; }
	for (k = 0; k < sizeof held / sizeof held[0]; k++) if (*held[k]) { release_g(*held[k]); *held[k] = 0; }
}
static void v_Reset(XCtx *c) {
	unbind_all();
	memcpy(dev.pp, xrc_mem + A(1), sizeof dev.pp);
	fprintf(stderr, "xrc: d3d9 Reset\n");
	setup_buffers();
	default_states();
	if (d3_be) d3_be->reset();
	memcpy(xrc_mem + A(1), dev.pp, sizeof dev.pp);
	RET(D3D_OK, 2);
}
/* a report at the first frame, then every 10 s: frames per second and draws per frame since the last one */
/* every XRC_FPS_MS (default 10000) ms: frames per second, draws per frame and the process's CPU time per frame (all
 * threads: the cost of a frame, also when the frame rate is locked) */
static void present(void) {
	static unsigned long long t0, f0, d0, c0, m0, w0;
	extern unsigned long long xrc_frame_wait_us;
	extern double xrc_gpu_ms_total;
	static double g0;
	static long every = -1;
	struct timespec ts;
	unsigned long long t, cpu, mcpu;
	if (every < 0) every = xrc_tool_env("XRC_FPS_MS") ? atol(xrc_tool_env("XRC_FPS_MS")) : 10000;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t = (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#ifdef _WIN32
	{ /* (winpthreads' process CPU clock counts only the calling thread) */
		unsigned long long cr, ex, kt, ut; /* (FILETIMEs: 100 ns) */
		GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kt, &ut);
		cpu = (kt + ut) / 10ull;
		GetThreadTimes(GetCurrentThread(), &cr, &ex, &kt, &ut);
		mcpu = (kt + ut) / 10ull;
	}
#else
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	cpu = (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); /* (the presenting thread: the game's main thread) */
	mcpu = (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
#endif
	xrc_prof_frame();
	dev.frames++;
	if (dev.frames % 600 == 0) survey_write();
	{ /* (call counts from XRC_PROFILE_DELAY seconds on) */
		static unsigned long long tm;
		void xrc_count_mark(void);
		if (!tm) tm = t + (xrc_tool_env("XRC_PROFILE_DELAY") ? (unsigned long long)(atof(xrc_tool_env("XRC_PROFILE_DELAY")) * 1000.0) : 0) + 1;
		if (tm != 1 && t >= tm) { xrc_count_mark(); tm = 1; }
	}
	if (dev.frames == 1 || t - t0 >= (unsigned long long)every) {
		struct timespec s7a; clock_gettime(CLOCK_MONOTONIC, &s7a);
		static double s7_last; /* spike7: how long the previous report block took */
		if (getenv("XRC_SPIKE_FPS")) { struct timespec q; clock_gettime(CLOCK_MONOTONIC, &q); (void)q; }
		if (dev.frames > 1)
			fprintf(stderr, "xrc: d3d9 frame %llu: %.1f fps, %llu draws/frame, %.2f ms cpu/frame (main thread %.2f; busy %.2f ms/frame)\n", dev.frames,
			        (double)(dev.frames - f0) * 1000.0 / (double)(t - t0), (ndraws - d0) / (dev.frames - f0),
			        (double)(cpu - c0) / 1000.0 / (double)(dev.frames - f0), (double)(mcpu - m0) / 1000.0 / (double)(dev.frames - f0),
			        ((double)(t - t0) * 1000.0 - (double)(xrc_frame_wait_us - w0)) / 1000.0 / (double)(dev.frames - f0));
		{ struct timespec q; clock_gettime(CLOCK_MONOTONIC, &q); if (getenv("XRC_SPIKE_FPS")) fprintf(stderr, "xrc: spike7: first fprintf %.2f ms\n", (q.tv_sec - s7a.tv_sec) * 1e3 + (q.tv_nsec - s7a.tv_nsec) * 1e-6); }
		if (dev.frames > 1 && xrc_gpu_ms_total >= 0) /* (the GPU's time: its command buffers, start to end) */
			fprintf(stderr, "xrc: d3d9 frame %llu: GPU %.2f ms/frame\n", dev.frames, (xrc_gpu_ms_total - g0) / (double)(dev.frames - f0));
#ifdef XRC_NAN_PROBE
		{
			extern unsigned long long xrc_nonfinite_n[2];
			fprintf(stderr, "xrc:   non-finite so far: %llu NaNs found by tests, %llu compares with an infinity\n", xrc_nonfinite_n[0], xrc_nonfinite_n[1]);
		}
#endif
		{ /* XRC_DISP_STATS: and the dispatched calls per frame */
			unsigned long long xrc_ncalls(void);
			static unsigned long long n0;
			unsigned long long n = xrc_ncalls();
			if (xrc_tool_env("XRC_DISP_STATS") && dev.frames > 1) fprintf(stderr, "xrc:   %.0f dispatched calls/frame\n", (double)(n - n0) / (double)(dev.frames - f0));
			n0 = n;
			{ void xrc_disp_dump(void); static int done; if (!done && xrc_tool_env("XRC_DISP_STATS") && dev.frames >= (unsigned long long)atoll(xrc_tool_env("XRC_DISP_STATS"))) { done = 1; xrc_disp_dump(); } }
		}
		/* XRC_THREAD_CPU: and the per-thread breakdown (a silent baseline at the first frame) */
		if (getenv("XRC_THREAD_CPU")) xrc_thread_report(dev.frames > 1 ? stderr : 0, dev.frames > 1 ? dev.frames - f0 : 1);
#ifdef XRC_REFERENCE
		{
			extern unsigned long long xrc_ref_query_wait_us, xrc_ref_query_polls, xrc_ref_query_blocks;
			static unsigned long long qw0, qp0, qb0;
			if (dev.frames > 1)
				fprintf(stderr, "xrc:   waiting for real queries (the GPU): %.2f ms/frame, %.0f polls, %.1f blocking waits/frame\n",
				        (double)(xrc_ref_query_wait_us - qw0) / 1000.0 / (double)(dev.frames - f0),
				        (double)(xrc_ref_query_polls - qp0) / (double)(dev.frames - f0),
				        (double)(xrc_ref_query_blocks - qb0) / (double)(dev.frames - f0));
			qw0 = xrc_ref_query_wait_us; qp0 = xrc_ref_query_polls; qb0 = xrc_ref_query_blocks;
			{
				extern unsigned long long xrc_ref_ub[8];
				static unsigned long long u0[8];
				double nf = (double)(dev.frames - f0);
				if (dev.frames > 1 && xrc_tool_env("XRC_IMPORT_TIME"))
					fprintf(stderr, "xrc:   buffer uploads, ms/frame: lock %.2f copy %.2f unlock %.2f; %.0f KB/frame\n",
					        (double)(xrc_ref_ub[0] - u0[0]) / 1000.0 / nf, (double)(xrc_ref_ub[1] - u0[1]) / 1000.0 / nf,
					        (double)(xrc_ref_ub[2] - u0[2]) / 1000.0 / nf, (double)(xrc_ref_ub[3] - u0[3]) / 1024.0 / nf);
				{
					extern unsigned long long xrc_ref_filtered, xrc_ref_forwarded;
					static unsigned long long f0_, w0_;
					if (dev.frames > 1 && xrc_tool_env("XRC_IMPORT_TIME"))
						fprintf(stderr, "xrc:   state calls: %.0f/frame not forwarded (no change), %.0f forwarded\n",
						        (double)(xrc_ref_filtered - f0_) / nf, (double)(xrc_ref_forwarded - w0_) / nf);
					f0_ = xrc_ref_filtered; w0_ = xrc_ref_forwarded;
				}
				if (dev.frames > 1 && xrc_tool_env("XRC_IMPORT_TIME"))
					fprintf(stderr, "xrc:   post_Unlock total %.2f ms/frame\n", (double)(xrc_ref_ub[4] - u0[4]) / 1000.0 / nf);
				memcpy(u0, xrc_ref_ub, sizeof u0);
			}
		}
#endif
		if (xrc_tool_env("XRC_IMPORT_TIME")) xrc_import_report(dev.frames > 1 ? stderr : 0, dev.frames > 1 ? dev.frames - f0 : 1);
		if (xrc_tool_env("XRC_TRACE_WAITS")) {
			void xrc_wait_report(FILE *f, unsigned long long nframes);
			xrc_wait_report(dev.frames > 1 ? stderr : 0, dev.frames > 1 ? dev.frames - f0 : 1);
		}
		if (dev.frames == 1) fprintf(stderr, "xrc: d3d9 frame 1\n");
		t0 = t; f0 = dev.frames; d0 = ndraws; c0 = cpu; m0 = mcpu; w0 = xrc_frame_wait_us; g0 = xrc_gpu_ms_total;
		{ struct timespec s7b; clock_gettime(CLOCK_MONOTONIC, &s7b); s7_last = (s7b.tv_sec - s7a.tv_sec) * 1e3 + (s7b.tv_nsec - s7a.tv_nsec) * 1e-6; if (getenv("XRC_SPIKE_FPS")) fprintf(stderr, "xrc: spike7: fps block %.2f ms\n", s7_last); }
	}
}
static void v_Present(XCtx *c) { if (d3_be) d3_be->present(); present(); RET(D3D_OK, 5); }
static void v_GetBackBuffer(XCtx *c) {
	if (A(1) || A(2)) RET(D3DERR_INVALIDCALL, 5);
	addref_g(dev.backbuf);
	M32(A(4)) = dev.backbuf;
	RET(D3D_OK, 5);
}
/* D3DRASTER_STATUS: InVBlank ScanLine */
int xrc_raster_mode(void); /* ovr_frame.c: the vblank model (XRC_RASTER=0: this stub's old {0, 0}) */
void xrc_raster_status(unsigned *in_vblank, unsigned *scanline);
static void raster_fill(u32 a) {
	unsigned v = 0, l = 0;
	if (xrc_raster_mode()) xrc_raster_status(&v, &l);
	M32(a) = v; M32(a + 4) = l;
}
static void v_GetRasterStatus(XCtx *c) { raster_fill(A(2)); RET(D3D_OK, 3); }
static void v_GetGammaRamp(XCtx *c) {
	u32 p = A(2), k;
	for (k = 0; k < 256; k++) M16(p + 2 * k) = M16(p + 512 + 2 * k) = M16(p + 1024 + 2 * k) = (u16)(k * 257);
	RET(0, 3);
}

/* textures */
static u32 make_tex(int kind, Iface *f, u32 w, u32 h, u32 d, u32 levels, u32 usage, u32 fmt, u32 pool) {
	u32 i = new_obj(kind, f);
	Obj *o = &OBJ(i);
	o->w = w; o->h = h; o->d = d; o->fmt = fmt; o->usage = usage; o->pool = pool;
	o->levels = (usage & 0x400 /* AUTOGENMIPMAP */) ? 1 : nlevels(w, kind == K_CUBE ? w : h, kind == K_VTEX ? d : 1, levels);
	if (tracing()) fprintf(stderr, "xrc: d3d9 create %s %ux%ux%u levels %u fmt %08x usage %x pool %u -> %08x\n", f->iface, w, h, d, o->levels, fmt, usage, pool, o->guest);
	return o->guest;
}
static void v_CreateTexture(XCtx *c) {
	M32(A(7)) = make_tex(K_TEX, &if_tex, A(1), A(2), 1, A(3), A(4), A(5), A(6));
	RET(D3D_OK, 9);
}
static void v_CreateVolumeTexture(XCtx *c) {
	M32(A(8)) = make_tex(K_VTEX, &if_vtex, A(1), A(2), A(3), A(4), A(5), A(6), A(7));
	RET(D3D_OK, 10);
}
static void v_CreateCubeTexture(XCtx *c) {
	M32(A(6)) = make_tex(K_CUBE, &if_cube, A(1), A(1), 1, A(2), A(3), A(4), A(5));
	RET(D3D_OK, 8);
}
static void v_CreateVertexBuffer(XCtx *c) {
	u32 i = new_obj(K_VB, &if_vb);
	Obj *o = &OBJ(i);
	o->size = A(1); o->usage = A(2); o->fvf = A(3); o->pool = A(4); o->fmt = 100; /* D3DFMT_VERTEXDATA */
	M32(A(5)) = o->guest;
	RET(D3D_OK, 7);
}
static void v_CreateIndexBuffer(XCtx *c) {
	u32 i = new_obj(K_IB, &if_ib);
	Obj *o = &OBJ(i);
	o->size = A(1); o->usage = A(2); o->fmt = A(3); o->pool = A(4);
	M32(A(5)) = o->guest;
	RET(D3D_OK, 7);
}
static void v_CreateRenderTarget(XCtx *c) { M32(A(7)) = new_surface(A(1), A(2), A(3), 1, 0, A(4), A(5)); RET(D3D_OK, 9); }
static void v_CreateDepthStencilSurface(XCtx *c) { M32(A(7)) = new_surface(A(1), A(2), A(3), 2, 0, A(4), A(5)); RET(D3D_OK, 9); }
static void v_CreateOffscreenPlainSurface(XCtx *c) { M32(A(5)) = new_surface(A(1), A(2), A(3), 0, A(4), 0, 0); RET(D3D_OK, 7); }
static void v_SetRenderTarget(XCtx *c) {
	if (A(1) >= 4) RET(D3DERR_INVALIDCALL, 3);
	if (A(2)) addref_g(A(2));
	if (dev.rt[A(1)]) release_g(dev.rt[A(1)]);
	dev.rt[A(1)] = A(2);
	if (A(1) == 0 && A(2)) {
		Obj *o = obj_of(A(2));
		if (o) {
			dev.vp[0] = dev.vp[1] = 0; dev.vp[2] = o->w; dev.vp[3] = o->h; dev.vp[4] = 0; dev.vp[5] = f32bits(1.0f);
			dev.scissor[0] = dev.scissor[1] = 0; dev.scissor[2] = o->w; dev.scissor[3] = o->h;
		}
	}
	RET(D3D_OK, 3);
}
static void v_GetRenderTarget(XCtx *c) {
	u32 s = A(1) < 4 ? dev.rt[A(1)] : 0;
	if (!s) { M32(A(2)) = 0; RET(0x8876087du /* D3DERR_NOTFOUND */, 3); }
	addref_g(s);
	M32(A(2)) = s;
	RET(D3D_OK, 3);
}
static void v_SetDepthStencilSurface(XCtx *c) {
	if (A(1)) addref_g(A(1));
	if (dev.ds) release_g(dev.ds);
	dev.ds = A(1);
	RET(D3D_OK, 2);
}
static void v_GetDepthStencilSurface(XCtx *c) {
	if (!dev.ds) { M32(A(1)) = 0; RET(0x8876087du, 2); }
	addref_g(dev.ds);
	M32(A(1)) = dev.ds;
	RET(D3D_OK, 2);
}
/* bound objects keep a reference while bound */
static void bind(u32 *slot, u32 g) {
	if (*slot == g) return; /* (rebinding what is bound: the same references) */
	if (g) addref_g(g);
	if (*slot) release_g(*slot);
	*slot = g;
}
static void v_SetTexture(XCtx *c) {
	u32 s = A(1);
	if (s >= 257 && s <= 260) s = 16 + (s - 257); /* D3DVERTEXTEXTURESAMPLER0.. */
	if (s >= 32) RET(D3DERR_INVALIDCALL, 3);
	bind(&dev.tex[s], A(2));
	RET(D3D_OK, 3);
}
static void v_GetTexture(XCtx *c) {
	u32 s = A(1);
	if (s >= 257 && s <= 260) s = 16 + (s - 257);
	if (s >= 32) RET(D3DERR_INVALIDCALL, 3);
	if (dev.tex[s]) addref_g(dev.tex[s]);
	M32(A(2)) = dev.tex[s];
	RET(D3D_OK, 3);
}
static void v_SetStreamSource(XCtx *c) {
	if (A(1) >= 16) RET(D3DERR_INVALIDCALL, 5);
	bind(&dev.stream[A(1)][0], A(2));
	dev.stream[A(1)][1] = A(3); dev.stream[A(1)][2] = A(4);
	RET(D3D_OK, 5);
}
static void v_GetStreamSource(XCtx *c) {
	if (A(1) >= 16) RET(D3DERR_INVALIDCALL, 5);
	if (dev.stream[A(1)][0]) addref_g(dev.stream[A(1)][0]);
	M32(A(2)) = dev.stream[A(1)][0]; M32(A(3)) = dev.stream[A(1)][1]; M32(A(4)) = dev.stream[A(1)][2];
	RET(D3D_OK, 5);
}
static void v_SetStreamSourceFreq(XCtx *c) { if (A(1) < 16) dev.freq[A(1)] = A(2); RET(D3D_OK, 3); }
static void v_GetStreamSourceFreq(XCtx *c) { M32(A(2)) = A(1) < 16 ? dev.freq[A(1)] : 1; RET(D3D_OK, 3); }
/* XRC_TRACE_BB: the first 300 draws / StretchRects into the back buffer, with the viewport and scissor (aspect bugs) */
static int bb_trace(void) { static int t = -1, n; if (t < 0) t = getenv("XRC_TRACE_BB") != 0; return t && n++ < 300; }
/* the draws: counted, surveyed (XRC_D3D_SURVEY), drawn by the backend */
static void draw(XCtx *c, int kind, int n) {
	u32 a[8], k;
	for (k = 0; k < (u32)n - 1 && k < 8; k++) a[k] = A(k + 1);
	ndraws++;
	if (dev.rt[0] == dev.backbuf && bb_trace())
		fprintf(stderr, "xrc: bb draw %d prims %u: vp %u,%u %ux%u scissor %d,%d-%d,%d\n", kind, a[kind == 1 ? 5 : 2], dev.vp[0], dev.vp[1], dev.vp[2], dev.vp[3],
		        (int)dev.scissor[0], (int)dev.scissor[1], (int)dev.scissor[2], (int)dev.scissor[3]);
	{ /* XRC_TRACE_RTW=width: the first 600 draws into render targets that wide (debug) */
		static int tw = -1, tn;
		Obj *ro, *t0;
		if (tw < 0) tw = getenv("XRC_TRACE_RTW") ? atoi(getenv("XRC_TRACE_RTW")) : 0;
		if (tw && (ro = obj_of(dev.rt[0])) && ro->w == (u32)tw && tn++ < 600) {
			t0 = obj_of(dev.tex[0]);
			fprintf(stderr, "xrc: rtw draw %d rt %x prims %u vp %u,%u %ux%u sc %d,%d-%d,%d tex0 %ux%u draw# %u\n", kind, dev.rt[0], a[kind == 1 ? 5 : 2], dev.vp[0], dev.vp[1],
			        dev.vp[2], dev.vp[3], (int)dev.scissor[0], (int)dev.scissor[1], (int)dev.scissor[2], (int)dev.scissor[3], t0 ? t0->w : 0, t0 ? t0->h : 0, (u32)ndraws);
		}
	}
	if (survey_dir()) survey_draw(kind, a[0]);
	if (d3_be) d3_be->draw(kind, a);
}
static void v_DrawPrimitive(XCtx *c) { draw(c, 0, 4); RET(D3D_OK, 4); }
static void v_DrawIndexedPrimitive(XCtx *c) { draw(c, 1, 7); RET(D3D_OK, 7); }
/* the UP draws leave stream 0 unset (and DrawIndexedPrimitiveUP the indices), as the real ones do */
static void v_DrawPrimitiveUP(XCtx *c) {
	draw(c, 2, 5);
	bind(&dev.stream[0][0], 0); dev.stream[0][1] = dev.stream[0][2] = 0;
	RET(D3D_OK, 5);
}
static void v_DrawIndexedPrimitiveUP(XCtx *c) {
	draw(c, 3, 9);
	bind(&dev.stream[0][0], 0); dev.stream[0][1] = dev.stream[0][2] = 0;
	bind(&dev.ib, 0);
	RET(D3D_OK, 9);
}
static void v_Clear(XCtx *c) {
	u32 a[6] = {A(1), A(2), A(3), A(4), A(5), A(6)};
	if (dev.rt[0] == dev.backbuf && bb_trace()) fprintf(stderr, "xrc: bb Clear count %u rects %x flags %x color %08x vp %u,%u %ux%u\n", a[0], a[1], a[2], a[3], dev.vp[0], dev.vp[1], dev.vp[2], dev.vp[3]);
	if (d3_be) d3_be->clear(a);
	RET(D3D_OK, 7);
}
static void v_StretchRect(XCtx *c) {
	u32 a[5] = {A(1), A(2), A(3), A(4), A(5)};
	if ((a[2] == dev.backbuf || a[0] == dev.backbuf) && bb_trace()) {
		Obj *so = obj_of(a[0]), *d = obj_of(a[2]);
		fprintf(stderr, "xrc: bb StretchRect %x (%ux%u) rect %s -> %x (%ux%u) rect %s filter %u\n", a[0], so ? so->w : 0, so ? so->h : 0, a[1] ? "set" : "all",
		        a[2], d ? d->w : 0, d ? d->h : 0, a[3] ? "set" : "all", a[4]);
		if (a[1]) fprintf(stderr, "xrc:   src %d,%d-%d,%d\n",(int)M32(a[1]), (int)M32(a[1] + 4), (int)M32(a[1] + 8), (int)M32(a[1] + 12));
		if (a[3]) fprintf(stderr, "xrc:   dst %d,%d-%d,%d\n",(int)M32(a[3]), (int)M32(a[3] + 4), (int)M32(a[3] + 8), (int)M32(a[3] + 12));
	}
	if (a[2] == dev.backbuf && getenv("XRC_BB_FULLRECT")) a[3] = 0; /* (debug: the copy into the back buffer stretched to all of it) */
	if (d3_be) d3_be->stretch_rect(a);
	RET(D3D_OK, 6);
}
static void v_ColorFill(XCtx *c) {
	u32 a[3] = {A(1), A(2), A(3)};
	if (d3_be) d3_be->color_fill(a);
	RET(D3D_OK, 4);
}
static void v_GetRenderTargetData(XCtx *c) {
	u32 a[2] = {A(1), A(2)};
	RET(d3_be ? d3_be->rt_data(a) : D3D_OK, 3);
}
/* UpdateSurface(src, srcRect, dst, dstPoint) / UpdateTexture(src, dst): copied in guest memory (what the GPU has of
 * the destination follows from its next use) */
static void v_UpdateSurface(XCtx *c) {
	Obj *s = obj_of(A(1)), *d = obj_of(A(3));
	u32 sp, dp, sb, db, x0 = 0, y0 = 0, x1, y1, dx = 0, dy = 0, bpp, y, rows, bx;
	if (!s || !d || s->fmt != d->fmt) RET(D3DERR_INVALIDCALL, 5);
	x1 = s->w; y1 = s->h;
	if (A(2)) { x0 = M32(A(2)); y0 = M32(A(2) + 4); x1 = M32(A(2) + 8); y1 = M32(A(2) + 12); }
	if (A(4)) { dx = M32(A(4)); dy = M32(A(4) + 4); }
	sb = surf_bits(s, &sp); db = surf_bits(d, &dp);
	bpp = fmt_bytes(s->fmt);
	if (is_dxt(s->fmt)) { x0 /= 4; x1 = (x1 + 3) / 4; y0 /= 4; y1 = (y1 + 3) / 4; dx /= 4; dy /= 4; }
	rows = y1 > y0 ? y1 - y0 : 0;
	bx = x1 > x0 ? (x1 - x0) * bpp : 0;
	for (y = 0; y < rows; y++) memmove(xrc_mem + db + (dy + y) * dp + dx * bpp, xrc_mem + sb + (y0 + y) * sp + x0 * bpp, bx);
	if (d3_be) {
		if (d->parent) d3_be->locked(obj_of(d->parent), d->face, d->level, 0, 0);
		else d3_be->locked(d, 0, 0, 0, 0);
	}
	RET(D3D_OK, 5);
}
static void v_UpdateTexture(XCtx *c) {
	Obj *s = obj_of(A(1)), *d = obj_of(A(2));
	u32 f, l, k, faces;
	if (!s || !d || s->kind != d->kind || s->fmt != d->fmt) RET(D3DERR_INVALIDCALL, 3);
	faces = d->kind == K_CUBE ? 6 : 1;
	for (l = 0; l < d->levels; l++) /* (the source level of the same size) */
		for (k = 0; k < s->levels; k++)
			if (dim(s->w, k) == dim(d->w, l) && dim(s->h, k) == dim(d->h, l)) {
				for (f = 0; f < faces; f++) {
					memcpy(xrc_mem + level_mem(d, f, l), xrc_mem + level_mem(s, f, k), level_size(d, l));
					if (d3_be) d3_be->locked(d, f, l, 0, 0);
				}
				break;
			}
	RET(D3D_OK, 3);
}
static void v_SetIndices(XCtx *c) { bind(&dev.ib, A(1)); RET(D3D_OK, 2); }
static void v_GetIndices(XCtx *c) { if (dev.ib) addref_g(dev.ib); M32(A(1)) = dev.ib; RET(D3D_OK, 2); }
static void v_SetVertexDeclaration(XCtx *c) { bind(&dev.decl, A(1)); RET(D3D_OK, 2); }
static void v_GetVertexDeclaration(XCtx *c) { if (dev.decl) addref_g(dev.decl); M32(A(1)) = dev.decl; RET(D3D_OK, 2); }
static void v_SetVertexShader(XCtx *c) { bind(&dev.vs, A(1)); RET(D3D_OK, 2); }
static void v_GetVertexShader(XCtx *c) { if (dev.vs) addref_g(dev.vs); M32(A(1)) = dev.vs; RET(D3D_OK, 2); }
static void v_SetPixelShader(XCtx *c) { bind(&dev.ps, A(1)); RET(D3D_OK, 2); }
static void v_GetPixelShader(XCtx *c) { if (dev.ps) addref_g(dev.ps); M32(A(1)) = dev.ps; RET(D3D_OK, 2); }
static void v_SetFVF(XCtx *c) { dev.fvf = A(1); RET(D3D_OK, 2); }
static void v_GetFVF(XCtx *c) { M32(A(1)) = dev.fvf; RET(D3D_OK, 2); }
static void v_SetRenderState(XCtx *c) { if (A(1) < 256) dev.rs[A(1)] = A(2); RET(D3D_OK, 3); }
static void v_GetRenderState(XCtx *c) { M32(A(2)) = A(1) < 256 ? dev.rs[A(1)] : 0; RET(D3D_OK, 3); }
static u32 sampler(u32 s) { return s >= 257 && s <= 260 ? 16 + (s - 257) : s; }
static void v_SetSamplerState(XCtx *c) { u32 s = sampler(A(1)); if (s < 32 && A(2) < 16) dev.ss[s][A(2)] = A(3); RET(D3D_OK, 4); }
static void v_GetSamplerState(XCtx *c) { u32 s = sampler(A(1)); M32(A(3)) = s < 32 && A(2) < 16 ? dev.ss[s][A(2)] : 0; RET(D3D_OK, 4); }
static void v_SetTextureStageState(XCtx *c) { if (A(1) < 8 && A(2) < 33) dev.tss[A(1)][A(2)] = A(3); RET(D3D_OK, 4); }
static void v_GetTextureStageState(XCtx *c) { M32(A(3)) = A(1) < 8 && A(2) < 33 ? dev.tss[A(1)][A(2)] : 0; RET(D3D_OK, 4); }
static void v_SetTransform(XCtx *c) { if (A(1) < 512) memcpy(dev.xf[A(1)], xrc_mem + A(2), 64); RET(D3D_OK, 3); }
static void v_GetTransform(XCtx *c) { if (A(1) < 512) memcpy(xrc_mem + A(2), dev.xf[A(1)], 64); RET(D3D_OK, 3); }
static void v_SetViewport(XCtx *c) { memcpy(dev.vp, xrc_mem + A(1), 24); RET(D3D_OK, 2); }
static void v_GetViewport(XCtx *c) { memcpy(xrc_mem + A(1), dev.vp, 24); RET(D3D_OK, 2); }
static void v_SetScissorRect(XCtx *c) { memcpy(dev.scissor, xrc_mem + A(1), 16); RET(D3D_OK, 2); }
static void v_GetScissorRect(XCtx *c) { memcpy(xrc_mem + A(1), dev.scissor, 16); RET(D3D_OK, 2); }
#define CONSTS(name, arr, max, w) \
	static void v_Set##name(XCtx *c) { if (A(1) + A(3) <= (max)) memcpy(&dev.arr[A(1) * (w)], xrc_mem + A(2), A(3) * (w) * 4); RET(D3D_OK, 4); } \
	static void v_Get##name(XCtx *c) { if (A(1) + A(3) <= (max)) memcpy(xrc_mem + A(2), &dev.arr[A(1) * (w)], A(3) * (w) * 4); RET(D3D_OK, 4); }
CONSTS(VertexShaderConstantF, vsf, 256, 4)
CONSTS(PixelShaderConstantF, psf, 224, 4)
CONSTS(VertexShaderConstantI, vsi, 16, 4)
CONSTS(PixelShaderConstantI, psi, 16, 4)
CONSTS(VertexShaderConstantB, vsb, 16, 1)
CONSTS(PixelShaderConstantB, psb, 16, 1)
static void v_GetSoftwareVertexProcessing(XCtx *c) { RET(0, 1); }
static void v_GetNPatchMode(XCtx *c) { c->st0 = 0.0; RET(0, 1); }
static void v_ValidateDevice(XCtx *c) { if (A(1)) M32(A(1)) = 1; RET(D3D_OK, 2); }
static void v_EndStateBlock(XCtx *c) { M32(A(1)) = OBJ(new_obj(K_SB, &if_sb)).guest; RET(D3D_OK, 2); }
static void v_CreateStateBlock(XCtx *c) { M32(A(2)) = OBJ(new_obj(K_SB, &if_sb)).guest; RET(D3D_OK, 3); }
static void v_Present_sw(XCtx *c) { if (d3_be) d3_be->present(); present(); RET(D3D_OK, 6); }

/* shaders and declarations keep a copy of their bytecode / elements */
static u32 shader_len(u32 p) {
	u32 n = 1;
	while (M32(p + 4 * (n - 1)) != 0x0000ffffu && n < (1u << 20)) n++;
	return 4 * n;
}
static void v_CreateVertexShader(XCtx *c) {
	u32 i = new_obj(K_VS, &if_vs), n = shader_len(A(1));
	OBJ(i).size = n; OBJ(i).mem = xrc_heap_alloc(n, 0); memcpy(xrc_mem + OBJ(i).mem, xrc_mem + A(1), n);
	M32(A(2)) = OBJ(i).guest;
	survey_shader(OBJ(i).guest, OBJ(i).mem, n, 0);
	RET(D3D_OK, 3);
}
static void v_CreatePixelShader(XCtx *c) {
	u32 i = new_obj(K_PS, &if_ps), n = shader_len(A(1));
	OBJ(i).size = n; OBJ(i).mem = xrc_heap_alloc(n, 0); memcpy(xrc_mem + OBJ(i).mem, xrc_mem + A(1), n);
	M32(A(2)) = OBJ(i).guest;
	survey_shader(OBJ(i).guest, OBJ(i).mem, n, 1);
	RET(D3D_OK, 3);
}
static void s_GetFunction(XCtx *c) {
	Obj *o = self(c);
	if (!A(1)) { M32(A(2)) = o->size; RET(D3D_OK, 3); }
	if (M32(A(2)) < o->size) RET(D3DERR_INVALIDCALL, 3);
	memcpy(xrc_mem + A(1), xrc_mem + o->mem, o->size);
	RET(D3D_OK, 3);
}
/* D3DVERTEXELEMENT9 {Stream u16, Offset u16, Type u8, Method u8, Usage u8, UsageIndex u8}, ends with Stream 0xff */
static void v_CreateVertexDeclaration(XCtx *c) {
	u32 i = new_obj(K_DECL, &if_decl), n = 1, p = A(1);
	while (M16(p + 8 * (n - 1)) != 0xff && n < 65) n++;
	OBJ(i).size = n; OBJ(i).mem = xrc_heap_alloc(8 * n, 0); memcpy(xrc_mem + OBJ(i).mem, xrc_mem + p, 8 * n);
	M32(A(2)) = OBJ(i).guest;
	RET(D3D_OK, 3);
}
static void dc_GetDeclaration(XCtx *c) {
	Obj *o = self(c);
	if (A(1)) memcpy(xrc_mem + A(1), xrc_mem + o->mem, 8 * o->size);
	M32(A(2)) = o->size;
	RET(D3D_OK, 3);
}

/* queries: everything is done at once */
static void v_CreateQuery(XCtx *c) {
	u32 i;
	if (!A(2)) RET(D3D_OK, 3); /* "is this query type supported" */
	i = new_obj(K_QUERY, &if_query);
	OBJ(i).qtype = A(1);
	M32(A(2)) = OBJ(i).guest;
	RET(D3D_OK, 3);
}
static u32 query_size(u32 t) {
	switch (t) {
	case 8: return 4;  /* EVENT: BOOL */
	case 9: return 4;  /* OCCLUSION: DWORD */
	case 10: return 8; /* TIMESTAMP: UINT64 */
	case 11: return 4; /* TIMESTAMPDISJOINT */
	case 12: return 8; /* TIMESTAMPFREQ */
	default: return 0;
	}
}
static void q_GetType(XCtx *c) { RET(self(c)->qtype, 1); }
static void q_GetDataSize(XCtx *c) { RET(query_size(self(c)->qtype), 1); }
static void q_GetData(XCtx *c) {
	Obj *o = self(c);
	u32 n = query_size(o->qtype);
	if (A(1) && A(2)) {
		memset(xrc_mem + A(1), 0, A(2));
		if (o->qtype == 8) M32(A(1)) = 1;
		if (o->qtype == 9 && A(2) >= 4) M32(A(1)) = 1000;
		if (o->qtype == 10 && A(2) >= 8) M64(A(1)) = dev.frames * 16666667ull;
		if (o->qtype == 12 && A(2) >= 8) M64(A(1)) = 1000000000ull;
	}
	(void)n;
	RET(D3D_OK, 4);
}

/* ============================================================================================ resources */
static u32 res_type(Obj *o) {
	switch (o->kind) {
	case K_SURF: return 1; case K_VOL: return 2; case K_TEX: return 3; case K_VTEX: return 4;
	case K_CUBE: return 5; case K_VB: return 6; case K_IB: return 7;
	}
	return 0;
}
static void r_GetType(XCtx *c) { RET(res_type(self(c)), 1); }
static void r_GetPriority(XCtx *c) { RET(0, 1); }
static void r_SetPriority(XCtx *c) { RET(0, 2); }
/* private data: per object, by GUID; D3DSPD_IUNKNOWN (1) data is an interface pointer, AddRef'd while stored */
static PData *pd_find(u32 o, u32 g) {
	unsigned k;
	for (k = 0; k < npdata; k++)
		if (pdata[k].obj == o && !memcmp(pdata[k].guid, xrc_mem + g, 16)) return &pdata[k];
	return 0;
}
static void pd_free(PData *p) {
	if (p->iunk) { u32 i = M32(p->data); if (i) xrc_call_guest_release(i); }
	xrc_heap_free(p->data);
	*p = pdata[--npdata];
}
static void r_SetPrivateData(XCtx *c) {
	PData *p = pd_find(A(0), A(1));
	u32 size = A(3), flags = A(4);
	if (p) pd_free(p);
	if (npdata == cap_pdata) {
		cap_pdata = cap_pdata ? cap_pdata * 2 : 256;
		pdata = (PData *)realloc(pdata, cap_pdata * sizeof(PData));
	}
	p = &pdata[npdata++];
	p->obj = A(0);
	memcpy(p->guid, xrc_mem + A(1), 16);
	p->size = (flags & 1) ? 4 : size;
	p->data = xrc_heap_alloc(p->size ? p->size : 1, 0);
	p->iunk = flags & 1;
	if (p->iunk) { M32(p->data) = A(2); if (A(2)) xrc_call_guest_addref(A(2)); }
	else memcpy(xrc_mem + p->data, xrc_mem + A(2), size);
	RET(D3D_OK, 5);
}
static void r_GetPrivateData(XCtx *c) {
	PData *p = pd_find(A(0), A(1));
	if (!p) RET(0x8876087du /* D3DERR_NOTFOUND */, 4);
	if (!A(2)) { M32(A(3)) = p->size; RET(D3D_OK, 4); }
	if (M32(A(3)) < p->size) { M32(A(3)) = p->size; RET(0x88760868u /* D3DERR_MOREDATA */, 4); }
	memcpy(xrc_mem + A(2), xrc_mem + p->data, p->size);
	if (p->iunk && M32(p->data)) xrc_call_guest_addref(M32(p->data));
	M32(A(3)) = p->size;
	RET(D3D_OK, 4);
}
static void r_FreePrivateData(XCtx *c) {
	PData *p = pd_find(A(0), A(1));
	if (!p) RET(0x8876087du, 2);
	pd_free(p);
	RET(D3D_OK, 2);
}
static void t_GetLevelCount(XCtx *c) { RET(self(c)->levels, 1); }
static void t_GetLOD(XCtx *c) { RET(0, 1); }
static void t_SetLOD(XCtx *c) { RET(0, 2); }
static void t_GetAutoGenFilterType(XCtx *c) { RET(2, 1); }
static void put_sdesc(u32 p, u32 fmt, u32 type, u32 usage, u32 pool, u32 ms, u32 msq, u32 w, u32 h) {
	M32(p) = fmt; M32(p + 4) = type; M32(p + 8) = usage; M32(p + 12) = pool; M32(p + 16) = ms; M32(p + 20) = msq;
	M32(p + 24) = w; M32(p + 28) = h;
}
u32 level_mem(Obj *t, u32 face, u32 l) {
	if (!t->lvl[face][l]) {
		t->lvl[face][l] = xrc_heap_alloc(level_size(t, l), 1);
		if (!t->lvl[face][l]) xrc_fatal(0, "d3d9: out of guest memory for a %ux%u texture level", dim(t->w, l), dim(t->h, l));
	}
	return t->lvl[face][l];
}
/* LockRect into D3DLOCKED_RECT {Pitch, pBits}; rect (optional) offsets the pointer */
static void lock_out(u32 lr, u32 base, u32 fmt, u32 w, u32 rect) {
	u32 pitch = pitch_of(fmt, w), p = base;
	if (rect) {
		u32 x = M32(rect), y = M32(rect + 4);
		p += is_dxt(fmt) ? (y / 4) * pitch + (x / 4) * fmt_bytes(fmt) : y * pitch + x * fmt_bytes(fmt);
	}
	M32(lr) = pitch;
	M32(lr + 4) = p;
}
static void tx_GetLevelDesc(XCtx *c) {
	Obj *o = self(c);
	if (A(1) >= o->levels) RET(D3DERR_INVALIDCALL, 3);
	put_sdesc(A(2), o->fmt, 1, o->usage, o->pool, 0, 0, dim(o->w, A(1)), dim(o->h, A(1)));
	RET(D3D_OK, 3);
}
static u32 level_surface(Obj *t, u32 face, u32 l) {
	if (!t->sub[face][l]) {
		u32 g = new_surface(dim(t->w, l), dim(t->h, l), t->fmt, t->usage, t->pool, 0, 0);
		Obj *s = obj_of(g);
		s->parent = t->guest; s->level = l; s->face = face;
		t->sub[face][l] = g;
	}
	return t->sub[face][l];
}
static void tx_GetSurfaceLevel(XCtx *c) {
	Obj *o = self(c);
	if (A(1) >= o->levels) RET(D3DERR_INVALIDCALL, 3);
	M32(A(2)) = level_surface(o, 0, A(1));
	o->refs++;
	RET(D3D_OK, 3);
}
static void tx_LockRect(XCtx *c) {
	Obj *o = self(c);
	if (A(1) >= o->levels) RET(D3DERR_INVALIDCALL, 5);
	lock_out(A(2), level_mem(o, 0, A(1)), o->fmt, dim(o->w, A(1)), A(3));
	if (d3_be && !(A(4) & 0x10)) d3_be->locked(o, 0, A(1), 0, 0); /* (not D3DLOCK_READONLY) */
	RET(D3D_OK, 5);
}
static void tx_UnlockRect(XCtx *c) { RET(D3D_OK, 2); }
static void cb_GetLevelDesc(XCtx *c) { tx_GetLevelDesc(c); }
static void cb_GetCubeMapSurface(XCtx *c) {
	Obj *o = self(c);
	if (A(1) >= 6 || A(2) >= o->levels) RET(D3DERR_INVALIDCALL, 4);
	M32(A(3)) = level_surface(o, A(1), A(2));
	o->refs++;
	RET(D3D_OK, 4);
}
static void cb_LockRect(XCtx *c) {
	Obj *o = self(c);
	if (A(1) >= 6 || A(2) >= o->levels) RET(D3DERR_INVALIDCALL, 6);
	lock_out(A(3), level_mem(o, A(1), A(2)), o->fmt, dim(o->w, A(2)), A(4));
	if (d3_be && !(A(5) & 0x10)) d3_be->locked(o, A(1), A(2), 0, 0);
	RET(D3D_OK, 6);
}
static void cb_UnlockRect(XCtx *c) { RET(D3D_OK, 3); }
/* D3DVOLUME_DESC: Format Type Usage Pool Width Height Depth; D3DLOCKED_BOX: RowPitch SlicePitch pBits */
static void vt_GetLevelDesc(XCtx *c) {
	Obj *o = self(c);
	u32 p = A(2), l = A(1);
	if (l >= o->levels) RET(D3DERR_INVALIDCALL, 3);
	M32(p) = o->fmt; M32(p + 4) = 2; M32(p + 8) = o->usage; M32(p + 12) = o->pool;
	M32(p + 16) = dim(o->w, l); M32(p + 20) = dim(o->h, l); M32(p + 24) = dim(o->d, l);
	RET(D3D_OK, 3);
}
static void vt_LockBox(XCtx *c) {
	Obj *o = self(c);
	u32 l = A(1), b = A(2), pitch;
	if (l >= o->levels) RET(D3DERR_INVALIDCALL, 5);
	pitch = pitch_of(o->fmt, dim(o->w, l));
	M32(b) = pitch;
	M32(b + 4) = pitch * rows_of(o->fmt, dim(o->h, l));
	M32(b + 8) = level_mem(o, 0, l);
	if (A(3)) M32(b + 8) += M32(A(3) + 16) * M32(b + 4) + M32(A(3) + 4) * pitch + M32(A(3)) * fmt_bytes(o->fmt);
	if (d3_be && !(A(4) & 0x10)) d3_be->locked(o, 0, l, 0, 0);
	RET(D3D_OK, 5);
}
static void vt_UnlockBox(XCtx *c) { RET(D3D_OK, 2); }

/* surfaces */
static void sf_GetDesc(XCtx *c) {
	Obj *o = self(c);
	put_sdesc(A(1), o->fmt, 1, o->usage, o->pool, o->ms, o->msq, o->w, o->h);
	RET(D3D_OK, 2);
}
/* the container: a level surface's texture (any texture interface asked for), otherwise the device or the swap chain
 * only when that interface is the one asked for */
static void sf_GetContainer(XCtx *c) {
	Obj *o = self(c);
	u32 iid = M32(A(1)), g = 0;
	if (o->parent) {
		if (iid != 0xd0223b96u && iid != 0x794950f2u) g = o->parent;
	} else if (iid == 0xd0223b96u) {
		g = the_dev;
	} else if (iid == 0x794950f2u) {
		if (!dev.swap) dev.swap = OBJ(new_obj(K_SWAP, &if_swap)).guest;
		g = dev.swap;
	}
	if (!g) { M32(A(2)) = 0; RET(E_NOINTERFACE, 3); }
	addref_g(g);
	M32(A(2)) = g;
	RET(D3D_OK, 3);
}
static void sf_LockRect(XCtx *c) {
	Obj *o = self(c);
	u32 base;
	if (o->parent) base = level_mem(obj_of(o->parent), o->face, o->level);
	else {
		if (!o->mem) o->mem = xrc_heap_alloc(pitch_of(o->fmt, o->w) * rows_of(o->fmt, o->h), 1);
		base = o->mem;
	}
	lock_out(A(1), base, o->fmt, o->w, A(2));
	if (d3_be && !(A(3) & 0x10)) {
		if (o->parent) d3_be->locked(obj_of(o->parent), o->face, o->level, 0, 0);
		else d3_be->locked(o, 0, 0, 0, 0);
	}
	RET(D3D_OK, 4);
}
static void sf_UnlockRect(XCtx *c) { RET(D3D_OK, 1); }
static void sf_GetDC(XCtx *c) { M32(A(1)) = 0x00017000u; RET(D3D_OK, 2); }
static void sf_ReleaseDC(XCtx *c) { RET(D3D_OK, 2); }

/* vertex / index buffers: Lock(offset, size, void **, flags) */
static void b_Lock(XCtx *c) {
	Obj *o = self(c);
	if (!o->mem) {
		o->mem = xrc_heap_alloc(o->size ? o->size : 1, 1);
		if (!o->mem) xrc_fatal(c, "d3d9: out of guest memory for a %u-byte buffer", o->size);
	}
	M32(A(3)) = o->mem + A(1);
	if (d3_be && !(A(4) & 0x10)) d3_be->locked(o, 0, 0, A(1), A(2));
	RET(D3D_OK, 5);
}
static void b_Unlock(XCtx *c) { RET(D3D_OK, 1); }
/* D3DVERTEXBUFFER_DESC: Format Type Usage Pool Size FVF; D3DINDEXBUFFER_DESC: Format Type Usage Pool Size */
static void b_GetDesc(XCtx *c) {
	Obj *o = self(c);
	u32 p = A(1);
	M32(p) = o->fmt; M32(p + 4) = res_type(o); M32(p + 8) = o->usage; M32(p + 12) = o->pool; M32(p + 16) = o->size;
	if (o->kind == K_VB) M32(p + 20) = o->fvf;
	RET(D3D_OK, 2);
}

/* swap chain */
static void sw_GetBackBuffer(XCtx *c) {
	if (A(1)) RET(D3DERR_INVALIDCALL, 4);
	addref_g(dev.backbuf);
	M32(A(3)) = dev.backbuf;
	RET(D3D_OK, 4);
}
static void sw_GetPresentParameters(XCtx *c) { memcpy(xrc_mem + A(1), dev.pp, sizeof dev.pp); RET(D3D_OK, 2); }
static void sw_GetDisplayMode(XCtx *c) { put_mode(A(1), (u32)disp_w(), (u32)disp_h(), 22); RET(D3D_OK, 2); }
static void sw_GetRasterStatus(XCtx *c) { raster_fill(A(1)); RET(D3D_OK, 2); }

/* ============================================================================================ D3DX (d3dx9_43.dll)
 * ID3DXConstantTable: the CTAB comment block of the shader's bytecode, copied into guest memory (the names, type
 * infos and defaults are then guest pointers into it). D3DXSHADER_CONSTANTTABLE: Size Creator Version Constants
 * ConstantInfo Flags Target; D3DXSHADER_CONSTANTINFO (20 bytes): Name RegisterSet:16 RegisterIndex:16
 * RegisterCount:16 Reserved:16 TypeInfo DefaultValue; D3DXSHADER_TYPEINFO: Class Type Rows Columns Elements
 * StructMembers (u16 each) StructMemberInfo. Offsets are from the table's start. A D3DXHANDLE is the guest address of
 * the constant's CONSTANTINFO (a top-level constant) -- or of its name string, which D3DX also accepts. */
static u32 ct_base(Obj *o) { return o->mem; }
static u32 ct_count(Obj *o) { return M32(o->mem + 12); }
static u32 ct_info(Obj *o, u32 i) { return o->mem + M32(o->mem + 16) + 20 * i; }
static u32 ct_handle(Obj *o, u32 h) { /* a handle (or a name) -> CONSTANTINFO, 0 if none */
	u32 i, n = ct_count(o);
	if (!h) return 0;
	for (i = 0; i < n; i++) if (ct_info(o, i) == h) return h;
	for (i = 0; i < n; i++) if (strcmp(S(ct_base(o) + M32(ct_info(o, i))), S(h)) == 0) return ct_info(o, i);
	return 0;
}
static void hle_D3DXGetShaderConstantTable(XCtx *c) {
	u32 p = A(0), k, i;
	if (!p || !A(1)) RET(D3DERR_INVALIDCALL, 2);
	for (k = 1; k < (1u << 20); ) { /* tokens after the version */
		u32 t = M32(p + 4 * k);
		if (t == 0x0000ffffu) break;
		if ((t & 0xffffu) == 0xfffeu) {
			u32 len = t >> 16;
			if (len >= 1 && M32(p + 4 * k + 4) == FOURCC('C', 'T', 'A', 'B')) {
				u32 sz = (len - 1) * 4;
				i = new_obj(K_CTAB, &if_ctab);
				OBJ(i).size = sz;
				OBJ(i).mem = xrc_heap_alloc(sz, 0);
				memcpy(xrc_mem + OBJ(i).mem, xrc_mem + p + 4 * k + 8, sz);
				M32(A(1)) = OBJ(i).guest;
				RET(D3D_OK, 2);
			}
			k += 1 + len;
			continue;
		}
		k++;
	}
	M32(A(1)) = 0;
	RET(0x80004005u /* E_FAIL: no constant table */, 2);
}
static void ct_GetBufferPointer(XCtx *c) { RET(self(c)->mem, 1); }
static void ct_GetBufferSize(XCtx *c) { RET(self(c)->size, 1); }
/* D3DXCONSTANTTABLE_DESC: Creator Version Constants */
static void ct_GetDesc(XCtx *c) {
	Obj *o = self(c);
	M32(A(1)) = o->mem + M32(o->mem + 4);
	M32(A(1) + 4) = M32(o->mem + 8);
	M32(A(1) + 8) = ct_count(o);
	RET(D3D_OK, 2);
}
/* D3DXCONSTANT_DESC: Name RegisterSet RegisterIndex RegisterCount Class Type Rows Columns Elements StructMembers
 * Bytes DefaultValue */
static void ct_GetConstantDesc(XCtx *c) {
	Obj *o = self(c);
	u32 ci = ct_handle(o, A(1)), d = A(2), ti;
	if (!ci) RET(D3DERR_INVALIDCALL, 4);
	if (A(3) && !d) { M32(A(3)) = 1; RET(D3D_OK, 4); }
	ti = o->mem + M32(ci + 12);
	M32(d) = o->mem + M32(ci);
	M32(d + 4) = M16(ci + 4); M32(d + 8) = M16(ci + 6); M32(d + 12) = M16(ci + 8);
	M32(d + 16) = M16(ti); M32(d + 20) = M16(ti + 2); M32(d + 24) = M16(ti + 4); M32(d + 28) = M16(ti + 6);
	M32(d + 32) = M16(ti + 8) ? M16(ti + 8) : 1; M32(d + 36) = M16(ti + 10);
	M32(d + 40) = 4u * M16(ti + 4) * M16(ti + 6) * (M16(ti + 8) ? M16(ti + 8) : 1);
	M32(d + 44) = M32(ci + 16) ? o->mem + M32(ci + 16) : 0;
	if (A(3)) M32(A(3)) = 1;
	RET(D3D_OK, 4);
}
static void ct_GetSamplerIndex(XCtx *c) {
	Obj *o = self(c);
	u32 ci = ct_handle(o, A(1));
	RET(ci && M16(ci + 4) == 3 ? M16(ci + 6) : 0xffffffffu, 2);
}
static void ct_GetConstant(XCtx *c) {
	Obj *o = self(c);
	if (A(1)) xrc_fatal(c, "ID3DXConstantTable::GetConstant of a struct member: not implemented yet");
	RET(A(2) < ct_count(o) ? ct_info(o, A(2)) : 0, 3);
}
static void ct_GetConstantByName(XCtx *c) {
	Obj *o = self(c);
	u32 i, n = ct_count(o);
	if (A(1)) xrc_fatal(c, "ID3DXConstantTable::GetConstantByName in a struct (%s): not implemented yet", S(A(2)));
	for (i = 0; i < n; i++)
		if (strcmp(S(o->mem + M32(ct_info(o, i))), S(A(2))) == 0) RET(ct_info(o, i), 3);
	RET(0, 3);
}

/* pixel conversion between the common uncompressed formats (through 8-bit ARGB) */
static int px_ok(u32 f) { return f == 21 || f == 22 || f == 32 || f == 33 || f == 23 || f == 25 || f == 24 || f == 26 || f == 28 || f == 50 || f == 51; }
static u32 px_get(u32 f, u32 a) { /* -> A8R8G8B8 */
	u32 v, r, g, b, al;
	switch (f) {
	case 21: return M32(a);
	case 22: return M32(a) | 0xff000000u;
	case 32: v = M32(a); return (v & 0xff00ff00u) | (v & 0xff) << 16 | (v >> 16 & 0xff);
	case 33: v = M32(a); return 0xff000000u | (v & 0xff00u) | (v & 0xff) << 16 | (v >> 16 & 0xff);
	case 23: v = M16(a); r = v >> 11; g = v >> 5 & 63; b = v & 31; return 0xff000000u | (r * 255 / 31) << 16 | (g * 255 / 63) << 8 | b * 255 / 31;
	case 24: case 25: v = M16(a); al = f == 25 ? (v >> 15 ? 255 : 0) : 255; r = v >> 10 & 31; g = v >> 5 & 31; b = v & 31;
		return al << 24 | (r * 255 / 31) << 16 | (g * 255 / 31) << 8 | b * 255 / 31;
	case 26: v = M16(a); return (v >> 12) * 17u << 24 | (v >> 8 & 15) * 17u << 16 | (v >> 4 & 15) * 17u << 8 | (v & 15) * 17u;
	case 28: return (u32)M8(a) << 24;
	case 50: v = M8(a); return 0xff000000u | v << 16 | v << 8 | v;
	case 51: v = M8(a); return (u32)M8(a + 1) << 24 | v << 16 | v << 8 | v;
	}
	return 0;
}
static void px_put(u32 f, u32 a, u32 p) {
	u32 al = p >> 24, r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
	switch (f) {
	case 21: case 22: M32(a) = p; break;
	case 32: case 33: M32(a) = al << 24 | b << 16 | g << 8 | r; break;
	case 23: M16(a) = (u16)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); break;
	case 24: case 25: M16(a) = (u16)((al >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); break;
	case 26: M16(a) = (u16)((al >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); break;
	case 28: M8(a) = (u8)al; break;
	case 50: M8(a) = (u8)((r * 77 + g * 151 + b * 28) >> 8); break;
	case 51: M8(a) = (u8)((r * 77 + g * 151 + b * 28) >> 8); M8(a + 1) = (u8)al; break;
	}
}
/* a surface's memory and pitch (a texture level's, or its own) */
u32 surf_bits(Obj *o, u32 *pitch) {
	*pitch = pitch_of(o->fmt, o->w);
	if (o->parent) return level_mem(obj_of(o->parent), o->face, o->level);
	if (!o->mem) o->mem = xrc_heap_alloc(pitch_of(o->fmt, o->w) * rows_of(o->fmt, o->h), 1);
	return o->mem;
}
/* ============================================================================================ D3DX through the vtables
 * D3DX works on the objects it is given the way the real DLL does: through their vtables (creating with the device's
 * Create* methods, reading and writing with LockRect / UnlockRect). The game may hand it its own wrappers of the
 * runtime's objects (FF13's proxy device wraps every resource; its methods unwrap and call ours). */
static int mslot(Iface *f, const char *m) {
	int k;
	for (k = 0; k < f->n; k++) if (!strcmp(f->m[k], m)) return k;
	xrc_fatal(0, "d3d9: %s has no method %s", f->iface, m);
	return -1;
}
/* obj->method(args...) through the object's own vtable (n arguments after this) */
static u32 gcall(XCtx *c, u32 obj, Iface *f, const char *m, int n, const u32 *args) {
	u32 a[12];
	int k;
	a[0] = obj;
	for (k = 0; k < n; k++) a[k + 1] = args[k];
	return xrc_call_guest(c, M32(M32(obj) + 4u * (u32)mslot(f, m)), n + 1, a);
}
/* guest scratch memory below the stack (the HLE function's own arguments stay above it) */
static u32 scratch(XCtx *c, u32 n) { c->esp -= (n + 15u) & ~15u; return c->esp; }
typedef struct { u32 bits, pitch, fmt, w, h; } View;
/* a surface (any object with the surface vtable) locked: its description and memory */
static int view_surface(XCtx *c, u32 s, View *v, u32 flags) {
	u32 sp = c->esp, desc = scratch(c, 32), lr = scratch(c, 8), a[3];
	int ok;
	a[0] = desc;
	ok = gcall(c, s, &if_surf, "GetDesc", 1, a) == D3D_OK;
	a[0] = lr; a[1] = 0; a[2] = flags;
	ok = ok && gcall(c, s, &if_surf, "LockRect", 3, a) == D3D_OK;
	if (ok) { v->fmt = M32(desc); v->w = M32(desc + 24); v->h = M32(desc + 28); v->pitch = M32(lr); v->bits = M32(lr + 4); }
	c->esp = sp;
	return ok;
}
static void unview_surface(XCtx *c, u32 s) { gcall(c, s, &if_surf, "UnlockRect", 0, 0); }

/* copies a source image (bits, pitch, format, rect) into a surface rect: the same format and size row by row, else
 * pixel by pixel (point sampling when the sizes differ) */
static int blit_to_surface(const View *d, u32 drect, u32 src, u32 sfmt, u32 spitch, u32 srect) {
	u32 dpitch = d->pitch, dbits = d->bits;
	int dl = 0, dt = 0, dr = (int)d->w, db = (int)d->h, sl, st, sr, sb, x, y;
	if (drect) { dl = (int)M32(drect); dt = (int)M32(drect + 4); dr = (int)M32(drect + 8); db = (int)M32(drect + 12); }
	if (!srect) return 0;
	sl = (int)M32(srect); st = (int)M32(srect + 4); sr = (int)M32(srect + 8); sb = (int)M32(srect + 12);
	if (sfmt == d->fmt && sr - sl == dr - dl && sb - st == db - dt) {
		u32 rows = rows_of(sfmt, (u32)(sb - st)), w = pitch_of(sfmt, (u32)(sr - sl)), k;
		u32 so = is_dxt(sfmt) ? (u32)(st / 4) * spitch + (u32)(sl / 4) * fmt_bytes(sfmt) : (u32)st * spitch + (u32)sl * fmt_bytes(sfmt);
		u32 dof = is_dxt(sfmt) ? (u32)(dt / 4) * dpitch + (u32)(dl / 4) * fmt_bytes(sfmt) : (u32)dt * dpitch + (u32)dl * fmt_bytes(sfmt);
		for (k = 0; k < rows; k++) memmove(xrc_mem + dbits + dof + k * dpitch, xrc_mem + src + so + k * spitch, w);
		return 1;
	}
	if (!px_ok(sfmt) || !px_ok(d->fmt)) return -1;
	for (y = dt; y < db; y++)
		for (x = dl; x < dr; x++) {
			int sx = sl + (x - dl) * (sr - sl) / (dr - dl), sy = st + (y - dt) * (sb - st) / (db - dt);
			u32 p = px_get(sfmt, src + (u32)sy * spitch + (u32)sx * fmt_bytes(sfmt));
			px_put(d->fmt, dbits + (u32)y * dpitch + (u32)x * fmt_bytes(d->fmt), p);
		}
	return 1;
}
/* D3DXLoadSurfaceFromMemory(dest, destPalette, destRect, src, srcFormat, srcPitch, srcPalette, srcRect, filter, key) */
static void hle_D3DXLoadSurfaceFromMemory(XCtx *c) {
	u32 a[10], k;
	View d;
	int r;
	for (k = 0; k < 10; k++) a[k] = A(k);
	if (!a[0] || !view_surface(c, a[0], &d, 0)) RET(D3DERR_INVALIDCALL, 10);
	r = blit_to_surface(&d, a[2], a[3], a[4], a[5], a[7]);
	unview_surface(c, a[0]);
	if (r < 0) xrc_fatal(c, "D3DXLoadSurfaceFromMemory: format %08x -> %08x not implemented yet", a[4], d.fmt);
	RET(r ? D3D_OK : D3DERR_INVALIDCALL, 10);
}
/* D3DXLoadSurfaceFromSurface(dest, destPalette, destRect, src, srcPalette, srcRect, filter, key) */
static void hle_D3DXLoadSurfaceFromSurface(XCtx *c) {
	u32 a[8], k, sp, rect;
	View d, sv;
	int r;
	for (k = 0; k < 8; k++) a[k] = A(k);
	if (!a[0] || !a[3] || !view_surface(c, a[3], &sv, 0x10 /* READONLY */)) RET(D3DERR_INVALIDCALL, 8);
	if (!view_surface(c, a[0], &d, 0)) { unview_surface(c, a[3]); RET(D3DERR_INVALIDCALL, 8); }
	sp = c->esp;
	rect = scratch(c, 16);
	if (a[5]) memcpy(xrc_mem + rect, xrc_mem + a[5], 16);
	else { M32(rect) = 0; M32(rect + 4) = 0; M32(rect + 8) = sv.w; M32(rect + 12) = sv.h; }
	r = blit_to_surface(&d, a[2], sv.bits, sv.fmt, sv.pitch, rect);
	c->esp = sp;
	unview_surface(c, a[0]);
	unview_surface(c, a[3]);
	if (r < 0) xrc_fatal(c, "D3DXLoadSurfaceFromSurface: format %08x -> %08x not implemented yet", sv.fmt, d.fmt);
	RET(D3D_OK, 8);
}

u8 *xrc_png_decode(const u8 *d, size_t max, u32 *pw, u32 *ph);
static u32 pow2up(u32 v) { u32 p = 1; while (p < v) p <<= 1; return p; }
/* a DDS file's format (D3DFMT), 0 if not one handled: the DXT FourCCs, 32-bit ARGB / XRGB / ABGR, 16-bit 565 */
static u32 dds_format(u32 h) {
	u32 pf = h + 76, fl = M32(pf + 4), fcc = M32(pf + 8), bits = M32(pf + 12), r = M32(pf + 16), a = M32(pf + 28);
	if (fl & 4) return fcc == 0x74 ? 116 : fcc == 0x71 ? 113 : fcc == 0x72 ? 114 : fcc; /* FourCC (or a D3DFMT number) */
	if (bits == 32) return r == 0x00ff0000u ? (a && (fl & 1) ? 21 : 22) : (a && (fl & 1) ? 32 : 33);
	if (bits == 16 && r == 0xf800u) return 23;
	if (bits == 8 && (fl & 0x20000)) return 50;
	return 0;
}
/* a texture made by the device (its vtable): K_TEX / K_CUBE / K_VTEX; 0 and *hr on failure */
static u32 dx_new(XCtx *c, u32 dev, int kind, u32 w, u32 h, u32 d, u32 mips, u32 usage, u32 fmt, u32 pool, u32 *hr) {
	u32 sp = c->esp, out = scratch(c, 4), a[9], r;
	M32(out) = 0;
	if (kind == K_TEX) {
		a[0] = w; a[1] = h; a[2] = mips; a[3] = usage; a[4] = fmt; a[5] = pool; a[6] = out; a[7] = 0;
		*hr = gcall(c, dev, &if_dev, "CreateTexture", 8, a);
	} else if (kind == K_CUBE) {
		a[0] = w; a[1] = mips; a[2] = usage; a[3] = fmt; a[4] = pool; a[5] = out; a[6] = 0;
		*hr = gcall(c, dev, &if_dev, "CreateCubeTexture", 7, a);
	} else {
		a[0] = w; a[1] = h; a[2] = d; a[3] = mips; a[4] = usage; a[5] = fmt; a[6] = pool; a[7] = out; a[8] = 0;
		*hr = gcall(c, dev, &if_dev, "CreateVolumeTexture", 9, a);
	}
	r = *hr == D3D_OK ? M32(out) : 0;
	c->esp = sp;
	return r;
}
/* a 2D texture's level locked (through its vtable) */
static int view_level(XCtx *c, u32 t, u32 l, View *v) {
	u32 sp = c->esp, desc = scratch(c, 32), lr = scratch(c, 8), a[4];
	int ok;
	a[0] = l; a[1] = desc;
	ok = gcall(c, t, &if_tex, "GetLevelDesc", 2, a) == D3D_OK;
	a[0] = l; a[1] = lr; a[2] = 0; a[3] = 0;
	ok = ok && gcall(c, t, &if_tex, "LockRect", 4, a) == D3D_OK;
	if (ok) { v->fmt = M32(desc); v->w = M32(desc + 24); v->h = M32(desc + 28); v->pitch = M32(lr); v->bits = M32(lr + 4); }
	c->esp = sp;
	return ok;
}
static void unview_level(XCtx *c, u32 t, u32 l) { gcall(c, t, &if_tex, "UnlockRect", 1, &l); }

/* D3DXCreateTextureFromFileInMemoryEx(device, data, size, w, h, mips, usage, fmt, pool, filter, mipfilter, colorkey,
 * D3DXIMAGE_INFO *, palette, texture **). PNG: decoded to RGBA, point-scaled to the requested size (D3DX_DEFAULT:
 * the next power of two), mips box-filtered, stored in the requested format (default A8R8G8B8). DDS (2D): the file's
 * levels as they are (its own format). */
static void hle_D3DXCreateTextureFromFileInMemoryEx(XCtx *c) {
	u32 dev = A(0), p = A(1), n = A(2), info = A(12), tex, fmt = A(7), mips = A(5), tw = A(3), th = A(4), usage = A(6), pool = A(8);
	u32 outp = A(14), hr, nl;
	View v;
	unsigned long long max = n == 0xffffffffu || (unsigned long long)p + n > XRC_GUEST_SIZE ? XRC_GUEST_SIZE - p : n;
	u32 iw, ih, l;
	if (max >= 128 && M32(p) == 0x20534444u /* "DDS " */) {
		u32 h = p + 4, f = dds_format(h), caps2 = M32(h + 108), off = p + 128, nm = M32(h + 24) ? M32(h + 24) : 1;
		iw = M32(h + 12); ih = M32(h + 8);
		if (!f || (caps2 & 0x200200u)) xrc_fatal(c, "D3DX DDS texture: format %08x / caps2 %x not handled yet", f, caps2);
		if (fmt && fmt != 0xffffffffu && fmt != 0xfffffffdu && fmt != f) xrc_fatal(c, "D3DX DDS texture: converting %08x to %08x not handled yet", f, fmt);
		tex = dx_new(c, dev, K_TEX, iw, ih, 1, nm, usage, f, pool, &hr);
		if (!tex) { M32(outp) = 0; RET(hr, 15); }
		nl = gcall(c, tex, &if_tex, "GetLevelCount", 0, 0);
		for (l = 0; l < nl && l < nm; l++) {
			u32 lw = iw >> l ? iw >> l : 1, lh = ih >> l ? ih >> l : 1, rows = rows_of(f, lh), row = pitch_of(f, lw), k;
			if (view_level(c, tex, l, &v)) {
				for (k = 0; k < rows; k++) memcpy(xrc_mem + v.bits + k * v.pitch, xrc_mem + off + k * row, row);
				unview_level(c, tex, l);
			}
			off += rows * row;
		}
		if (info) { M32(info) = iw; M32(info + 4) = ih; M32(info + 8) = 1; M32(info + 12) = nm; M32(info + 16) = f; M32(info + 20) = 3; M32(info + 24) = 4; }
		M32(outp) = tex;
		RET(D3D_OK, 15);
	}
	{
		u8 *rgba = xrc_png_decode(xrc_mem + p, (size_t)max, &iw, &ih), *cur;
		u32 cw, ch;
		if (!rgba) xrc_fatal(c, "D3DXCreateTextureFromFileInMemoryEx: not a PNG / DDS the runtime decodes (%02x %02x %02x %02x)", M8(p), M8(p + 1), M8(p + 2), M8(p + 3));
		if (fmt == 0 || fmt == 0xffffffffu || fmt == 0xfffffffdu) fmt = 21;
		if (!px_ok(fmt)) xrc_fatal(c, "D3DX texture from a PNG into format %08x: not handled yet", fmt);
		tw = tw == 0xfffffffeu ? iw : (tw == 0 || tw == 0xffffffffu || tw == 0xfffffffdu) ? pow2up(iw) : tw;
		th = th == 0xfffffffeu ? ih : (th == 0 || th == 0xffffffffu || th == 0xfffffffdu) ? pow2up(ih) : th;
		mips = mips == 0xfffffffdu ? 1 : mips == 0xffffffffu ? 0 : mips;
		tex = dx_new(c, dev, K_TEX, tw, th, 1, mips, usage, fmt, pool, &hr);
		if (!tex) { free(rgba); M32(outp) = 0; RET(hr, 15); }
		nl = gcall(c, tex, &if_tex, "GetLevelCount", 0, 0);
		/* level 0: point-scaled from the image */
		cur = (u8 *)malloc((size_t)tw * th * 4);
		{
			u32 x, y;
			for (y = 0; y < th; y++)
				for (x = 0; x < tw; x++) memcpy(cur + ((size_t)y * tw + x) * 4, rgba + ((size_t)(y * ih / th) * iw + x * iw / tw) * 4, 4);
		}
		free(rgba);
		cw = tw; ch = th;
		for (l = 0; l < nl; l++) {
			u32 x, y;
			if (view_level(c, tex, l, &v)) {
				for (y = 0; y < ch && y < v.h; y++)
					for (x = 0; x < cw && x < v.w; x++) {
						const u8 *q = cur + ((size_t)y * cw + x) * 4;
						px_put(v.fmt, v.bits + y * v.pitch + x * fmt_bytes(v.fmt), (u32)q[3] << 24 | (u32)q[0] << 16 | (u32)q[1] << 8 | q[2]);
					}
				unview_level(c, tex, l);
			}
			if (l + 1 < nl) { /* the next level: 2x2 box */
				u32 nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1, k;
				u8 *nx = (u8 *)malloc((size_t)nw * nh * 4);
				for (y = 0; y < nh; y++)
					for (x = 0; x < nw; x++)
						for (k = 0; k < 4; k++) {
							u32 x0 = x * cw / nw, y0 = y * ch / nh, x1 = cw > 1 ? x0 + 1 : x0, y1 = ch > 1 ? y0 + 1 : y0;
							nx[((size_t)y * nw + x) * 4 + k] = (u8)((cur[((size_t)y0 * cw + x0) * 4 + k] + cur[((size_t)y0 * cw + x1) * 4 + k] +
							                                         cur[((size_t)y1 * cw + x0) * 4 + k] + cur[((size_t)y1 * cw + x1) * 4 + k] + 2) / 4);
						}
				free(cur);
				cur = nx; cw = nw; ch = nh;
			}
		}
		free(cur);
		if (info) { M32(info) = iw; M32(info + 4) = ih; M32(info + 8) = 1; M32(info + 12) = 1; M32(info + 16) = 21; M32(info + 20) = 3; M32(info + 24) = 3; }
		M32(outp) = tex;
		RET(D3D_OK, 15);
	}
}

static u32 str_vs, str_ps;
static u32 guest_str(u32 *slot, const char *s) {
	if (!*slot) { *slot = xrc_heap_alloc((u32)strlen(s) + 1, 0); memcpy(S(*slot), s, strlen(s) + 1); }
	return *slot;
}
static void hle_D3DXGetVertexShaderProfile(XCtx *c) { RET(guest_str(&str_vs, "vs_3_0"), 1); }
static void hle_D3DXGetPixelShaderProfile(XCtx *c) { RET(guest_str(&str_ps, "ps_3_0"), 1); }
/* D3DX_DEFAULT (0xffffffff) sizes: 256; mips 0 / D3DX_DEFAULT: the full chain */
static u32 dx_def(u32 v, u32 d) { return v == 0xffffffffu || v == 0 ? d : v; }
static u32 dx_mips(u32 v) { return v == 0xffffffffu ? 0 : v == 0xfffffffeu ? 1 : v; }
static u32 dx_fmt(u32 f) { return f == 0 || f == 0xffffffffu ? 21 : f; }
/* D3DXCreateTexture(device, w, h, mips, usage, fmt, pool, tex **) */
static void hle_D3DXCreateTexture(XCtx *c) {
	u32 a[8], k, hr;
	for (k = 0; k < 8; k++) a[k] = A(k);
	M32(a[7]) = dx_new(c, a[0], K_TEX, dx_def(a[1], 256), dx_def(a[2], 256), 1, dx_mips(a[3]), a[4], dx_fmt(a[5]), a[6], &hr);
	RET(hr, 8);
}
/* D3DXCreateCubeTexture(device, size, mips, usage, fmt, pool, tex **) */
static void hle_D3DXCreateCubeTexture(XCtx *c) {
	u32 a[7], k, hr;
	for (k = 0; k < 7; k++) a[k] = A(k);
	M32(a[6]) = dx_new(c, a[0], K_CUBE, dx_def(a[1], 256), 0, 1, dx_mips(a[2]), a[3], dx_fmt(a[4]), a[5], &hr);
	RET(hr, 7);
}
/* D3DXCreateVolumeTexture(device, w, h, d, mips, usage, fmt, pool, tex **) */
static void hle_D3DXCreateVolumeTexture(XCtx *c) {
	u32 a[9], k, hr;
	for (k = 0; k < 9; k++) a[k] = A(k);
	M32(a[8]) = dx_new(c, a[0], K_VTEX, dx_def(a[1], 256), dx_def(a[2], 256), dx_def(a[3], 1), dx_mips(a[4]), a[5], dx_fmt(a[6]), a[7], &hr);
	RET(hr, 9);
}

/* ============================================================================================ d3d9.dll exports */
static void hle_Direct3DCreate9(XCtx *c) {
	u32 i = new_obj(K_D3D, &if_d3d9);
	fprintf(stderr, "xrc: Direct3DCreate9(%u)\n", A(0));
	RET(OBJ(i).guest, 1);
}
static void hle_D3DPERF_BeginEvent(XCtx *c) { RET(0, 2); }
static void hle_D3DPERF_EndEvent(XCtx *c) { RET(0, 0); }

/* ============================================================================================ the table
 * explicit methods first (hle_find takes the first match), then every method of every interface -> m_generic */
#define D(i, m, f) {"d3d9", i "::" #m, f}
#define GEN_D3D9(m, a) {"d3d9", "IDirect3D9::" #m, m_generic},
#define GEN_DEV(m, a) {"d3d9", "IDirect3DDevice9::" #m, m_generic},
#define GEN_TEX(m, a) {"d3d9", "IDirect3DTexture9::" #m, m_generic},
#define GEN_CUBE(m, a) {"d3d9", "IDirect3DCubeTexture9::" #m, m_generic},
#define GEN_VTEX(m, a) {"d3d9", "IDirect3DVolumeTexture9::" #m, m_generic},
#define GEN_SURF(m, a) {"d3d9", "IDirect3DSurface9::" #m, m_generic},
#define GEN_VOL(m, a) {"d3d9", "IDirect3DVolume9::" #m, m_generic},
#define GEN_VB(m, a) {"d3d9", "IDirect3DVertexBuffer9::" #m, m_generic},
#define GEN_IB(m, a) {"d3d9", "IDirect3DIndexBuffer9::" #m, m_generic},
#define GEN_DECL(m, a) {"d3d9", "IDirect3DVertexDeclaration9::" #m, m_generic},
#define GEN_VS(m, a) {"d3d9", "IDirect3DVertexShader9::" #m, m_generic},
#define GEN_PS(m, a) {"d3d9", "IDirect3DPixelShader9::" #m, m_generic},
#define GEN_QUERY(m, a) {"d3d9", "IDirect3DQuery9::" #m, m_generic},
#define GEN_SB(m, a) {"d3d9", "IDirect3DStateBlock9::" #m, m_generic},
#define GEN_SWAP(m, a) {"d3d9", "IDirect3DSwapChain9::" #m, m_generic},
#define GEN_CTAB(m, a) {"d3d9", "ID3DXConstantTable::" #m, m_generic},
/* IUnknown + IDirect3DResource9 for every interface that has them */
#define COMMON(i) D(i, QueryInterface, m_QueryInterface), D(i, AddRef, m_AddRef), D(i, Release, m_Release), \
	D(i, GetDevice, m_GetDevice)
#define RESOURCE(i) COMMON(i), D(i, GetType, r_GetType), D(i, GetPriority, r_GetPriority), \
	D(i, SetPriority, r_SetPriority), D(i, GetPrivateData, r_GetPrivateData), D(i, SetPrivateData, r_SetPrivateData), \
	D(i, FreePrivateData, r_FreePrivateData)
#define BASETEXTURE(i) RESOURCE(i), D(i, GetLevelCount, t_GetLevelCount), D(i, GetLOD, t_GetLOD), \
	D(i, SetLOD, t_SetLOD), D(i, GetAutoGenFilterType, t_GetAutoGenFilterType)
const XrcImport xrc_hle_d3d9[] = {
	{"d3d9.dll", "Direct3DCreate9", hle_Direct3DCreate9},
	{"d3d9.dll", "D3DPERF_BeginEvent", hle_D3DPERF_BeginEvent},
	{"d3d9.dll", "D3DPERF_EndEvent", hle_D3DPERF_EndEvent},
	{"d3dx9_43.dll", "D3DXGetShaderConstantTable", hle_D3DXGetShaderConstantTable},
	{"d3dx9_43.dll", "D3DXGetVertexShaderProfile", hle_D3DXGetVertexShaderProfile},
	{"d3dx9_43.dll", "D3DXGetPixelShaderProfile", hle_D3DXGetPixelShaderProfile},
	{"d3dx9_43.dll", "D3DXCreateTexture", hle_D3DXCreateTexture},
	{"d3dx9_43.dll", "D3DXCreateTextureFromFileInMemoryEx", hle_D3DXCreateTextureFromFileInMemoryEx},
	{"d3dx9_43.dll", "D3DXLoadSurfaceFromMemory", hle_D3DXLoadSurfaceFromMemory},
	{"d3dx9_43.dll", "D3DXLoadSurfaceFromSurface", hle_D3DXLoadSurfaceFromSurface},
	{"d3dx9_43.dll", "D3DXCreateCubeTexture", hle_D3DXCreateCubeTexture},
	{"d3dx9_43.dll", "D3DXCreateVolumeTexture", hle_D3DXCreateVolumeTexture},
	D("ID3DXConstantTable", QueryInterface, m_QueryInterface), D("ID3DXConstantTable", AddRef, m_AddRef),
	D("ID3DXConstantTable", Release, m_Release),
	D("ID3DXConstantTable", GetBufferPointer, ct_GetBufferPointer), D("ID3DXConstantTable", GetBufferSize, ct_GetBufferSize),
	D("ID3DXConstantTable", GetDesc, ct_GetDesc), D("ID3DXConstantTable", GetConstantDesc, ct_GetConstantDesc),
	D("ID3DXConstantTable", GetSamplerIndex, ct_GetSamplerIndex), D("ID3DXConstantTable", GetConstant, ct_GetConstant),
	D("ID3DXConstantTable", GetConstantByName, ct_GetConstantByName),
	D("IDirect3D9", QueryInterface, m_QueryInterface), D("IDirect3D9", AddRef, m_AddRef), D("IDirect3D9", Release, m_Release),
	D("IDirect3D9", RegisterSoftwareDevice, d_RegisterSoftwareDevice), D("IDirect3D9", GetAdapterCount, d_GetAdapterCount),
	D("IDirect3D9", GetAdapterIdentifier, d_GetAdapterIdentifier), D("IDirect3D9", GetAdapterModeCount, d_GetAdapterModeCount),
	D("IDirect3D9", EnumAdapterModes, d_EnumAdapterModes), D("IDirect3D9", GetAdapterDisplayMode, d_GetAdapterDisplayMode),
	D("IDirect3D9", CheckDeviceType, d_CheckDeviceType), D("IDirect3D9", CheckDeviceFormat, d_CheckDeviceFormat),
	D("IDirect3D9", CheckDeviceMultiSampleType, d_CheckDeviceMultiSampleType),
	D("IDirect3D9", CheckDepthStencilMatch, d_CheckDepthStencilMatch),
	D("IDirect3D9", CheckDeviceFormatConversion, d_CheckDeviceFormatConversion),
	D("IDirect3D9", GetDeviceCaps, d_GetDeviceCaps), D("IDirect3D9", GetAdapterMonitor, d_GetAdapterMonitor),
	D("IDirect3D9", CreateDevice, d_CreateDevice),
#define DV(m) D("IDirect3DDevice9", m, v_##m)
	D("IDirect3DDevice9", QueryInterface, m_QueryInterface), D("IDirect3DDevice9", AddRef, m_AddRef),
	D("IDirect3DDevice9", Release, m_Release),
	DV(TestCooperativeLevel), DV(GetAvailableTextureMem), DV(GetDirect3D), DV(GetDeviceCaps), DV(GetDisplayMode),
	DV(GetCreationParameters), DV(GetNumberOfSwapChains), DV(GetSwapChain), DV(Reset), DV(Present), DV(GetBackBuffer),
	DV(GetRasterStatus), DV(GetGammaRamp), DV(CreateTexture), DV(CreateVolumeTexture), DV(CreateCubeTexture),
	DV(CreateVertexBuffer), DV(CreateIndexBuffer), DV(CreateRenderTarget), DV(CreateDepthStencilSurface),
	DV(CreateOffscreenPlainSurface), DV(SetRenderTarget), DV(GetRenderTarget), DV(SetDepthStencilSurface),
	DV(GetDepthStencilSurface), DV(SetTexture), DV(GetTexture), DV(SetStreamSource), DV(GetStreamSource),
	DV(SetStreamSourceFreq), DV(GetStreamSourceFreq), DV(SetIndices), DV(GetIndices), DV(DrawPrimitiveUP), DV(DrawIndexedPrimitiveUP), DV(SetVertexDeclaration),
	DV(DrawPrimitive), DV(DrawIndexedPrimitive), DV(Clear), DV(StretchRect), DV(ColorFill), DV(GetRenderTargetData),
	DV(UpdateSurface), DV(UpdateTexture),
	DV(GetVertexDeclaration), DV(SetVertexShader), DV(GetVertexShader), DV(SetPixelShader), DV(GetPixelShader),
	DV(SetFVF), DV(GetFVF), DV(SetRenderState), DV(GetRenderState), DV(SetSamplerState), DV(GetSamplerState),
	DV(SetTextureStageState), DV(GetTextureStageState), DV(SetTransform), DV(GetTransform), DV(SetViewport),
	DV(GetViewport), DV(SetScissorRect), DV(GetScissorRect), DV(SetVertexShaderConstantF),
	DV(GetVertexShaderConstantF), DV(SetPixelShaderConstantF), DV(GetPixelShaderConstantF),
	DV(SetVertexShaderConstantI), DV(GetVertexShaderConstantI), DV(SetPixelShaderConstantI),
	DV(GetPixelShaderConstantI), DV(SetVertexShaderConstantB), DV(GetVertexShaderConstantB),
	DV(SetPixelShaderConstantB), DV(GetPixelShaderConstantB), DV(GetSoftwareVertexProcessing), DV(GetNPatchMode),
	DV(ValidateDevice), DV(EndStateBlock), DV(CreateStateBlock), DV(CreateVertexShader), DV(CreatePixelShader),
	DV(CreateVertexDeclaration), DV(CreateQuery),
	BASETEXTURE("IDirect3DTexture9"), D("IDirect3DTexture9", GetLevelDesc, tx_GetLevelDesc),
	D("IDirect3DTexture9", GetSurfaceLevel, tx_GetSurfaceLevel), D("IDirect3DTexture9", LockRect, tx_LockRect),
	D("IDirect3DTexture9", UnlockRect, tx_UnlockRect),
	BASETEXTURE("IDirect3DCubeTexture9"), D("IDirect3DCubeTexture9", GetLevelDesc, cb_GetLevelDesc),
	D("IDirect3DCubeTexture9", GetCubeMapSurface, cb_GetCubeMapSurface), D("IDirect3DCubeTexture9", LockRect, cb_LockRect),
	D("IDirect3DCubeTexture9", UnlockRect, cb_UnlockRect),
	BASETEXTURE("IDirect3DVolumeTexture9"), D("IDirect3DVolumeTexture9", GetLevelDesc, vt_GetLevelDesc),
	D("IDirect3DVolumeTexture9", LockBox, vt_LockBox), D("IDirect3DVolumeTexture9", UnlockBox, vt_UnlockBox),
	RESOURCE("IDirect3DSurface9"), D("IDirect3DSurface9", GetDesc, sf_GetDesc),
	D("IDirect3DSurface9", GetContainer, sf_GetContainer), D("IDirect3DSurface9", LockRect, sf_LockRect),
	D("IDirect3DSurface9", UnlockRect, sf_UnlockRect), D("IDirect3DSurface9", GetDC, sf_GetDC),
	D("IDirect3DSurface9", ReleaseDC, sf_ReleaseDC),
	RESOURCE("IDirect3DVertexBuffer9"), D("IDirect3DVertexBuffer9", Lock, b_Lock),
	D("IDirect3DVertexBuffer9", Unlock, b_Unlock), D("IDirect3DVertexBuffer9", GetDesc, b_GetDesc),
	RESOURCE("IDirect3DIndexBuffer9"), D("IDirect3DIndexBuffer9", Lock, b_Lock),
	D("IDirect3DIndexBuffer9", Unlock, b_Unlock), D("IDirect3DIndexBuffer9", GetDesc, b_GetDesc),
	COMMON("IDirect3DVertexDeclaration9"), D("IDirect3DVertexDeclaration9", GetDeclaration, dc_GetDeclaration),
	COMMON("IDirect3DVertexShader9"), D("IDirect3DVertexShader9", GetFunction, s_GetFunction),
	COMMON("IDirect3DPixelShader9"), D("IDirect3DPixelShader9", GetFunction, s_GetFunction),
	COMMON("IDirect3DQuery9"), D("IDirect3DQuery9", GetType, q_GetType), D("IDirect3DQuery9", GetDataSize, q_GetDataSize),
	D("IDirect3DQuery9", GetData, q_GetData),
	COMMON("IDirect3DStateBlock9"),
	COMMON("IDirect3DSwapChain9"), D("IDirect3DSwapChain9", Present, v_Present_sw),
	D("IDirect3DSwapChain9", GetBackBuffer, sw_GetBackBuffer),
	D("IDirect3DSwapChain9", GetPresentParameters, sw_GetPresentParameters),
	D("IDirect3DSwapChain9", GetDisplayMode, sw_GetDisplayMode), D("IDirect3DSwapChain9", GetRasterStatus, sw_GetRasterStatus),
	I_D3D9(GEN_D3D9) I_DEVICE(GEN_DEV) I_TEXTURE(GEN_TEX) I_CUBE(GEN_CUBE) I_VOLTEX(GEN_VTEX) I_SURFACE(GEN_SURF)
	I_VOLUME(GEN_VOL) I_VB(GEN_VB) I_IB(GEN_IB) I_DECL(GEN_DECL) I_SHADER(GEN_VS) I_SHADER(GEN_PS) I_QUERY(GEN_QUERY)
	I_SB(GEN_SB) I_SWAP(GEN_SWAP) I_CTAB(GEN_CTAB)
};
const unsigned xrc_hle_d3d9_n = sizeof xrc_hle_d3d9 / sizeof xrc_hle_d3d9[0];
