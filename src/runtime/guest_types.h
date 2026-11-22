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
GT_OFF(SystemTime, day, 6); GT_OFF(SystemTime, milliseconds, 14); GT_SIZE(SystemTime, 16);
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
	u8 pad_00[0x34];
	u32 last_error;
} Teb;
GT_OFF(Teb, last_error, 0x34);

#endif
