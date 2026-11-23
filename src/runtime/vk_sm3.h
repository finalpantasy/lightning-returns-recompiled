/* vk_sm3.h - Direct3D 9 shader model 3 bytecode (vs_3_0 / ps_3_0) translated to SPIR-V for Vulkan (vk_sm3.c).
 *
 * The interface the translated shaders have (vk_d3d9.c builds its pipelines and descriptor sets on it):
 *   set 0, binding 0 (vertex) / 1 (pixel): uniform block {vec4 c[256]; ivec4 i[16]; ivec4 b[16];} -- the D3D
 *     constant registers (b: .x nonzero = true). Constants a shader defines itself (def / defi / defb) are its own.
 *   set 0, binding 2 + s: pixel sampler s (0..15); binding 18 + s: vertex sampler s (0..3). Combined image samplers
 *     of the dimension the shader declares (Sm3Info.sampler).
 *   push constants: vec4 posfix (offset 0, vertex: gl_Position.xy += posfix.xy * gl_Position.w -- D3D9's pixel centres;
 *     posfix.zw unused); int alpha_func (16), float alpha_ref (20) (pixel: D3D9 alpha test on oC0.a; func D3DCMP_*,
 *     8 = ALWAYS: none).
 *   vertex inputs: location = the v# register (Sm3Info.in_usage / in_index: the declaration's semantic for it).
 *   vertex outputs / pixel inputs: location sm3_location(usage, index) (the same for every shader: they link by
 *     semantic, as D3D9's do). */
#ifndef VK_SM3_H
#define VK_SM3_H
#include <stdint.h>

#define SM3_UBO_SIZE (256 * 16 + 16 * 16 + 16 * 16)
#define SM3_PUSH_SIZE 32

typedef struct {
	int ps;                      /* a pixel shader */
	uint32_t *spirv;             /* malloc'd */
	uint32_t nwords;
	uint32_t in_mask;            /* the v# declared (vertex: attributes) */
	uint8_t in_usage[16], in_index[16];
	uint8_t sampler[20];         /* per s#: 0 none, 2 2D, 3 cube, 4 volume (pixel 0..15, vertex 16..19) */
	uint32_t max_const;          /* c registers read: below this (relative reads: 256) */
	int writes_depth;
} Sm3Info;

/* 0 and a message in err when the bytecode uses something not translated */
int sm3_translate(const uint32_t *code, uint32_t nwords, Sm3Info *out, char *err, int errlen);
/* flags: SM3_NO_ALPHA_TEST -- a pixel shader without the alpha test (no discard in it: the GPU can then test depth
 * before the shader runs), for draws with the alpha test off */
#define SM3_NO_ALPHA_TEST 1
/* SM3_NO_DEPTH_OUT -- a pixel shader's oDepth not written (the rasterized depth kept: the GPU can test depth before
 * it runs), for draws whose depth buffer nothing reads but their own depth tests */
#define SM3_NO_DEPTH_OUT 2
int sm3_translate_ex(const uint32_t *code, uint32_t nwords, Sm3Info *out, char *err, int errlen, int flags);
void sm3_free(Sm3Info *s);
/* the interface location of a vertex output / pixel input semantic (D3DDECLUSAGE, index) */
int sm3_location(int usage, int index);

#include "xrc_quiet.h" /* (-DXRC_NO_TOOLS: nothing printed) */
#endif
