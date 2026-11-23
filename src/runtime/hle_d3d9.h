/* hle_d3d9.h -- what hle_d3d9.c shares with the reference target's forwarder (ref_d3d9.c): the interfaces' method
 * lists, the object and device records, and the helpers that find an object's guest memory. */
#ifndef HLE_D3D9_H
#define HLE_D3D9_H
#include "xrc_rt.h"

/* ============================================================================================ interfaces
 * X(method, argument count including this) in vtable order */
#define UNK(X) X(QueryInterface, 3) X(AddRef, 1) X(Release, 1)
#define RES(X) UNK(X) X(GetDevice, 2) X(SetPrivateData, 5) X(GetPrivateData, 4) X(FreePrivateData, 2) \
	X(SetPriority, 2) X(GetPriority, 1) X(PreLoad, 1) X(GetType, 1)
#define BASETEX(X) RES(X) X(SetLOD, 2) X(GetLOD, 1) X(GetLevelCount, 1) X(SetAutoGenFilterType, 2) \
	X(GetAutoGenFilterType, 1) X(GenerateMipSubLevels, 1)
#define I_D3D9(X) UNK(X) X(RegisterSoftwareDevice, 2) X(GetAdapterCount, 1) X(GetAdapterIdentifier, 4) \
	X(GetAdapterModeCount, 3) X(EnumAdapterModes, 5) X(GetAdapterDisplayMode, 3) X(CheckDeviceType, 6) \
	X(CheckDeviceFormat, 7) X(CheckDeviceMultiSampleType, 7) X(CheckDepthStencilMatch, 6) \
	X(CheckDeviceFormatConversion, 5) X(GetDeviceCaps, 4) X(GetAdapterMonitor, 2) X(CreateDevice, 7)
#define I_DEVICE(X) UNK(X) X(TestCooperativeLevel, 1) X(GetAvailableTextureMem, 1) X(EvictManagedResources, 1) \
	X(GetDirect3D, 2) X(GetDeviceCaps, 2) X(GetDisplayMode, 3) X(GetCreationParameters, 2) \
	X(SetCursorProperties, 4) X(SetCursorPosition, 4) X(ShowCursor, 2) X(CreateAdditionalSwapChain, 3) \
	X(GetSwapChain, 3) X(GetNumberOfSwapChains, 1) X(Reset, 2) X(Present, 5) X(GetBackBuffer, 5) \
	X(GetRasterStatus, 3) X(SetDialogBoxMode, 2) X(SetGammaRamp, 4) X(GetGammaRamp, 3) X(CreateTexture, 9) \
	X(CreateVolumeTexture, 10) X(CreateCubeTexture, 8) X(CreateVertexBuffer, 7) X(CreateIndexBuffer, 7) \
	X(CreateRenderTarget, 9) X(CreateDepthStencilSurface, 9) X(UpdateSurface, 5) X(UpdateTexture, 3) \
	X(GetRenderTargetData, 3) X(GetFrontBufferData, 3) X(StretchRect, 6) X(ColorFill, 4) \
	X(CreateOffscreenPlainSurface, 7) X(SetRenderTarget, 3) X(GetRenderTarget, 3) X(SetDepthStencilSurface, 2) \
	X(GetDepthStencilSurface, 2) X(BeginScene, 1) X(EndScene, 1) X(Clear, 7) X(SetTransform, 3) \
	X(GetTransform, 3) X(MultiplyTransform, 3) X(SetViewport, 2) X(GetViewport, 2) X(SetMaterial, 2) \
	X(GetMaterial, 2) X(SetLight, 3) X(GetLight, 3) X(LightEnable, 3) X(GetLightEnable, 3) X(SetClipPlane, 3) \
	X(GetClipPlane, 3) X(SetRenderState, 3) X(GetRenderState, 3) X(CreateStateBlock, 3) X(BeginStateBlock, 1) \
	X(EndStateBlock, 2) X(SetClipStatus, 2) X(GetClipStatus, 2) X(GetTexture, 3) X(SetTexture, 3) \
	X(GetTextureStageState, 4) X(SetTextureStageState, 4) X(GetSamplerState, 4) X(SetSamplerState, 4) \
	X(ValidateDevice, 2) X(SetPaletteEntries, 3) X(GetPaletteEntries, 3) X(SetCurrentTexturePalette, 2) \
	X(GetCurrentTexturePalette, 2) X(SetScissorRect, 2) X(GetScissorRect, 2) X(SetSoftwareVertexProcessing, 2) \
	X(GetSoftwareVertexProcessing, 1) X(SetNPatchMode, 2) X(GetNPatchMode, 1) X(DrawPrimitive, 4) \
	X(DrawIndexedPrimitive, 7) X(DrawPrimitiveUP, 5) X(DrawIndexedPrimitiveUP, 9) X(ProcessVertices, 7) \
	X(CreateVertexDeclaration, 3) X(SetVertexDeclaration, 2) X(GetVertexDeclaration, 2) X(SetFVF, 2) \
	X(GetFVF, 2) X(CreateVertexShader, 3) X(SetVertexShader, 2) X(GetVertexShader, 2) \
	X(SetVertexShaderConstantF, 4) X(GetVertexShaderConstantF, 4) X(SetVertexShaderConstantI, 4) \
	X(GetVertexShaderConstantI, 4) X(SetVertexShaderConstantB, 4) X(GetVertexShaderConstantB, 4) \
	X(SetStreamSource, 5) X(GetStreamSource, 5) X(SetStreamSourceFreq, 3) X(GetStreamSourceFreq, 3) \
	X(SetIndices, 2) X(GetIndices, 2) X(CreatePixelShader, 3) X(SetPixelShader, 2) X(GetPixelShader, 2) \
	X(SetPixelShaderConstantF, 4) X(GetPixelShaderConstantF, 4) X(SetPixelShaderConstantI, 4) \
	X(GetPixelShaderConstantI, 4) X(SetPixelShaderConstantB, 4) X(GetPixelShaderConstantB, 4) \
	X(DrawRectPatch, 4) X(DrawTriPatch, 4) X(DeletePatch, 2) X(CreateQuery, 3)
#define I_TEXTURE(X) BASETEX(X) X(GetLevelDesc, 3) X(GetSurfaceLevel, 3) X(LockRect, 5) X(UnlockRect, 2) \
	X(AddDirtyRect, 2)
#define I_CUBE(X) BASETEX(X) X(GetLevelDesc, 3) X(GetCubeMapSurface, 4) X(LockRect, 6) X(UnlockRect, 3) \
	X(AddDirtyRect, 3)
#define I_VOLTEX(X) BASETEX(X) X(GetLevelDesc, 3) X(GetVolumeLevel, 3) X(LockBox, 5) X(UnlockBox, 2) \
	X(AddDirtyBox, 2)
#define I_SURFACE(X) RES(X) X(GetContainer, 3) X(GetDesc, 2) X(LockRect, 4) X(UnlockRect, 1) X(GetDC, 2) \
	X(ReleaseDC, 2)
#define I_VOLUME(X) UNK(X) X(GetDevice, 2) X(SetPrivateData, 5) X(GetPrivateData, 4) X(FreePrivateData, 2) \
	X(GetContainer, 3) X(GetDesc, 2) X(LockBox, 4) X(UnlockBox, 1)
#define I_VB(X) RES(X) X(Lock, 5) X(Unlock, 1) X(GetDesc, 2)
#define I_IB(X) RES(X) X(Lock, 5) X(Unlock, 1) X(GetDesc, 2)
#define I_DECL(X) UNK(X) X(GetDevice, 2) X(GetDeclaration, 3)
#define I_SHADER(X) UNK(X) X(GetDevice, 2) X(GetFunction, 3)
#define I_QUERY(X) UNK(X) X(GetDevice, 2) X(GetType, 1) X(GetDataSize, 1) X(Issue, 2) X(GetData, 4)
#define I_SB(X) UNK(X) X(GetDevice, 2) X(Capture, 1) X(Apply, 1)
#define I_CTAB(X) UNK(X) X(GetBufferPointer, 1) X(GetBufferSize, 1) X(GetDesc, 2) X(GetConstantDesc, 4) \
	X(GetSamplerIndex, 2) X(GetConstant, 3) X(GetConstantByName, 3) X(GetConstantElement, 3) X(SetDefaults, 2) \
	X(SetValue, 5) X(SetBool, 4) X(SetBoolArray, 5) X(SetInt, 4) X(SetIntArray, 5) X(SetFloat, 4) \
	X(SetFloatArray, 5) X(SetVector, 4) X(SetVectorArray, 5) X(SetMatrix, 4) X(SetMatrixArray, 5) \
	X(SetMatrixPointerArray, 5) X(SetMatrixTranspose, 4) X(SetMatrixTransposeArray, 5) \
	X(SetMatrixTransposePointerArray, 5)
#define I_SWAP(X) UNK(X) X(Present, 6) X(GetFrontBufferData, 2) X(GetBackBuffer, 4) X(GetRasterStatus, 2) \
	X(GetDisplayMode, 2) X(GetDevice, 2) X(GetPresentParameters, 2)

typedef struct { const char *iface; const char *const *m; const int *nargs; int n; u32 vtable; } Iface;
extern Iface *const d3_ifaces[];
extern const int d3_nifaces;

/* ============================================================================================ objects */
enum { K_FREE, K_D3D, K_DEV, K_TEX, K_CUBE, K_VTEX, K_SURF, K_VOL, K_VB, K_IB, K_DECL, K_VS, K_PS, K_QUERY, K_SB, K_SWAP, K_CTAB };
#define MAXLEVELS 16
typedef struct {
	int kind;
	u32 guest, refs;
	u32 w, h, d, levels, fmt, usage, pool, size, fvf, ms, msq, qtype;
	u32 parent, level, face;     /* a surface of a texture: its container, level and cube face */
	u32 mem;                     /* VB / IB / standalone surface / a shader's or declaration's copy */
	/* (everything a draw reads is above the level tables: in the first two cache lines) */
	u32 locked;
	void *host;     /* the backend's object / reference targets: the real object (0: not made yet) */
	int host_owned; /* it holds a reference of ours (released with the object); else borrowed from its container */
	unsigned char *shadow; /* reference: what the real buffer holds (managed / system memory ones of 4 KB and more) */
	int host_stale; /* the real one is newer than the guest copy (GetRenderTargetData's destination) */
	u32 lvl[6][MAXLEVELS];       /* texture level memory (per face) */
	u32 sub[6][MAXLEVELS];       /* the level surface objects (GetSurfaceLevel) */
} Obj;

typedef struct {
	u32 pp[14];          /* D3DPRESENT_PARAMETERS */
	u32 hwnd, flags, devtype;
	u32 backbuf, depth;  /* implicit surfaces */
	u32 rt[4], ds;
	u32 tex[32];
	u32 stream[16][3];   /* vb offset stride */
	u32 freq[16];
	u32 ib, decl, vs, ps, fvf;
	u32 rs[256], ss[32][16], tss[8][33];
	u32 vp[6];
	u32 scissor[4];
	float vsf[256 * 4], psf[224 * 4];
	int vsi[16 * 4], psi[16 * 4], vsb[16], psb[16];
	u32 xf[512][16];
	u32 swap;
	unsigned long long frames;
	u32 d3d;
	int in_sb;
} Dev;
extern Dev dev;
extern u32 the_dev;

#define obj_of d3_obj_of
#define level_mem d3_level_mem
#define surf_bits d3_surf_bits
#define pitch_of d3_pitch_of
#define rows_of d3_rows_of
#define fmt_bytes d3_fmt_bytes
#define is_dxt d3_is_dxt
#define dim d3_dim
#define level_size d3_level_size
/* the object of a guest D3D object pointer (0: none); inline, ~3 calls per forwarded D3D call. The table is in
 * chunks of 1024 that never move */
extern Obj *d3_ochunks[];
extern u32 d3_nobjs;
static inline Obj *obj_of(u32 g) {
	u32 i;
	Obj *o;
	if (!g || M32(g + 8) != 0xd3d90b1eu) return 0;
	i = M32(g + 4);
	if (!i || i >= d3_nobjs) return 0;
	o = &d3_ochunks[i / 1024u][i % 1024u];
	return o->kind != K_FREE ? o : 0;
}
u32 level_mem(Obj *t, u32 face, u32 l);
u32 surf_bits(Obj *o, u32 *pitch);
u32 pitch_of(u32 f, u32 w);
u32 rows_of(u32 f, u32 h);
u32 fmt_bytes(u32 f);
int is_dxt(u32 f);
u32 dim(u32 x, u32 l);
u32 level_size(Obj *o, u32 l);

/* the rendering backend (vk_d3d9.c, the linux_x64 target): set by its d3_be_init at CreateDevice; 0 = none (the
 * headless targets, the reference target, or Vulkan did not start). It draws from the HLE's state (dev, the objects'
 * guest memory): the HLE calls it where the GPU has to act. Method arguments are the guest's (after `this`). */
typedef struct {
	void (*draw)(int method, const u32 *a);  /* 0 DrawPrimitive 1 DrawIndexedPrimitive 2 DrawPrimitiveUP 3 DrawIndexedPrimitiveUP */
	void (*clear)(const u32 *a);             /* Count Rects Flags Color Z Stencil */
	void (*present)(void);
	void (*locked)(Obj *o, u32 face, u32 level, u32 off, u32 size); /* guest memory will change (buffers: the range, 0 = all) */
	void (*release)(Obj *o);                 /* the object goes */
	void (*stretch_rect)(const u32 *a);      /* src srcRect dst dstRect filter */
	void (*color_fill)(const u32 *a);        /* surface rect color */
	u32 (*rt_data)(const u32 *a);            /* GetRenderTargetData: rt, dst (system memory) -> HRESULT */
	void (*reset)(void);                     /* after the HLE's Reset made the new buffers */
} D3Backend;
extern const D3Backend *d3_be;
const D3Backend *d3_be_init(void); /* (vk_d3d9.c; a stub returning 0 elsewhere) */

/* the reference forwarder's hooks (no-ops in the HLE-only targets) */
#ifdef XRC_REFERENCE
void ref_d3d_release(Obj *o); /* the object goes: its real one is released */
#else
#define ref_d3d_release(o) (d3_be ? d3_be->release(o) : (void)0)
#endif
#include "xrc_quiet.h" /* (-DXRC_NO_TOOLS: nothing printed) */
#endif
