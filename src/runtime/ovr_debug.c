/* The debug free camera left in LRFF13.exe, reached from the runtime (XRC_FREECAM=1).
 *
 * What the retail exe still has (Scene.cpp, 2026-11-20):
 *   - every scene makes a "default_camera" (00671be0) with a white::scene::SceneCameraControlByDebugPad on it (vtable
 *     020903ec; its update 0070b3a0, run every frame): a fly camera driven by the SECOND pad (Hid [0x24d94ec] + 8).
 *   - Scene::GetActiveCamera (00669850) renders that camera instead of the game's ([scene + 0x40]) while the scene's
 *     flags [scene + 0x30] have bit 4.
 *   - the toggle (00666a70, once a frame): only when the AppGame flags [[0x2350974] + 0x680] have 0x1000 (never set
 *     in retail: the only test of that bit); then the second pad's button 0x4000 (just pressed) flips bit 4, and on
 *     the switch copies the game camera's position and fov into the debug camera and blinks "---- DEBUG CAMERA ----"
 *     (debug print, not shown in retail). Scene flag 0x40000 asks for that copy without a flip.
 *
 * The runtime's toggle: F6 (the game's window in front) or XRC_FREECAM_AT=secs,secs,.. (tests, seconds since the
 * first scene update) flips scene bit 4 and sets 0x40000 (the game then copies the camera itself), with the AppGame
 * gate bit set. While on, the first pad's state (the player's pad / keyboard emulation) is moved to the second pad
 * after each Hid update (00742420), so the debug camera flies with it and Lightning stands still. XRC_FREECAM_GAMEPAD2=1: leave the second pad driving it (two pads).
 * Photo mode (2026-11-20): while the camera is free the HUD is hidden with the game's own GuiApi::showHideFieldMenu
 * (00799b60; XRC_FREECAM_HUD=1 keeps it), the fly speed is scaled (XRC_FREECAM_SPEED, default 0.05) by scaling the frame
 * delta [[0x234f130] + 0x44] the camera's update reads, Shift / Ctrl (host keys) or the first pad's RT / LT boost or slow
 * it, F9 (or XRC_FREECAM_PAUSE=1 with the camera) pauses the world with scene flags (see fc_pause_apply), F12 saves a
 * PNG through vk_d3d9.c's xrc_shot_request.
 * XRC_FREECAM_SCRIPT="secs:cam,secs:pause[=hexmask],secs:shot,secs:mod=b|s|n,secs:hud=0|1" scripts the same for tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "xrc.h"
#include "xrc_rt.h"
#include "guest_types.h"
#include <pthread.h>

#define FN_SHOWHIDE_FIELDMENU 0x00799b60u /* cdecl GuiApi::showHideFieldMenu(visible, 0) */

static int fc_mode = -1;     /* XRC_FREECAM: 0 off, 1 on */
static int fc_on;            /* the debug camera shown (our view of scene bit 4) */
static int fc_pad2;          /* XRC_FREECAM_GAMEPAD2 */
static int fc_pause_auto;    /* XRC_FREECAM_PAUSE: pause the world whenever the camera goes on */
static int fc_keep_hud;      /* XRC_FREECAM_HUD */
static float fc_speed = 2.5f; /* XRC_FREECAM_SPEED: m/s at a full stick */
static float fc_rot = 60.0f;  /* XRC_FREECAM_TURN: degrees / s at a full right stick */
static double fc_rot_acc;
static u32 fc_pause_mask = SC_PAUSE | SC_MOVEPAUSE; /* XRC_FREECAM_PAUSE_MASK (hex): the scene flags a pause sets */
static volatile int fc_paused;
static volatile int fc_hud_req = -1;    /* a showHideFieldMenu(0|1) call for the game thread (Hid hook) */
static int fc_hud_hidden;
static u32 fc_hud_was = 1;
static volatile int fc_mod_script;      /* the script's override: 0 none, 1 boost, -1 slow, 2 normal */
static volatile u32 fc_dbg_set[3];    /* bits of the same words set every frame while the camera is free */
static volatile u32 fc_dbg_clear[3];  /* bits of the debug-settings words 0x50e75c4 / c8 / cc cleared every frame while the camera is free */
static volatile int fc_force_turn;      /* script: a full right stick (tests) */
static volatile int fc_xi_mod;          /* keys / pad: 1 boost, -1 slow */
#define MAX_EV 64
static struct { double t; char kind; u32 arg; } fc_ev[MAX_EV];
static int fc_nev, fc_nextev;
static u32 fc_dbg_arg[MAX_EV];
static unsigned long long fc_t0;
static int fc_log;

void xrc_shot_request(const char *path); /* vk_d3d9.c: save the next presented frame as a PNG at path */

int xrc_freecam_active(void) { return fc_on; }

static void fc_script_parse(const char *a) {
	while (a && *a && fc_nev < MAX_EV) {
		char *n;
		double t = strtod(a, &n);
		char *w;
		if (n == a || *n != ':') break;
		w = n + 1;
		fc_ev[fc_nev].arg = 0;
		if (!strncmp(w, "cam", 3)) { fc_ev[fc_nev].kind = 'c'; w += 3; }
		else if (!strncmp(w, "pause", 5)) {
			w += 5;
			fc_ev[fc_nev].kind = 'p';
			fc_ev[fc_nev].arg = 0xffffffffu; /* (toggle) */
			if (*w == '=') fc_ev[fc_nev].arg = (u32)strtoul(w + 1, &w, 16);
		}
		else if (!strncmp(w, "shot", 4)) { fc_ev[fc_nev].kind = 's'; w += 4; }
		else if (!strncmp(w, "mod=", 4)) { fc_ev[fc_nev].kind = 'm'; fc_ev[fc_nev].arg = w[4] == 'b' ? 1 : w[4] == 's' ? (u32)-1 : 2; w += 5; }
		else if (!strncmp(w, "turn=", 5)) { fc_ev[fc_nev].kind = 't'; fc_ev[fc_nev].arg = (u32)(w[5] != '0'); w += 6; }
		else if (!strncmp(w, "dbgset=", 7)) { fc_ev[fc_nev].kind = 'D'; fc_ev[fc_nev].arg = (u32)strtoul(w + 7, &w, 16); if (*w == ':') w++; fc_dbg_arg[fc_nev] = (u32)strtoul(w, &w, 16); }
		else if (!strncmp(w, "dbg=", 4)) { fc_ev[fc_nev].kind = 'd'; fc_ev[fc_nev].arg = (u32)strtoul(w + 4, &w, 16); if (*w == ':') w++; fc_dbg_arg[fc_nev] = (u32)strtoul(w, &w, 16); }
		else if (!strncmp(w, "hud=", 4)) { fc_ev[fc_nev].kind = 'h'; fc_ev[fc_nev].arg = (u32)(w[4] != '0'); w += 5; }
		else break;
		fc_ev[fc_nev++].t = t;
		a = *w == ',' ? w + 1 : w;
	}
}

static void fc_init(void) {
	const char *e = getenv("XRC_FREECAM"), *a = getenv("XRC_FREECAM_AT"), *v;
	fc_mode = e && *e && *e != '0';
	fc_pad2 = getenv("XRC_FREECAM_GAMEPAD2") != 0;
	fc_pause_auto = getenv("XRC_FREECAM_PAUSE") && *getenv("XRC_FREECAM_PAUSE") != '0';
	fc_keep_hud = getenv("XRC_FREECAM_HUD") && *getenv("XRC_FREECAM_HUD") != '0';
	fc_log = getenv("XRC_FREECAM_LOG") != 0;
	if ((v = getenv("XRC_FREECAM_SPEED")) && atof(v) > 0) fc_speed = (float)atof(v);
	if ((v = getenv("XRC_FREECAM_TURN")) && atof(v) > 0) fc_rot = (float)atof(v);
	if ((v = getenv("XRC_FREECAM_PAUSE_MASK")) && *v) fc_pause_mask = (u32)strtoul(v, 0, 16);
	while (a && *a && fc_nev < MAX_EV) {
		char *n;
		double t = strtod(a, &n);
		if (n == a) break;
		fc_ev[fc_nev].kind = 'c'; fc_ev[fc_nev].arg = 0; fc_ev[fc_nev++].t = t;
		a = *n == ',' ? n + 1 : n;
	}
	fc_script_parse(getenv("XRC_FREECAM_SCRIPT"));
	if (fc_mode)
		fprintf(stderr, "xrc: freecam: F6 camera, F9 pause the world, F12 photo (PNG), Shift boost / Ctrl slow (pad: RT / LT); speed %.3g m/s, turn %.3g deg/s%s%s\n",
		        fc_speed, fc_rot, fc_nev ? ", scripted" : "", fc_keep_hud ? ", HUD kept" : "");
}

/* host keys / pad (the game's window in front only) */
static int fg_ours(void) {
#ifdef _WIN32
	HWND w = GetForegroundWindow();
	DWORD pid = 0;
	if (w) GetWindowThreadProcessId(w, &pid);
	return pid == GetCurrentProcessId();
#else
	return 0;
#endif
}
static int key_edge(int vk, int *was, int ours) {
#ifdef _WIN32
	int down = ours && (GetAsyncKeyState(vk) & 0x8000) != 0, edge = down && !*was;
	*was = down;
	return edge;
#else
	(void)vk; (void)was; (void)ours;
	return 0;
#endif
}
#ifdef _WIN32
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } XiGp;
typedef struct { DWORD pkt; XiGp gp; } XiState;
static int xi_mod(void) { /* the first pad's triggers: RT boost, LT slow */
	static DWORD (WINAPI *get)(DWORD, XiState *);
	static int tried;
	XiState st;
	if (!tried) {
		HMODULE m = LoadLibraryA("xinput1_4.dll");
		tried = 1;
		if (!m) m = LoadLibraryA("xinput9_1_0.dll");
		if (m) get = (DWORD (WINAPI *)(DWORD, XiState *))(void *)GetProcAddress(m, "XInputGetState");
	}
	if (!get || get(0, &st) != 0) return 0;
	return st.gp.bRT > 100 ? 1 : st.gp.bLT > 100 ? -1 : 0;
}
#endif

/* the world pause: scene flags (Scene 006681e0 reads them every frame: 0x20000 = time scale 0, 0x20 = the move objects
 * pause); the debug camera's update goes on (Scene 00666a70 runs either way) */
static void fc_pause_apply(u32 scene, int on) {
	if (on) GF(Scene, scene, flags) |= fc_pause_mask; else GF(Scene, scene, flags) &= ~fc_pause_mask;
	fc_paused = on;
	fprintf(stderr, "xrc: freecam: world %s (scene flags %08x, mask %08x)\n", on ? "paused" : "running", GF(Scene, scene, flags), fc_pause_mask);
}
static void fc_toggle_pause(u32 set) { /* set: 0xffffffff toggle, else a mask (0 = run) */
	u32 scene = M32(G_SCENE);
	if (!scene) return;
	if (set != 0xffffffffu) {
		if (!set) fc_pause_apply(scene, 0);
		else { if (fc_paused) fc_pause_apply(scene, 0); fc_pause_mask = set; fc_pause_apply(scene, 1); }
	} else fc_pause_apply(scene, !fc_paused);
}
static void fc_toggle(void) {
	u32 scene = M32(G_SCENE), app = M32(G_APP);
	if (!scene || !app) return;
	GF(AppGame, app, flags) |= APP_DEBUGCAM;
	GF(Scene, scene, flags) ^= SC_DEBUGCAM;
	GF(Scene, scene, flags) |= SC_RESYNC; /* (the game copies its camera into the debug one: 00666a70) */
	fc_on = (GF(Scene, scene, flags) & SC_DEBUGCAM) != 0;
	if (fc_on && !fc_keep_hud) fc_hud_req = 0;
	if (!fc_on) { fc_hud_req = fc_hud_hidden ? 1 : -1; if (fc_paused) fc_pause_apply(scene, 0); }
	if (fc_on && fc_pause_auto && !fc_paused) fc_pause_apply(scene, 1);
	fprintf(stderr, "xrc: freecam: debug words c4 %08x c8 %08x cc %08x\n", GFI(DebugWords, G_DEBUG_WORDS, w, 0), GFI(DebugWords, G_DEBUG_WORDS, w, 1), GFI(DebugWords, G_DEBUG_WORDS, w, 2));
	fprintf(stderr, "xrc: freecam %s at %.1f s (scene %08x flags %08x, app flags %08x)\n", fc_on ? "on" : "off",
	        (double)(xrc_now_us() - fc_t0) * 1e-6, scene, GF(Scene, scene, flags), GF(AppGame, app, flags));
}
static void fc_photo(void) {
	char path[700];
	const char *d = getenv("XRC_SHOT_DIR");
	static int n;
	unsigned long long t = xrc_now_us() / 1000000ull;
	snprintf(path, sizeof path, "%s/photo_%llu_%02d.png", d && *d ? d : ".", t, n++);
	xrc_shot_request(path);
	fprintf(stderr, "xrc: freecam: photo requested -> %s\n", path);
}

/* the toggle's own thread: the keys and XRC_FREECAM_SCRIPT are seen even while the game is paused */
static void *fc_poll(void *arg) {
	int w6 = 0, w7 = 0, w12 = 0;
	(void)arg;
	for (;;) {
		int ours = fg_ours(), go = key_edge(VK_F6, &w6, ours), go7 = key_edge(VK_F9, &w7, ours), go12 = key_edge(VK_F12, &w12, ours);
		double now = (double)(xrc_now_us() - fc_t0) * 1e-6;
		int m = 0;
#ifdef _WIN32
		if (ours) m = ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) - ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0);
		if (!m && ours) m = xi_mod();
#endif
		fc_xi_mod = m;
		if (fc_mode <= 0) { w6 = w7 = w12 = 0; Sleep(50); continue; } /* (the settings menu turned it off) */
		while (fc_nextev < fc_nev && now >= fc_ev[fc_nextev].t) {
			u32 a = fc_ev[fc_nextev].arg;
			switch (fc_ev[fc_nextev++].kind) {
			case 'c': go = 1; break;
			case 'p': xrc_gil_acquire(); fc_toggle_pause(a); xrc_gil_release(); break;
			case 's': go12 = 1; break;
			case 'm': fc_mod_script = (int)a; break;
			case 'h': fc_hud_req = (int)a; break;
			case 't': fc_force_turn = (int)a; break;
			case 'd': if (a < 3) fc_dbg_clear[a] = fc_dbg_arg[fc_nextev - 1]; break;
			case 'D': if (a < 3) fc_dbg_set[a] = fc_dbg_arg[fc_nextev - 1]; break;
			}
		}
		if (go) { xrc_gil_acquire(); fc_toggle(); xrc_gil_release(); }
		if (go7) { xrc_gil_acquire(); fc_toggle_pause(0xffffffffu); xrc_gil_release(); }
		if (go12) fc_photo();
#ifdef _WIN32
		Sleep(15);
#endif
	}
	return 0;
}

/* xrc_menu.c's "Free camera" setting (live): on lets F6 / F9 / F12 work; off puts the game's camera back first */
void xrc_freecam_enable(int on) {
	if (fc_mode < 0) fc_init();
	on = on != 0;
	if (!on && fc_mode && fc_on) {
		int held = xrc_gil_held();
		if (!held) xrc_gil_acquire();
		fc_toggle();
		if (!held) xrc_gil_release();
	}
	fc_mode = on;
}

/* 0070b3a0 white::scene::SceneCameraControlByDebugPad::vf9 (the debug camera's update; thiscall, 3 args). Retail:
 * it moves by stick * [[0x234f130] + 0x44] * 3 * 0.0334 per call. That delta is a fixed 2 per update (not wall-clock), so
 * the fly speed follows the update rate (12 m/s at 60 fps, 26 m/s at 130 fps) -- and the turn rate (0.0334 rad per call at
 * a full right stick) too. Here: the delta is replaced for this call by speed * wall-clock time / 0.1001 (m/s at a full
 * stick), and the right stick is dropped on some calls so the turn is deg/s. */
void XRC_CC xo_0070b3a0(XCtx *c) {
	static XFn orig;
	static unsigned long long last;
	u32 scene, tm, hid, p1 = 0;
	float dt = 0, mult;
	int m;
	if (!orig) orig = xrc_translation_of(0x0070b3a0u);
	if (fc_mode < 0) fc_init();
	if (!fc_mode) { orig(c); return; }
	if (!fc_t0) {
		pthread_t t;
		fc_t0 = xrc_now_us();
		if (!pthread_create(&t, 0, fc_poll, 0)) pthread_detach(t);
	}
	scene = M32(G_SCENE);
	if (scene) fc_on = (GF(Scene, scene, flags) & SC_DEBUGCAM) != 0; /* (a second pad's own toggle, a scene change) */
	tm = M32(G_TIME);
	hid = M32(G_HID);
	m = fc_mod_script == 2 ? 0 : fc_mod_script ? fc_mod_script : fc_xi_mod;
	mult = m > 0 ? 5.0f : m < 0 ? 0.2f : 1.0f;
	if (fc_on) {
		unsigned long long now = xrc_now_us();
		double wall = last ? (double)(now - last) * 1e-6 : 0.016;
		last = now;
		if (wall < 0.001) wall = 0.001;
		if (wall > 0.1) wall = 0.1;
		if (tm) { dt = GF(FrameTimer, tm, delta); GF(FrameTimer, tm, delta) = (float)(fc_speed * mult * wall / 0.1001); }
		/* the turn: the right stick (+0x2c, +0x30 of the second pad) kept on a share of the calls */
		if (hid && (p1 = GF(Hid, hid, pad1))) {
			double f = (double)fc_rot * mult / 1.912 * wall;
			fc_rot_acc += f;
			if (fc_rot_acc > 2.0) fc_rot_acc = 2.0;
			if (fc_rot_acc >= 1.0) fc_rot_acc -= 1.0;
			else { GF(Pad, p1, right_stick_x) = 0; GF(Pad, p1, right_stick_y) = 0; }
		}
	} else last = 0;
	if (fc_log && fc_on) {
		static unsigned n;
		if (n++ % 30 == 0) {
			u32 o = arg_u32(c, 1);
			fprintf(stderr, "xrc: freecam: dt %g->%g mult %g pos %.2f %.2f %.2f yaw %.1f paused %d scene flags %08x t %.2f\n", dt, tm ? GF(FrameTimer, tm, delta) : 0.f, mult,
			        GF(DebugCamera, o, pos_x), GF(DebugCamera, o, pos_y), GF(DebugCamera, o, pos_z), atan2(GF(DebugCamera, o, unk_170), GF(DebugCamera, o, unk_178)) * 57.29578, fc_paused, scene ? GF(Scene, scene, flags) : 0,
			        (double)(xrc_now_us() - fc_t0) * 1e-6);
		}
	}
	orig(c);
	if (tm && fc_on) GF(FrameTimer, tm, delta) = dt;
}

/* 00742420 white::io::Hid::vf7, the pads' update once a frame (before the actors and the scene). While the camera is
 * free, the first pad's state (buttons +4..+0x18, sticks / triggers +0x1c..+0x40; the keyboard emulation lands there
 * too) moves to the second pad, which the debug camera reads, and the first pad reads as idle: Lightning stands
 * still and the game's menus do not open. Also the game thread's hook for the HUD call. */
void XRC_CC xo_00742420(XCtx *c) {
	static XFn orig;
	u32 hid = c->ecx, p0, p1;
	if (!orig) {
		orig = xrc_translation_of(0x00742420u);
		if (getenv("XRC_FREECAM_LOG")) fprintf(stderr, "xrc: freecam: Hid::Update dispatched (hid %08x)\n", hid);
	}
	orig(c);
	if (fc_mode > 0 && fc_hud_req >= 0) {
		u32 gm = M32(G_GUIMGR);
		int want = fc_hud_req;
		if (gm) {
			XCtx sv = *c;
			u32 a[2];
			if ((!want) == (fc_hud_hidden != 0)) fc_hud_req = -1; /* (already so) */
			else {
				if (!want) fc_hud_was = (GF(GuiManager, gm, flags) >> 4) & 1;
				a[0] = (u32)want; a[1] = 0;
				if (want && !fc_hud_was) a[0] = 0; /* (it was hidden before the camera: leave it so) */
				xrc_call_guest(c, FN_SHOWHIDE_FIELDMENU, 2, a);
				*c = sv;
				fc_hud_hidden = !want;
				fc_hud_req = -1;
				fprintf(stderr, "xrc: freecam: HUD %s (GuiManager %08x flags %08x)\n", want ? "shown" : "hidden", gm, GF(GuiManager, gm, flags));
			}
		}
	}
	{ static int gui_off; /* bit 18 of word c8: the game's GUI-draw skip (hides the battle HUD too) */
		if (fc_mode > 0 && fc_on && !fc_keep_hud) { GFI(DebugWords, G_DEBUG_WORDS, w, 1) |= DBGW1_GUI_DRAW_SKIP; gui_off = 1; }
		else if (gui_off) { GFI(DebugWords, G_DEBUG_WORDS, w, 1) &= ~DBGW1_GUI_DRAW_SKIP; gui_off = 0; } }
	if (fc_mode > 0 && fc_on) { int k; for (k = 0; k < 3; k++) { GFI(DebugWords, G_DEBUG_WORDS, w, k) &= ~fc_dbg_clear[k]; GFI(DebugWords, G_DEBUG_WORDS, w, k) |= fc_dbg_set[k]; } }
	if (fc_mode <= 0 || !fc_on || fc_pad2 || !hid) return;
	p0 = GF(Hid, hid, pad0); p1 = GF(Hid, hid, pad1);
	if (!p0 || !p1) return;
	memcpy(XM + p1 + PAD_STATE_LO, XM + p0 + PAD_STATE_LO, PAD_STATE_HI - PAD_STATE_LO);
	memset(XM + p0 + PAD_STATE_LO, 0, PAD_STATE_HI - PAD_STATE_LO);
	if (fc_force_turn) GF(Pad, p1, right_stick_x) = 1.0f;
}

const XrcManual xrc_manual_dbg[] = {
	{0x00742420u, xo_00742420, "Hid::Update: the first pad drives the free camera (ovr_debug.c)"},
	{0x0070b3a0u, xo_0070b3a0, "SceneCameraControlByDebugPad update: the free camera (ovr_debug.c)"},
};
const unsigned xrc_manual_dbg_n = sizeof xrc_manual_dbg / sizeof xrc_manual_dbg[0];
