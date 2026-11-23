/* guest_types.h - the guest (LRFF13.exe, 32-bit) structures our own override code reads and writes, declared once.
 *
 * Guest memory is one flat block (XM, see xrc.h); a guest pointer is a u32 offset into it. The structs below have the
 * layout the guest has (fixed-width fields, 4-byte guest pointers as gptr32), so the code reads like source (fields are reached with GF below):
 *     u32 scene = M32(G_SCENE);   GF(Scene, scene, flags) |= SC_DEBUGCAM;
 * Every named field has a _Static_assert on its offset. A field whose meaning is not proven is named unk_<hex offset>,
 * a gap is pad_<hex offset>[n]. Sources of the layouts: analysis/recon/render_path.md and frame_clock.md,
 * the comments of the files that use them (ovr_debug.c, ovr_config_menu.c, ovr_miles.c, ovr_math.c, hle_lr.c) and the
 * Win32 documentation for the API structs. Include after xrc.h / xrc_rt.h. */
#ifndef GUEST_TYPES_H
#define GUEST_TYPES_H
#include <stddef.h>

typedef u32 gptr32; /* a guest pointer: an offset into guest memory (0 = null) */

/* A field of the struct T that lives at guest address g, as an lvalue: GF(Scene, scene, flags) |= SC_DEBUGCAM.
 * It is *(type *)(XM + (u32)(g + offsetof(T, field))): the same u32 address arithmetic, so the same machine code, as
 * the M32(g + 0x30) it replaces (a host `T *` + `->` would let the compiler fold the offset into the address and
 * change the code). `f` may be a nested designator (rows.count). */
#define GF(T, g, f) (*(__typeof__(((T *)0)->f) *)(XM + (u32)((g) + (u32)offsetof(T, f))))
/* element i of an array field: GFI(ConfigRowList, list, ids, k) */
#define GFI(T, g, f, i) (*(__typeof__(((T *)0)->f[0]) *)(XM + (u32)((g) + (u32)offsetof(T, f) + (u32)sizeof(((T *)0)->f[0]) * (i))))
/* GFN: the same, with the offset added to the host pointer (XM + g + off), no 32-bit wrap: for the places that had
 * *(float *)(XM + g + off) already (the machine code of that form differs from the u32 one) */
#define GFN(T, g, f) (*(__typeof__(((T *)0)->f) *)(XM + (g) + offsetof(T, f)))
/* the guest address of a field (to pass to a guest call): GADDR(Pad, p, right_stick_x) */
#define GADDR(T, g, f) ((u32)((g) + (u32)offsetof(T, f)))
/* a guest pointer as a host pointer to the struct (for memcpy / memset of a range; no bounds check) */
#define GP(T, g) ((T *)(XM + (u32)(g)))

#define GT_OFF(T, f, off) _Static_assert(offsetof(T, f) == (off), #T "." #f " offset")
#define GT_SIZE(T, n) _Static_assert(sizeof(T) == (n), #T " size")

/* ---- the guest's stack arguments at a cdecl / stdcall / thiscall entry (esp -> return address, then the arguments) ---- */
static inline u32 ret_addr(const XCtx *c) { return M32(c->esp); }
/* arg_u32(c, 1) = the first argument ([esp + 4]), 2 = the second, ... (xrc_arg() is the same, 0-based, out of line) */
static inline u32 arg_u32(const XCtx *c, u32 n) { return M32(c->esp + 4u * n); }
/* the same slot when a callee frame is built: set_arg_u32(c, 0, ra) = the return address */
static inline void set_arg_u32(const XCtx *c, u32 n, u32 v) { M32(c->esp + 4u * n) = v; }

/* a callee frame being built at guest address s (s -> return address): FRAME_ARG(s, 1) = [s + 4] */
#define FRAME_ARG(s, n) M32((s) + 4u * (n))
/* element i of a u32 array at guest address g */
#define GARR32(g, i) M32((g) + 4u * (u32)(i))

/* a virtual call target: the function at byte offset `off` of the object's vtable */
static inline u32 guest_vfn(gptr32 obj, u32 off) { return M32(M32(obj) + off); }

/* ---- globals (guest addresses) ---- */
#define G_HID 0x24d94ecu       /* white::io::Hid * */
#define G_SCENE 0x24d0db0u     /* white::scene::Scene * */
#define G_APP 0x2350974u       /* white::AppGame * */
#define G_GUIMGR 0x50f19f4u    /* white::gui::GuiManager * (also the object of the menu's text lookup / play_se) */
#define G_TIME 0x234f130u      /* FrameTimer * */
#define G_PAD_MGR 0x24dab1cu   /* PadManager * */
#define G_FRAME_CLOCK 0x511555cu /* FrameClock * */
#define G_FRAME_CFG 0x5115590u /* FrameCfg * */
#define G_QPF_CACHE 0x24c3ad8u /* QpfCache (inline) */
#define G_WAIT_FLAG 0x510e940u /* u32: nonzero = run the original wait */
#define G_WAIT_OBJ 0x510e944u  /* the object FrameLimiter::WaitQpcSleep (00a96590) polls: a function at vtable +0x4c */
#define G_DEBUG_WORDS 0x50e75c4u /* DebugWords (inline) */
#define G_B09D50_STATICS 0x5115780u /* Fn00b09d50Statics (inline) */

/* ---- white::scene::Scene (G_SCENE). ovr_debug.c comments. ---- */
typedef struct Scene {
	u8 pad_00[0x30];
	u32 flags;               /* SC_*: bit 4 = show the debug camera; 0x20000 / 0x20 pause the world */
	u8 pad_34[8];
	gptr32 debug_camera;     /* the default camera (SceneCameraControlByDebugPad's) */
	gptr32 game_camera;
} Scene;
GT_OFF(Scene, flags, 0x30); GT_OFF(Scene, debug_camera, 0x3c); GT_OFF(Scene, game_camera, 0x40);
#define SC_DEBUGCAM 4u         /* GetActiveCamera renders the debug camera */
#define SC_RESYNC 0x40000u     /* the game copies its camera into the debug one (00666a70) */
#define SC_PAUSE 0x20000u      /* Scene time scale 0 (006681e0 stores 0 in [0x23311dc]) */
#define SC_MOVEPAUSE 0x20u     /* "[MoveObjectPause] <pause>" (006681e0: bits 0x60 pause the scene's move objects) */

/* ---- white::AppGame (G_APP) ---- */
typedef struct AppGame {
	u8 pad_00[0x680];
	u32 flags;               /* APP_* */
} AppGame;
GT_OFF(AppGame, flags, 0x680);
#define APP_DEBUGCAM 0x1000u   /* the debug camera toggle is allowed (never set in retail) */

/* ---- the frame timer (G_TIME) ---- */
typedef struct FrameTimer {
	u8 pad_00[0x44];
	float delta;             /* the frame delta the debug camera's update reads */
} FrameTimer;
GT_OFF(FrameTimer, delta, 0x44);

/* ---- white::io::Hid (G_HID) and one pad ---- */
typedef struct Pad {
	u8 unk_00[4];
	u8 buttons_04[0x14];     /* buttons: +4..+0x18 (ovr_debug.c) */
	u8 unk_18[4];
	u8 axes_1c[0x10];        /* sticks / triggers: +0x1c..+0x40 */
	float right_stick_x;     /* the right stick the debug camera turns with */
	float right_stick_y;
	u8 axes_34[0x10];
} Pad;
GT_OFF(Pad, buttons_04, 4); GT_OFF(Pad, axes_1c, 0x1c); GT_OFF(Pad, right_stick_x, 0x2c); GT_OFF(Pad, right_stick_y, 0x30);
GT_SIZE(Pad, 0x44);
#define PAD_STATE_LO 4u        /* the pad state copied between pads: [LO, HI) */
#define PAD_STATE_HI 0x44u
typedef struct Hid {
	u8 unk_00[4];
	gptr32 pad0;             /* the player's pad */
	gptr32 pad1;             /* the second pad (the debug pad) */
} Hid;
GT_OFF(Hid, pad0, 4); GT_OFF(Hid, pad1, 8);

/* SceneCameraControlByDebugPad's camera (argument 1 of 0070b3a0) */
typedef struct DebugCamera {
	u8 pad_00[0x170];
	float unk_170;           /* the log's yaw is atan2(unk_170, unk_178) */
	float unk_174;
	float unk_178;
	float unk_17c;
	float pos_x, pos_y, pos_z; /* the camera position (logged as "pos") */
} DebugCamera;
GT_OFF(DebugCamera, unk_170, 0x170); GT_OFF(DebugCamera, unk_178, 0x178); GT_OFF(DebugCamera, pos_x, 0x180); GT_OFF(DebugCamera, pos_z, 0x188);

/* ---- white::gui::GuiManager (G_GUIMGR) ---- */
typedef struct GuiManager {
	u8 pad_00[0x15b8];
	u32 flags;               /* GUIMGR_* */
} GuiManager;
GT_OFF(GuiManager, flags, 0x15b8);
#define GUIMGR_FIELD_HUD_SHOWN (1u << 4) /* the field HUD is shown */

/* the three debug words at G_DEBUG_WORDS: word 1 bit 18 = the game's GUI-draw skip (hides the battle HUD too) */
typedef struct DebugWords { u32 w[3]; } DebugWords;
#define DBGW1_GUI_DRAW_SKIP 0x40000u

/* ---- FrameClock (G_FRAME_CLOCK), frame_clock.md ---- */
typedef struct FrameClock {
	float fps;               /* current / measured fps (init 59.94) */
	float target_fps;        /* SetTargetFps */
	u8 pad_08[0x20];
	u32 ring_index;          /* 0..3, (i+1)&3 per GetQuantizedDelta call */
	u8 pad_2c[4];
	u64 frame_end_qpc;       /* QPC timestamp of the frame end */
	u64 prev_quant_qpc;      /* previous quantized QPC stamp */
	gptr32 callback;         /* optional callback */
} FrameClock;
GT_OFF(FrameClock, fps, 0); GT_OFF(FrameClock, target_fps, 4); GT_OFF(FrameClock, ring_index, 0x28);
GT_OFF(FrameClock, frame_end_qpc, 0x30); GT_OFF(FrameClock, prev_quant_qpc, 0x38); GT_OFF(FrameClock, callback, 0x40);

/* the config struct at G_FRAME_CFG: +8 = the max frame rate float (the clamp = round(1 / max_fps * 300000)) */
typedef struct FrameCfg {
	u8 pad_00[8];
	float max_fps;
} FrameCfg;
GT_OFF(FrameCfg, max_fps, 8);

/* cached QueryPerformanceFrequency (frame_clock.md): valid when bit 0 of init_flags is set */
typedef struct QpfCache {
	union { u64 freq; struct { u32 freq_lo, freq_hi; }; };
	union { u32 init_flags; u8 init_flags_b0; };
} QpfCache;
GT_OFF(QpfCache, freq, 0); GT_OFF(QpfCache, freq_hi, 4); GT_OFF(QpfCache, init_flags, 8);
#define QPF_VALID 1u

/* two 32-bit words in guest memory (the poll's out value in 00a96590: tested for zero) */
typedef struct U32Pair { u32 w0, w1; } U32Pair;

/* ---- the pad manager (G_PAD_MGR) ---- */
typedef struct PadManager {
	u8 pad_00[8];
	u32 device_change_count; /* up = the game rescans its pads */
} PadManager;
GT_OFF(PadManager, device_change_count, 8);

/* ---- GuiMainConfigMenu (code 00806000-00810000), ovr_config_menu.c ---- */
#define CFG_ROWS_MAX 29u
typedef struct ConfigRowList {
	u32 ids[CFG_ROWS_MAX];     /* option ids */
	u32 choice[CFG_ROWS_MAX];  /* the current choice of each row */
	u32 count;                 /* row count */
} ConfigRowList;
GT_OFF(ConfigRowList, ids, 0); GT_OFF(ConfigRowList, choice, 0x74); GT_OFF(ConfigRowList, count, 0xe8);
typedef struct ConfigListObj {   /* GuiMainConfigMenu + 0x8c */
	u8 pad_00[0xc4];
	ConfigRowList rows;
	u8 pad_1b0[0x18];
	u32 unk_1c8;               /* nonzero: the confirm is not for a row */
	u8 pad_1cc[0x10];
	u32 selected_row;
} ConfigListObj;
GT_OFF(ConfigListObj, rows, 0xc4); GT_OFF(ConfigListObj, unk_1c8, 0x1c8); GT_OFF(ConfigListObj, selected_row, 0x1dc);
typedef struct ConfigMenu {      /* GuiMainConfigMenu */
	u8 pad_00[0x8c];
	gptr32 list;               /* ConfigListObj * */
	u8 pad_90[0x10];
	u32 unk_a0;
	u8 pad_a4[4];
	u32 unk_a8;
	u8 pad_ac[8];
	u32 unk_b4;
} ConfigMenu;
GT_OFF(ConfigMenu, list, 0x8c); GT_OFF(ConfigMenu, unk_a0, 0xa0); GT_OFF(ConfigMenu, unk_a8, 0xa8); GT_OFF(ConfigMenu, unk_b4, 0xb4);

/* ---- Miles Sound System double buffer (00c18810), ovr_miles.c ---- */
typedef struct MssDoubleBuffer {
	u8 unk_00[4];
	gptr32 base;               /* the two halves start here */
	u32 shared;                /* shared pointer: the half being read (-1: the writer holds it) */
	u8 unk_0c[4];
	u32 cur;                   /* the half being read, 0 / 1 */
	u32 half_size;
	u32 ready;                 /* 1 = a new half is ready */
} MssDoubleBuffer;
GT_OFF(MssDoubleBuffer, base, 4); GT_OFF(MssDoubleBuffer, shared, 8); GT_OFF(MssDoubleBuffer, cur, 0x10);
GT_OFF(MssDoubleBuffer, half_size, 0x14); GT_OFF(MssDoubleBuffer, ready, 0x18);

/* ---- quantized AABB-tree query 00b09d50 (ovr_math.c) ---- */
typedef struct QuantAabbTree {   /* the `this` */
	u8 pad_00[0x18];
	gptr32 nodes;              /* the node base */
	gptr32 nodes_end;
} QuantAabbTree;
GT_OFF(QuantAabbTree, nodes, 0x18); GT_OFF(QuantAabbTree, nodes_end, 0x1c);
typedef struct QuantAabbNode { u16 w[8]; } QuantAabbNode; /* 16 bytes of 8 words; word 3 = next-node offset / 16, word 7 = 0xffff: skip */
GT_SIZE(QuantAabbNode, 16);
/* the function-local statics of 00b09d50, built lazily under the guard bits of `flags` */
typedef struct Fn00b09d50Statics {
	QuantAabbNode zero;        /* 0x5115780 */
	QuantAabbNode m3;          /* 0x5115790 */
	QuantAabbNode m4;          /* 0x51157a0 */
	u32 flags;                 /* 0x51157b0: bit 0 m4, bit 1 m3, bit 2 zero */
} Fn00b09d50Statics;
GT_OFF(Fn00b09d50Statics, m3, 16); GT_OFF(Fn00b09d50Statics, m4, 32); GT_OFF(Fn00b09d50Statics, flags, 48);
#define B09D50_M4_INIT 1u
#define B09D50_M3_INIT 2u
#define B09D50_ZERO_INIT 4u

/* ---- ovr_math.c self-test layouts of 00b8edb0 (thiscall, ret 0x14) ---- */
typedef struct TableSelector { u16 index; } TableSelector;   /* its `this`: the u16 index A into the entry table */
typedef struct TableEntry8 { u16 idx0, idx1, idx2, unk_06; } TableEntry8; /* 8 bytes: words 0..2 index the float table (after * scale) */
GT_SIZE(TableEntry8, 8);

/* ---- Win32 structures (documented layouts) used by hle_lr.c / ovr_input.c ---- */
typedef struct Guid {
	u32 data1;
	union { struct { u16 data2, data3; }; u32 data2_3; };
	u8 data4[8];
} Guid;
GT_OFF(Guid, data1, 0); GT_OFF(Guid, data2_3, 4); GT_SIZE(Guid, 16);
typedef struct SystemTime {
	u16 year, month, day_of_week, day, hour, minute, second, milliseconds;
} SystemTime;
GT_OFF(SystemTime, year, 0); GT_OFF(SystemTime, month, 2); GT_OFF(SystemTime, day_of_week, 4); GT_OFF(SystemTime, day, 6);
GT_OFF(SystemTime, hour, 8); GT_OFF(SystemTime, minute, 10); GT_OFF(SystemTime, second, 12); GT_OFF(SystemTime, milliseconds, 14); GT_SIZE(SystemTime, 16);
typedef struct OsVersionInfoEx {   /* OSVERSIONINFOA (148 bytes) / OSVERSIONINFOEXA (156) */
	u32 size, major, minor, build, platform_id;
	char csd_version[128];
	u16 service_pack_major, service_pack_minor, suite_mask;
	u8 product_type, reserved;
} OsVersionInfoEx;
GT_OFF(OsVersionInfoEx, build, 12); GT_OFF(OsVersionInfoEx, csd_version, 20); GT_OFF(OsVersionInfoEx, service_pack_major, 148);
GT_SIZE(OsVersionInfoEx, 156);
typedef struct XInputState {       /* XINPUT_STATE */
	u32 packet_number;
	u16 buttons;
	u8 left_trigger, right_trigger;
	s16 thumb_lx, thumb_ly, thumb_rx, thumb_ry;
} XInputState;
GT_OFF(XInputState, buttons, 4); GT_SIZE(XInputState, 16);
typedef struct Bstr { u32 byte_len; u16 chars[]; } Bstr; /* a BSTR: the byte length precedes the characters */
GT_OFF(Bstr, chars, 4);
typedef struct StartupInfoW { u32 cb; u8 unk_04[64]; } StartupInfoW; /* STARTUPINFOW: 68 bytes */
GT_SIZE(StartupInfoW, 68);
typedef struct SecurityDescriptor { u8 revision, sbz1; u16 control; gptr32 owner, group, sacl, dacl; } SecurityDescriptor; /* 20 bytes */
GT_OFF(SecurityDescriptor, revision, 0); GT_SIZE(SecurityDescriptor, 20);
typedef struct FileTime { union { u64 value; struct { u32 lo, hi; }; }; } FileTime; /* 100 ns ticks since 1601 */
GT_SIZE(FileTime, 8);
typedef struct Teb {
	u32 seh_chain;           /* 0xffffffff = end of chain */
	u32 stack_base;
	u32 stack_limit;
	u8 pad_0c[0xc];
	gptr32 self;             /* NtTib.Self (+0x18) */
	u8 pad_1c[4];
	u32 process_id;
	u32 thread_id;
	u8 pad_28[4];
	gptr32 tls_pointer;      /* ThreadLocalStoragePointer: the TLS blocks of the loaded modules */
	gptr32 peb;
	u32 last_error;
	u8 pad_38[0xe10 - 0x38];
	u32 tls_slots[64];       /* TlsAlloc / TlsGetValue slots */
} Teb;
GT_OFF(Teb, seh_chain, 0); GT_OFF(Teb, stack_base, 4); GT_OFF(Teb, stack_limit, 8); GT_OFF(Teb, self, 0x18);
GT_OFF(Teb, process_id, 0x20); GT_OFF(Teb, thread_id, 0x24); GT_OFF(Teb, tls_pointer, 0x2c); GT_OFF(Teb, peb, 0x30);
GT_OFF(Teb, last_error, 0x34); GT_OFF(Teb, tls_slots, 0xe10);
typedef struct Peb {
	u8 pad_00[8];
	u32 image_base_address;
	u8 pad_0c[0xc];
	u32 process_heap;        /* a handle value: HeapAlloc is HLE */
} Peb;
GT_OFF(Peb, image_base_address, 8); GT_OFF(Peb, process_heap, 0x18);

/* ---- D3D9 / D3DX structures (32-bit layouts of d3d9types.h / d3d9caps.h / d3dx9shader.h) used by hle_d3d9.c ---- */
typedef struct GRect { s32 left, top, right, bottom; } GRect;
typedef struct GPoint { s32 x, y; } GPoint;
GT_OFF(GPoint, x, 0x0); GT_OFF(GPoint, y, 0x4); GT_SIZE(GPoint, 8);   /* RECT */
GT_OFF(GRect, left, 0x0); GT_OFF(GRect, top, 0x4); GT_OFF(GRect, right, 0x8); GT_OFF(GRect, bottom, 0xc); GT_SIZE(GRect, 0x10);
typedef struct D3dDisplayMode { u32 width, height, refresh_rate, format; } D3dDisplayMode;
GT_OFF(D3dDisplayMode, width, 0x0); GT_OFF(D3dDisplayMode, height, 0x4); GT_OFF(D3dDisplayMode, refresh_rate, 0x8); GT_OFF(D3dDisplayMode, format, 0xc); GT_SIZE(D3dDisplayMode, 0x10);
typedef struct D3dCreationParams { u32 adapter_ordinal, device_type, focus_window, behavior_flags; } D3dCreationParams;
GT_OFF(D3dCreationParams, adapter_ordinal, 0x0); GT_OFF(D3dCreationParams, device_type, 0x4); GT_OFF(D3dCreationParams, focus_window, 0x8); GT_OFF(D3dCreationParams, behavior_flags, 0xc); GT_SIZE(D3dCreationParams, 0x10);
typedef struct D3dObjHeader {     /* the 16-byte guest object every D3D9 / D3DX interface pointer points at */
	gptr32 vtable;
	u32 id;                  /* index into the runtime's object table */
	u32 magic;               /* 0xd3d90b1e */
	u32 unk_0c;
} D3dObjHeader;
GT_OFF(D3dObjHeader, vtable, 0x0); GT_OFF(D3dObjHeader, id, 0x4); GT_OFF(D3dObjHeader, magic, 0x8); GT_OFF(D3dObjHeader, unk_0c, 0xc); GT_SIZE(D3dObjHeader, 0x10);
typedef struct D3dSurfaceDesc {   /* D3DSURFACE_DESC (texture / surface levels) */
	u32 format, type, usage, pool, multisample_type, multisample_quality, width, height;
} D3dSurfaceDesc;
GT_OFF(D3dSurfaceDesc, format, 0x0); GT_OFF(D3dSurfaceDesc, type, 0x4); GT_OFF(D3dSurfaceDesc, usage, 0x8); GT_OFF(D3dSurfaceDesc, pool, 0xc); GT_OFF(D3dSurfaceDesc, multisample_type, 0x10); GT_OFF(D3dSurfaceDesc, multisample_quality, 0x14); GT_OFF(D3dSurfaceDesc, width, 0x18); GT_OFF(D3dSurfaceDesc, height, 0x1c); GT_SIZE(D3dSurfaceDesc, 0x20);
typedef struct D3dVolumeDesc { u32 format, type, usage, pool, width, height, depth; } D3dVolumeDesc;
GT_OFF(D3dVolumeDesc, format, 0x0); GT_OFF(D3dVolumeDesc, type, 0x4); GT_OFF(D3dVolumeDesc, usage, 0x8); GT_OFF(D3dVolumeDesc, pool, 0xc); GT_OFF(D3dVolumeDesc, width, 0x10); GT_OFF(D3dVolumeDesc, height, 0x14); GT_OFF(D3dVolumeDesc, depth, 0x18); GT_SIZE(D3dVolumeDesc, 0x1c);
typedef struct D3dBufferDesc { u32 format, type, usage, pool, size, fvf; } D3dBufferDesc;  /* D3DVERTEXBUFFER_DESC (D3DINDEXBUFFER_DESC stops before fvf) */
GT_OFF(D3dBufferDesc, format, 0x0); GT_OFF(D3dBufferDesc, type, 0x4); GT_OFF(D3dBufferDesc, usage, 0x8); GT_OFF(D3dBufferDesc, pool, 0xc); GT_OFF(D3dBufferDesc, size, 0x10); GT_OFF(D3dBufferDesc, fvf, 0x14); GT_SIZE(D3dBufferDesc, 0x18);
typedef struct D3dLockedRect { u32 pitch; gptr32 bits; } D3dLockedRect;
GT_OFF(D3dLockedRect, pitch, 0x0); GT_OFF(D3dLockedRect, bits, 0x4); GT_SIZE(D3dLockedRect, 0x8);
typedef struct D3dLockedBox { u32 row_pitch, slice_pitch; gptr32 bits; } D3dLockedBox;
GT_OFF(D3dLockedBox, row_pitch, 0x0); GT_OFF(D3dLockedBox, slice_pitch, 0x4); GT_OFF(D3dLockedBox, bits, 0x8); GT_SIZE(D3dLockedBox, 0xc);
typedef struct D3dBox { u32 left, top, right, bottom, front, back; } D3dBox;
GT_OFF(D3dBox, left, 0x0); GT_OFF(D3dBox, top, 0x4); GT_OFF(D3dBox, right, 0x8); GT_OFF(D3dBox, bottom, 0xc); GT_OFF(D3dBox, front, 0x10); GT_OFF(D3dBox, back, 0x14); GT_SIZE(D3dBox, 0x18);
typedef struct D3dAdapterIdentifier9 {
	char driver[512];
	char description[512];
	char device_name[32];
	u32 driver_version_lo, driver_version_hi;   /* LARGE_INTEGER */
	u32 vendor_id, device_id, sub_sys_id, revision;
	u32 device_identifier_0, device_identifier_1, device_identifier_2, device_identifier_3;   /* a GUID as four dwords */
	u32 whql_level;
} D3dAdapterIdentifier9;
GT_OFF(D3dAdapterIdentifier9, driver, 0x0); GT_OFF(D3dAdapterIdentifier9, description, 0x200); GT_OFF(D3dAdapterIdentifier9, device_name, 0x400); GT_OFF(D3dAdapterIdentifier9, driver_version_lo, 0x420); GT_OFF(D3dAdapterIdentifier9, driver_version_hi, 0x424); GT_OFF(D3dAdapterIdentifier9, vendor_id, 0x428); GT_OFF(D3dAdapterIdentifier9, device_id, 0x42c); GT_OFF(D3dAdapterIdentifier9, sub_sys_id, 0x430); GT_OFF(D3dAdapterIdentifier9, revision, 0x434); GT_OFF(D3dAdapterIdentifier9, device_identifier_0, 0x438); GT_OFF(D3dAdapterIdentifier9, device_identifier_1, 0x43c); GT_OFF(D3dAdapterIdentifier9, device_identifier_2, 0x440); GT_OFF(D3dAdapterIdentifier9, device_identifier_3, 0x444); GT_OFF(D3dAdapterIdentifier9, whql_level, 0x448); GT_SIZE(D3dAdapterIdentifier9, 0x44c);
typedef struct D3dCaps9 {         /* D3DCAPS9. Fields the runtime stores with f32bits() are u32 here (float bits). */
	u32 device_type, adapter_ordinal, caps, caps2, caps3, presentation_intervals, cursor_caps, dev_caps;
	u32 primitive_misc_caps, raster_caps, z_cmp_caps, src_blend_caps, dest_blend_caps, alpha_cmp_caps, shade_caps;
	u32 texture_caps, texture_filter_caps, cube_texture_filter_caps, volume_texture_filter_caps;
	u32 texture_address_caps, volume_texture_address_caps, line_caps;
	u32 max_texture_width, max_texture_height, max_volume_extent, max_texture_repeat, max_texture_aspect_ratio, max_anisotropy;
	u32 max_vertex_w;                                  /* float */
	u32 guard_band_left, guard_band_top, guard_band_right, guard_band_bottom;   /* float */
	u32 extents_adjust;                                /* float */
	u32 stencil_caps, fvf_caps, texture_op_caps, max_texture_blend_stages, max_simultaneous_textures;
	u32 vertex_processing_caps, max_active_lights, max_user_clip_planes, max_vertex_blend_matrices, max_vertex_blend_matrix_index;
	u32 max_point_size;                                /* float */
	u32 max_primitive_count, max_vertex_index, max_streams, max_stream_stride;
	u32 vertex_shader_version, max_vertex_shader_const, pixel_shader_version;
	u32 pixel_shader_1x_max_value;                     /* float */
	u32 dev_caps2;
	u32 max_npatch_tessellation_level;                 /* float */
	u32 reserved5, master_adapter_ordinal, adapter_ordinal_in_group, number_of_adapters_in_group;
	u32 decl_types, num_simultaneous_rts, stretch_rect_filter_caps;
	u32 vs20_caps, vs20_dynamic_flow_control_depth, vs20_num_temps, vs20_static_flow_control_depth;
	u32 ps20_caps, ps20_dynamic_flow_control_depth, ps20_num_temps, ps20_static_flow_control_depth, ps20_num_instruction_slots;
	u32 vertex_texture_filter_caps, max_vshader_instructions_executed, max_pshader_instructions_executed;
	u32 max_vertex_shader30_instruction_slots, max_pixel_shader30_instruction_slots;
} D3dCaps9;
GT_OFF(D3dCaps9, device_type, 0x0); GT_OFF(D3dCaps9, adapter_ordinal, 0x4); GT_OFF(D3dCaps9, caps, 0x8); GT_OFF(D3dCaps9, caps2, 0xc); GT_OFF(D3dCaps9, caps3, 0x10); GT_OFF(D3dCaps9, presentation_intervals, 0x14); GT_OFF(D3dCaps9, cursor_caps, 0x18); GT_OFF(D3dCaps9, dev_caps, 0x1c); GT_OFF(D3dCaps9, primitive_misc_caps, 0x20); GT_OFF(D3dCaps9, raster_caps, 0x24); GT_OFF(D3dCaps9, z_cmp_caps, 0x28); GT_OFF(D3dCaps9, src_blend_caps, 0x2c); GT_OFF(D3dCaps9, dest_blend_caps, 0x30); GT_OFF(D3dCaps9, alpha_cmp_caps, 0x34); GT_OFF(D3dCaps9, shade_caps, 0x38); GT_OFF(D3dCaps9, texture_caps, 0x3c); GT_OFF(D3dCaps9, texture_filter_caps, 0x40); GT_OFF(D3dCaps9, cube_texture_filter_caps, 0x44); GT_OFF(D3dCaps9, volume_texture_filter_caps, 0x48); GT_OFF(D3dCaps9, texture_address_caps, 0x4c); GT_OFF(D3dCaps9, volume_texture_address_caps, 0x50); GT_OFF(D3dCaps9, line_caps, 0x54); GT_OFF(D3dCaps9, max_texture_width, 0x58); GT_OFF(D3dCaps9, max_texture_height, 0x5c); GT_OFF(D3dCaps9, max_volume_extent, 0x60); GT_OFF(D3dCaps9, max_texture_repeat, 0x64); GT_OFF(D3dCaps9, max_texture_aspect_ratio, 0x68); GT_OFF(D3dCaps9, max_anisotropy, 0x6c); GT_OFF(D3dCaps9, max_vertex_w, 0x70); GT_OFF(D3dCaps9, guard_band_left, 0x74); GT_OFF(D3dCaps9, guard_band_top, 0x78); GT_OFF(D3dCaps9, guard_band_right, 0x7c); GT_OFF(D3dCaps9, guard_band_bottom, 0x80); GT_OFF(D3dCaps9, extents_adjust, 0x84); GT_OFF(D3dCaps9, stencil_caps, 0x88); GT_OFF(D3dCaps9, fvf_caps, 0x8c); GT_OFF(D3dCaps9, texture_op_caps, 0x90); GT_OFF(D3dCaps9, max_texture_blend_stages, 0x94); GT_OFF(D3dCaps9, max_simultaneous_textures, 0x98); GT_OFF(D3dCaps9, vertex_processing_caps, 0x9c); GT_OFF(D3dCaps9, max_active_lights, 0xa0); GT_OFF(D3dCaps9, max_user_clip_planes, 0xa4); GT_OFF(D3dCaps9, max_vertex_blend_matrices, 0xa8); GT_OFF(D3dCaps9, max_vertex_blend_matrix_index, 0xac); GT_OFF(D3dCaps9, max_point_size, 0xb0); GT_OFF(D3dCaps9, max_primitive_count, 0xb4); GT_OFF(D3dCaps9, max_vertex_index, 0xb8); GT_OFF(D3dCaps9, max_streams, 0xbc); GT_OFF(D3dCaps9, max_stream_stride, 0xc0); GT_OFF(D3dCaps9, vertex_shader_version, 0xc4); GT_OFF(D3dCaps9, max_vertex_shader_const, 0xc8); GT_OFF(D3dCaps9, pixel_shader_version, 0xcc); GT_OFF(D3dCaps9, pixel_shader_1x_max_value, 0xd0); GT_OFF(D3dCaps9, dev_caps2, 0xd4); GT_OFF(D3dCaps9, max_npatch_tessellation_level, 0xd8); GT_OFF(D3dCaps9, reserved5, 0xdc); GT_OFF(D3dCaps9, master_adapter_ordinal, 0xe0); GT_OFF(D3dCaps9, adapter_ordinal_in_group, 0xe4); GT_OFF(D3dCaps9, number_of_adapters_in_group, 0xe8); GT_OFF(D3dCaps9, decl_types, 0xec); GT_OFF(D3dCaps9, num_simultaneous_rts, 0xf0); GT_OFF(D3dCaps9, stretch_rect_filter_caps, 0xf4); GT_OFF(D3dCaps9, vs20_caps, 0xf8); GT_OFF(D3dCaps9, vs20_dynamic_flow_control_depth, 0xfc); GT_OFF(D3dCaps9, vs20_num_temps, 0x100); GT_OFF(D3dCaps9, vs20_static_flow_control_depth, 0x104); GT_OFF(D3dCaps9, ps20_caps, 0x108); GT_OFF(D3dCaps9, ps20_dynamic_flow_control_depth, 0x10c); GT_OFF(D3dCaps9, ps20_num_temps, 0x110); GT_OFF(D3dCaps9, ps20_static_flow_control_depth, 0x114); GT_OFF(D3dCaps9, ps20_num_instruction_slots, 0x118); GT_OFF(D3dCaps9, vertex_texture_filter_caps, 0x11c); GT_OFF(D3dCaps9, max_vshader_instructions_executed, 0x120); GT_OFF(D3dCaps9, max_pshader_instructions_executed, 0x124); GT_OFF(D3dCaps9, max_vertex_shader30_instruction_slots, 0x128); GT_OFF(D3dCaps9, max_pixel_shader30_instruction_slots, 0x12c); GT_SIZE(D3dCaps9, 0x130);
/* D3DX constant table (the CTAB blob of a compiled shader) */
typedef struct CtabHeader {
	u32 size;
	u32 creator;             /* offset of the creator string */
	u32 version;
	u32 constants;           /* count */
	u32 constant_info;       /* offset of the CtabConstantInfo array */
	u32 flags;
	u32 target;
} CtabHeader;
GT_OFF(CtabHeader, size, 0x0); GT_OFF(CtabHeader, creator, 0x4); GT_OFF(CtabHeader, version, 0x8); GT_OFF(CtabHeader, constants, 0xc); GT_OFF(CtabHeader, constant_info, 0x10); GT_OFF(CtabHeader, flags, 0x14); GT_OFF(CtabHeader, target, 0x18); GT_SIZE(CtabHeader, 0x1c);
typedef struct CtabConstantInfo {   /* D3DXSHADER_CONSTANTINFO */
	u32 name;                /* offset */
	u16 register_set, register_index, register_count, reserved;
	u32 type_info;           /* offset of a CtabTypeInfo */
	u32 default_value;       /* offset */
} CtabConstantInfo;
GT_OFF(CtabConstantInfo, name, 0x0); GT_OFF(CtabConstantInfo, register_set, 0x4); GT_OFF(CtabConstantInfo, register_index, 0x6); GT_OFF(CtabConstantInfo, register_count, 0x8); GT_OFF(CtabConstantInfo, reserved, 0xa); GT_OFF(CtabConstantInfo, type_info, 0xc); GT_OFF(CtabConstantInfo, default_value, 0x10); GT_SIZE(CtabConstantInfo, 0x14);
typedef struct CtabTypeInfo {       /* D3DXSHADER_TYPEINFO */
	u16 klass, type, rows, columns, elements, struct_members;
	u32 struct_member_info;
} CtabTypeInfo;
GT_OFF(CtabTypeInfo, klass, 0x0); GT_OFF(CtabTypeInfo, type, 0x2); GT_OFF(CtabTypeInfo, rows, 0x4); GT_OFF(CtabTypeInfo, columns, 0x6); GT_OFF(CtabTypeInfo, elements, 0x8); GT_OFF(CtabTypeInfo, struct_members, 0xa); GT_OFF(CtabTypeInfo, struct_member_info, 0xc); GT_SIZE(CtabTypeInfo, 0x10);
typedef struct D3dxConstantDesc {   /* D3DXCONSTANT_DESC */
	u32 name, register_set, register_index, register_count, klass, type, rows, columns, elements, struct_members, bytes, default_value;
} D3dxConstantDesc;
GT_OFF(D3dxConstantDesc, name, 0x0); GT_OFF(D3dxConstantDesc, register_set, 0x4); GT_OFF(D3dxConstantDesc, register_index, 0x8); GT_OFF(D3dxConstantDesc, register_count, 0xc); GT_OFF(D3dxConstantDesc, klass, 0x10); GT_OFF(D3dxConstantDesc, type, 0x14); GT_OFF(D3dxConstantDesc, rows, 0x18); GT_OFF(D3dxConstantDesc, columns, 0x1c); GT_OFF(D3dxConstantDesc, elements, 0x20); GT_OFF(D3dxConstantDesc, struct_members, 0x24); GT_OFF(D3dxConstantDesc, bytes, 0x28); GT_OFF(D3dxConstantDesc, default_value, 0x2c); GT_SIZE(D3dxConstantDesc, 0x30);
typedef struct D3dxConstTableDesc { u32 creator, version, constants; } D3dxConstTableDesc;
GT_OFF(D3dxConstTableDesc, creator, 0x0); GT_OFF(D3dxConstTableDesc, version, 0x4); GT_OFF(D3dxConstTableDesc, constants, 0x8); GT_SIZE(D3dxConstTableDesc, 0xc);
typedef struct D3dxImageInfo {      /* D3DXIMAGE_INFO */
	u32 width, height, depth, mip_levels, format, resource_type, image_file_format;
} D3dxImageInfo;
GT_OFF(D3dxImageInfo, width, 0x0); GT_OFF(D3dxImageInfo, height, 0x4); GT_OFF(D3dxImageInfo, depth, 0x8); GT_OFF(D3dxImageInfo, mip_levels, 0xc); GT_OFF(D3dxImageInfo, format, 0x10); GT_OFF(D3dxImageInfo, resource_type, 0x14); GT_OFF(D3dxImageInfo, image_file_format, 0x18); GT_SIZE(D3dxImageInfo, 0x1c);
typedef struct DdsHeader {          /* DDS_HEADER (124 bytes, after the 4-byte "DDS " magic) */
	u32 size, flags, height, width, pitch_or_linear_size, depth, mip_map_count;
	u32 reserved1[11];
	u32 pf_size, pf_flags, pf_four_cc, pf_rgb_bit_count, pf_r_mask, pf_g_mask, pf_b_mask, pf_a_mask;
	u32 caps, caps2, caps3, caps4, reserved2;
} DdsHeader;
GT_OFF(DdsHeader, size, 0x0); GT_OFF(DdsHeader, flags, 0x4); GT_OFF(DdsHeader, height, 0x8); GT_OFF(DdsHeader, width, 0xc); GT_OFF(DdsHeader, pitch_or_linear_size, 0x10); GT_OFF(DdsHeader, depth, 0x14); GT_OFF(DdsHeader, mip_map_count, 0x18); GT_OFF(DdsHeader, reserved1, 0x1c); GT_OFF(DdsHeader, pf_size, 0x48); GT_OFF(DdsHeader, pf_flags, 0x4c); GT_OFF(DdsHeader, pf_four_cc, 0x50); GT_OFF(DdsHeader, pf_rgb_bit_count, 0x54); GT_OFF(DdsHeader, pf_r_mask, 0x58); GT_OFF(DdsHeader, pf_g_mask, 0x5c); GT_OFF(DdsHeader, pf_b_mask, 0x60); GT_OFF(DdsHeader, pf_a_mask, 0x64); GT_OFF(DdsHeader, caps, 0x68); GT_OFF(DdsHeader, caps2, 0x6c); GT_OFF(DdsHeader, caps3, 0x70); GT_OFF(DdsHeader, caps4, 0x74); GT_OFF(DdsHeader, reserved2, 0x78); GT_SIZE(DdsHeader, 0x7c);

/* ---- render_path.md objects (the ones vk_d3d9.c's shadow / decl diagnostics read) ---- */
typedef struct D3dVertexElement {   /* D3DVERTEXELEMENT9 (8 bytes) */
	u16 stream, offset;
	u8 type, method, usage, usage_index;
} D3dVertexElement;
GT_OFF(D3dVertexElement, stream, 0x0); GT_OFF(D3dVertexElement, offset, 0x2); GT_OFF(D3dVertexElement, type, 0x4); GT_OFF(D3dVertexElement, method, 0x5); GT_OFF(D3dVertexElement, usage, 0x6); GT_OFF(D3dVertexElement, usage_index, 0x7); GT_SIZE(D3dVertexElement, 0x8);
typedef struct GraphicsManager {    /* at [0x5107a00]; ctor a30d80 */
	u8 pad_00[0x20];
	float camera_view[16];           /* the camera / view matrix */
	u8 pad_60[0x80];                 /* +0xa0: 3 x vec4 uploaded to VS constants c16..c18 */
	float projection[16];
	u8 pad_120[0xc4];                /* +0x160: matrix passed to AabbTree::BuildViewList; +0x1d0: vec4 from the view size */
	float cull_frustum[16];          /* SetCullFrustum (a31990) copies a frustum here */
	u8 pad_224[0xc0];
	gptr32 current_mesh_info;        /* +0x2e4 (written by a46c20) */
	u32 last_batch_key;              /* +0x2e8 */
	u32 unk_2ec;
	u32 unk_2f0;
	u32 layer_mask;                  /* +0x2f4: (layer_mask & node.layer_mask) != layer_required culls the node */
	u32 layer_required;              /* +0x2f8 */
	u8 pad_2fc[0x98];
	u32 shadow_mode;                 /* +0x394 ("mode394" in the shadow trace) */
	u32 unk_398;
	u8 pad_39c[0x90];
	float shadow_f42c[6];            /* +0x42c..0x440: shadow near / far / blend / far-scale (shadows.md) */
	u8 pad_444[0x5c];
	float unk_4a0;
	u32 pad_4a4;
	u8 unk_4a8;
	u8 pad_4a9[3];
	u32 unk_4ac;
	float unk_4b0;
	u32 pad_4b4[2];
	float unk_4bc;
} GraphicsManager;
GT_OFF(GraphicsManager, camera_view, 0x20); GT_OFF(GraphicsManager, projection, 0xe0); GT_OFF(GraphicsManager, cull_frustum, 0x1e4); GT_OFF(GraphicsManager, current_mesh_info, 0x2e4); GT_OFF(GraphicsManager, last_batch_key, 0x2e8); GT_OFF(GraphicsManager, unk_2ec, 0x2ec); GT_OFF(GraphicsManager, unk_2f0, 0x2f0); GT_OFF(GraphicsManager, layer_mask, 0x2f4); GT_OFF(GraphicsManager, layer_required, 0x2f8); GT_OFF(GraphicsManager, shadow_mode, 0x394); GT_OFF(GraphicsManager, unk_398, 0x398); GT_OFF(GraphicsManager, shadow_f42c, 0x42c); GT_OFF(GraphicsManager, unk_4a0, 0x4a0); GT_OFF(GraphicsManager, unk_4a8, 0x4a8); GT_OFF(GraphicsManager, unk_4ac, 0x4ac); GT_OFF(GraphicsManager, unk_4b0, 0x4b0); GT_OFF(GraphicsManager, unk_4bc, 0x4bc); GT_SIZE(GraphicsManager, 0x4c0);
typedef struct ModelInstanceNode {  /* RTTI vtable 02152434; stride 0xd0 in DrawableObject */
	u8 pad_00[8];
	u32 layer_mask;                  /* tested against the GraphicsManager masks */
	u8 pad_0c[0x78];
	gptr32 mesh_infos;               /* +0x84: MeshInfo ** */
	u32 pad_88;
	u32 mesh_count;                  /* +0x8c */
} ModelInstanceNode;
GT_OFF(ModelInstanceNode, layer_mask, 0x8); GT_OFF(ModelInstanceNode, mesh_infos, 0x84); GT_OFF(ModelInstanceNode, mesh_count, 0x8c); GT_SIZE(ModelInstanceNode, 0x90);
typedef struct MeshInfo {           /* the "batch": RTTI MeshInfo@ModelInstanceNode, vtable 021523d4 */
	u8 pad_00[4];
	u32 key;                         /* +4: pass / key id (compared with GraphicsManager+0x2e8) */
	u8 pad_08[0x18];
	u8 enabled;                      /* +0x20 */
	u8 pad_21;
	u8 unk_22;                       /* 0 = never culled */
	u8 pad_23;
	gptr32 indexed_batch;            /* +0x24: the IndexedBatch (geometry) */
	gptr32 sub_draw;                 /* +0x28: non-null = a sub-draw path (a65b60 / a67080) */
	u8 pad_2c[0x24];
	float aabb_min[4];               /* +0x50 */
	float aabb_max[4];               /* +0x60 */
	u8 pad_70[0x20];
	gptr32 mesh_object;              /* +0x90 */
	gptr32 material;                 /* +0x94 */
	u8 pad_98[0x38];
	u8 unk_d0;
	u8 visible;                      /* +0xd1: visible / active, tested with enabled */
} MeshInfo;
GT_OFF(MeshInfo, key, 0x4); GT_OFF(MeshInfo, enabled, 0x20); GT_OFF(MeshInfo, unk_22, 0x22); GT_OFF(MeshInfo, indexed_batch, 0x24); GT_OFF(MeshInfo, sub_draw, 0x28); GT_OFF(MeshInfo, aabb_min, 0x50); GT_OFF(MeshInfo, aabb_max, 0x60); GT_OFF(MeshInfo, mesh_object, 0x90); GT_OFF(MeshInfo, material, 0x94); GT_OFF(MeshInfo, unk_d0, 0xd0); GT_OFF(MeshInfo, visible, 0xd1); GT_SIZE(MeshInfo, 0xd4);

/* ---- XAudio2 / WAVEFORMAT structures (32-bit layouts, packed as the guest sees them) used by hle_xaudio2.c ---- */
typedef struct WaveFormatExtensible {   /* WAVEFORMATEX (18 bytes) + WAVEFORMATEXTENSIBLE tail (40 bytes) */
	u16 format_tag, channels;
	u32 samples_per_sec, avg_bytes_per_sec;
	u16 block_align, bits_per_sample;
	u16 cb_size;
	u16 valid_bits_per_sample;       /* (a union with samples_per_block for ADPCM) */
	u32 channel_mask;
	u32 sub_format_data1;            /* SubFormat GUID, flattened */
	u16 sub_format_data2, sub_format_data3;
	u8 sub_format_data4[8];
} WaveFormatExtensible;
GT_OFF(WaveFormatExtensible, format_tag, 0x0); GT_OFF(WaveFormatExtensible, channels, 0x2); GT_OFF(WaveFormatExtensible, samples_per_sec, 0x4); GT_OFF(WaveFormatExtensible, avg_bytes_per_sec, 0x8); GT_OFF(WaveFormatExtensible, block_align, 0xc); GT_OFF(WaveFormatExtensible, bits_per_sample, 0xe); GT_OFF(WaveFormatExtensible, cb_size, 0x10); GT_OFF(WaveFormatExtensible, valid_bits_per_sample, 0x12); GT_OFF(WaveFormatExtensible, channel_mask, 0x14); GT_OFF(WaveFormatExtensible, sub_format_data1, 0x18); GT_OFF(WaveFormatExtensible, sub_format_data2, 0x1c); GT_OFF(WaveFormatExtensible, sub_format_data3, 0x1e); GT_OFF(WaveFormatExtensible, sub_format_data4, 0x20); GT_SIZE(WaveFormatExtensible, 0x28);
typedef struct XaObject {           /* the 16-byte guest object of a voice (id) or an effect (refs) */
	gptr32 vtable;
	u32 id;                          /* voice index */
	u32 refs;                        /* effect object reference count */
	u32 unk_0c;
} XaObject;
GT_OFF(XaObject, vtable, 0x0); GT_OFF(XaObject, id, 0x4); GT_OFF(XaObject, refs, 0x8); GT_OFF(XaObject, unk_0c, 0xc); GT_SIZE(XaObject, 0x10);
typedef struct XaDeviceDetails {    /* XAUDIO2_DEVICE_DETAILS (1068 bytes) */
	u16 device_id[256];
	u16 display_name[256];
	u32 role;                        /* GlobalDefaultDevice = 0xf */
	WaveFormatExtensible output_format;
} XaDeviceDetails;
GT_OFF(XaDeviceDetails, device_id, 0x0); GT_OFF(XaDeviceDetails, display_name, 0x200); GT_OFF(XaDeviceDetails, role, 0x400); GT_OFF(XaDeviceDetails, output_format, 0x404); GT_SIZE(XaDeviceDetails, 0x42c);
typedef struct XaPerformanceData {  /* XAUDIO2_PERFORMANCE_DATA (64 bytes) */
	u32 audio_cycles_lo, audio_cycles_hi, total_cycles_lo, total_cycles_hi;
	u32 minimum_cycles_per_quantum, maximum_cycles_per_quantum, memory_usage_in_bytes;
	u32 current_latency_in_samples, glitches_since_engine_started;
	u32 active_source_voice_count, total_source_voice_count, active_submix_voice_count;
	u32 active_resampler_count, active_matrix_mix_count, active_xma_source_voices, active_xma_streams;
} XaPerformanceData;
GT_OFF(XaPerformanceData, audio_cycles_lo, 0x0); GT_OFF(XaPerformanceData, audio_cycles_hi, 0x4); GT_OFF(XaPerformanceData, total_cycles_lo, 0x8); GT_OFF(XaPerformanceData, total_cycles_hi, 0xc); GT_OFF(XaPerformanceData, minimum_cycles_per_quantum, 0x10); GT_OFF(XaPerformanceData, maximum_cycles_per_quantum, 0x14); GT_OFF(XaPerformanceData, memory_usage_in_bytes, 0x18); GT_OFF(XaPerformanceData, current_latency_in_samples, 0x1c); GT_OFF(XaPerformanceData, glitches_since_engine_started, 0x20); GT_OFF(XaPerformanceData, active_source_voice_count, 0x24); GT_OFF(XaPerformanceData, total_source_voice_count, 0x28); GT_OFF(XaPerformanceData, active_submix_voice_count, 0x2c); GT_OFF(XaPerformanceData, active_resampler_count, 0x30); GT_OFF(XaPerformanceData, active_matrix_mix_count, 0x34); GT_OFF(XaPerformanceData, active_xma_source_voices, 0x38); GT_OFF(XaPerformanceData, active_xma_streams, 0x3c); GT_SIZE(XaPerformanceData, 0x40);
typedef struct XaVoiceDetails { u32 creation_flags, input_channels, input_sample_rate; } XaVoiceDetails;
GT_OFF(XaVoiceDetails, creation_flags, 0x0); GT_OFF(XaVoiceDetails, input_channels, 0x4); GT_OFF(XaVoiceDetails, input_sample_rate, 0x8); GT_SIZE(XaVoiceDetails, 0xc);
typedef struct XaVolumeMeterLevels { gptr32 peak_levels, rms_levels; u32 channel_count; } XaVolumeMeterLevels;
GT_OFF(XaVolumeMeterLevels, peak_levels, 0x0); GT_OFF(XaVolumeMeterLevels, rms_levels, 0x4); GT_OFF(XaVolumeMeterLevels, channel_count, 0x8); GT_SIZE(XaVolumeMeterLevels, 0xc);
typedef struct XaFilterParameters { u32 type, frequency, one_over_q; } XaFilterParameters;   /* frequency / one_over_q: float bits */
GT_OFF(XaFilterParameters, type, 0x0); GT_OFF(XaFilterParameters, frequency, 0x4); GT_OFF(XaFilterParameters, one_over_q, 0x8); GT_SIZE(XaFilterParameters, 0xc);
typedef struct XaBuffer {           /* XAUDIO2_BUFFER (36 bytes) */
	u32 flags, audio_bytes;
	gptr32 audio_data;
	u32 play_begin, play_length, loop_begin, loop_length, loop_count;
	gptr32 context;
} XaBuffer;
GT_OFF(XaBuffer, flags, 0x0); GT_OFF(XaBuffer, audio_bytes, 0x4); GT_OFF(XaBuffer, audio_data, 0x8); GT_OFF(XaBuffer, play_begin, 0xc); GT_OFF(XaBuffer, play_length, 0x10); GT_OFF(XaBuffer, loop_begin, 0x14); GT_OFF(XaBuffer, loop_length, 0x18); GT_OFF(XaBuffer, loop_count, 0x1c); GT_OFF(XaBuffer, context, 0x20); GT_SIZE(XaBuffer, 0x24);
typedef struct XaVoiceState {       /* XAUDIO2_VOICE_STATE */
	gptr32 current_buffer_context;
	u32 buffers_queued;
	u32 samples_played_lo, samples_played_hi;
} XaVoiceState;
GT_OFF(XaVoiceState, current_buffer_context, 0x0); GT_OFF(XaVoiceState, buffers_queued, 0x4); GT_OFF(XaVoiceState, samples_played_lo, 0x8); GT_OFF(XaVoiceState, samples_played_hi, 0xc); GT_SIZE(XaVoiceState, 0x10);
typedef struct XaSendDescriptor { u32 flags; gptr32 output_voice; } XaSendDescriptor;
GT_OFF(XaSendDescriptor, flags, 0x0); GT_OFF(XaSendDescriptor, output_voice, 0x4); GT_SIZE(XaSendDescriptor, 0x8);
typedef struct XaVoiceSends { u32 send_count; gptr32 sends; } XaVoiceSends;   /* XAUDIO2_VOICE_SENDS */
GT_OFF(XaVoiceSends, send_count, 0x0); GT_OFF(XaVoiceSends, sends, 0x4); GT_SIZE(XaVoiceSends, 0x8);

/* ---- user32 structures (32-bit Win32 layouts) used by hle_user32.c / hle_user32_lr.inc ---- */
typedef struct GSize { s32 cx, cy; } GSize;
GT_OFF(GSize, cx, 0x0); GT_OFF(GSize, cy, 0x4); GT_SIZE(GSize, 0x8);
typedef struct WndClassEx {         /* WNDCLASSEXA / W (48 bytes) */
	u32 size, style;
	gptr32 wndproc;
	s32 cls_extra, wnd_extra;
	u32 instance, icon, cursor, background;
	gptr32 menu_name, class_name;
	u32 icon_sm;
} WndClassEx;
GT_OFF(WndClassEx, size, 0x0); GT_OFF(WndClassEx, style, 0x4); GT_OFF(WndClassEx, wndproc, 0x8); GT_OFF(WndClassEx, cls_extra, 0xc); GT_OFF(WndClassEx, wnd_extra, 0x10); GT_OFF(WndClassEx, instance, 0x14); GT_OFF(WndClassEx, icon, 0x18); GT_OFF(WndClassEx, cursor, 0x1c); GT_OFF(WndClassEx, background, 0x20); GT_OFF(WndClassEx, menu_name, 0x24); GT_OFF(WndClassEx, class_name, 0x28); GT_OFF(WndClassEx, icon_sm, 0x2c); GT_SIZE(WndClassEx, 0x30);
typedef struct CreateStruct {       /* CREATESTRUCT (48 bytes) */
	u32 create_params, instance, menu, parent;
	s32 cy, cx, y, x;
	u32 style;
	gptr32 name, class_name;
	u32 ex_style;
} CreateStruct;
GT_OFF(CreateStruct, create_params, 0x0); GT_OFF(CreateStruct, instance, 0x4); GT_OFF(CreateStruct, menu, 0x8); GT_OFF(CreateStruct, parent, 0xc); GT_OFF(CreateStruct, cy, 0x10); GT_OFF(CreateStruct, cx, 0x14); GT_OFF(CreateStruct, y, 0x18); GT_OFF(CreateStruct, x, 0x1c); GT_OFF(CreateStruct, style, 0x20); GT_OFF(CreateStruct, name, 0x24); GT_OFF(CreateStruct, class_name, 0x28); GT_OFF(CreateStruct, ex_style, 0x2c); GT_SIZE(CreateStruct, 0x30);
typedef struct WinMsg {             /* MSG (28 bytes) */
	u32 hwnd, message, wparam, lparam, time;
	s32 pt_x, pt_y;
} WinMsg;
GT_OFF(WinMsg, hwnd, 0x0); GT_OFF(WinMsg, message, 0x4); GT_OFF(WinMsg, wparam, 0x8); GT_OFF(WinMsg, lparam, 0xc); GT_OFF(WinMsg, time, 0x10); GT_OFF(WinMsg, pt_x, 0x14); GT_OFF(WinMsg, pt_y, 0x18); GT_SIZE(WinMsg, 0x1c);
typedef struct WindowPlacement {    /* WINDOWPLACEMENT (44 bytes) */
	u32 length, flags, show_cmd;
	s32 min_position_x, min_position_y, max_position_x, max_position_y;
	s32 normal_left, normal_top, normal_right, normal_bottom;
} WindowPlacement;
GT_OFF(WindowPlacement, length, 0x0); GT_OFF(WindowPlacement, flags, 0x4); GT_OFF(WindowPlacement, show_cmd, 0x8); GT_OFF(WindowPlacement, min_position_x, 0xc); GT_OFF(WindowPlacement, min_position_y, 0x10); GT_OFF(WindowPlacement, max_position_x, 0x14); GT_OFF(WindowPlacement, max_position_y, 0x18); GT_OFF(WindowPlacement, normal_left, 0x1c); GT_OFF(WindowPlacement, normal_top, 0x20); GT_OFF(WindowPlacement, normal_right, 0x24); GT_OFF(WindowPlacement, normal_bottom, 0x28); GT_SIZE(WindowPlacement, 0x2c);
typedef struct DevMode {            /* DEVMODEA (156 bytes), the fields the runtime fills */
	u8 device_name[32];
	u16 spec_version, driver_version;
	u16 size, driver_extra;
	u32 fields;
	u8 unk_2c[0x3c];
	u32 bits_per_pel, pels_width, pels_height;
	u32 unk_74;
	u32 display_frequency;
	u8 unk_7c[0x20];
} DevMode;
GT_OFF(DevMode, device_name, 0x0); GT_OFF(DevMode, spec_version, 0x20); GT_OFF(DevMode, driver_version, 0x22); GT_OFF(DevMode, size, 0x24); GT_OFF(DevMode, driver_extra, 0x26); GT_OFF(DevMode, fields, 0x28); GT_OFF(DevMode, unk_2c, 0x2c); GT_OFF(DevMode, bits_per_pel, 0x68); GT_OFF(DevMode, pels_width, 0x6c); GT_OFF(DevMode, pels_height, 0x70); GT_OFF(DevMode, unk_74, 0x74); GT_OFF(DevMode, display_frequency, 0x78); GT_OFF(DevMode, unk_7c, 0x7c); GT_SIZE(DevMode, 0x9c);
typedef struct DisplayDevice {      /* DISPLAY_DEVICEA (424 bytes) */
	u32 cb;
	char device_name[32];
	char device_string[128];
	u32 state_flags;
	char device_id[128];
	char device_key[128];
} DisplayDevice;
GT_OFF(DisplayDevice, cb, 0x0); GT_OFF(DisplayDevice, device_name, 0x4); GT_OFF(DisplayDevice, device_string, 0x24); GT_OFF(DisplayDevice, state_flags, 0xa4); GT_OFF(DisplayDevice, device_id, 0xa8); GT_OFF(DisplayDevice, device_key, 0x128); GT_SIZE(DisplayDevice, 0x1a8);
typedef struct MonitorInfoEx {      /* MONITORINFOEXA (72 bytes) */
	u32 size;
	s32 monitor_left, monitor_top, monitor_right, monitor_bottom;
	s32 work_left, work_top, work_right, work_bottom;
	u32 flags;
	char device[32];
} MonitorInfoEx;
GT_OFF(MonitorInfoEx, size, 0x0); GT_OFF(MonitorInfoEx, monitor_left, 0x4); GT_OFF(MonitorInfoEx, monitor_top, 0x8); GT_OFF(MonitorInfoEx, monitor_right, 0xc); GT_OFF(MonitorInfoEx, monitor_bottom, 0x10); GT_OFF(MonitorInfoEx, work_left, 0x14); GT_OFF(MonitorInfoEx, work_top, 0x18); GT_OFF(MonitorInfoEx, work_right, 0x1c); GT_OFF(MonitorInfoEx, work_bottom, 0x20); GT_OFF(MonitorInfoEx, flags, 0x24); GT_OFF(MonitorInfoEx, device, 0x28); GT_SIZE(MonitorInfoEx, 0x48);
typedef struct GuiThreadInfo {      /* GUITHREADINFO (48 bytes) */
	u32 size, flags, hwnd_active, hwnd_focus, hwnd_capture, hwnd_menu_owner, hwnd_move_size, hwnd_caret;
	s32 caret_left, caret_top, caret_right, caret_bottom;
} GuiThreadInfo;
GT_OFF(GuiThreadInfo, size, 0x0); GT_OFF(GuiThreadInfo, flags, 0x4); GT_OFF(GuiThreadInfo, hwnd_active, 0x8); GT_OFF(GuiThreadInfo, hwnd_focus, 0xc); GT_OFF(GuiThreadInfo, hwnd_capture, 0x10); GT_OFF(GuiThreadInfo, hwnd_menu_owner, 0x14); GT_OFF(GuiThreadInfo, hwnd_move_size, 0x18); GT_OFF(GuiThreadInfo, hwnd_caret, 0x1c); GT_OFF(GuiThreadInfo, caret_left, 0x20); GT_OFF(GuiThreadInfo, caret_top, 0x24); GT_OFF(GuiThreadInfo, caret_right, 0x28); GT_OFF(GuiThreadInfo, caret_bottom, 0x2c); GT_SIZE(GuiThreadInfo, 0x30);
typedef struct MenuItemInfo {       /* MENUITEMINFOA / W (48 bytes) */
	u32 size, mask, type, state, id, sub_menu, bmp_checked, bmp_unchecked, item_data;
	gptr32 type_data;
	u32 cch, bmp_item;
} MenuItemInfo;
GT_OFF(MenuItemInfo, size, 0x0); GT_OFF(MenuItemInfo, mask, 0x4); GT_OFF(MenuItemInfo, type, 0x8); GT_OFF(MenuItemInfo, state, 0xc); GT_OFF(MenuItemInfo, id, 0x10); GT_OFF(MenuItemInfo, sub_menu, 0x14); GT_OFF(MenuItemInfo, bmp_checked, 0x18); GT_OFF(MenuItemInfo, bmp_unchecked, 0x1c); GT_OFF(MenuItemInfo, item_data, 0x20); GT_OFF(MenuItemInfo, type_data, 0x24); GT_OFF(MenuItemInfo, cch, 0x28); GT_OFF(MenuItemInfo, bmp_item, 0x2c); GT_SIZE(MenuItemInfo, 0x30);

/* ---- kernel32 structures (32-bit Win32 layouts) used by hle_kernel32.c / hle_kernel32_io.c ---- */
typedef struct LargeInteger { u32 lo, hi; } LargeInteger;
GT_OFF(LargeInteger, lo, 0x0); GT_OFF(LargeInteger, hi, 0x4); GT_SIZE(LargeInteger, 0x8);
typedef struct MemoryStatus {       /* MEMORYSTATUS (32 bytes) */
	u32 length, memory_load, total_phys, avail_phys, total_page_file, avail_page_file, total_virtual, avail_virtual;
} MemoryStatus;
GT_OFF(MemoryStatus, length, 0x0); GT_OFF(MemoryStatus, memory_load, 0x4); GT_OFF(MemoryStatus, total_phys, 0x8); GT_OFF(MemoryStatus, avail_phys, 0xc); GT_OFF(MemoryStatus, total_page_file, 0x10); GT_OFF(MemoryStatus, avail_page_file, 0x14); GT_OFF(MemoryStatus, total_virtual, 0x18); GT_OFF(MemoryStatus, avail_virtual, 0x1c); GT_SIZE(MemoryStatus, 0x20);
typedef struct MemoryStatusEx {     /* MEMORYSTATUSEX (64 bytes) */
	u32 length, memory_load;
	LargeInteger total_phys, avail_phys, total_page_file, avail_page_file, total_virtual, avail_virtual, avail_extended_virtual;
} MemoryStatusEx;
GT_OFF(MemoryStatusEx, length, 0x0); GT_OFF(MemoryStatusEx, memory_load, 0x4); GT_OFF(MemoryStatusEx, total_phys, 0x8); GT_OFF(MemoryStatusEx, avail_phys, 0x10); GT_OFF(MemoryStatusEx, total_page_file, 0x18); GT_OFF(MemoryStatusEx, avail_page_file, 0x20); GT_OFF(MemoryStatusEx, total_virtual, 0x28); GT_OFF(MemoryStatusEx, avail_virtual, 0x30); GT_OFF(MemoryStatusEx, avail_extended_virtual, 0x38); GT_SIZE(MemoryStatusEx, 0x40);
typedef struct CriticalSection {    /* RTL_CRITICAL_SECTION (24 bytes) */
	gptr32 debug_info;
	s32 lock_count;                  /* -1 = unlocked */
	u32 recursion_count;
	u32 owning_thread;               /* the runtime's thread id */
	u32 lock_semaphore;
	u32 spin_count;
} CriticalSection;
GT_OFF(CriticalSection, debug_info, 0x0); GT_OFF(CriticalSection, lock_count, 0x4); GT_OFF(CriticalSection, recursion_count, 0x8); GT_OFF(CriticalSection, owning_thread, 0xc); GT_OFF(CriticalSection, lock_semaphore, 0x10); GT_OFF(CriticalSection, spin_count, 0x14); GT_SIZE(CriticalSection, 0x18);
typedef struct CpInfo {             /* CPINFO (20 bytes) */
	u32 max_char_size;
	u8 default_char[2];
	u8 lead_byte[12];
	u8 pad_12[2];
} CpInfo;
GT_OFF(CpInfo, max_char_size, 0x0); GT_OFF(CpInfo, default_char, 0x4); GT_OFF(CpInfo, lead_byte, 0x6); GT_SIZE(CpInfo, 0x14);
typedef struct ThreadNameInfo {     /* the debugger's SetThreadName record (RaiseException 0x406d1388) */
	u32 type;                        /* 0x1000 */
	gptr32 name;
	u32 thread_id;                   /* -1 = the calling thread */
	u32 flags;
} ThreadNameInfo;
GT_OFF(ThreadNameInfo, type, 0x0); GT_OFF(ThreadNameInfo, name, 0x4); GT_OFF(ThreadNameInfo, thread_id, 0x8); GT_OFF(ThreadNameInfo, flags, 0xc); GT_SIZE(ThreadNameInfo, 0x10);

/* ---- the collision broadphase's BVH builder (ovr_bvh.c, 00d785d0) ---- */
typedef struct BvhNode {            /* 64 bytes; the builder's node vector element */
	float box_min[4];                /* +0x00 (vec4) */
	float box_max[4];                /* +0x10 */
	u16 child_a, child_b;            /* +0x20 / +0x22 (0xffff in a leaf) */
	u16 object;                      /* +0x24 (0xffff: internal node) */
	u16 parent;                      /* +0x26 */
	u16 unk_28;
	u16 skip;                        /* +0x2a: the first node after the subtree (written by a later pass, 00d77e90) */
	u16 unk_2c, unk_2e, unk_30, unk_32, unk_34, pad_36;
	u8 unk_38, pad_39[3];
	u32 unk_3c;
} BvhNode;
GT_OFF(BvhNode, box_min, 0x0); GT_OFF(BvhNode, box_max, 0x10); GT_OFF(BvhNode, child_a, 0x20); GT_OFF(BvhNode, child_b, 0x22); GT_OFF(BvhNode, object, 0x24); GT_OFF(BvhNode, parent, 0x26); GT_OFF(BvhNode, unk_28, 0x28); GT_OFF(BvhNode, skip, 0x2a); GT_OFF(BvhNode, unk_2c, 0x2c); GT_OFF(BvhNode, unk_2e, 0x2e); GT_OFF(BvhNode, unk_30, 0x30); GT_OFF(BvhNode, unk_32, 0x32); GT_OFF(BvhNode, unk_34, 0x34); GT_OFF(BvhNode, unk_38, 0x38); GT_OFF(BvhNode, unk_3c, 0x3c); GT_SIZE(BvhNode, 0x40);
typedef struct BvhEntry {           /* the 160-byte per-object entry (the leaf's) */
	u8 pad_00[0x44];
	u16 unk_44;
	u8 pad_46[4];
	u8 unk_4a, unk_4b;
	u32 unk_4c;
	u8 pad_50[4];
	u8 unk_54;
	u8 pad_55[0x4b];
} BvhEntry;
GT_OFF(BvhEntry, unk_44, 0x44); GT_OFF(BvhEntry, unk_4a, 0x4a); GT_OFF(BvhEntry, unk_4b, 0x4b); GT_OFF(BvhEntry, unk_4c, 0x4c); GT_OFF(BvhEntry, unk_54, 0x54); GT_SIZE(BvhEntry, 0xa0);
typedef struct BvhRecord {          /* the 112-byte object record */
	u8 pad_00[0x40];
	float box_min[4];                /* +0x40 */
	float box_max[4];                /* +0x50 */
	u8 pad_60[6];
	u16 unk_66;
	u8 pad_68[8];
} BvhRecord;
GT_OFF(BvhRecord, box_min, 0x40); GT_OFF(BvhRecord, box_max, 0x50); GT_OFF(BvhRecord, unk_66, 0x66); GT_SIZE(BvhRecord, 0x70);
typedef struct BvhVector { gptr32 begin, end, cap; } BvhVector;   /* a std::vector of the build (indices, points) */
GT_OFF(BvhVector, begin, 0x0); GT_OFF(BvhVector, end, 0x4); GT_OFF(BvhVector, cap, 0x8); GT_SIZE(BvhVector, 0xc);
typedef struct BvhBuilder {         /* `this` of 00d785d0 */
	u8 pad_00[4];
	gptr32 nodes_begin;              /* +4: std::vector<BvhNode> begin / end / capacity */
	gptr32 nodes_end;
	gptr32 nodes_cap;
	gptr32 indices;                  /* +0x10 -> BvhVector: the object-index array (reordered in place) */
	gptr32 points;                   /* +0x14 -> BvhVector: one vec4 point per object */
	u8 pad_18[8];
	u32 node_count;                  /* +0x20: nodes in use */
	u8 pad_24[4];
	gptr32 entries;                  /* +0x28: BvhEntry array */
	gptr32 records;                  /* +0x2c: BvhRecord array */
} BvhBuilder;
GT_OFF(BvhBuilder, nodes_begin, 0x4); GT_OFF(BvhBuilder, nodes_end, 0x8); GT_OFF(BvhBuilder, nodes_cap, 0xc); GT_OFF(BvhBuilder, indices, 0x10); GT_OFF(BvhBuilder, points, 0x14); GT_OFF(BvhBuilder, node_count, 0x20); GT_OFF(BvhBuilder, entries, 0x28); GT_OFF(BvhBuilder, records, 0x2c); GT_SIZE(BvhBuilder, 0x30);

/* ---- DirectInput 8 (hle_dinput.c) and Bink (hle_bink.c) structures ---- */
typedef struct DiObject {           /* the 16-byte guest object of an IDirectInput8 / IDirectInputDevice8 */
	gptr32 vtable;
	u32 kind;                        /* D_KEYBOARD / D_MOUSE / ... */
	u32 refs;
	u32 unk_0c;
} DiObject;
GT_OFF(DiObject, vtable, 0x0); GT_OFF(DiObject, kind, 0x4); GT_OFF(DiObject, refs, 0x8); GT_OFF(DiObject, unk_0c, 0xc); GT_SIZE(DiObject, 0x10);
typedef struct DiDeviceInstance {   /* DIDEVICEINSTANCEA (the runtime's 0x254-byte one) */
	u32 size;
	u32 guid_instance_data1, guid_instance_data2_3, guid_instance_data4_lo, guid_instance_data4_hi;
	u32 guid_product_data1, guid_product_data2_3, guid_product_data4_lo, guid_product_data4_hi;
	u32 dev_type;
	char instance_name[260];
	char product_name[260];
	u8 unk_230[36];
} DiDeviceInstance;
GT_OFF(DiDeviceInstance, size, 0x0); GT_OFF(DiDeviceInstance, guid_instance_data1, 0x4); GT_OFF(DiDeviceInstance, guid_instance_data2_3, 0x8); GT_OFF(DiDeviceInstance, guid_instance_data4_lo, 0xc); GT_OFF(DiDeviceInstance, guid_instance_data4_hi, 0x10); GT_OFF(DiDeviceInstance, guid_product_data1, 0x14); GT_OFF(DiDeviceInstance, guid_product_data2_3, 0x18); GT_OFF(DiDeviceInstance, guid_product_data4_lo, 0x1c); GT_OFF(DiDeviceInstance, guid_product_data4_hi, 0x20); GT_OFF(DiDeviceInstance, dev_type, 0x24); GT_OFF(DiDeviceInstance, instance_name, 0x28); GT_OFF(DiDeviceInstance, product_name, 0x12c); GT_OFF(DiDeviceInstance, unk_230, 0x230); GT_SIZE(DiDeviceInstance, 0x254);
typedef struct DiDevCaps { u32 size, flags, dev_type, axes, buttons, povs; } DiDevCaps;   /* DIDEVCAPS (leading fields) */
GT_OFF(DiDevCaps, size, 0x0); GT_OFF(DiDevCaps, flags, 0x4); GT_OFF(DiDevCaps, dev_type, 0x8); GT_OFF(DiDevCaps, axes, 0xc); GT_OFF(DiDevCaps, buttons, 0x10); GT_OFF(DiDevCaps, povs, 0x14); GT_SIZE(DiDevCaps, 0x18);
typedef struct DiMouseState2 {      /* DIMOUSESTATE2 */
	s32 x, y, z;
	u8 buttons[8];
} DiMouseState2;
GT_OFF(DiMouseState2, x, 0x0); GT_OFF(DiMouseState2, y, 0x4); GT_OFF(DiMouseState2, z, 0x8); GT_OFF(DiMouseState2, buttons, 0xc); GT_SIZE(DiMouseState2, 0x14);
typedef struct BinkHead {           /* the BINK structure's leading fields (sync_head writes them) */
	u32 width, height, frames;
	u32 frame_num;                   /* +0x0c */
	u32 last_frame_num;
	u32 frame_rate, frame_rate_div;
} BinkHead;
GT_OFF(BinkHead, width, 0x0); GT_OFF(BinkHead, height, 0x4); GT_OFF(BinkHead, frames, 0x8); GT_OFF(BinkHead, frame_num, 0xc); GT_OFF(BinkHead, last_frame_num, 0x10); GT_OFF(BinkHead, frame_rate, 0x14); GT_OFF(BinkHead, frame_rate_div, 0x18); GT_SIZE(BinkHead, 0x1c);
typedef struct BinkRealtime {       /* BINKREALTIME: 14 dwords (FF13's is on its stack: not a byte more) */
	u32 frame_num, frame_rate, frame_rate_div, unk_0c;
	u32 unk_10[7];
	u32 read_buffer_size;            /* +0x2c */
	u32 read_buffer_used;            /* +0x30 */
	u32 unk_34;
} BinkRealtime;
GT_OFF(BinkRealtime, frame_num, 0x0); GT_OFF(BinkRealtime, frame_rate, 0x4); GT_OFF(BinkRealtime, frame_rate_div, 0x8); GT_OFF(BinkRealtime, unk_0c, 0xc); GT_OFF(BinkRealtime, unk_10, 0x10); GT_OFF(BinkRealtime, read_buffer_size, 0x2c); GT_OFF(BinkRealtime, read_buffer_used, 0x30); GT_OFF(BinkRealtime, unk_34, 0x34); GT_SIZE(BinkRealtime, 0x38);
typedef struct BinkFrameBuffers {   /* BINKFRAMEBUFFERS: the header; two BINKFRAMEPLANESETs (0x30 bytes each) follow at +0x18 */
	u32 total_frames;
	u32 y_buffer_width, y_buffer_height;
	u32 cr_cb_buffer_width, cr_cb_buffer_height;
	u32 frame_num;                   /* the buffer shown, -1 = none */
} BinkFrameBuffers;
GT_OFF(BinkFrameBuffers, total_frames, 0x0); GT_OFF(BinkFrameBuffers, y_buffer_width, 0x4); GT_OFF(BinkFrameBuffers, y_buffer_height, 0x8); GT_OFF(BinkFrameBuffers, cr_cb_buffer_width, 0xc); GT_OFF(BinkFrameBuffers, cr_cb_buffer_height, 0x10); GT_OFF(BinkFrameBuffers, frame_num, 0x14); GT_SIZE(BinkFrameBuffers, 0x18);
typedef struct Variant {            /* VARIANT (16 bytes); the value is the union at +8 */
	u16 vt;
	u16 reserved1, reserved2, reserved3;
	union {
		u8 u8_val;
		u16 u16_val;
		u32 u32_val;
		gptr32 bstr_val;
		s16 s16_val;
		s32 s32_val;
		u64 u64_val;
	};
} Variant;
GT_OFF(Variant, vt, 0); GT_OFF(Variant, u8_val, 8); GT_OFF(Variant, u16_val, 8); GT_OFF(Variant, u32_val, 8); GT_OFF(Variant, bstr_val, 8);
GT_OFF(Variant, s16_val, 8); GT_OFF(Variant, s32_val, 8); GT_OFF(Variant, u64_val, 8); GT_SIZE(Variant, 16);

/* ---- PE image structures (xrc_rt.c's loader), CRT / kernel32 file structures, COM ---- */
typedef struct PeDataDirectory { u32 rva, size; } PeDataDirectory;
GT_OFF(PeDataDirectory, rva, 0x0); GT_OFF(PeDataDirectory, size, 0x4); GT_SIZE(PeDataDirectory, 0x8);
typedef struct PeOptionalHeader {   /* IMAGE_OPTIONAL_HEADER32 (the part the loader reads) */
	u8 pad_00[96];
	PeDataDirectory data_dir[16];    /* PE_DIR_* */
} PeOptionalHeader;
GT_OFF(PeOptionalHeader, data_dir, 0x60); GT_SIZE(PeOptionalHeader, 0xe0);
#define PE_DIR_IMPORT 1
#define PE_DIR_TLS 9
typedef struct PeImportDescriptor { u32 original_first_thunk, time_date_stamp, forwarder_chain, name, first_thunk; } PeImportDescriptor;
GT_OFF(PeImportDescriptor, original_first_thunk, 0x0); GT_OFF(PeImportDescriptor, time_date_stamp, 0x4); GT_OFF(PeImportDescriptor, forwarder_chain, 0x8); GT_OFF(PeImportDescriptor, name, 0xc); GT_OFF(PeImportDescriptor, first_thunk, 0x10); GT_SIZE(PeImportDescriptor, 0x14);
typedef struct PeTlsDirectory { u32 start_raw_data, end_raw_data, address_of_index, address_of_callbacks, size_of_zero_fill, characteristics; } PeTlsDirectory;
GT_OFF(PeTlsDirectory, start_raw_data, 0x0); GT_OFF(PeTlsDirectory, end_raw_data, 0x4); GT_OFF(PeTlsDirectory, address_of_index, 0x8); GT_OFF(PeTlsDirectory, address_of_callbacks, 0xc); GT_OFF(PeTlsDirectory, size_of_zero_fill, 0x10); GT_OFF(PeTlsDirectory, characteristics, 0x14); GT_SIZE(PeTlsDirectory, 0x18);
typedef struct Win32FindData {      /* WIN32_FIND_DATAA (320 bytes); the FILETIMEs as lo / hi dwords */
	u32 file_attributes;
	u32 creation_time_lo, creation_time_hi, last_access_time_lo, last_access_time_hi, last_write_time_lo, last_write_time_hi;
	u32 file_size_high, file_size_low;
	u32 reserved0, reserved1;
	char file_name[260];
	char alternate_file_name[14];
	u8 pad_136[2];
} Win32FindData;
GT_OFF(Win32FindData, file_attributes, 0x0); GT_OFF(Win32FindData, creation_time_lo, 0x4); GT_OFF(Win32FindData, creation_time_hi, 0x8); GT_OFF(Win32FindData, last_access_time_lo, 0xc); GT_OFF(Win32FindData, last_access_time_hi, 0x10); GT_OFF(Win32FindData, last_write_time_lo, 0x14); GT_OFF(Win32FindData, last_write_time_hi, 0x18); GT_OFF(Win32FindData, file_size_high, 0x1c); GT_OFF(Win32FindData, file_size_low, 0x20); GT_OFF(Win32FindData, reserved0, 0x24); GT_OFF(Win32FindData, reserved1, 0x28); GT_OFF(Win32FindData, file_name, 0x2c); GT_OFF(Win32FindData, alternate_file_name, 0x130); GT_SIZE(Win32FindData, 0x140);
typedef struct CrtFindData64i32 {   /* _finddata64i32_t (296 bytes) */
	u32 attrib, pad_04;
	u64 time_create, time_access, time_write;
	u32 size;
	char name[260];
} CrtFindData64i32;
GT_OFF(CrtFindData64i32, attrib, 0x0); GT_OFF(CrtFindData64i32, time_create, 0x8); GT_OFF(CrtFindData64i32, time_access, 0x10); GT_OFF(CrtFindData64i32, time_write, 0x18); GT_OFF(CrtFindData64i32, size, 0x20); GT_OFF(CrtFindData64i32, name, 0x24); GT_SIZE(CrtFindData64i32, 0x128);
typedef struct CrtFile {            /* the guest's FILE (32 bytes), the part the runtime keeps */
	u8 pad_00[16];
	s32 file_no;                     /* _file */
	u8 pad_14[8];
	u32 stream_index;                /* +0x1c (the _tmpfname slot): the runtime's stream table index */
} CrtFile;
GT_OFF(CrtFile, file_no, 0x10); GT_OFF(CrtFile, stream_index, 0x1c); GT_SIZE(CrtFile, 0x20);
typedef struct StdException {       /* std::exception {vfptr, _Mywhat, _Mydofree} */
	gptr32 vfptr;
	gptr32 what;
	u8 do_free;
	u8 pad_09[3];
} StdException;
GT_OFF(StdException, vfptr, 0x0); GT_OFF(StdException, what, 0x4); GT_OFF(StdException, do_free, 0x8); GT_SIZE(StdException, 0xc);
typedef struct ComObject {          /* the 16-byte guest object of a COM class the runtime implements */
	gptr32 vtable;
	u32 id;                          /* index into the runtime's class table */
	u32 unk_08;
	u32 unk_0c;
} ComObject;
GT_OFF(ComObject, vtable, 0x0); GT_OFF(ComObject, id, 0x4); GT_OFF(ComObject, unk_08, 0x8); GT_OFF(ComObject, unk_0c, 0xc); GT_SIZE(ComObject, 0x10);

#endif
