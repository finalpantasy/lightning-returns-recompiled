/* xrc_menu.c - the in-game settings menu (F10 in the game's window; LR, Windows).
 *
 * The game's own settings (Configuration.ini: resolution, quality levels, frame rate, button layout) are read once at
 * boot, as on the original, where they needed the game's Reboot; they are lr.cfg's XRC_CFG_<Key> (hle_steam.c reads
 * the file with them) and take effect when the game starts again (the menu's Restart, or the next start). The
 * runtime's own options change at once (FXAA, HD cinematics). Every choice is written to lr.cfg (beside lr.exe: the
 * same file the launcher writes) and to this process's environment.
 *
 * The menu is an overlay drawn into the game's picture (vk_d3d9.c ovl_compose): a panel made here on the CPU
 * (premultiplied BGRA; text from Segoe UI through stb_truetype), blended over the dimmed game. It stays open until
 * F10 / Esc / the pad's B or its Close row; while it is open no key, mouse button or pad input reaches the game.
 * Keys and the mouse come from the window's thread (vk_d3d9.c own_proc: xrc_menu_key, xrc_menu_mouse) into a queue;
 * the present thread (xrc_menu_frame) reads the pad, handles the queue and redraws the panel when something changed.
 * Shift+F10: the old Win32 popup (xrc_settings_menu). */
#if defined(XRC_LR) && defined(_WIN32)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "xrc_rt.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "third_party/stb_truetype.h"
#pragma clang diagnostic pop

void xrc_relaunch_at_exit(void);
void xrc_vk_set_fxaa(int q);
void xrc_vk_set_tex_hd(int on);
void xrc_vk_set_tex_ui(int on);
void xrc_vk_set_aniso(int n);
void xrc_vk_set_shadow_hold(int n);
void xrc_vk_set_borderless(int on) __attribute__((weak)); /* (vk_d3d9.c: its own window only) */
void xrc_freecam_enable(int on); /* (ovr_debug.c) */
#include "vk_post.h" /* (xrc_vk_set_cas / set_downscale / set_deband) */

typedef struct {
	const char *label, *key;
	int live; /* 1: applies at once; 0: when the game starts again */
	const char *vals[13], *names[13]; /* vals[0] "" = not set in lr.cfg (the game's own file decides) */
	const char *desc;                 /* the overlay's description line: what it changes, its cost, when it applies */
} Opt;
#define GAME_SAVED "Game's saved setting"
#define ADV_STD {"", "Advanced", "Standard"}, {GAME_SAVED, "High quality", "Low quality"}
#define ADV_STD_TAIL " High quality is the game's Advanced level and Low quality its Standard level; the higher level can cost some fps. Needs a restart."
/* (the order is the XRC_SETTINGS_PICK ids': 0x7000 + index * 16 + value) */
static const Opt opts[] = {
	{"Render resolution", "XRC_CFG_Graphics_Resolution", 0,
	 {"", "3840x2160", "3200x1800", "2880x1620", "2560x1440", "2400x1350", "2160x1215", "1920x1080", "1600x900", "1440x810", "1366x768", "1280x720"},
	 {GAME_SAVED, "4K 3840x2160", "3200x1800", "2880x1620", "1440p 2560x1440", "2400x1350", "2160x1215", "1080p 1920x1080",
	  "1600x900", "1440x810", "1366x768", "720p 1280x720"},
	 "The size the game draws its picture at before it is fit to your screen. Higher is sharper but costs fps; 4K needs a strong graphics card. Needs a restart."},
	{"Display scaling", "XRC_CFG_Graphics_Scaling", 0, {"", "Advanced", "Standard", "None"}, {GAME_SAVED, "High quality", "Standard", "Off"},
	 "The game's \"scaling of the display area\" option: how it scales its picture to fill the display. High quality is its Advanced filter, Standard its basic one, Off turns its scaling off. Needs a restart."},
	{"Color correction", "XRC_CFG_Graphics_ColorCorrection", 0, ADV_STD,
	 "The color grading pass that sets the tone and tint of each scene." ADV_STD_TAIL},
	{"Glare", "XRC_CFG_Graphics_Glare", 0, ADV_STD,
	 "The glow and glare around bright lights and the sun." ADV_STD_TAIL},
	{"Depth of field", "XRC_CFG_Graphics_DepthOfField", 0, ADV_STD,
	 "Blurs the parts of the scene that are out of focus, such as the far background." ADV_STD_TAIL},
	{"Shadows", "XRC_CFG_Graphics_Shadowing", 0, ADV_STD,
	 "The quality of the shadows cast by characters and scenery." ADV_STD_TAIL},
	{"Lighting", "XRC_CFG_Graphics_Lighting", 0, ADV_STD,
	 "The quality of the scene's lighting." ADV_STD_TAIL},
	{"Texture filtering", "XRC_CFG_Graphics_TextureFiltering", 0, ADV_STD,
	 "The game's own texture smoothing. Texture sharpness (below) overrides it for surfaces seen at an angle unless that is set to Game's own." ADV_STD_TAIL},
	{"Frame rate", "XRC_CFG_Graphics_FrameRate", 0, {"", "Variable", "Stability"}, {GAME_SAVED, "Unlocked to 60 fps", "Locked 30 fps"},
	 "Unlocked runs as fast as your PC allows, up to 60 fps. Locked 30 fps holds a steady 30, which can feel smoother on a slow PC. Needs a restart."},
	{"Confirm button", "XRC_CFG_Control_ConfirmButtonLayout", 0, {"", "Normal", "Opposite"}, {GAME_SAVED, "Normal: A confirms", "Opposite: B confirms"},
	 "Which pad button confirms in the game's menus. Normal: A confirms and B cancels. Opposite: B confirms and A cancels, as in the Japanese release. Needs a restart."},
	{"Anti-aliasing", "XRC_FXAA", 1, {"0", "2", "4", "8", "16"}, {"Off", "FXAA low", "FXAA medium", "FXAA high", "SMAA"},
	 "Smooths jagged edges in the 3D scene; the HUD and menus stay sharp. FXAA costs almost nothing but softens the picture a little; SMAA keeps more detail and costs a bit more. Applies at once."},
	{"Display mode", "XRC_WINDOW", 1, {"", "windowed"}, {"Borderless fullscreen", "Windowed"},
	 "Borderless fullscreen covers the whole monitor without changing its resolution; Windowed shows the game in a movable window. F11 or Alt+Enter also switches. Applies at once, no fps cost."},
	{"HD cinematics", "XRC_MOVIE_HD", 1, {"1", "0"}, {"On", "Off (original 720p)"},
	 "Plays the upscaled cutscene movies made by the launcher instead of the original 720p ones (no effect if they were not made). Applies from the next movie."},
	{"HD textures", "XRC_TEX_HD", 1, {"1", "0"}, {"On", "Off (original textures)"},
	 "Draws the upscaled texture pack prepared in the launcher instead of the original textures. Uses more video memory. Applies at once; F8 also switches texture sets."},
	{"Texture sharpness", "XRC_ANISO", 1, {"", "8", "4", "0"}, {"16x (sharpest)", "8x", "4x", "Game's own filtering"},
	 "Anisotropic filtering keeps floors and walls seen at an angle sharp instead of blurry. 16x is the sharpest and costs almost nothing on current graphics cards. Applies at once."},
	{"HD UI and fonts", "XRC_TEX_UI", 1, {"", "1"}, {"Off (original)", "On"},
	 "Also uses the HD pack's versions of menus, fonts and logos. Needs HD textures on. Applies at once."},
	/* (vk_post.c's passes: xrc_vk_set_cas / set_downscale / set_deband) */
	{"Sharpening", "XRC_CAS", 1, {"", "0.25", "0.5", "0.75"}, {"Off", "Low 25% (recommended)", "Medium 50%", "High 75%"},
	 "Contrast-adaptive sharpening (the AMD FidelityFX CAS method) on the final picture: soft detail gets crisper, edges that are already sharp are left alone. Low suits most screens. Costs a little GPU time. Applies at once."},
	{"Supersampling filter", "XRC_DOWNSCALE", 1, {"0", "1"}, {"Standard (bilinear)", "High quality (Lanczos)"},
	 "Used when the render resolution is higher than your screen. High quality counts every rendered pixel, so fine detail stays steady; Standard skips pixels and can shimmer. Applies at once."},
	{"Debanding", "XRC_DEBAND", 1, {"", "1", "2"}, {"Off", "Dither only", "Deband and dither"},
	 "Hides color banding in smooth gradients such as skies and fog. Dither adds faint noise to break up the steps; Deband also smooths them first. Small GPU cost. Applies at once."},
	{"Game thread priority", "XRC_MAIN_PRIO", 0, {"", "0"}, {"High (default)", "Normal"},
	 "High keeps gameplay smooth when other programs are busy; Normal leaves more CPU for them. Needs a restart."},
	{"Free camera", "XRC_FREECAM", 1, {"", "1"}, {"Off", "On (F6 toggles)"},
	 "Turns on the debug free camera: F6 then detaches the camera from the player, and the first pad's sticks fly it. Off puts the camera back. Applies at once."},
	{"Shadow stability", "XRC_SHADOW_HOLD", 1, {"0", "900"}, {"Off", "On"},
	 "Stops shadows from blinking at the edge of the screen: an object seen in the last 900 frames keeps casting its shadow. Costs about 1 to 3 percent fps. Applies at once."},
};
#define NOPT ((int)(sizeof opts / sizeof opts[0]))
#define ID_BASE 0x7000
#define ID_RESTART 0x7f00
#define ID_QUIT 0x7f01

static const char *def_val(const Opt *o) { return !strcmp(o->key, "XRC_MOVIE_HD") || !strcmp(o->key, "XRC_TEX_HD") ? "1" : !strcmp(o->key, "XRC_FXAA") ? "16" : !strcmp(o->key, "XRC_DOWNSCALE") ? "1" : !strcmp(o->key, "XRC_SHADOW_HOLD") ? "900" : ""; } /* (integ5: Lanczos and shadow stability default on; their Off is saved as 0) */ /* (integ3: SMAA is the default; Off is saved as 0) */
static const char *cur(const Opt *o) {
	const char *v = getenv(o->key);
	if (!v) v = def_val(o);
	return v;
}

/* the game's own Configuration.ini values as read at boot (hle_steam.c cfg_patched, before lr.cfg's overrides): what
 * "the game's saved setting" is, shown by name */
static char *saved_val[NOPT];
static SRWLOCK saved_l = SRWLOCK_INIT;
void xrc_menu_cfg_seen(const char *key, const char *v, int n) {
	char env[112];
	int i;
	snprintf(env, sizeof env, "XRC_CFG_%s", key);
	for (i = 0; i < NOPT; i++)
		if (!strcmp(opts[i].key, env)) {
			char *s = (char *)malloc((size_t)(n < 0 ? 0 : n) + 1);
			if (!s) return;
			memcpy(s, v, (size_t)(n < 0 ? 0 : n)); s[n < 0 ? 0 : n] = 0;
			AcquireSRWLockExclusive(&saved_l);
			free(saved_val[i]); saved_val[i] = s;
			ReleaseSRWLockExclusive(&saved_l);
			return;
		}
}
/* value k's name; for vals[0] "" of a game option, the saved value: "High quality (saved)" */
static const char *val_name(int oi, int k, char *buf, size_t cap) {
	const Opt *o = &opts[oi];
	int j;
	if (k != 0 || o->live || *o->vals[0]) return o->names[k];
	AcquireSRWLockShared(&saved_l);
	if (!saved_val[oi]) snprintf(buf, cap, "%s", o->names[0]);
	else {
		const char *nm = 0;
		for (j = 1; j < 13 && o->names[j]; j++) if (!_stricmp(o->vals[j], saved_val[oi])) nm = o->names[j];
		snprintf(buf, cap, "%s (saved)", nm ? nm : saved_val[oi]);
	}
	ReleaseSRWLockShared(&saved_l);
	return buf;
}

/* the restart-only options as the game read them at boot: a value different now waits for a restart */
static char *boot_val[NOPT];
static void snap(void) {
	static int done;
	int i;
	if (done) return;
	done = 1;
	for (i = 0; i < NOPT; i++) boot_val[i] = _strdup(cur(&opts[i]));
}
static int changed(int i) { snap(); return !opts[i].live && boot_val[i] && strcmp(boot_val[i], cur(&opts[i])) != 0; }
static int npending(void) { int i, n = 0; for (i = 0; i < NOPT; i++) n += changed(i); return n; }

/* lr.cfg: the program's name with .cfg (xrc_rt.c load_cfg) */
static int cfg_path(char *out, size_t cap) {
	DWORD n = GetModuleFileNameA(0, out, (DWORD)cap - 8);
	char *dot, *sl;
	if (!n || n >= cap - 8) return 0;
	out[n] = 0;
	dot = strrchr(out, '.');
	sl = strrchr(out, '\\');
	if (dot && (!sl || dot > sl)) strcpy(dot, ".cfg"); else strcat(out, ".cfg");
	return 1;
}
/* key=value in lr.cfg (replaced, or added; "" removes it), written whole to a temporary file and moved over */
static int cfg_set(const char *key, const char *val) {
	char path[MAX_PATH], tmp[MAX_PATH + 8], line[4096];
	size_t kl = strlen(key);
	FILE *in, *out;
	int done = 0;
	if (!cfg_path(path, sizeof path)) return 0;
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (!(out = fopen(tmp, "w"))) return 0;
	if ((in = fopen(path, "r"))) {
		while (fgets(line, sizeof line, in)) {
			if (!strncmp(line, key, kl) && line[kl] == '=') {
				if (!done && *val) fprintf(out, "%s=%s\n", key, val);
				done = 1;
				continue;
			}
			fputs(line, out);
			if (line[0] && line[strlen(line) - 1] != '\n') fputc('\n', out);
		}
		fclose(in);
	}
	if (!done && *val) fprintf(out, "%s=%s\n", key, val);
	if (fclose(out) || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return 0; }
	return 1;
}

static void apply(const Opt *o, const char *v) {
	snap();
	if (!strcmp(cur(o), v)) return;
	_putenv_s(o->key, v); /* ("" unsets it) */
	if (!cfg_set(o->key, v)) fprintf(stderr, "xrc: settings: could not write lr.cfg\n");
	fprintf(stderr, "xrc: settings: %s = %s%s\n", o->key, *v ? v : o->live ? "(default)" : "(the game's own)", o->live ? "" : " (on the next start)");
	if (!strcmp(o->key, "XRC_FXAA")) xrc_vk_set_fxaa(atoi(v));
	if (!strcmp(o->key, "XRC_TEX_HD")) xrc_vk_set_tex_hd(atoi(v));
	if (!strcmp(o->key, "XRC_TEX_UI")) xrc_vk_set_tex_ui(atoi(v));
	if (!strcmp(o->key, "XRC_ANISO")) xrc_vk_set_aniso(*v ? atoi(v) : 16);
	if (!strcmp(o->key, "XRC_WINDOW") && xrc_vk_set_borderless) xrc_vk_set_borderless(strcmp(v, "windowed") != 0);
	if (!strcmp(o->key, "XRC_CAS")) xrc_vk_set_cas(*v ? (float)atof(v) : 0.0f);
	if (!strcmp(o->key, "XRC_DOWNSCALE")) xrc_vk_set_downscale(*v ? atoi(v) : 1);
	if (!strcmp(o->key, "XRC_FREECAM")) xrc_freecam_enable(*v ? atoi(v) : 0);
	if (!strcmp(o->key, "XRC_DEBAND")) xrc_vk_set_deband(atoi(v));
	if (!strcmp(o->key, "XRC_SHADOW_HOLD")) xrc_vk_set_shadow_hold(atoi(v));
}

/* a pick, from the menu or a test (XRC_SETTINGS_PICK) */
static void pick(int id) {
	if (id == ID_RESTART) {
		fprintf(stderr, "xrc: settings: restarting the game\n");
		xrc_relaunch_at_exit();
		xrc_exit_now(0);
	}
	if (id == ID_QUIT) xrc_exit_now(0);
	if (id >= ID_BASE && id < ID_BASE + NOPT * 16) {
		const Opt *o = &opts[(id - ID_BASE) / 16];
		int k = (id - ID_BASE) % 16;
		if (k < 13 && o->names[k]) apply(o, o->vals[k]);
	}
}

/* ============================================================================================ the old Win32 popup
 * (Shift+F10): closes after each choice */
static HMENU sub_of(const Opt *o, int oi) {
	HMENU m = CreatePopupMenu();
	const char *c = cur(o);
	int k;
	for (k = 0; k < 13 && o->names[k]; k++)
		AppendMenuA(m, MF_STRING | (!strcmp(c, o->vals[k]) ? MF_CHECKED : 0), (UINT_PTR)(ID_BASE + oi * 16 + k), o->names[k]);
	return m;
}
static void add_sub(HMENU m, const Opt *o, int oi) {
	char t[160];
	const char *c = cur(o);
	int k;
	const char *now = "";
	for (k = 0; k < 13 && o->names[k]; k++) if (!strcmp(c, o->vals[k])) now = o->names[k];
	snprintf(t, sizeof t, "%s\t%s", o->label, now);
	AppendMenuA(m, MF_POPUP | MF_STRING, (UINT_PTR)sub_of(o, oi), t);
}
void xrc_settings_menu(void *hwnd) {
	HWND h = (HWND)hwnd;
	HMENU m = CreatePopupMenu(), game = CreatePopupMenu();
	POINT pt = {0, 0};
	char t[96];
	int i, id, pending = npending();
	for (i = 0; i < NOPT; i++) if (!opts[i].live) add_sub(game, &opts[i], i);
	AppendMenuA(m, MF_STRING | MF_GRAYED, 0, "Lightning Returns (recomp) settings");
	AppendMenuA(m, MF_SEPARATOR, 0, 0);
	for (i = 0; i < NOPT; i++) if (opts[i].live) add_sub(m, &opts[i], i);
	AppendMenuA(m, MF_POPUP | MF_STRING, (UINT_PTR)game, "Game graphics and controls (apply on restart)");
	AppendMenuA(m, MF_SEPARATOR, 0, 0);
	if (pending) { snprintf(t, sizeof t, "Restart now to apply %d change%s (unsaved progress is lost)", pending, pending == 1 ? "" : "s"); }
	else snprintf(t, sizeof t, "Restart the game (unsaved progress is lost)");
	AppendMenuA(m, MF_STRING, ID_RESTART, t);
	AppendMenuA(m, MF_STRING, ID_QUIT, "Quit");
	xrc_input_release_all();
	ClientToScreen(h, &pt);
	SetForegroundWindow(h);
	id = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x + 8, pt.y + 8, 0, h, 0);
	DestroyMenu(m); /* (and its submenus) */
	if (id > 0) pick(id);
}

/* ============================================================================================ the overlay: input */
static volatile int ov_open;   /* (set by the window's thread on F10; cleared by the present thread) */
static volatile int pad_hold;  /* closed with the pad: the game's pad stays at rest until every button is let go */
typedef struct { int type, a, b, c, d, e; } Ev; /* type 1 key (vk, repeat); 2 mouse (kind, x, y, client w, h; e wheel) */
static Ev evq[64];
static int evn;
static SRWLOCK evl = SRWLOCK_INIT;
static void ev_push(Ev e) {
	AcquireSRWLockExclusive(&evl);
	if (evn < (int)(sizeof evq / sizeof evq[0])) evq[evn++] = e;
	ReleaseSRWLockExclusive(&evl);
}
int xrc_menu_is_open(void) { return ov_open; }
/* the game's Settings menu "PC Graphics" row (ovr_config_menu.c): opens this overlay. Called on the guest main thread with
 * the GIL held: only sets the flag, as F10 does */
void xrc_settings_open(void) {
	if (ov_open) return;
	fprintf(stderr, "xrc: settings: the menu opened (PC Graphics row)\n");
	xrc_input_release_all();
	ov_open = 1;
}
/* a key in the game's window: 1 when it is the menu's (F10, or any key while the menu is open) */
int xrc_menu_key(unsigned vk, int down, int repeat) {
	Ev e;
	if (!ov_open) {
		if (vk != VK_F10) return 0;
		if (down && !repeat) {
			fprintf(stderr, "xrc: settings: the menu opened\n");
			xrc_input_release_all(); /* (keys held for the game: let go) */
			ov_open = 1;
		}
		return 1;
	}
	if (!down) return 1;
	if (repeat && (vk == VK_F10 || vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_SPACE)) return 1;
	memset(&e, 0, sizeof e);
	e.type = 1; e.a = (int)vk; e.b = repeat;
	ev_push(e);
	return 1;
}
void xrc_menu_mouse(int kind, int x, int y, int cw, int ch, int wheel) {
	Ev e;
	if (!ov_open) return;
	e.type = 2; e.a = kind; e.b = x; e.c = y; e.d = cw | ch << 16; e.e = wheel;
	ev_push(e);
}
/* the game's XInput state (hle_lr.c xi_get_state): at rest while the menu is open */
void xrc_menu_pad_filter(unsigned char *st) {
	if (!ov_open && !pad_hold) return;
	if (!ov_open) { /* (closed: the game gets the pad back once nothing is held) */
		short lx = (short)(st[8] | st[9] << 8), ly = (short)(st[10] | st[11] << 8);
		if (!(st[4] | st[5]) && st[6] < 30 && st[7] < 30 && abs(lx) < 8000 && abs(ly) < 8000) { pad_hold = 0; return; }
	}
	memset(st + 4, 0, 12);
}

/* the host's pad, for the menu (any of the four) */
static DWORD (WINAPI *xi_get)(DWORD, void *);
static unsigned pad_read(void) {
	static int tried;
	unsigned char st[16];
	unsigned b = 0;
	DWORD k;
	if (!tried) {
		const char *dll[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
		HMODULE h = 0;
		int i;
		tried = 1;
		if ((!getenv("XRC_VK_HEADLESS") || getenv("XRC_HOST_PAD")) && !getenv("XRC_NO_PAD"))
			for (i = 0; i < 3 && !h; i++) h = LoadLibraryA(dll[i]);
		if (h) *(FARPROC *)&xi_get = GetProcAddress(h, "XInputGetState");
	}
	if (!xi_get) return 0;
	for (k = 0; k < 4; k++) {
		short lx, ly;
		if (xi_get(k, st) != 0) continue;
		b |= (unsigned)(st[4] | st[5] << 8);
		lx = (short)(st[8] | st[9] << 8); ly = (short)(st[10] | st[11] << 8);
		if (ly > 16000) b |= 1; if (ly < -16000) b |= 2; if (lx < -16000) b |= 4; if (lx > 16000) b |= 8; /* (the stick as the D-pad) */
	}
	return b;
}

/* ============================================================================================ the overlay: drawing */
static stbtt_fontinfo face_fi[2];
static int face_ok[2];
typedef struct { unsigned char *bmp; int w, h, xo, yo, done; } Gl;
typedef struct { int face; float px, sc, asc, lh; Gl g[95][4]; } Sz; /* (4 sub-pixel positions) */
enum { F_BODY, F_BODYB, F_TITLE, F_SECT, F_SMALL, F_DESC, F_HINT, NSZ };
static Sz sz[NSZ];
static float sz_u;
static void fonts_load(void) {
	static int tried;
	const char *name[2][2] = {{"segoeui.ttf", "arial.ttf"}, {"seguisb.ttf", "arialbd.ttf"}};
	char dir[MAX_PATH], path[MAX_PATH + 32];
	int f, k;
	if (tried) return;
	tried = 1;
	if (!GetWindowsDirectoryA(dir, sizeof dir)) strcpy(dir, "C:\\Windows");
	for (f = 0; f < 2; f++)
		for (k = 0; k < 2 && !face_ok[f]; k++) {
			FILE *fp;
			long n;
			unsigned char *data;
			snprintf(path, sizeof path, "%s\\Fonts\\%s", dir, name[f][k]);
			if (!(fp = fopen(path, "rb"))) continue;
			fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
			data = n > 0 ? (unsigned char *)malloc((size_t)n) : 0;
			if (data && fread(data, 1, (size_t)n, fp) == (size_t)n && stbtt_InitFont(&face_fi[f], data, stbtt_GetFontOffsetForIndex(data, 0))) face_ok[f] = 1;
			else free(data);
			fclose(fp);
		}
	if (!face_ok[1] && face_ok[0]) { face_fi[1] = face_fi[0]; face_ok[1] = 1; }
	fprintf(stderr, "xrc: settings: menu font %s\n", face_ok[0] ? "loaded" : "missing (no text)");
}
static void sz_set(Sz *s, int face, float px) {
	int a, d, g, i, k;
	for (i = 0; i < 95; i++) for (k = 0; k < 4; k++) if (s->g[i][k].bmp) stbtt_FreeBitmap(s->g[i][k].bmp, 0);
	memset(s, 0, sizeof *s);
	s->face = face; s->px = px;
	if (!face_ok[face]) return;
	s->sc = stbtt_ScaleForMappingEmToPixels(&face_fi[face], px);
	stbtt_GetFontVMetrics(&face_fi[face], &a, &d, &g);
	s->asc = a * s->sc; s->lh = (a - d + g) * s->sc;
}
static void fonts_for(float u) {
	if (u == sz_u) return;
	sz_u = u;
	fonts_load();
	sz_set(&sz[F_BODY], 0, 19 * u);
	sz_set(&sz[F_BODYB], 1, 19 * u);
	sz_set(&sz[F_TITLE], 1, 30 * u);
	sz_set(&sz[F_SECT], 1, 14.5f * u);
	sz_set(&sz[F_SMALL], 1, (12.5f * u < 9 ? 9 : 12.5f * u));
	sz_set(&sz[F_DESC], 0, 17 * u);
	sz_set(&sz[F_HINT], 0, 15 * u);
}
static Gl *glyph(Sz *s, int c, int sub) {
	Gl *g;
	if (c < 32 || c > 126) c = '?';
	g = &s->g[c - 32][sub];
	if (!g->done) {
		g->done = 1;
		g->bmp = stbtt_GetCodepointBitmapSubpixel(&face_fi[s->face], s->sc, s->sc, sub * 0.25f, 0, c, &g->w, &g->h, &g->xo, &g->yo);
	}
	return g;
}
static float adv(Sz *s, int c, int next) {
	int a, l;
	float v;
	if (!s->sc) return 0;
	stbtt_GetCodepointHMetrics(&face_fi[s->face], c, &a, &l);
	v = a * s->sc;
	if (next) v += stbtt_GetCodepointKernAdvance(&face_fi[s->face], c, next) * s->sc;
	return v;
}
static float text_w(Sz *s, const char *t) {
	float w = 0;
	for (; *t; t++) w += adv(s, (unsigned char)*t, (unsigned char)t[1]);
	return w;
}

/* the canvas: the panel, premultiplied BGRA (B in the low byte), and two rows of the dimming colour below it */
static unsigned *cv;
static int cvw, cvcap, clip0, clip1; /* (clip0..clip1: the rows drawn into) */
static unsigned C(int r, int g, int b, int a) { /* (straight alpha in, premultiplied out) */
	return (unsigned)(b * a / 255) | (unsigned)(g * a / 255) << 8 | (unsigned)(r * a / 255) << 16 | (unsigned)a << 24;
}
static void over(unsigned *d, unsigned s, int cov) {
	unsigned sb = s & 255, sg = s >> 8 & 255, sr = s >> 16 & 255, sa = s >> 24, ia, x = *d;
	if (cov <= 0) return;
	if (cov < 255) { sb = sb * cov / 255; sg = sg * cov / 255; sr = sr * cov / 255; sa = sa * cov / 255; }
	ia = 255 - sa;
	*d = (sb + ((x & 255) * ia + 127) / 255) | (sg + ((x >> 8 & 255) * ia + 127) / 255) << 8 |
	     (sr + ((x >> 16 & 255) * ia + 127) / 255) << 16 | (sa + ((x >> 24) * ia + 127) / 255) << 24;
}
static void rrect(float x, float y, float w, float h, float r, unsigned col) {
	int x0 = (int)floorf(x), x1 = (int)ceilf(x + w), y0 = (int)floorf(y), y1 = (int)ceilf(y + h), px, py;
	float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - r, hy = h / 2 - r;
	if (x0 < 0) x0 = 0;
	if (x1 > cvw) x1 = cvw;
	if (y0 < clip0) y0 = clip0;
	if (y1 > clip1) y1 = clip1;
	for (py = y0; py < y1; py++) {
		float qy = fabsf(py + 0.5f - cy) - hy;
		unsigned *row = cv + (size_t)py * cvw;
		for (px = x0; px < x1; px++) {
			float qx = fabsf(px + 0.5f - cx) - hx, mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0, d, c;
			d = sqrtf(mx * mx + my * my) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
			c = 0.5f - d;
			if (c <= 0) continue;
			over(&row[px], col, c >= 1 ? 255 : (int)(c * 255));
		}
	}
}
/* a chevron, '<' (dir -1) or '>' (dir 1), centred at (cx, cy), half height s, line width t */
static void chevron(float cx, float cy, float s, float t, int dir, unsigned col) {
	float ax = cx + dir * -s / 2, bx = cx + dir * s / 2;
	int x0 = (int)(cx - s - t), x1 = (int)(cx + s + t) + 1, y0 = (int)(cy - s - t), y1 = (int)(cy + s + t) + 1, px, py, k;
	if (x0 < 0) x0 = 0;
	if (x1 > cvw) x1 = cvw;
	if (y0 < clip0) y0 = clip0;
	if (y1 > clip1) y1 = clip1;
	for (py = y0; py < y1; py++)
		for (px = x0; px < x1; px++) {
			float x = px + 0.5f, y = py + 0.5f, best = 1e9f, c;
			for (k = 0; k < 2; k++) { /* (the two strokes: tip (bx, cy) to (ax, cy -/+ s)) */
				float ex = ax - bx, ey = (k ? s : -s), wx = x - bx, wy = y - cy, h = (wx * ex + wy * ey) / (ex * ex + ey * ey), dx, dy, d;
				if (h < 0) h = 0;
				if (h > 1) h = 1;
				dx = wx - ex * h; dy = wy - ey * h;
				d = sqrtf(dx * dx + dy * dy);
				if (d < best) best = d;
			}
			c = t / 2 + 0.5f - best;
			if (c > 0) over(&cv[(size_t)py * cvw + px], col, c >= 1 ? 255 : (int)(c * 255));
		}
}
/* text at x (left), its baseline at y; returns the width */
static float text(Sz *s, float x, float y, unsigned col, const char *t) {
	float x0 = x;
	int by = (int)floorf(y + 0.5f);
	if (!s->sc) return 0;
	for (; *t; t++) {
		int c = (unsigned char)*t, ix = (int)floorf(x), sub = (int)((x - ix) * 4) & 3, i, j;
		Gl *g = glyph(s, c, sub);
		if (g->bmp)
			for (j = 0; j < g->h; j++) {
				int py = by + g->yo + j;
				if (py < clip0 || py >= clip1) continue;
				for (i = 0; i < g->w; i++) {
					int px = ix + g->xo + i;
					if (px < 0 || px >= cvw) continue;
					over(&cv[(size_t)py * cvw + px], col, g->bmp[j * g->w + i]);
				}
			}
		x += adv(s, c, (unsigned char)t[1]);
	}
	return x - x0;
}
/* text cut to max width with "..." */
static void text_fit(Sz *s, float x, float y, unsigned col, const char *t, float maxw, int centre) {
	char b[256];
	size_t n = strlen(t);
	float w;
	if (n >= sizeof b - 4) n = sizeof b - 4;
	memcpy(b, t, n); b[n] = 0;
	while ((w = text_w(s, b)) > maxw && n > 0) { n--; while (n > 0 && b[n - 1] == ' ') n--; strcpy(b + n, "..."); }
	text(s, centre ? x + (maxw - w) / 2 : x, y, col, b);
}
/* wrapped text (at spaces), at most `lines` lines */
static void text_wrap(Sz *s, float x, float y, float maxw, int lines, unsigned col, const char *t) {
	char b[512];
	while (*t && lines-- > 0) {
		size_t n = 0, cut = 0;
		while (t[n] && n < sizeof b - 1) {
			b[n] = t[n]; b[n + 1] = 0;
			if (text_w(s, b) > maxw) break;
			if (t[n] == ' ') cut = n;
			n++;
		}
		if (!t[n]) cut = n;
		else if (!cut) cut = n > 0 ? n : 1;
		memcpy(b, t, cut); b[cut] = 0;
		text(s, x, y, col, b);
		t += cut;
		while (*t == ' ') t++;
		y += s->lh;
	}
}

/* ============================================================================================ the overlay: the menu */
enum { IT_HDR, IT_OPT, IT_RESTART, IT_QUIT, IT_CLOSE, IT_RESET };
typedef struct { int kind, opt; const char *text, *note; } Item;
static const struct { const char *title, *note; const char *keys[9]; } sections[] = {
	{"DISPLAY", 0, {"XRC_WINDOW", "XRC_CFG_Graphics_Resolution", "XRC_CFG_Graphics_Scaling", "XRC_CFG_Graphics_FrameRate"}},
	{"GRAPHICS", "apply on restart", {"XRC_CFG_Graphics_ColorCorrection", "XRC_CFG_Graphics_Glare", "XRC_CFG_Graphics_DepthOfField",
	                                  "XRC_CFG_Graphics_Shadowing", "XRC_CFG_Graphics_Lighting", "XRC_CFG_Graphics_TextureFiltering"}},
	{"ENHANCEMENTS", "live", {"XRC_FXAA", "XRC_CAS", "XRC_DOWNSCALE", "XRC_DEBAND", "XRC_ANISO", "XRC_TEX_HD", "XRC_TEX_UI", "XRC_MOVIE_HD", "XRC_SHADOW_HOLD"}},
	{"ADVANCED", 0, {"XRC_MAIN_PRIO", "XRC_FREECAM"}},
	{"SYSTEM", 0, {"XRC_CFG_Control_ConfirmButtonLayout"}},
};
static Item items[48];
static int nitems;
static int sel = -1, hover = -1, confirm = -1; /* (items; confirm: the Restart / Quit row asked once) */
static int sel_kind = IT_OPT, sel_opt = 11;   /* (the selection kept across rebuilds: the Restart row comes and goes) */
static float scroll, content_h, view_h;
static int want_visible, dirty = 1;
/* the last drawing's geometry, for the mouse (canvas coordinates) */
static int pan_x, pan_y, pan_w, pan_h, list_y0, list_y1, vbox_x0, vbox_x1, chev_w;
static int it_y0[48], it_y1[48];
static int last_sw, last_sh;

static int opt_index(const char *key) { int i; for (i = 0; i < NOPT; i++) if (!strcmp(opts[i].key, key)) return i; return -1; }
static void build(void) {
	int s, k, n = 0, p = npending();
	for (s = 0; s < (int)(sizeof sections / sizeof sections[0]); s++) {
		items[n].kind = IT_HDR; items[n].opt = -1; items[n].text = sections[s].title; items[n].note = sections[s].note; n++;
		for (k = 0; k < 9 && sections[s].keys[k]; k++) {
			int i = opt_index(sections[s].keys[k]);
			if (i < 0) continue;
			items[n].kind = IT_OPT; items[n].opt = i; items[n].text = opts[i].label; items[n].note = 0; n++;
		}
	}
	items[n].kind = IT_RESET; items[n].opt = -1; items[n].text = "Reset to defaults"; items[n].note = 0; n++;
	if (p) { items[n].kind = IT_RESTART; items[n].opt = -1; items[n].text = 0; items[n].note = 0; n++; }
	items[n].kind = IT_QUIT; items[n].opt = -1; items[n].text = "Quit game"; items[n].note = 0; n++;
	items[n].kind = IT_CLOSE; items[n].opt = -1; items[n].text = "Close menu"; items[n].note = 0; n++;
	nitems = n;
	sel = -1;
	for (k = 0; k < n; k++) if (items[k].kind == sel_kind && items[k].opt == sel_opt) sel = k;
	if (sel < 0) for (k = 0; k < n; k++) if (items[k].kind != IT_HDR) { sel = k; break; }
	if (hover >= n) hover = -1;
}
static void select_item(int k) {
	if (k < 0 || k >= nitems || items[k].kind == IT_HDR) return;
	if (k != sel) confirm = -1;
	sel = k; sel_kind = items[k].kind; sel_opt = items[k].opt;
	want_visible = 1; dirty = 1;
}
static void move(int d) {
	int k = sel;
	do { k += d; } while (k >= 0 && k < nitems && items[k].kind == IT_HDR);
	if (k < 0 || k >= nitems) { /* (wraps) */
		k = d > 0 ? 0 : nitems - 1;
		while (k >= 0 && k < nitems && items[k].kind == IT_HDR) k += d > 0 ? 1 : -1;
	}
	select_item(k);
}
static int val_index(const Opt *o) {
	const char *c = cur(o);
	int k;
	for (k = 0; k < 13 && o->names[k]; k++) if (!strcmp(c, o->vals[k])) return k;
	return -1;
}
static int val_count(const Opt *o) { int k = 0; while (k < 13 && o->names[k]) k++; return k; }
static void change(int it, int d) {
	const Opt *o;
	int k, n;
	if (it < 0 || it >= nitems || items[it].kind != IT_OPT) return;
	o = &opts[items[it].opt];
	n = val_count(o); k = val_index(o);
	k = k < 0 ? (d > 0 ? 0 : n - 1) : (k + d + n) % n;
	apply(o, o->vals[k]);
	build();
	dirty = 1;
}
static void menu_close(int by_pad) {
	ov_open = 0;
	confirm = -1;
	hover = -1;
	if (by_pad) pad_hold = 1;
	fprintf(stderr, "xrc: settings: the menu closed\n");
}
static void activate(int it, int by_pad) {
	if (it < 0 || it >= nitems) return;
	switch (items[it].kind) {
	case IT_OPT: change(it, 1); break;
	case IT_CLOSE: menu_close(by_pad); break;
	case IT_RESTART: case IT_QUIT: case IT_RESET:
		if (confirm != it) { confirm = it; dirty = 1; break; }
		if (items[it].kind == IT_RESET) { /* every option to its default (the game's: its own saved value) */
			int i;
			fprintf(stderr, "xrc: settings: reset to defaults\n");
			for (i = 0; i < NOPT; i++) apply(&opts[i], def_val(&opts[i]));
			confirm = -1;
			build();
			dirty = 1;
			break;
		}
		pick(items[it].kind == IT_RESTART ? ID_RESTART : ID_QUIT);
		break;
	}
}
static void on_key(int vk, int by_pad) {
	switch (vk) {
	case VK_UP: move(-1); break;
	case VK_DOWN: move(1); break;
	case VK_LEFT: if (sel >= 0 && items[sel].kind == IT_OPT) change(sel, -1); break;
	case VK_RIGHT: if (sel >= 0 && items[sel].kind == IT_OPT) change(sel, 1); break;
	case VK_HOME: select_item(1); break;
	case VK_END: select_item(nitems - 1); break;
	case VK_PRIOR: { int k; for (k = 0; k < 6; k++) move(-1); break; }
	case VK_NEXT: { int k; for (k = 0; k < 6; k++) move(1); break; }
	case VK_RETURN: case VK_SPACE: activate(sel, by_pad); break;
	case VK_ESCAPE: case VK_BACK:
		if (confirm >= 0) { confirm = -1; dirty = 1; break; }
		menu_close(by_pad);
		break;
	case VK_F10: menu_close(by_pad); break;
	}
}
static int hit_item(int x, int y) { /* (canvas coordinates) */
	int k;
	if (y < list_y0 || y >= list_y1 || x < 0 || x >= pan_w) return -1;
	for (k = 0; k < nitems; k++) if (items[k].kind != IT_HDR && y >= it_y0[k] && y < it_y1[k]) return k;
	return -1;
}
static void on_mouse(int kind, int x, int y, int cw, int ch, int wheel) {
	int k;
	if (cw > 0 && ch > 0 && last_sw > 0) { x = (int)((long long)x * last_sw / cw); y = (int)((long long)y * last_sh / ch); }
	x -= pan_x; y -= pan_y;
	k = hit_item(x, y);
	if (kind == 0) { if (k != hover) { hover = k; dirty = 1; } return; }
	if (kind == 3) { scroll -= (float)wheel / 120.0f * (it_y1[1] - it_y0[1]) * 2; dirty = 1; return; }
	if (kind != 1 || k < 0) return;
	if (k != sel) { select_item(k); if (items[k].kind == IT_OPT && x < vbox_x0) return; } /* (a click on a label selects it) */
	if (items[k].kind == IT_OPT) {
		if (x >= vbox_x0 && x < vbox_x0 + chev_w) change(k, -1);
		else if (x >= vbox_x0) change(k, 1);
		return;
	}
	activate(k, 0);
}

static void draw(int sw, int sh) {
	float u = sh / 1080.0f, PAD, TH, ROW, HDR, DH, FH, y, lx, top, panel_r;
	int PW, PH, k, need, n_pend = npending();
	unsigned white = C(242, 244, 248, 255), soft = C(196, 202, 214, 255), dimc = C(140, 148, 162, 255), accent_t = C(126, 176, 255, 255);
	unsigned amber = C(255, 190, 92, 255);
	char buf[160];
	if (u < 0.5f) u = 0.5f;
	fonts_for(u);
	build();
	PAD = 28 * u; TH = 58 * u; ROW = 30 * u; HDR = 31 * u; FH = 40 * u; panel_r = 16 * u;
	DH = 3 * sz[F_DESC].lh + 22 * u;
	PW = (int)(900 * u);
	if (PW > sw - 16) PW = sw - 16;
	content_h = 0;
	for (k = 0; k < nitems; k++) content_h += items[k].kind == IT_HDR ? HDR : ROW;
	{
		float fixed = TH + 6 * u + 12 * u + DH + FH, maxh = sh - 22 * u;
		PH = (int)(fixed + content_h + 0.5f);
		if (PH > (int)maxh) PH = (int)maxh;
		view_h = PH - fixed;
	}
	if (PH < 64 || PW < 64) { pan_w = 0; return; }
	/* the canvas */
	need = PW * (PH + 2);
	if (need > cvcap) { free(cv); cv = (unsigned *)malloc((size_t)need * 4); cvcap = cv ? need : 0; if (!cv) { pan_w = 0; return; } }
	cvw = PW;
	memset(cv, 0, (size_t)PW * PH * 4);
	for (k = 0; k < 2 * PW; k++) cv[(size_t)PH * PW + k] = C(0, 0, 0, 150); /* (the dimming over the whole game) */
	pan_w = PW; pan_h = PH; pan_x = (sw - PW) / 2; pan_y = (sh - PH) / 2;
	clip0 = 0; clip1 = PH;
	/* the panel: a hairline edge, the body */
	rrect(0, 0, (float)PW, (float)PH, panel_r, C(255, 255, 255, 46));
	{ float e = u < 1 ? 1 : u; rrect(e, e, PW - 2 * e, PH - 2 * e, panel_r - e, C(14, 17, 24, 232)); }
	/* the title */
	text(&sz[F_TITLE], PAD, PAD * 0.55f + sz[F_TITLE].asc, white, "Settings");
	{
		const char *t = "LIGHTNING RETURNS: FINAL FANTASY XIII";
		text(&sz[F_SECT], PW - PAD - text_w(&sz[F_SECT], t), PAD * 0.55f + sz[F_TITLE].asc, dimc, t);
	}
	rrect(PAD / 2, TH, PW - PAD, u < 1 ? 1 : u, 0, C(255, 255, 255, 30));
	/* the list (scrolled) */
	list_y0 = (int)(TH + 6 * u); list_y1 = (int)(list_y0 + view_h);
	if (want_visible && sel >= 0) { /* (the selection scrolled into view) */
		float a = 0;
		for (k = 0; k < sel; k++) a += items[k].kind == IT_HDR ? HDR : ROW;
		if (k > 0 && items[k - 1].kind == IT_HDR) a -= HDR; /* (its section's title too) */
		if (a < scroll) scroll = a;
		a = 0;
		for (k = 0; k <= sel; k++) a += items[k].kind == IT_HDR ? HDR : ROW;
		if (a > scroll + view_h) scroll = a - view_h;
	}
	want_visible = 0;
	if (scroll > content_h - view_h) scroll = content_h - view_h;
	if (scroll < 0) scroll = 0;
	clip0 = list_y0; clip1 = list_y1;
	lx = PAD + 14 * u;
	vbox_x1 = (int)(PW - PAD - 6 * u); vbox_x0 = (int)(vbox_x1 - 380 * u); chev_w = (int)(44 * u);
	top = list_y0 - scroll;
	for (k = 0, y = top; k < nitems; k++) {
		const Item *it = &items[k];
		float h = it->kind == IT_HDR ? HDR : ROW, base = y + h / 2 + sz[F_BODY].asc * 0.36f;
		it_y0[k] = (int)y; it_y1[k] = (int)(y + h);
		if (y + h < list_y0 || y > list_y1) { y += h; continue; }
		if (it->kind == IT_HDR) {
			float hb = y + h - 11 * u, w = text(&sz[F_SECT], PAD, hb, accent_t, it->text);
			if (it->note) text(&sz[F_HINT], PAD + w + 10 * u, hb, dimc, it->note);
			rrect(PAD, y + h - 5 * u, PW - 2 * PAD, u < 1 ? 1 : u, 0, C(126, 176, 255, 40));
			y += h;
			continue;
		}
		if (k == sel) {
			rrect(PAD / 2, y + 2 * u, PW - PAD, h - 4 * u, 8 * u, C(70, 120, 220, 92));
			rrect(PAD / 2, y + 7 * u, 4 * u, h - 14 * u, 2 * u, C(126, 176, 255, 255));
		} else if (k == hover) rrect(PAD / 2, y + 2 * u, PW - PAD, h - 4 * u, 8 * u, C(255, 255, 255, 16));
		if (it->kind == IT_OPT) {
			const Opt *o = &opts[it->opt];
			int vi = val_index(o);
			float lw = text(k == sel ? &sz[F_BODYB] : &sz[F_BODY], lx, base, k == sel ? white : soft, it->text);
			char vb[96];
			const char *v = vi >= 0 ? val_name(it->opt, vi, vb, sizeof vb) : *cur(o) ? cur(o) : "(not set)";
			float cx0 = vbox_x0 + chev_w / 2.0f, cx1 = vbox_x1 - chev_w / 2.0f, cy = y + h / 2;
			if (!o->live) { /* the "restart" tag: amber while this change waits for one */
				const char *t = changed(it->opt) ? "RESTART PENDING" : "RESTART";
				float tw = text_w(&sz[F_SMALL], t), tx = lx + lw + 12 * u, th = sz[F_SMALL].px + 8 * u;
				if (tx + tw + 12 * u < vbox_x0 - 8 * u) {
					int on = changed(it->opt);
					rrect(tx, cy - th / 2, tw + 12 * u, th, th / 2, on ? C(255, 190, 92, 46) : C(255, 255, 255, 18));
					text(&sz[F_SMALL], tx + 6 * u, cy + sz[F_SMALL].asc * 0.36f, on ? amber : dimc, t);
				}
			}
			if (k == sel || k == hover) rrect((float)vbox_x0, y + 5 * u, (float)(vbox_x1 - vbox_x0), h - 10 * u, 6 * u, C(255, 255, 255, k == sel ? 20 : 12));
			chevron(cx0, cy, 5.5f * u, 2.2f * u, -1, k == sel ? accent_t : dimc);
			chevron(cx1, cy, 5.5f * u, 2.2f * u, 1, k == sel ? accent_t : dimc);
			text_fit(&sz[F_BODY], (float)(vbox_x0 + chev_w), base, k == sel ? white : soft, v, (float)(vbox_x1 - vbox_x0 - 2 * chev_w), 1);
		} else {
			const char *t = it->text;
			unsigned col = k == sel ? white : soft;
			if (it->kind == IT_RESTART) {
				snprintf(buf, sizeof buf, "Restart game to apply %d change%s", n_pend, n_pend == 1 ? "" : "s");
				t = buf; col = amber;
			}
			if (confirm == k) {
				t = it->kind == IT_RESTART ? "Press Enter / A again to restart" : it->kind == IT_RESET ? "Press Enter / A again to reset everything" : "Press Enter / A again to quit";
				col = amber;
			}
			text(k == sel ? &sz[F_BODYB] : &sz[F_BODY], lx, base, col, t);
			if (it->kind == IT_RESTART || it->kind == IT_QUIT) {
				const char *n = "unsaved progress is lost";
				text(&sz[F_HINT], vbox_x1 - text_w(&sz[F_HINT], n), base, dimc, n);
			} else if (it->kind == IT_RESET) {
				const char *n = "all options on this page";
				text(&sz[F_HINT], vbox_x1 - text_w(&sz[F_HINT], n), base, dimc, n);
			} else {
				const char *n = "F10 / Esc / B";
				text(&sz[F_HINT], vbox_x1 - text_w(&sz[F_HINT], n), base, dimc, n);
			}
		}
		y += h;
	}
	clip0 = 0; clip1 = PH;
	if (content_h > view_h) { /* the scroll bar */
		float tr = view_h - 8 * u, bh = tr * view_h / content_h, by = list_y0 + 4 * u + (tr - bh) * scroll / (content_h - view_h);
		rrect(PW - 11 * u, list_y0 + 4 * u, 4 * u, tr, 2 * u, C(255, 255, 255, 16));
		rrect(PW - 11 * u, by, 4 * u, bh, 2 * u, C(255, 255, 255, 90));
	}
	/* the description of the selected row */
	{
		float dy = list_y1 + 12 * u;
		const char *d = "";
		if (sel >= 0) switch (items[sel].kind) {
		case IT_OPT: d = opts[items[sel].opt].desc; break;
		case IT_RESTART: d = "Closes the game and starts it again, so the settings marked RESTART take effect. Unsaved progress is lost."; break;
		case IT_QUIT: d = "Closes the game. Unsaved progress is lost."; break;
		case IT_RESET: d = "Puts every option back to its default: the enhancements to the runtime's defaults and the game's own options to the values saved in its settings file. Changed RESTART options still need a restart."; break;
		case IT_CLOSE: d = "Back to the game. Every change is already saved (lr.cfg) and kept for the next start."; break;
		}
		rrect(PAD / 2, dy, PW - PAD, DH, 10 * u, C(255, 255, 255, 12));
		text_wrap(&sz[F_DESC], PAD / 2 + 16 * u, dy + 11 * u + sz[F_DESC].asc, PW - PAD - 32 * u, 3, soft, d);
	}
	{ /* the footer */
		const char *t = "Arrows / D-pad: move     Left / Right: change     Enter / A: select     F10 / B: close";
		Sz *s = &sz[F_HINT];
		if (text_w(s, t) > PW - 2 * PAD) t = "Arrows: move   Left/Right: change   Enter/A: select   F10/B: close";
		text(s, (PW - text_w(s, t)) / 2, PH - FH / 2 + s->asc * 0.36f, dimc, t);
	}
}

/* each frame while the menu is open (vk_d3d9.c, the present thread): the panel for a picture sw x sh -- its place and
 * size, *dirty when it changed since the last call; 0 when the menu is closed */
const unsigned *xrc_menu_frame(int sw, int sh, int *x, int *y, int *w, int *h, int *dirtyp) {
	static int was_open;
	static unsigned pad_prev;
	static ULONGLONG pad_next;
	Ev q[64];
	int n, k;
	unsigned pb;
	if (!ov_open) { was_open = 0; return 0; }
	if (!was_open) { /* (just opened: the pad's held buttons do nothing until let go) */
		was_open = 1;
		snap();
		pad_prev = pad_read();
		pad_next = 0;
		want_visible = 1;
		dirty = 1;
	}
	AcquireSRWLockExclusive(&evl);
	n = evn; memcpy(q, evq, (size_t)n * sizeof q[0]); evn = 0;
	ReleaseSRWLockExclusive(&evl);
	for (k = 0; k < n && ov_open; k++) {
		if (q[k].type == 1) on_key(q[k].a, 0);
		else on_mouse(q[k].a, q[k].b, q[k].c, q[k].d & 0xffff, q[k].d >> 16, q[k].e);
	}
	/* the pad: A select, B close / back, the D-pad or the left stick to move (repeating while held) */
	pb = pad_read();
	if (ov_open && pb) {
		unsigned press = pb & ~pad_prev, dirs = pb & 15;
		ULONGLONG now = GetTickCount64();
		if (press & 0x1000) on_key(VK_RETURN, 1);
		else if (press & 0x2000) on_key(VK_ESCAPE, 1);
		else if (dirs && ((press & 15) || now >= pad_next)) {
			pad_next = now + ((press & 15) ? 380 : 110);
			on_key(dirs & 1 ? VK_UP : dirs & 2 ? VK_DOWN : dirs & 4 ? VK_LEFT : VK_RIGHT, 1);
		}
	}
	pad_prev = pb;
	if (!ov_open) { was_open = 0; return 0; }
	if (sw != last_sw || sh != last_sh) { last_sw = sw; last_sh = sh; dirty = 1; }
	*dirtyp = dirty;
	if (dirty) { draw(sw, sh); dirty = 0; }
	if (pan_w <= 0) return 0;
	*x = pan_x; *y = pan_y; *w = pan_w; *h = pan_h;
	return cv;
}

/* XRC_SETTINGS_PICK=secs:id[,secs:id...]: picks at those times, from the present thread (a test without a window).
 * XRC_MENU_KEYS=secs:key[,secs:key...]: keys for the overlay at those times (F10 Up Down Left Right Enter Esc Home End; Mx_y
 * a mouse move, Cx_y a click, Wx_y a wheel notch down, at picture pixel x, y) */
void xrc_settings_poll(double secs) {
	static int init, n, next, nk, nextk;
	static double at[8], atk[48];
	static int ids[8], keys[48], mx[48], my[48];
	static double t0;
	if (!init) t0 = secs; /* (secs: a clock from the machine's start; the times count from the first frame) */
	secs -= t0;
	if (!init) {
		const char *e = getenv("XRC_SETTINGS_PICK");
		init = 1;
		while (e && *e && n < 8) {
			at[n] = atof(e);
			if (!strchr(e, ':')) break;
			ids[n++] = (int)strtol(strchr(e, ':') + 1, 0, 0);
			if (!(e = strchr(e, ','))) break;
			e++;
		}
		e = getenv("XRC_MENU_KEYS");
		while (e && *e && nk < 48) {
			static const struct { const char *n; int vk; } names[] = {{"F10", VK_F10}, {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT},
				{"Right", VK_RIGHT}, {"Enter", VK_RETURN}, {"Esc", VK_ESCAPE}, {"Home", VK_HOME}, {"End", VK_END}};
			const char *c = strchr(e, ':');
			int i;
			if (!c) break;
			atk[nk] = atof(e);
			if ((c[1] == 'M' || c[1] == 'C' || c[1] == 'W') && c[2] >= '0' && c[2] <= '9') { /* Mx_y move, Cx_y click, Wx_y wheel down (picture pixels) */
				mx[nk] = atoi(c + 2); my[nk] = strchr(c, '_') ? atoi(strchr(c, '_') + 1) : 0;
				keys[nk++] = c[1] == 'M' ? -1 : c[1] == 'C' ? -2 : -3;
			} else
				for (i = 0; i < 9; i++) if (!strncmp(c + 1, names[i].n, strlen(names[i].n))) { keys[nk++] = names[i].vk; break; }
			if (!(e = strchr(c, ','))) break;
			e++;
		}
	}
	while (next < n && secs >= at[next]) { fprintf(stderr, "xrc: settings: test pick %#x\n", ids[next]); pick(ids[next++]); }
	while (nextk < nk && secs >= atk[nextk]) {
		int k = nextk++;
		fprintf(stderr, "xrc: settings: test key %d\n", keys[k]);
		if (keys[k] >= 0) xrc_menu_key((unsigned)keys[k], 1, 0);
		else {
			xrc_menu_mouse(0, mx[k], my[k], 0, 0, 0);
			if (keys[k] == -2) xrc_menu_mouse(1, mx[k], my[k], 0, 0, 0);
			if (keys[k] == -3) xrc_menu_mouse(3, mx[k], my[k], 0, 0, -120);
		}
	}
}
#endif
