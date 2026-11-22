/* hle_lr.c - the small imports Lightning Returns adds over FFXIII: KERNEL32 extras, ADVAPI32, IMAGEHLP, ole32,
 * OLEAUT32, SHELL32, IMM32 (no IME), WS2_32 (no network), XInput (no pad yet), D3D9 / D3DX9 debug switches.
 * All WINAPI (stdcall) unless noted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "xrc_rt.h"

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define S(a) ((char *)(xrc_mem + (u32)(a)))
static void set_err(u32 e) { M32(xrc_teb + 0x34) = e; }

/* ============================================================================================ KERNEL32 */
static void hle_GetStartupInfoW(XCtx *c) { memset(xrc_mem + A(0), 0, 68); M32(A(0)) = 68; RET(0, 1); }
static void hle_EncodePointer(XCtx *c) { RET(A(0), 1); }
static void hle_DecodePointer(XCtx *c) { RET(A(0), 1); }
static void hle_HeapSetInformation(XCtx *c) { RET(1, 4); }
/* the instruction sets the translated code handles: SSE, SSE2, RDTSC (no MMX, no SSE3) */
static void hle_IsProcessorFeaturePresent(XCtx *c) {
	u32 f = A(0), ra = M32(c->esp);
	/* Miles' mixer picker (00c1f1b0) asks for SSE2 at 00c1f2de and 00c1f367; a yes picks 00c1e430 / 00c1e980, which use
	 * pslldq (not translated). No there: it picks its other mixers (translated), as on a CPU without SSE2. */
	if (f == 10 && (ra == 0x00c1f2e4u || ra == 0x00c1f36fu)) RET(0, 1);
	RET(f == 6 || f == 10 || f == 8, 1);
}
/* Miles Sound System's CPU probe (00de8000, in its own MSSMIXER section, so not translated: push ds / push es / cpuid).
 * Its bits: 1 MMX, 2 3DNow, 4 SSE, 8 SSE2. The answer 0 sends Miles to its C sample converters (00c17e80, 00c17db0,
 * translated), not the MMX ones (00c180e0, 00c17f60: packssdw, not translated). Without this the game died about
 * 3.5 minutes into a walk from the Dead Dunes station (2026-11-20 soak3). */
void XRC_CC xrc_mss_cpu_probe(XCtx *c) {
	static int said;
	if (!said++) fprintf(stderr, "xrc: Miles CPU probe: no MMX / SSE (its C mixers)\n");
	c->eax = 0;
	xrc_cdecl_ret(c);
}
/* GetVersionExA: Windows 7 SP1 (what the 2014 release was built against) */
static void hle_GetVersionExA(XCtx *c) {
	u32 p = A(0), size = M32(p);
	if (size != 148 && size != 156) { set_err(122); RET(0, 1); }
	memset(xrc_mem + p + 4, 0, size - 4);
	M32(p + 4) = 6; M32(p + 8) = 1; M32(p + 12) = 7601; M32(p + 16) = 2;
	strcpy(S(p + 20), "Service Pack 1");
	if (size == 156) M16(p + 148) = 1; /* wServicePackMajor */
	RET(1, 1);
}
static void put_systime(u32 p, const struct tm *t, unsigned ms) {
	M16(p) = (u16)(t->tm_year + 1900); M16(p + 2) = (u16)(t->tm_mon + 1); M16(p + 4) = (u16)t->tm_wday;
	M16(p + 6) = (u16)t->tm_mday; M16(p + 8) = (u16)t->tm_hour; M16(p + 10) = (u16)t->tm_min;
	M16(p + 12) = (u16)t->tm_sec; M16(p + 14) = (u16)ms;
}
static void hle_GetSystemTime(XCtx *c) {
	struct timespec ts;
	struct tm t;
	time_t s;
	clock_gettime(CLOCK_REALTIME, &ts);
	s = ts.tv_sec;
#ifdef _WIN32
	gmtime_s(&t, &s);
#else
	gmtime_r(&s, &t);
#endif
	put_systime(A(0), &t, (unsigned)(ts.tv_nsec / 1000000));
	RET(0, 1);
}
/* days from 1601-01-01 to y-m-d (proleptic Gregorian) */
static long long days_1601(int y, int m, int d) {
	long long yy = y - (m <= 2), era, yoe, doy, doe;
	era = (yy >= 0 ? yy : yy - 399) / 400;
	yoe = yy - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 584388; /* 584388: 0000-03-01 .. 1601-01-01 */
}
static void hle_SystemTimeToFileTime(XCtx *c) {
	u32 st = A(0), ft = A(1);
	int y = M16(st), mo = M16(st + 2), d = M16(st + 6);
	if (y < 1601 || mo < 1 || mo > 12 || d < 1 || d > 31 || M16(st + 8) > 23 || M16(st + 10) > 59 || M16(st + 12) > 59 || M16(st + 14) > 999) { set_err(87); RET(0, 2); }
	{
		long long t = ((days_1601(y, mo, d) * 24 + M16(st + 8)) * 60 + M16(st + 10)) * 60 + M16(st + 12);
		u64 v = (u64)t * 10000000ull + (u64)M16(st + 14) * 10000ull;
		M64(ft) = v;
	}
	RET(1, 2);
}
/* GetFileSize(h, *high) by seeking (the runtime's SetFilePointer) */
static void hle_GetFileSize(XCtx *c) {
	u32 h = A(0), hi = A(1), cur, end, sp = c->esp, scr = (c->esp - 32) & ~15u;
	c->esp = scr;
	M32(scr) = 0;
	{ u32 a[4] = {h, 0, 0, 1}; cur = xrc_k32(c, "SetFilePointer", 4, a); }
	{ u32 a[4] = {h, 0, scr, 2}; end = xrc_k32(c, "SetFilePointer", 4, a); }
	{ u32 a[4] = {h, cur, 0, 0}; xrc_k32(c, "SetFilePointer", 4, a); }
	if (hi) M32(hi) = M32(scr);
	c->esp = sp;
	RET(end, 2);
}
/* CopyFileA(src, dst, fail_if_exists): a read and a write through the runtime's files (its path mapping) */
static void hle_CopyFileA(XCtx *c) {
	u32 src = A(0), dst = A(1), fail = A(2), hs, hd, buf, ok = 1, sp = c->esp, scr = (c->esp - 32) & ~15u;
	c->esp = scr;
	{ u32 a[7] = {src, 0x80000000u, 1, 0, 3, 0x80, 0}; hs = xrc_k32(c, "CreateFileA", 7, a); }
	if (hs == 0xffffffffu) { c->esp = sp; RET(0, 3); }
	{ u32 a[7] = {dst, 0x40000000u, 0, 0, fail ? 1u : 2u, 0x80, 0}; hd = xrc_k32(c, "CreateFileA", 7, a); }
	if (hd == 0xffffffffu) { u32 a[1] = {hs}; xrc_k32(c, "CloseHandle", 1, a); c->esp = sp; RET(0, 3); }
	buf = xrc_heap_alloc(65536, 0);
	for (;;) {
		u32 got;
		M32(scr) = 0;
		{ u32 a[5] = {hs, buf, 65536, scr, 0}; if (!xrc_k32(c, "ReadFile", 5, a)) { ok = 0; break; } }
		got = M32(scr);
		if (!got) break;
		{ u32 a[5] = {hd, buf, got, scr + 4, 0}; if (!xrc_k32(c, "WriteFile", 5, a)) { ok = 0; break; } }
	}
	xrc_heap_free(buf);
	{ u32 a[1] = {hs}; xrc_k32(c, "CloseHandle", 1, a); }
	{ u32 a[1] = {hd}; xrc_k32(c, "CloseHandle", 1, a); }
	c->esp = sp;
	RET(ok, 3);
}

/* ============================================================================================ others */
static void hle_InitializeSecurityDescriptor(XCtx *c) { memset(xrc_mem + A(0), 0, 20); M8(A(0)) = 1; RET(1, 2); }
static void hle_SetSecurityDescriptorDacl(XCtx *c) { RET(1, 4); }
/* MakeSureDirectoryPathExists(path): every directory up to the last separator */
static void hle_MakeSureDirectoryPathExists(XCtx *c) {
	u32 p = A(0), n = (u32)strlen(S(p)), k, tmp = xrc_heap_alloc(n + 1, 0);
	memcpy(S(tmp), S(p), n + 1);
	for (k = 0; k < n; k++) {
		char ch = S(tmp)[k];
		if ((ch == '\\' || ch == '/') && k > 0 && S(tmp)[k - 1] != ':') {
			u32 a[2] = {tmp, 0};
			S(tmp)[k] = 0;
			xrc_k32(c, "CreateDirectoryA", 2, a);
			S(tmp)[k] = ch;
		}
	}
	xrc_heap_free(tmp);
	RET(1, 1);
}
static void hle_CoTaskMemAlloc(XCtx *c) { RET(xrc_heap_alloc(A(0) ? A(0) : 1, 0), 1); }
static void hle_CoTaskMemFree(XCtx *c) { if (A(0)) xrc_heap_free(A(0)); RET(0, 1); }
/* SysAllocString(wstr): a BSTR (byte length before it, NUL after) */
static void hle_SysAllocString(XCtx *c) {
	u32 s = A(0), n = 0, p;
	if (!s) RET(0, 1);
	while (M16(s + 2 * n)) n++;
	p = xrc_heap_alloc(4 + 2 * n + 2, 0);
	if (!p) RET(0, 1);
	M32(p) = 2 * n;
	memcpy(xrc_mem + p + 4, xrc_mem + s, 2 * n);
	M16(p + 4 + 2 * n) = 0;
	RET(p + 4, 1);
}
/* ShellExecuteA(hwnd, op, file, params, dir, show): opens nothing (web pages, the manual); "success" */
static void hle_ShellExecuteA(XCtx *c) { fprintf(stderr, "xrc: ShellExecuteA(%s) ignored\n", A(2) ? S(A(2)) : ""); RET(42, 6); }

/* IMM32: no input method */
static void hle_ImmGetContext(XCtx *c) { RET(0, 1); }
static void hle_ImmReleaseContext(XCtx *c) { RET(1, 2); }
static void hle_ImmDisableIME(XCtx *c) { RET(1, 1); }
static void hle_ImmIsIME(XCtx *c) { RET(0, 1); }
static void hle_ImmGetOpenStatus(XCtx *c) { RET(0, 1); }
static void hle_ImmSetOpenStatus(XCtx *c) { RET(1, 2); }
static void hle_ImmSetCompositionWindow(XCtx *c) { RET(1, 2); }
static void hle_ImmGetCompositionStringW(XCtx *c) { RET(0, 4); }

/* WS2_32 by ordinal: no network (WSAStartup fails with WSASYSNOTREADY; nothing else should then run) */
static void ws_startup(XCtx *c) { RET(10091, 2); }   /* #115 WSAStartup */
static void ws_cleanup(XCtx *c) { RET(0, 0); }       /* #116 WSACleanup */
static void ws_socket(XCtx *c) { RET(0xffffffffu, 3); } /* #23 socket: INVALID_SOCKET */
static void ws_closesocket(XCtx *c) { RET(0, 1); }   /* #3 */
static void ws_htons(XCtx *c) { u32 v = A(0) & 0xffff; RET(((v & 0xff) << 8) | (v >> 8), 1); } /* #9 */
static void ws_inet_addr(XCtx *c) {                  /* #11 inet_addr("a.b.c.d") */
	unsigned a, b, d, e;
	if (sscanf(S(A(0)), "%u.%u.%u.%u", &a, &b, &d, &e) != 4 || a > 255 || b > 255 || d > 255 || e > 255) RET(0xffffffffu, 1);
	RET(a | b << 8 | d << 16 | e << 24, 1);
}
static void ws_recv(XCtx *c) { RET(0xffffffffu, 4); }   /* #16 */
static void ws_send(XCtx *c) { RET(0xffffffffu, 4); }   /* #19 */
static void ws_bind(XCtx *c) { RET(0xffffffffu, 3); }   /* #2 */
static void ws_sendto(XCtx *c) { RET(0xffffffffu, 6); } /* #20 */

/* XInput 1.3 by ordinal: no controller connected (ERROR_DEVICE_NOT_CONNECTED) */
/* #2 XInputGetState(index, XINPUT_STATE *): the host's pad (XINPUT_STATE is 16 bytes on both sides). None (1167,
 * ERROR_DEVICE_NOT_CONNECTED) in headless runs (XRC_VK_HEADLESS: scripted input only), with XRC_NO_PAD=1, or without an
 * XInput DLL on the host */
int xrc_pad_scripted(void);
/* the scripted pad plugged in: XRC_PAD_AT=secs (tools) keeps it unplugged until secs after its first check (the
 * boot's controller scan), to test pads connected after boot */
static int pad_plugged(void) {
	/* XRC_PAD_GAP=out,back (tools): unplugged again from out to back secs after that first check (hot-plug tests) */
	static double t0 = -1, at = -2, out = -1, back = -1;
	struct timespec ts;
	double t;
	if (at == -2) {
		const char *g = xrc_tool_env("XRC_PAD_GAP");
		at = xrc_tool_env("XRC_PAD_AT") ? atof(xrc_tool_env("XRC_PAD_AT")) : -1;
		if (g && sscanf(g, "%lf,%lf", &out, &back) != 2) out = back = -1;
	}
	if (at < 0 && out < 0) return 1;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
	if (t0 < 0) t0 = t;
	t -= t0;
	if (out >= 0 && t >= out && t < back) return 0;
	return t >= at;
}
/* a scripted pad (XRC_INPUT pHHHH taps, hle_dinput.c): pad 0 connected, its buttons from the script */
static int xi_scripted(XCtx *c) {
	static u32 packet;
	int b = xrc_pad_scripted();
	if (b < 0) return 0;
	if (A(0) != 0 || A(1) == 0 || !pad_plugged()) { c->eax = 1167; return 1; }
	memset(xrc_mem + A(1), 0, 16);
	M32(A(1)) = ++packet;
	M16(A(1) + 4) = (u16)b;
	c->eax = 0;
	return 1;
}
#ifdef _WIN32
#include <windows.h>
/* the host's XInputGetState (none headless, with XRC_NO_PAD, or without an XInput DLL) */
static DWORD (WINAPI *host_xi_get(void))(DWORD, void *) {
	static int tried;
	static DWORD (WINAPI *get)(DWORD, void *);
	if (!tried) {
		const char *dll[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
		HMODULE h = 0;
		int k;
		tried = 1;
		/* (XRC_HOST_PAD=1: the host's pads in a headless run too -- tests of the real pad path) */
		if ((!getenv("XRC_VK_HEADLESS") || getenv("XRC_HOST_PAD")) && !getenv("XRC_NO_PAD"))
			for (k = 0; k < 3 && !h; k++) h = LoadLibraryA(dll[k]);
		if (h) *(FARPROC *)&get = GetProcAddress(h, "XInputGetState");
		fprintf(stderr, "xrc: xinput: %s\n", get ?"the host's pads" : "no pad");
	}
	return get;
}
static void xi_get_state(XCtx *c) {
	DWORD (WINAPI *get)(DWORD, void *);
	unsigned char st[16];
	DWORD r;
	if (xi_scripted(c)) RET(c->eax, 2);
	get = host_xi_get();
	if (!get || A(1) == 0) RET(1167, 2);
	r = get(A(0), st);
	if (r == 0) {
		static int said;
		void xrc_menu_pad_filter(unsigned char *st); /* (xrc_menu.c: the settings overlay open -> the pad at rest) */
		xrc_menu_pad_filter(st);
		memcpy(xrc_mem + A(1), st, sizeof st);
		if (!said++)
			fprintf(stderr, "xrc: xinput: host pad %u read by the game (packet %u, buttons %04x)\n", (unsigned)A(0),
				(unsigned)(st[0] | st[1] << 8 | st[2] << 16 | (unsigned)st[3] << 24), (unsigned)(st[4] | st[5] << 8));
	}
	RET(r, 2);
}
/* the XInput users connected now (bit n: user n): the scripted pad is user 0; else the host's pads */
u32 xrc_xinput_users(void) {
	DWORD (WINAPI *get)(DWORD, void *);
	unsigned char st[16];
	u32 m = 0, k;
	if (xrc_pad_scripted() >= 0) return pad_plugged() ? 1 : 0;
	if (!(get = host_xi_get())) return 0;
	for (k = 0; k < 4; k++)
		if (get(k, st) == 0) m |= 1u << k;
	return m;
}
/* pads plugged in or out after boot: the game rescans its controllers (EnumDevices, the next three seconds, 00747100)
 * only when its device-change count (the pad manager [0x24dab1c] + 8) is up, which its window's device-change path
 * raises by 3 (00744d50). The host's pads are checked every 2 s (on the game's keyboard read, once a frame) and a
 * change raises the count the same way. */
void xrc_pad_hotplug_poll(void) {
	static ULONGLONG next;
	static u32 last;
	static int started;
	ULONGLONG now = GetTickCount64();
	u32 m, mgr;
	if (now < next || (xrc_pad_scripted() < 0 && !host_xi_get())) return;
	next = now + 2000;
	m = xrc_xinput_users();
	if (!started) { started = 1; last = m; return; } /* (the boot's own scan finds the pads there then) */
	if (m == last) return;
	fprintf(stderr, "xrc: xinput: pads %x -> %x, the game rescans\n", last, m);
	last = m;
	if ((mgr = M32(0x24dab1cu)) != 0) __atomic_fetch_add((u32 *)(xrc_mem + mgr + 8), 3u, __ATOMIC_SEQ_CST);
}
#else
void xrc_pad_hotplug_poll(void) {}
static void xi_get_state(XCtx *c) { if (xi_scripted(c)) RET(c->eax, 2); RET(1167, 2); }
u32 xrc_xinput_users(void) { return xrc_pad_scripted() >= 0 && pad_plugged() ? 1 : 0; }
#endif
static void xi_enable(XCtx *c) { RET(0, 1); }       /* #5 XInputEnable(BOOL): void (#3 is XInputSetState) */

static void hle_D3DPERF_SetOptions(XCtx *c) { RET(0, 1); }
static void hle_D3DXDebugMute(XCtx *c) { RET(0, 1); }

#define K(n) {"KERNEL32.dll", #n, hle_##n}
const XrcImport xrc_hle_lr[] = {
	K(GetStartupInfoW), K(EncodePointer), K(DecodePointer), K(HeapSetInformation), K(IsProcessorFeaturePresent),
	K(GetVersionExA), K(GetSystemTime), K(SystemTimeToFileTime), K(GetFileSize), K(CopyFileA),
	{"ADVAPI32.dll", "InitializeSecurityDescriptor", hle_InitializeSecurityDescriptor},
	{"ADVAPI32.dll", "SetSecurityDescriptorDacl", hle_SetSecurityDescriptorDacl},
	{"IMAGEHLP.dll", "MakeSureDirectoryPathExists", hle_MakeSureDirectoryPathExists},
	{"ole32.dll", "CoTaskMemAlloc", hle_CoTaskMemAlloc}, {"ole32.dll", "CoTaskMemFree", hle_CoTaskMemFree},
	{"OLEAUT32.dll", "#2", hle_SysAllocString}, {"SHELL32.dll", "ShellExecuteA", hle_ShellExecuteA},
	{"IMM32.dll", "ImmGetContext", hle_ImmGetContext}, {"IMM32.dll", "ImmReleaseContext", hle_ImmReleaseContext},
	{"IMM32.dll", "ImmDisableIME", hle_ImmDisableIME}, {"IMM32.dll", "ImmIsIME", hle_ImmIsIME},
	{"IMM32.dll", "ImmGetOpenStatus", hle_ImmGetOpenStatus}, {"IMM32.dll", "ImmSetOpenStatus", hle_ImmSetOpenStatus},
	{"IMM32.dll", "ImmSetCompositionWindow", hle_ImmSetCompositionWindow},
	{"IMM32.dll", "ImmGetCompositionStringW", hle_ImmGetCompositionStringW},
	{"WS2_32.dll", "#115", ws_startup}, {"WS2_32.dll", "#116", ws_cleanup}, {"WS2_32.dll", "#23", ws_socket},
	{"WS2_32.dll", "#3", ws_closesocket}, {"WS2_32.dll", "#9", ws_htons}, {"WS2_32.dll", "#11", ws_inet_addr},
	{"WS2_32.dll", "#16", ws_recv}, {"WS2_32.dll", "#19", ws_send}, {"WS2_32.dll", "#2", ws_bind},
	{"WS2_32.dll", "#20", ws_sendto},
	{"XINPUT1_3.dll", "#2", xi_get_state}, {"XINPUT1_3.dll", "#5", xi_enable},
	{"d3d9.dll", "D3DPERF_SetOptions", hle_D3DPERF_SetOptions}, {"d3dx9_43.dll", "D3DXDebugMute", hle_D3DXDebugMute},
};
const unsigned xrc_hle_lr_n = sizeof xrc_hle_lr / sizeof xrc_hle_lr[0];
