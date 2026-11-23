/* hle_dinput.c -- DirectInput 8 (DINPUT8.dll): the system keyboard and mouse, their states from the host window's
 * events (xrc_input_*: vk_d3d9.c's SDL window; without one, nothing is pressed); EnumDevices lists no game controllers.
 *
 * Objects are {vtable, kind} in guest memory; vtable slots are imports "IDirectInput8A::Method" /
 * "IDirectInputDevice8A::Method" (xrc_import_slot). Methods without their own function are accepted when they only
 * set something (DI_OK, arguments popped from the counts below); others stop the run with their name. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "xrc_rt.h"
#include "guest_types.h"

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define SCRATCH_BEGIN(c_, bytes_) u32 sp_save_ = (c_)->esp, scr_ = ((c_)->esp - (u32)(bytes_)) & ~15u; (c_)->esp = scr_
#define SCRATCH_END(c_) (c_)->esp = sp_save_
#define XRC_PAD_GUID1 0x50435258u /* "XRCP" */
#define XRC_PAD_GUID2 0x00004441u /* "AD" */
#define S(a) ((char *)(xrc_mem + (u32)(a)))

#define DI_OK 0u
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_INVALIDPARAM 0x80070057u

#define I_DI8(X) X(QueryInterface, 3) X(AddRef, 1) X(Release, 1) X(CreateDevice, 4) X(EnumDevices, 5) \
	X(GetDeviceStatus, 2) X(RunControlPanel, 3) X(Initialize, 3) X(FindDevice, 4) X(EnumDevicesBySemantics, 6) \
	X(ConfigureDevices, 5)
#define I_DEV8(X) X(QueryInterface, 3) X(AddRef, 1) X(Release, 1) X(GetCapabilities, 2) X(EnumObjects, 4) \
	X(GetProperty, 3) X(SetProperty, 3) X(Acquire, 1) X(Unacquire, 1) X(GetDeviceState, 3) X(GetDeviceData, 5) \
	X(SetDataFormat, 2) X(SetEventNotification, 2) X(SetCooperativeLevel, 3) X(GetObjectInfo, 4) \
	X(GetDeviceInfo, 2) X(RunControlPanel, 3) X(Initialize, 4) X(CreateEffect, 5) X(EnumEffects, 4) \
	X(GetEffectInfo, 3) X(GetForceFeedbackState, 2) X(SendForceFeedbackCommand, 2) \
	X(EnumCreatedEffectObjects, 4) X(Escape, 2) X(Poll, 1) X(SendDeviceData, 5) X(EnumEffectsInFile, 5) \
	X(WriteEffectToFile, 5) X(BuildActionMap, 4) X(SetActionMap, 4) X(GetImageInfo, 2)
#define NAME(m, a) #m,
#define NARG(m, a) a,
static const char *const di8_m[] = {I_DI8(NAME)};
static const int di8_a[] = {I_DI8(NARG)};
static const char *const dev8_m[] = {I_DEV8(NAME)};
static const int dev8_a[] = {I_DEV8(NARG)};
#define N_DI8 (int)(sizeof di8_a / sizeof di8_a[0])
#define N_DEV8 (int)(sizeof dev8_a / sizeof dev8_a[0])

static u32 make_vtable(const char *iface, const char *const *m, int n) {
	u32 vt = xrc_heap_alloc(4u * (u32)n, 1);
	char nm[96];
	int k;
	for (k = 0; k < n; k++) {
		snprintf(nm, sizeof nm, "%s::%s", iface, m[k]);
		M32(vt + 4u * (u32)k) = xrc_import_slot("DINPUT8.dll", nm);
	}
	return vt;
}
enum { D_KEYBOARD = 1, D_MOUSE = 2, D_CONTROLLER = 3 };
static u32 vt_di8, vt_dev8;
static u32 new_obj(u32 *vt, const char *iface, const char *const *m, int n, u32 kind) {
	u32 o = xrc_heap_alloc(16, 1);
	if (!*vt) *vt = make_vtable(iface, m, n);
	GF(DiObject, o, vtable) = *vt;
	GF(DiObject, o, kind) = kind;
	GF(DiObject, o, refs) = 1; /* refs */
	return o;
}

/* the generic method: accept the setters */
static int nargs_of(const char *name) {
	const char *m = strstr(name, "::");
	int k;
	if (!m) return -1;
	if (!strncmp(name, "IDirectInput8A::", 16)) { for (k = 0; k < N_DI8; k++) if (!strcmp(di8_m[k], m + 2)) return di8_a[k]; }
	else for (k = 0; k < N_DEV8; k++) if (!strcmp(dev8_m[k], m + 2)) return dev8_a[k];
	return -1;
}
static void m_generic(XCtx *c) {
	const char *name = xrc_cur_import, *m = strstr(name, "::");
	int n = nargs_of(name);
	static const char *const accept[] = {"Set", "Acquire", "Unacquire", "Initialize", "SendForceFeedbackCommand", "Escape"};
	unsigned k;
	if (n < 0 || !m) xrc_fatal(c, "dinput: unknown method %s", name);
	for (k = 0; k < sizeof accept / sizeof accept[0]; k++)
		if (!strncmp(m + 2, accept[k], strlen(accept[k]))) RET(DI_OK, n);
	xrc_fatal(c, "dinput: %s is not implemented yet", name);
}

static void m_QueryInterface(XCtx *c) { GF(DiObject, A(0), refs)++; M32(A(2)) = A(0); RET(DI_OK, 3); }
static void m_AddRef(XCtx *c) { RET(++GF(DiObject, A(0), refs), 1); }
static void m_Release(XCtx *c) { u32 r = GF(DiObject, A(0), refs); if (r) GF(DiObject, A(0), refs) = --r; RET(r, 1); }

/* GUID_SysMouse {6F1D2B60-D5A0-11CF-BFC7-444553540000}, GUID_SysKeyboard {6F1D2B61-...} */
static void di_CreateDevice(XCtx *c) {
	u32 g = A(1), d1 = M32(g);
	u32 kind = d1 == 0x6f1d2b61u ? D_KEYBOARD : d1 == 0x6f1d2b60u ? D_MOUSE : 0;
	if (!kind || GF(Guid, g, data2_3) != 0x11cfd5a0u) {
		fprintf(stderr, "xrc: DirectInput CreateDevice(%08x-...): no such device\n", d1);
		M32(A(2)) = 0;
		RET(DIERR_DEVICENOTREG, 4);
	}
	M32(A(2)) = new_obj(&vt_dev8, "IDirectInputDevice8A", dev8_m, N_DEV8, kind);
	RET(DI_OK, 4);
}
/* EnumDevices(this, devtype, callback, ref, flags): the XInput pads connected (xrc_xinput_users) as game
 * controllers, users 0..the highest one connected (the game polls user i for its i-th XInput device). Their product
 * GUID is XRC_PAD_GUID1-...: the game's IsXInputDevice (00744820, WMI's "IG_" check) is overridden to accept it
 * (ovr_input.c), so the game adds an XInput slot and never opens them through DirectInput. Keyboard and mouse are not
 * asked for. */
u32 xrc_xinput_users(void);
void xrc_pad_hotplug_poll(void);
static void di_EnumDevices(XCtx *c) {
	u32 type = A(1), cb = A(2), ref = A(3), users, n = 0, k, args[2];
	if (type != 0 && type != 4 /* DI8DEVCLASS_ALL, _GAMECTRL */ && (type & 0xff) != 0x15 /* DI8DEVTYPE_GAMEPAD */) RET(DI_OK, 5);
	users = xrc_xinput_users();
	while (users >> n) n++;
	{
		SCRATCH_BEGIN(c, 0x260);
		for (k = 0; k < n; k++) {
			u32 p = scr_;
			memset(xrc_mem + p, 0, 0x254);
			GF(DiDeviceInstance, p, size) = 0x254; /* sizeof DIDEVICEINSTANCEA */
			GF(DiDeviceInstance, p, guid_instance_data1) = XRC_PAD_GUID1 + 0x100 + k; GF(DiDeviceInstance, p, guid_instance_data2_3) = XRC_PAD_GUID2; /* guidInstance */
			GF(DiDeviceInstance, p, guid_product_data1) = XRC_PAD_GUID1; GF(DiDeviceInstance, p, guid_product_data2_3) = XRC_PAD_GUID2; /* guidProduct */
			GF(DiDeviceInstance, p, dev_type) = 0x10215; /* HID, gamepad, standard */
			snprintf(S(p + 40), 260, "XInput pad %u", k + 1);
			snprintf(S(p + 300), 260, "XInput pad");
			args[0] = p; args[1] = ref;
			fprintf(stderr, "xrc: DirectInput EnumDevices: XInput pad %u%s\n", k + 1, users >> k & 1 ? "" : " (not connected)");
			if (!xrc_call_guest(c, cb, 2, args)) break;
		}
		SCRATCH_END(c);
	}
	RET(DI_OK, 5);
}
static void di_GetDeviceStatus(XCtx *c) {
	u32 d1 = M32(A(1));
	RET(d1 == 0x6f1d2b61u || d1 == 0x6f1d2b60u ? DI_OK : 1 /* DI_NOTATTACHED */, 2);
}

/* DIDEVCAPS: dwSize dwFlags dwDevType dwAxes dwButtons dwPOVs ... */
static void dv_GetCapabilities(XCtx *c) {
	u32 p = A(1), kind = GF(DiObject, A(0), kind);
	u32 sz = GF(DiDevCaps, p, size);
	memset(xrc_mem + p + 4, 0, sz > 4 ? sz - 4 : 0);
	GF(DiDevCaps, p, flags) = 1; /* DIDC_ATTACHED */
	GF(DiDevCaps, p, dev_type) = kind == D_KEYBOARD ? 0x413u : 0x112u;
	GF(DiDevCaps, p, axes) = kind == D_MOUSE ? 3 : 0;
	GF(DiDevCaps, p, buttons) = kind == D_MOUSE ? 5 : 128;
	RET(DI_OK, 2);
}
static void dv_GetProperty(XCtx *c) { RET(DIERR_UNSUPPORTED, 3); }
/* XRC_INPUT: scripted input for unattended runs (benchmarks): "secs:what,..." taps at seconds since the first device
 * read; what = kHH (a keyboard key, DIK code in hex) or mN (mouse button N). A tap starts at the first read of its
 * device at or after its time (a loading stall does not lose it) and is held for 150 ms and at least 3 reads, or for
 * "secs:kHH:hold" hold seconds (walking somewhere). pHHHH: XInput pad 0 buttons (mask in hex: 1000 A, 2000 B, 4000 X,
 * 8000 Y, 0001-0008 d-pad up/down/left/right, 0010 Start, 0020 Back; hle_lr.c xi_get_state reports a pad then). */
static unsigned long long input_t0;
static u32 pad_buttons, pad_scripted = 2;
#define D_PAD 0x7061
#define NTAPS 128
void xrc_dinput_scripted(u32 kind, u32 p, u32 n) {
	static const char *script;
	static int read;
	static double start[NTAPS];
	static int reads[NTAPS];
	const char *s;
	struct timespec ts;
	double t;
	int i;
	if (!read) { script = xrc_tool_env("XRC_INPUT"); read = 1; }
	if (!script) return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (!input_t0) input_t0 = (unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000;
	t = ((double)((unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000) - (double)input_t0) / 1000.0;
	for (s = script, i = 0; *s && i < NTAPS; i++) {
		char *e;
		double at = strtod(s, &e), hold;
		unsigned v;
		char w;
		int mine;
		if (*e != ':') break;
		w = e[1];
		v = (unsigned)strtoul(e + 2, &e, 16);
		hold = 0.15;
		if (*e == ':') hold = strtod(e + 1, &e);
		mine = (w == 'k' && kind == D_KEYBOARD && v < n) || (w == 'm' && kind == D_MOUSE && 12 + v < n) || (w == 'p' && kind == D_PAD);
		if (mine && t >= at && reads[i] >= 0) {
			if (!reads[i]) {
				start[i] = t;
				if (xrc_tool_env("XRC_TRACE_TAPS")) fprintf(stderr, "xrc: input tap %c%x (at %.1f s) at %.2f s\n", w, v, at, t);
			}
			if (t < start[i] + hold || reads[i] < 3) {
				reads[i]++;
				if (w == 'p') pad_buttons |= v;
				else M8(p + (w == 'k' ? v : 12 + v)) = 0x80;
			} else reads[i] = -1; /* done */
		}
		if (*e != ',') break;
		s = e + 1;
	}
}
/* the scripted pad's buttons now; -1: the script has no pad taps (XInput then reports the host's pads) */
int xrc_pad_scripted(void) {
	if (pad_scripted == 2) { const char *s = xrc_tool_env("XRC_INPUT"); pad_scripted = s && strstr(s, ":p") != 0; }
	if (!pad_scripted) return -1;
	pad_buttons = 0;
	xrc_dinput_scripted(D_PAD, 0, 0);
	return (int)pad_buttons;
}
/* host input: the window's keyboard and mouse (vk_d3d9.c feeds SDL events here); the devices' states read it */
static volatile u8 host_keys[256], host_mb[8];
/* pressed since the device's last read: reported down at least once, so a press and release between two reads (the
 * game reads once a frame) is not lost */
static volatile u8 host_keys_hit[256], host_mb_hit[8];
static volatile int host_dx, host_dy, host_dz;
/* SDL scancodes (USB usage ids) -> DIK codes */
static int dik_of(int sc) {
	static const u8 az[26] = {0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
	                          0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
	static const u8 t40[] = {0x1c, 0x01, 0x0e, 0x0f, 0x39, 0x0c, 0x0d, 0x1a, 0x1b, 0x2b, 0x2b, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3a, /* 40..57 */
	                         0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,                            /* F1..F12 */
	                         0xb7, 0x46, 0xc5, 0xd2, 0xc7, 0xc9, 0xd3, 0xcf, 0xd1, 0xcd, 0xcb, 0xd0, 0xc8,                      /* 70..82 */
	                         0x45, 0xb5, 0x37, 0x4a, 0x4e, 0x9c, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49, 0x52, 0x53}; /* 83..99 */
	static const u8 mods[8] = {0x1d, 0x2a, 0x38, 0xdb, 0x9d, 0x36, 0xb8, 0xdc}; /* 224..231 */
	if (sc >= 4 && sc <= 29) return az[sc - 4];
	if (sc >= 30 && sc <= 38) return 0x02 + sc - 30; /* 1..9 */
	if (sc == 39) return 0x0b;
	if (sc >= 40 && sc < 40 + (int)sizeof t40) return t40[sc - 40];
	if (sc >= 224 && sc <= 231) return mods[sc - 224];
	return 0;
}
/* XRC_TRACE_INPUT=1 (tools builds): the window's keys and buttons as they come, and each change the game reads */
static int trace_input(void) { static int t = -1; if (t < 0) t = xrc_tool_env("XRC_TRACE_INPUT") != 0; return t; }
void xrc_input_key(int scancode, int down) {
	int k = dik_of(scancode);
	if (trace_input()) fprintf(stderr, "xrc: input: window key scancode %d %s -> DIK %02x\n", scancode, down ? "down" : "up", k);
	if (k) { host_keys[k] = down ? 0x80 : 0; if (down) host_keys_hit[k] = 0x80; }
}
/* a key by its DIK code (Windows: the window's scan code, 0x80 for an extended key) */
void xrc_input_dik(int k, int down) {
	if (trace_input()) fprintf(stderr, "xrc: input: window key DIK %02x %s\n", k, down ? "down" : "up");
	if (k > 0 && k < 256) { host_keys[k] = down ? 0x80 : 0; if (down) host_keys_hit[k] = 0x80; }
}
void xrc_input_mouse(int dx, int dy, int dz) { host_dx += dx; host_dy += dy; host_dz += dz; }
void xrc_input_button(int b, int down) {
	if (trace_input()) fprintf(stderr, "xrc: input: window mouse button %d %s\n", b, down ? "down" : "up");
	if (b >= 1 && b <= 8) { /* (SDL: 1 left 2 middle 3 right) */
		int i = (b == 2 ? 3 : b == 3 ? 2 : b) - 1;
		host_mb[i] = down ? 0x80 : 0;
		if (down) host_mb_hit[i] = 0x80;
	}
}
void xrc_input_release_all(void) { memset((void *)host_keys, 0, sizeof host_keys); memset((void *)host_mb, 0, sizeof host_mb); }
int xrc_input_dik_down(int dik) { return dik > 0 && dik < 256 && host_keys[dik]; }
static void dv_GetDeviceState(XCtx *c) {
	u32 kind = GF(DiObject, A(0), kind), n = A(1), p = A(2), k;
	memset(xrc_mem + p, 0, n);
	if (kind == D_KEYBOARD) {
		static u8 seen[256];
		xrc_pad_hotplug_poll(); /* (the game reads the keyboard once a frame) */
		for (k = 0; k < n && k < 256; k++) {
			M8(p + k) = host_keys[k] | host_keys_hit[k];
			host_keys_hit[k] = 0;
			if (trace_input() && seen[k] != host_keys[k]) { seen[k] = host_keys[k]; fprintf(stderr, "xrc: input: the game reads DIK %02x %s\n", k, seen[k] ? "down" : "up"); }
		}
	}
	else if (kind == D_MOUSE && n >= 16) { /* DIMOUSESTATE(2): lX lY lZ (relative), buttons */
		GF(DiMouseState2, p, x) = (u32)host_dx; GF(DiMouseState2, p, y) = (u32)host_dy; GF(DiMouseState2, p, z) = (u32)host_dz;
		host_dx = host_dy = host_dz = 0;
		for (k = 0; k < 8 && 12 + k < n; k++) { GFI(DiMouseState2, p, buttons, k) = host_mb[k] | host_mb_hit[k]; host_mb_hit[k] = 0; }
		if (trace_input()) for (k = 0; k < 3; k++) if (GFI(DiMouseState2, p, buttons, k)) fprintf(stderr, "xrc: input: the game reads mouse button %u down\n", k);
	}
	xrc_dinput_scripted(kind, p, n);
	RET(DI_OK, 3);
}
/* GetDeviceData(size, data, *inout, flags): no buffered events */
static void dv_GetDeviceData(XCtx *c) { if (A(3)) M32(A(3)) = 0; RET(DI_OK, 5); }
static void dv_Poll(XCtx *c) { RET(1 /* DI_NOEFFECT */, 1); }
static void dv_EnumObjects(XCtx *c) { RET(DI_OK, 4); }
static void dv_EnumEffects(XCtx *c) { RET(DI_OK, 4); }
/* DIDEVICEINSTANCEA: dwSize guidInstance guidProduct dwDevType tszInstanceName[260] tszProductName[260] ... */
static void dv_GetDeviceInfo(XCtx *c) {
	u32 p = A(1), kind = GF(DiObject, A(0), kind), sz = GF(DiDeviceInstance, p, size);
	memset(xrc_mem + p + 4, 0, sz > 4 ? sz - 4 : 0);
	GF(DiDeviceInstance, p, guid_instance_data1) = kind == D_KEYBOARD ? 0x6f1d2b61u : 0x6f1d2b60u; GF(DiDeviceInstance, p, guid_instance_data2_3) = 0x11cfd5a0u;
	GF(DiDeviceInstance, p, guid_instance_data4_lo) = 0x534bc7bfu; GF(DiDeviceInstance, p, guid_instance_data4_hi) = 0x00005345u;
	memcpy(xrc_mem + p + 20, xrc_mem + p + 4, 16);
	GF(DiDeviceInstance, p, dev_type) = kind == D_KEYBOARD ? 0x413u : 0x112u;
	snprintf(S(p + 40), 260, kind == D_KEYBOARD ? "Keyboard" : "Mouse");
	snprintf(S(p + 300), 260, kind == D_KEYBOARD ? "Keyboard" : "Mouse");
	RET(DI_OK, 2);
}

/* a device object for a game controller (the reference target's real one: ref_dinput.c) */
u32 xrc_dinput_new_device(void) { return new_obj(&vt_dev8, "IDirectInputDevice8A", dev8_m, N_DEV8, D_CONTROLLER); }

/* DirectInput8Create(hinst, version, riid, out, outer) */
static void hle_DirectInput8Create(XCtx *c) {
	M32(A(3)) = new_obj(&vt_di8, "IDirectInput8A", di8_m, N_DI8, 0);
	RET(DI_OK, 5);
}

#define M(i, n, f) {"DINPUT8.dll", i "::" #n, f}
#define GEN_DI8(m, a) {"DINPUT8.dll", "IDirectInput8A::" #m, m_generic},
#define GEN_DEV8(m, a) {"DINPUT8.dll", "IDirectInputDevice8A::" #m, m_generic},
const XrcImport xrc_hle_dinput[] = {
	{"DINPUT8.dll", "DirectInput8Create", hle_DirectInput8Create},
	M("IDirectInput8A", QueryInterface, m_QueryInterface), M("IDirectInput8A", AddRef, m_AddRef),
	M("IDirectInput8A", Release, m_Release), M("IDirectInput8A", CreateDevice, di_CreateDevice),
	M("IDirectInput8A", EnumDevices, di_EnumDevices), M("IDirectInput8A", GetDeviceStatus, di_GetDeviceStatus),
	M("IDirectInputDevice8A", QueryInterface, m_QueryInterface), M("IDirectInputDevice8A", AddRef, m_AddRef),
	M("IDirectInputDevice8A", Release, m_Release), M("IDirectInputDevice8A", GetCapabilities, dv_GetCapabilities),
	M("IDirectInputDevice8A", GetProperty, dv_GetProperty), M("IDirectInputDevice8A", GetDeviceState, dv_GetDeviceState),
	M("IDirectInputDevice8A", GetDeviceData, dv_GetDeviceData), M("IDirectInputDevice8A", Poll, dv_Poll),
	M("IDirectInputDevice8A", EnumObjects, dv_EnumObjects), M("IDirectInputDevice8A", EnumEffects, dv_EnumEffects),
	M("IDirectInputDevice8A", GetDeviceInfo, dv_GetDeviceInfo),
	I_DI8(GEN_DI8) I_DEV8(GEN_DEV8)
};
const unsigned xrc_hle_dinput_n = sizeof xrc_hle_dinput / sizeof xrc_hle_dinput[0];
