/* hle_system.c - the small system DLLs for the portable runtime: WINMM (timers), ADVAPI32, SHELL32, IMM32, VERSION,
 * ole32 / OLEAUT32 basics. COM object creation fails with REGDB_E_CLASSNOTREG (reported on stderr) so the game takes
 * its no-device paths until the platform layer provides the objects. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "xrc_rt.h"
#include "guest_types.h"
#ifdef _WIN32
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#define mkdir_(p) mkdir((p), 0777)
#endif

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define S(a) ((char *)(xrc_mem + (u32)(a)))
#define W16(a) ((u16 *)(xrc_mem + (u32)(a)))

/* ============================================================================================ WINMM */
static unsigned long long t0_ms;
static unsigned long long mono_ms(void) {
	static int scaled = -1;
	struct timespec ts;
	if (scaled < 0) scaled = xrc_tool_env("XRC_TIME_SCALE") != 0;
	if (scaled) { /* XRC_TIME_SCALE (hle_kernel32.c): timeGetTime and the timer threads (the game's frame pacer) too */
		extern unsigned long long xrc_qpc(void);
		return xrc_qpc() / 10000ull;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
static void hle_timeGetTime(XCtx *c) {
	if (!t0_ms) t0_ms = mono_ms() - 60000; /* "the system has been up a minute" */
	RET((u32)(mono_ms() - t0_ms), 0);
}
/* the timer resolution: on Windows the real one too -- the host sleeps (Sleep(1) in the timeSetEvent threads below,
 * which drive the game's frame pacer) are otherwise 15.6 ms, not 1 */
#ifdef _WIN32
unsigned __stdcall timeBeginPeriod(unsigned ms);
unsigned __stdcall timeEndPeriod(unsigned ms);
static void hle_timeBeginPeriod(XCtx *c) { RET(timeBeginPeriod(A(0)), 1); }
static void hle_timeEndPeriod(XCtx *c) { RET(timeEndPeriod(A(0)), 1); }
#else
static void hle_timeBeginPeriod(XCtx *c) { RET(0, 1); }
static void hle_timeEndPeriod(XCtx *c) { RET(0, 1); }
#endif
static void hle_timeGetDevCaps(XCtx *c) {
	if (A(1) >= 8) { M32(A(0)) = 1; M32(A(0) + 4) = 1000000; }
	RET(0, 2);
}
/* timeSetEvent(delay, resolution, callback, user, flags): each timer is a guest thread whose start routine is the HLE
 * loop below (the thread's param is the timer's index): it sleeps to the next due time and calls
 * callback(id, 0, user, 0, 0). A late timer does not catch up with missed ticks. */
typedef struct { u32 delay, cb, user, periodic; volatile int killed, used; } MmTimer;
#define NTIMERS 16
static MmTimer timers[NTIMERS];
static void hle_mm_timer_thread(XCtx *c) {
	u32 i = A(0), args[5];
	MmTimer *t = &timers[i];
	unsigned long long next = mono_ms() + t->delay;
	while (!t->killed) {
		unsigned long long now = mono_ms();
		if (now < next) { xrc_sleep_c((u32)(next - now), 2); continue; }
		next = now + (t->delay ? t->delay : 1);
		args[0] = i + 1; args[1] = 0; args[2] = t->user; args[3] = 0; args[4] = 0;
		xrc_call_guest(c, t->cb, 5, args);
		if (!t->periodic) break;
	}
	t->used = 0;
	RET(0, 1);
}
static void hle_timeSetEvent(XCtx *c) {
	u32 i, start;
	if (A(4) & 0x30) xrc_fatal(c, "timeSetEvent with an event handle (flags %x): not implemented yet", A(4));
	for (i = 0; i < NTIMERS; i++) if (!timers[i].used) break;
	if (i == NTIMERS) RET(0, 5);
	timers[i].delay = A(0); timers[i].cb = A(2); timers[i].user = A(3); timers[i].periodic = A(4) & 1;
	/* XRC_MMTIMER_MS: every timer's period instead (benchmarks: the game paces its frames by one) */
	if (getenv("XRC_MMTIMER_MS")) timers[i].delay = (u32)atoi(getenv("XRC_MMTIMER_MS"));
	fprintf(stderr, "xrc: timeSetEvent(%u ms, %s) -> timer %u%s\n", A(0), A(4) & 1 ? "periodic" : "once", i + 1,
	        getenv("XRC_MMTIMER_MS") ? " (XRC_MMTIMER_MS)" : "");
	timers[i].killed = 0; timers[i].used = 1;
#ifdef _WIN32
	{ /* (the real timeSetEvent runs at the resolution asked for: 1 ms -- Sleep's, for the timer thread) */
		static int done;
		if (!done) { done = 1; timeBeginPeriod(A(1) ? A(1) : 1); }
	}
#endif
	start = xrc_import_addr("WINMM.dll", "xrc_mm_timer_thread");
	if (!xrc_thread_create(start, i, 0x40000, 0)) { timers[i].used = 0; RET(0, 5); }
	RET(i + 1, 5);
}
static void hle_timeKillEvent(XCtx *c) {
	u32 i = A(0) - 1;
	if (i >= NTIMERS || !timers[i].used) RET(97 /* MMSYSERR_INVALPARAM */, 1);
	timers[i].killed = 1;
	RET(0, 1);
}

/* ============================================================================================ ADVAPI32 / SHELL32 */
#define USER_NAME "Player"
static void hle_GetUserNameA(XCtx *c) {
	u32 n = (u32)sizeof USER_NAME;
	if (M32(A(1)) < n) { M32(A(1)) = n; GF(Teb, xrc_teb, last_error) = 122; RET(0, 2); }
	memcpy(S(A(0)), USER_NAME, n);
	M32(A(1)) = n;
	RET(1, 2);
}
/* the special folders, as guest paths on C: (created on the host under $XRC_CDRIVE when asked to) */
static const char *folder(u32 csidl) {
	switch (csidl & 0xff) {
	case 0x05: return "C:\\Users\\" USER_NAME "\\Documents";             /* CSIDL_PERSONAL */
	case 0x1a: return "C:\\Users\\" USER_NAME "\\AppData\\Roaming";      /* CSIDL_APPDATA */
	case 0x1c: return "C:\\Users\\" USER_NAME "\\AppData\\Local";        /* CSIDL_LOCAL_APPDATA */
	case 0x23: return "C:\\ProgramData";                                 /* CSIDL_COMMON_APPDATA */
	case 0x2e: return "C:\\Users\\Public\\Documents";                    /* CSIDL_COMMON_DOCUMENTS */
	case 0x28: return "C:\\Users\\" USER_NAME;                           /* CSIDL_PROFILE */
	case 0x24: return "C:\\WINDOWS";                                     /* CSIDL_WINDOWS */
	case 0x25: return "C:\\WINDOWS\\system32";                           /* CSIDL_SYSTEM */
	case 0x26: return "C:\\Program Files";                               /* CSIDL_PROGRAM_FILES */
	}
	return 0;
}
static void make_dirs(const char *guest) {
	const char *root = getenv("XRC_CDRIVE");
	char hp[1024], *s;
	snprintf(hp, sizeof hp, "%s/%s", root && *root ? root : "cdrive", guest + 3);
	for (s = hp; *s; s++) if (*s == '\\') *s = '/';
	for (s = hp + 1; *s; s++)
		if (*s == '/') { *s = 0; mkdir_(hp); *s = '/'; }
	mkdir_(hp);
}
static void hle_SHGetFolderPathA(XCtx *c) {
	const char *p = folder(A(1));
	if (!p) { fprintf(stderr, "xrc: SHGetFolderPathA(CSIDL %#x): unknown folder\n", A(1)); RET(0x80070057u /* E_INVALIDARG */, 5); }
	if (A(1) & 0x8000u) make_dirs(p); /* CSIDL_FLAG_CREATE */
	snprintf(S(A(4)), 260, "%s", p);
	RET(0, 5);
}
static void hle_DragAcceptFiles(XCtx *c) { RET(0, 2); }
static void hle_ImmAssociateContext(XCtx *c) { RET(0, 2); }

/* ============================================================================================ VERSION */
static void hle_GetFileVersionInfoSizeA(XCtx *c) {
	if (A(1)) M32(A(1)) = 0;
	GF(Teb, xrc_teb, last_error) = 1813; /* ERROR_RESOURCE_TYPE_NOT_FOUND */
	RET(0, 2);
}
static void hle_GetFileVersionInfoA(XCtx *c) { GF(Teb, xrc_teb, last_error) = 1813; RET(0, 4); }
static void hle_VerQueryValueA(XCtx *c) { RET(0, 4); }

/* ============================================================================================ ole32 / OLEAUT32 */
static void guid_str(u32 g, char *out, size_t n) {
	snprintf(out, n, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", GF(Guid, g, data1), GF(Guid, g, data2), GF(Guid, g, data3), GFI(Guid, g, data4, 0), GFI(Guid, g, data4, 1),
	         GFI(Guid, g, data4, 2), GFI(Guid, g, data4, 3), GFI(Guid, g, data4, 4), GFI(Guid, g, data4, 5), GFI(Guid, g, data4, 6), GFI(Guid, g, data4, 7));
}
static void hle_CoInitialize(XCtx *c) { RET(0, 1); }
static void hle_CoInitializeEx(XCtx *c) { RET(0, 2); }
static void hle_CoUninitialize(XCtx *c) { RET(0, 0); }
static void hle_CoCreateInstance(XCtx *c) {
	char cls[64], iid[64];
	if (A(4) && xrc_com_create(A(0), A(3), A(4)) == 0) RET(0, 5);
	guid_str(A(0), cls, sizeof cls);
	guid_str(A(3), iid, sizeof iid);
	fprintf(stderr, "xrc: CoCreateInstance(%s, %s): not a class the runtime has -> REGDB_E_CLASSNOTREG\n", cls, iid);
	if (A(4)) M32(A(4)) = 0;
	RET(0x80040154u, 5);
}
static void hle_CoGetClassObject(XCtx *c) {
	char cls[64];
	if (A(4) && xrc_com_class_object(A(0), A(4)) == 0) RET(0, 5);
	guid_str(A(0), cls, sizeof cls);
	fprintf(stderr, "xrc: CoGetClassObject(%s): not a class the runtime has -> REGDB_E_CLASSNOTREG\n", cls);
	if (A(4)) M32(A(4)) = 0;
	RET(0x80040154u, 5);
}
static void hle_CoSetProxyBlanket(XCtx *c) { RET(0, 8); }
/* BSTR: a u32 byte length, then the UTF-16 text and a 0 terminator; the BSTR points at the text */
static u32 bstr_alloc(u32 src, u32 nchars) {
	u32 p = xrc_heap_alloc(4 + nchars * 2 + 2, 0);
	if (!p) return 0;
	GF(Bstr, p, byte_len) = nchars * 2;
	if (src) memcpy(xrc_mem + p + 4, xrc_mem + src, nchars * 2);
	else memset(xrc_mem + p + 4, 0, nchars * 2);
	GFI(Bstr, p, chars, nchars) = 0;
	return p + 4;
}
static void hle_SysAllocString(XCtx *c) { /* #2 */
	u32 n = 0;
	if (!A(0)) RET(0, 1);
	while (W16(A(0))[n]) n++;
	RET(bstr_alloc(A(0), n), 1);
}
static void hle_SysFreeString(XCtx *c) { if (A(0)) xrc_heap_free(A(0) - 4); RET(0, 1); } /* #6 */
static void hle_VariantInit(XCtx *c) { memset(xrc_mem + A(0), 0, 16); RET(0, 1); } /* #8 */
static void hle_VariantClear(XCtx *c) { /* #9 */
	u32 v = A(0);
	if (GF(Variant, v, vt) == 8 /* VT_BSTR */ && GF(Variant, v, bstr_val)) xrc_heap_free(GF(Variant, v, bstr_val) - 4);
	else if (GF(Variant, v, vt) == 9 || GF(Variant, v, vt) == 13) xrc_fatal(c, "VariantClear of an interface (vt %u): no COM objects yet", GF(Variant, v, vt));
	memset(xrc_mem + v, 0, 16);
	RET(0, 1);
}
/* VariantChangeType(dst, src, flags, vt): the numeric types and BOOL to BSTR, and a copy for the same type */
static void hle_VariantChangeType(XCtx *c) { /* #12 */
	u32 d = A(0), v = A(1), to = A(3) & 0xffff, from = GF(Variant, v, vt);
	char t[32];
	u8 tmp[16];
	memcpy(tmp, xrc_mem + v, 16);
	if (from == to && from != 8) { memcpy(xrc_mem + d, tmp, 16); RET(0, 4); }
	if (to != 8) xrc_fatal(c, "VariantChangeType(vt %u -> %u) is not implemented yet", from, to);
	switch (from) {
	case 8: { u32 n = M32(GF(Variant, v, bstr_val) - 4) / 2; u32 b = bstr_alloc(GF(Variant, v, bstr_val), n); memset(xrc_mem + d, 0, 16); GF(Variant, d, vt) = 8; GF(Variant, d, bstr_val) = b; RET(0, 4); }
	case 2: snprintf(t, sizeof t, "%d", GF(Variant, v, s16_val)); break;
	case 3: case 22: snprintf(t, sizeof t, "%d", GF(Variant, v, s32_val)); break;
	case 18: snprintf(t, sizeof t, "%u", GF(Variant, v, u16_val)); break;
	case 19: case 23: snprintf(t, sizeof t, "%u", GF(Variant, v, u32_val)); break;
	case 17: snprintf(t, sizeof t, "%u", GF(Variant, v, u8_val)); break;
	case 20: snprintf(t, sizeof t, "%lld", (long long)GF(Variant, v, u64_val)); break;
	case 21: snprintf(t, sizeof t, "%llu", (unsigned long long)GF(Variant, v, u64_val)); break;
	case 11: snprintf(t, sizeof t, "%s", GF(Variant, v, u16_val) ? "True" : "False"); break;
	default: xrc_fatal(c, "VariantChangeType(vt %u -> %u) is not implemented yet", from, to);
	}
	if (d == v && from == 8) xrc_heap_free(GF(Variant, v, bstr_val) - 4);
	memset(xrc_mem + d, 0, 16);
	GF(Variant, d, vt) = 8;
	GF(Variant, d, bstr_val) = xrc_bstr_from(t);
	RET(0, 4);
}

/* ============================================================================================ sgw.dll
 * The Steam game wrapper: WinMain exits unless LoadLibraryA("sgw.dll") succeeds, and a later init looks up
 * white_steam_init (no arguments) and keeps going only if it returns nonzero. No Steam here: report success. */
/* white_steam_init returns the wrapper's interface (thiscall methods, the callee pops its arguments). What the game
 * uses: [0] the game language (a string the game strncmp's: "english" ...), [1] one argument, [2] a bool gating the
 * Steam features (achievements ...; false: no Steam), [10] per frame (callbacks), [11] at shutdown. Other slots
 * stop the run with their index. */
#define SGW_NVT 16
static u32 sgw_lang;
static void sgw_m0(XCtx *c) { c->eax = sgw_lang; xrc_stdcall_ret(c, 0); }
static void sgw_m1(XCtx *c) { c->eax = 0; xrc_stdcall_ret(c, 1); }
static void sgw_m2(XCtx *c) { c->eax = 0; xrc_stdcall_ret(c, 0); }
static void sgw_m10(XCtx *c) { xrc_stdcall_ret(c, 0); }
static void sgw_m11(XCtx *c) { xrc_stdcall_ret(c, 0); }
#define SGW_UNK(k) static void sgw_m##k(XCtx *c) { xrc_fatal(c, "sgw.dll interface method %d (vtable +%#x) is not known yet", k, 4 * k); }
SGW_UNK(3) SGW_UNK(4) SGW_UNK(5) SGW_UNK(6) SGW_UNK(7) SGW_UNK(8) SGW_UNK(9) SGW_UNK(12) SGW_UNK(13) SGW_UNK(14)
SGW_UNK(15)
/* The game's language: XRC_LANGUAGE, else the one the player chose in Steam for it: "language" in
 * steamapps/appmanifest_292120.acf, two folders above the install (steamapps/common/FINAL FANTASY XIII) -- in its
 * MountedConfig (what Steam installed), else its UserConfig. The install: XRC_GAME_DIR (the launcher sets it), else
 * where cdrive/FINAL FANTASY XIII leads. The names are Steam's, which the game compares with: english, french,
 * german, italian, japanese, spanish, tchinese. Anything else: english. */
static const char *steam_language(void) {
	static char lang[32];
	static const char *const known[] = {"english", "french", "german", "italian", "japanese", "spanish", "tchinese"};
	const char *e = getenv("XRC_LANGUAGE"), *gd = getenv("XRC_GAME_DIR");
	char dir[4096], path[4200], line[512], mounted[32] = "", user[32] = "";
	int section = 0; /* 1: MountedConfig, 2: UserConfig */
	FILE *f;
	unsigned k;
	if (e && *e) { fprintf(stderr, "xrc: language %s (XRC_LANGUAGE)\n", e); return e; }
	if (gd && *gd) snprintf(dir, sizeof dir, "%s", gd);
	else {
		const char *cd = getenv("XRC_CDRIVE");
		snprintf(dir, sizeof dir, "%s/" XRC_GAME_FOLDER, cd && *cd ? cd : "cdrive");
#ifndef _WIN32
		{ char real[4096]; if (realpath(dir, real)) snprintf(dir, sizeof dir, "%s", real); }
#endif
	}
	/* steamapps: two folders up, by name (not "../..": an install that is a link would lead out of its library) */
	for (k = 0; k < 2; k++) {
		size_t n = strlen(dir);
		while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[--n] = 0;
		while (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\') dir[--n] = 0;
		while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[--n] = 0;
	}
	#ifdef XRC_LR
	snprintf(path, sizeof path, "%s/appmanifest_345350.acf", dir);
#else
	snprintf(path, sizeof path, "%s/appmanifest_292120.acf", dir);
#endif
	f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof line, f)) {
			char *q = line, *v;
			while (*q == ' ' || *q == '\t') q++;
			if (!strncmp(q, "\"MountedConfig\"", 15)) section = 1;
			else if (!strncmp(q, "\"UserConfig\"", 12)) section = 2;
			else if (*q == '}') section = 0;
			else if (section && !strncmp(q, "\"language\"", 10) && (v = strchr(q + 10, '"')) != 0) {
				char *end = strchr(v + 1, '"');
				if (end && end - v - 1 < 32) {
					char *dst = section == 1 ? mounted : user;
					memcpy(dst, v + 1, (size_t)(end - v - 1));
					dst[end - v - 1] = 0;
				}
			}
		}
		fclose(f);
	}
	snprintf(lang, sizeof lang, "%s", mounted[0] ? mounted : user[0] ? user : "english");
	for (k = 0; k < sizeof known / sizeof known[0]; k++) if (!strcmp(lang, known[k])) break;
	if (k == sizeof known / sizeof known[0]) {
		fprintf(stderr, "xrc: Steam's language for the game is \"%s\", which the game does not have: english\n", lang);
		snprintf(lang, sizeof lang, "english");
	}
	fprintf(stderr, "xrc: language %s%s\n", lang, f ? " (from Steam)" : " (no Steam manifest found)");
	return lang;
}

static void hle_white_steam_init(XCtx *c) {
	static u32 obj;
	if (!obj) {
		const char *lang = steam_language();
		u32 vt = xrc_heap_alloc(4 * SGW_NVT, 1), k;
		char nm[16];
		sgw_lang = xrc_heap_alloc((u32)strlen(lang) + 1, 0);
		memcpy(S(sgw_lang), lang, strlen(lang) + 1);
		for (k = 0; k < SGW_NVT; k++) {
			snprintf(nm, sizeof nm, "ISgw::m%u", k);
			M32(vt + 4 * k) = xrc_import_addr("sgw.dll", nm);
		}
		obj = xrc_heap_alloc(16, 1);
		M32(obj) = vt;
	}
	c->eax = obj;
	xrc_cdecl_ret(c);
}

/* ============================================================================================ USER32 (dialogs)
 * Message boxes are printed on stderr and answered with their first button: OK, Abort, Yes, Retry or Cancel by the
 * box's type (MB_*: uType & 0xf) -- the game's "Are you sure you wish to exit the game?" (MB_YESNO, after the window's
 * close button) is answered Yes, so closing the window ends the game. */
static u32 first_button(u32 type) {
	switch (type & 0xfu) {
	case 0: case 1: return 1;  /* MB_OK, MB_OKCANCEL: IDOK */
	case 2: return 3;          /* MB_ABORTRETRYIGNORE: IDABORT */
	case 3: case 4: return 6;  /* MB_YESNOCANCEL, MB_YESNO: IDYES */
	case 5: return 4;          /* MB_RETRYCANCEL: IDRETRY */
	case 6: return 2;          /* MB_CANCELTRYCONTINUE: IDCANCEL */
	}
	return 1;
}
static void print_w(u32 p) {
	u32 k;
	if (!p) return;
	for (k = 0; W16(p)[k]; k++) {
		u32 ch = W16(p)[k];
		if (ch < 0x80) fputc((int)ch, stderr);
		else if (ch < 0x800) { fputc(0xc0 | (ch >> 6), stderr); fputc(0x80 | (ch & 0x3f), stderr); }
		else { fputc(0xe0 | (ch >> 12), stderr); fputc(0x80 | ((ch >> 6) & 0x3f), stderr); fputc(0x80 | (ch & 0x3f), stderr); }
	}
}
static void hle_MessageBoxW(XCtx *c) {
	fprintf(stderr, "xrc: MessageBoxW [");
	print_w(A(2));
	fprintf(stderr, "] ");
	print_w(A(1));
	fprintf(stderr, " -> %u\n", first_button(A(3)));
	RET(first_button(A(3)), 4);
}
static void hle_MessageBoxA(XCtx *c) {
	fprintf(stderr, "xrc: MessageBoxA [%s] %s -> %u\n", A(2) ? S(A(2)) : "", A(1) ? S(A(1)) : "", first_button(A(3)));
	RET(first_button(A(3)), 4);
}

/* ============================================================================================ the table */
#define E(d, n) {d, #n, hle_##n}
#define O(d, ord, n) {d, "#" #ord, hle_##n}
const XrcImport xrc_hle_system[] = {
	E("WINMM.dll", timeGetTime), E("WINMM.dll", timeBeginPeriod), E("WINMM.dll", timeEndPeriod),
	E("WINMM.dll", timeGetDevCaps), E("WINMM.dll", timeSetEvent), E("WINMM.dll", timeKillEvent),
	{"WINMM.dll", "xrc_mm_timer_thread", hle_mm_timer_thread},
	E("ADVAPI32.dll", GetUserNameA), E("SHELL32.dll", SHGetFolderPathA), E("SHELL32.dll", DragAcceptFiles),
	E("IMM32.dll", ImmAssociateContext), E("sgw.dll", white_steam_init),
	E("USER32.dll", MessageBoxW), E("USER32.dll", MessageBoxA),
	{"sgw.dll", "ISgw::m0", sgw_m0}, {"sgw.dll", "ISgw::m1", sgw_m1}, {"sgw.dll", "ISgw::m2", sgw_m2},
	{"sgw.dll", "ISgw::m3", sgw_m3}, {"sgw.dll", "ISgw::m4", sgw_m4}, {"sgw.dll", "ISgw::m5", sgw_m5},
	{"sgw.dll", "ISgw::m6", sgw_m6}, {"sgw.dll", "ISgw::m7", sgw_m7}, {"sgw.dll", "ISgw::m8", sgw_m8},
	{"sgw.dll", "ISgw::m9", sgw_m9}, {"sgw.dll", "ISgw::m10", sgw_m10}, {"sgw.dll", "ISgw::m11", sgw_m11},
	{"sgw.dll", "ISgw::m12", sgw_m12}, {"sgw.dll", "ISgw::m13", sgw_m13}, {"sgw.dll", "ISgw::m14", sgw_m14},
	{"sgw.dll", "ISgw::m15", sgw_m15},
	E("VERSION.dll", GetFileVersionInfoSizeA), E("VERSION.dll", GetFileVersionInfoA), E("VERSION.dll", VerQueryValueA),
	E("ole32.dll", CoInitialize), E("ole32.dll", CoInitializeEx), E("ole32.dll", CoUninitialize),
	E("ole32.dll", CoCreateInstance), E("ole32.dll", CoGetClassObject), E("ole32.dll", CoSetProxyBlanket),
	O("OLEAUT32.dll", 2, SysAllocString), O("OLEAUT32.dll", 6, SysFreeString), O("OLEAUT32.dll", 8, VariantInit),
	O("OLEAUT32.dll", 9, VariantClear), O("OLEAUT32.dll", 12, VariantChangeType),
};
const unsigned xrc_hle_system_n = sizeof xrc_hle_system / sizeof xrc_hle_system[0];
