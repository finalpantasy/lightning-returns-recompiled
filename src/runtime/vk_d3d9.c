/* vk_d3d9.c - the D3D9 HLE's rendering backend on Vulkan (the linux_x64 target: -DXRC_VULKAN).
 *
 * The HLE (hle_d3d9.c) keeps every state the game sets (dev) and every resource's contents in guest memory; this draws
 * with them. At a draw it reads the device state, uploads what the guest changed since (the HLE reports locks:
 * D3Backend.locked), finds or makes the pipeline for the state, and records the draw. Shaders: vk_sm3.c translates the
 * game's vs_3_0 / ps_3_0 bytecode to SPIR-V (its interface: vk_sm3.h).
 *
 * Vulkan use, kept simple on purpose:
 *   - Vulkan 1.3 (dynamic rendering) and VK_KHR_push_descriptor (a set per draw, no pools). Each image is in the
 *     layout of its last use (Res.lay; img_use moves it): attachment-optimal while drawn into, shader-read-only while
 *     sampled, the transfer layouts for copies, uploads and clears -- GENERAL only for an image sampled by the rendering
 *     that draws into it. (Kept compressed: AMD's GPUs leave DCC / HTILE off in GENERAL.) Barriers only where a use
 *     depends on an earlier one: each image knows whether it was written / read since the last sync (img_access), and
 *     the layout changes and the sync a use needs are one vkCmdPipelineBarrier (bq_go), its stages those of the uses.
 *     XRC_VK_FULL_BARRIERS=1: a full barrier before every use (as before; to compare).
 *   - Two frames in flight, each with its command buffer, fence and streaming ring (host-visible: vertex / index data of
 *     dynamic buffers and UP draws, constants, staging of uploads). When a ring fills, the frame is submitted and waited
 *     for (and the ring starts over).
 *   - Static vertex / index buffers and textures are device memory, uploaded (in the command stream, in order with the
 *     draws) when the guest locked them; dynamic buffers (D3DUSAGE_DYNAMIC, system memory) are streamed per draw.
 *   - Memory: blocks of 64 MB per memory type, suballocated first-fit.
 *   - D3D9 conventions: the viewport is flipped (negative height) so clip space is D3D's; gl_Position.xy is moved by
 *     half a pixel (D3D9's pixel centres; push constants); triangle fans become lists; D3D's clockwise front faces.
 *
 * The window and the swap chain: SDL2 (loaded at run time: libSDL2-2.0.so.0; no headers needed), unless XRC_VK_HEADLESS
 * is set or there is no display -- then the back buffer is only read back for XRC_SHOT.
 *
 * Environment: XRC_VK_HEADLESS=1 no window; XRC_SHOT=secs[,secs] the back buffer as shot_<secs>.png (in the run
 * directory, since the first Present); XRC_VK_DEVICE=n the physical device; XRC_VK_VALIDATION=1 the Khronos validation
 * layer (when installed); XRC_VK_LOG=1 more messages (every 600 frames: pipelines, GPU wait / draw ms, uploads,
 * renderings, the GPU's time from timestamps); XRC_VK_PASSES=1 every 600 frames one frame's renderings with their GPU
 * times, sizes and draws (tools builds); XRC_VK_SCALE window size multiplier; XRC_VK_NODRAW=1 draws nothing (debugging); XRC_VK_PCACHE=file the
 * pipeline cache on disk (default vk_pipelines.bin in the run directory; "-": none); XRC_VK_SYNC_PIPELINES=1 pipelines made
 * in the draw that needs them (stalling it) instead of by the builder thread (whose draws are skipped meanwhile);
 * XRC_FRAME_TIME=0 hides the frame rate counter (top right, in FPS; only in builds with the debug tools: not with
 * -DXRC_NO_TOOLS).
 * The pipeline cache is loaded at start (when it is this driver's and GPU's) and saved again a few seconds after the
 * last new pipeline (copied out on the main thread, written by a thread: a process killed from WSL runs no exit
 * handlers), so a pipeline is compiled once, not once a session. GPU memory is suballocated from 64 MB blocks, and a
 * spare block of each kind in use is allocated ahead by a thread: a block filling up does not stall a frame on
 * vkAllocateMemory (which zeroes it). (One spare per memory kind: up to ~64 MB more each.)
 * Note: the game loads in the time its main thread has left in a frame; a GPU slower than the frame (lavapipe in 3D
 * scenes) slows loading down with it. */
#ifdef XRC_VULKAN
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* (pthread_setname_np) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#define dlopen(name, flags) ((void *)LoadLibraryA(name))
#define dlsym(lib, name) ((void *)GetProcAddress((HMODULE)(lib), name))
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "hle_d3d9.h"
#include "vk_sm3.h"
#include "xrc_disp.h" /* (hires2: the monitor / display the game is told about) */
#include "vk_post.h" /* (the last passes: Lanczos rescale, sharpening, deband -- post_blit) */
void xrc_vk_tex_cycle(void);
void xrc_vk_mesh_request(void); /* (F7: the mesh dump) */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* ({sType} initialisers) */

/* ============================================================================================ loading */
static void *vk_lib;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_;
#define VK_FNS(X) \
	X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)
#define VKI_FNS(X) \
	X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures2) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
	X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkDestroySurfaceKHR)
#define VKD_FNS(X) X(vkCmdBeginQuery) X(vkCmdEndQuery) \
	X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
	X(vkCreateFence) X(vkWaitForFences) \
	X(vkResetFences) X(vkCreateSemaphore) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateBuffer) \
	X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) \
	X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
	X(vkCreateSampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) \
	X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) \
	\
	\
	\
	\
	\
	\
	X(vkCreateQueryPool) X(vkGetQueryPoolResults) \
	X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
	X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)
#define DECL(f) static PFN_##f f;
VK_FNS(DECL) VKI_FNS(DECL) VKD_FNS(DECL)
/* present wait (VK_KHR_present_id + VK_KHR_present_wait): after presenting frame N the main thread waits until frame
 * N-1 is on the screen, so at most one finished frame waits for the display. Without it, when the frame limiter runs
 * a hair faster than the display's refresh, the swap chain's queue fills (vsync then paces the game, from
 * vkAcquireNextImageKHR) and every frame reaches the screen up to two refreshes late: input lag at a steady 60 fps.
 * XRC_VK_PRESENT_WAIT=0: off. */
static PFN_vkWaitForPresentKHR vkWaitForPresentKHR_;
static uint64_t pw_last, pw_first; /* the last present's id; the first id on the current swap chain */
#undef DECL

static int vlog;
static double wait_ms, draw_ms, up_bytes, n_renders, n_px;
/* tools builds: the GPU's time per frame (timestamps at the start and end of each command buffer; XRC_VK_LOG) and the
 * main thread's time blocked in vkAcquireNextImageKHR / vkQueuePresentKHR */
#ifndef XRC_NO_TOOLS
static VkQueryPool tspool;
#endif
static double gpu_ms, present_ms;
static double pace_bp, pace_bw; /* pace6: cumulative present/acquire and fence-wait ms (survive the 600-frame reset) */
void xrc_pace_get(double *pr, double *fw) { *pr = pace_bp + present_ms; *fw = pace_bw + wait_ms; }
#ifndef XRC_NO_TOOLS
/* XRC_VK_PASSES=1 (tools builds): every 600 frames, one frame's renderings with their GPU time (timestamps around
 * each), size, attachments, draws and clears -- which passes the GPU's time goes to */
#define PP_MAX 160
typedef struct { u32 w, h, ncol, depth, fmt, draws, clears, rt0, blits, fills, ups, upb, reads, n_at, n_zout, n_blend, n_zt, n_zw; } PPass;
static u32 pp_ops[5]; /* (blits, color fills, image uploads, buffer uploads, read-backs since the last rendering began) */
#define PP_OP(k) (pp_ops[k]++)
static VkQueryPool ppool;
static int pp_open;
static int pp_rec[3];
static u32 pp_n[3];
static PPass pp[3][PP_MAX];
/* XRC_VK_DRAWTIME=N (tools builds; not with the render thread): every N frames, one frame's draws each with a timestamp
 * and the pixel shader invocations they caused (a pipeline statistics query) -- summed per pixel shader and reported
 * every 600 frames: which shaders the GPU's time goes to, and whether for their pixels or their cost per pixel */
#define DT_MAX 4096
typedef struct { u32 ps, vs, pass, blend, at, nv, w, h, nsmp, words; } DtDraw; /* (ps 0: a rendering's start) */
typedef struct { double ms, frags; unsigned long long draws; u32 blend, at, w, h, nsmp, words, vs; } DtSum;
static VkQueryPool dtpool, dspool;
static u32 dt_every;
static int dt_rec[3];
static u32 dt_n[3], dt_pass;
static DtDraw dt[3][DT_MAX];
static DtSum dt_sum[8192];
static u32 dt_frames;
#else
#define PP_OP(k) ((void)0)
#endif
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6; }
#ifndef XRC_NO_TOOLS
/* XRC_HITCH_MS=N (tools builds): every frame's CPU-side times and counts and its GPU time (whole frame, and per
 * rendering: the timestamps of XRC_VK_PASSES, then taken every frame); a frame of over N ms -- present to present on
 * the CPU, or on the GPU -- is reported with them (once its GPU times are back, NFRAMES-1 frames later) */
typedef struct {
	unsigned long long frame;
	double interval, draw, present, fwait, flush, alloc, limit, gil, upkb, submit, cpu;
	unsigned long long t_end; /* (xrc_now_us at its present: XRC_PROFILE's clock -- its "#frame" lines) */
	u32 nflush, nalloc, nimg, npipe, nskip, ndraw;
	int valid;
} Hitch;
static double hitch_ms;
static Hitch hitch[3], hcur; /* (per frame slot: NFRAMES, checked below) */
static double h_last_present, h_avg_gpu, h_avg_int;
static unsigned long long h_reported;
/* the reports: kept in memory and written at the exit (written as they come, to a pipe, they made later frames slow --
 * and those were then reported in turn) */
static char *hbuf;
static size_t hlen, hcap = 64u << 20;
static void hprint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void hprint(const char *fmt, ...) {
	va_list ap;
	int n;
	if (!hitch_ms) {
		va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
		return;
	}
	if (!hbuf && !(hbuf = (char *)malloc(hcap))) return;
	if (hlen + 4096 > hcap) return; /* (full: the rest dropped) */
	va_start(ap, fmt);
	n = vsnprintf(hbuf + hlen, hcap - hlen, fmt, ap);
	va_end(ap);
	if (n > 0) hlen += (size_t)n < hcap - hlen ? (size_t)n : hcap - hlen - 1;
}
/* every frame's present-to-present interval, in 0.1 ms bins (the last: 500 ms and over): the distribution at the exit
 * (frames over 16.7 / 20 / 33 / 50 ms, the 1% and 0.1% lows -- the average rate of the slowest 1% / 0.1% of frames) */
#define H_BINS 5001
static unsigned h_hist[H_BINS];
static void h_hist_add(double ms) { int b = (int)(ms * 10.0); h_hist[b < 0 ? 0 : b >= H_BINS ? H_BINS - 1 : b]++; }
static void h_hist_report(void) {
	unsigned long long n = 0, o17 = 0, o20 = 0, o33 = 0, o50 = 0;
	double tot = 0, w1 = 0, w01 = 0;
	unsigned long long c1, c01, k;
	int b;
	for (b = 0; b < H_BINS; b++) {
		n += h_hist[b]; tot += h_hist[b] * (b + 0.5) / 10.0;
		if (b >= 167) o17 += h_hist[b];
		if (b >= 200) o20 += h_hist[b];
		if (b >= 333) o33 += h_hist[b];
		if (b >= 500) o50 += h_hist[b];
	}
	if (!n) return;
	c1 = n / 100 ? n / 100 : 1; c01 = n / 1000 ? n / 1000 : 1;
	for (b = H_BINS - 1, k = 0; b >= 0 && k < c1; b--) {
		unsigned long long t = h_hist[b] < c1 - k ? h_hist[b] : c1 - k;
		w1 += t * (b + 0.5) / 10.0;
		if (k < c01) w01 += (t < c01 - k ? t : c01 - k) * (b + 0.5) / 10.0;
		k += t;
	}
	fprintf(stderr, "xrc: frame times: %llu frames, avg %.2f ms (%.1f fps); over 16.7 ms %llu, over 20 ms %llu, over 33 ms %llu, over 50 ms %llu; 1%% low %.1f fps, 0.1%% low %.1f fps\n",
	        n, tot / n, 1000.0 * n / tot, o17, o20, o33, o50, 1000.0 * c1 / w1, 1000.0 * c01 / w01);
}
static void hitch_flush(void) {
	h_hist_report();
	if (hbuf && hlen) { fwrite(hbuf, 1, hlen, stderr); fprintf(stderr, "xrc: %llu hitches reported%s\n", h_reported, hlen + 4096 > hcap ? " (the report buffer filled up: later ones dropped)" : ""); fflush(stderr); }
	hlen = 0;
}
/* the calling thread's CPU time, ms (Windows: its cycles, at a rate measured once -- GetThreadTimes counts in clock ticks
 * of ~15.6 ms) */
static double h_thread_cpu_ms(void) {
#ifdef _WIN32
	static double cyc_per_ms;
	ULONG64 c;
	if (!cyc_per_ms) {
		ULONG64 c0, c1;
		double t0 = now_ms(), t1;
		QueryThreadCycleTime(GetCurrentThread(), &c0);
		do t1 = now_ms(); while (t1 - t0 < 20); /* (running all along: cycles per ms of real time) */
		QueryThreadCycleTime(GetCurrentThread(), &c1);
		cyc_per_ms = (double)(c1 - c0) / (t1 - t0);
	}
	QueryThreadCycleTime(GetCurrentThread(), &c);
	return (double)c / cyc_per_ms;
#else
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
#endif
}
#define HC(field, v) (hcur.field += (v))
#else
#define HC(field, v) ((void)0)
#endif
#define LOG(...) do { if (vlog) fprintf(stderr, __VA_ARGS__); } while (0)
static void die(const char *what, VkResult r) {
	fprintf(stderr, "xrc: vulkan: %s failed (%d)\n", what, (int)r);
	exit(3);
}
#define VKC(call) do { VkResult r_ = (call); if (r_ < 0) die(#call, r_); } while (0)

/* ============================================================================================ the window
 * Linux / macOS: an SDL2 window of our own (SDL2 loaded at run time). Windows (the windows_x64_reference build with
 * XRC_VK=1): the game's own device window, a real window there (ref_user32.c) with its input (ref_dinput.c). */
static void *window;
#ifdef _WIN32
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; HINSTANCE hinstance; HWND hwnd; } Win32SurfaceInfo;
typedef VkResult (VKAPI_PTR *PFN_CreateWin32Surface)(VkInstance, const Win32SurfaceInfo *, const VkAllocationCallbacks *, VkSurfaceKHR *);
#ifdef XRC_REFERENCE
void *xrc_ref_hwnd(u32 g);
static void *plat_device_window(void) {
	u32 g = dev.pp[7] ? dev.pp[7] : dev.hwnd; /* hDeviceWindow, else the focus window */
	return g ? xrc_ref_hwnd(g) : 0;
}
static int plat_window(const char **iext, u32 *niext) {
	u32 g = dev.pp[7] ? dev.pp[7] : dev.hwnd;
	window = plat_device_window();
	if (!window) { fprintf(stderr, "xrc: vulkan: the device window %08x has no real window: drawing without one\n", g); return 0; }
	iext[(*niext)++] = "VK_KHR_surface";
	iext[(*niext)++] = "VK_KHR_win32_surface";
	return 1;
}
static void plat_resize(int w, int h) { (void)w; (void)h; } /* (the game sizes its window itself) */
#else
/* the HLE build (user32 HLE: the game's windows are guest-only): a real window of our own, made and pumped by a thread
 * of its own; its keys go to the DirectInput HLE as DIK codes (the set-1 scan code, 0x80 for an extended key).
 * Closing it ends the game; losing the focus releases every key. XRC_VK_HEADLESS=1: none. */
void xrc_input_dik(int dik, int down);
static HANDLE own_ready;
static int own_w = 1280, own_h = 720;
#ifdef XRC_LR
void xrc_settings_menu(void *hwnd); /* (xrc_menu.c: the old Win32 popup, Shift+F10) */
int xrc_menu_key(unsigned vk, int down, int repeat); /* (xrc_menu.c: the overlay; 1: the key was the menu's) */
void xrc_menu_mouse(int kind, int x, int y, int cw, int ch, int wheel);
int xrc_menu_is_open(void);
#endif
/* borderless fullscreen (XRC_WINDOW=borderless, the default; =windowed: a window of the game's size): the window covers
 * its monitor without a frame, the desktop's mode kept; the picture is scaled to fit (aspect kept). F11 or Alt+Enter
 * toggles it; the settings menu sets it (xrc_vk_set_borderless). Only the window's thread changes it. */
#define OWN_WM_BORDERLESS (WM_APP + 7)
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
static volatile int own_fs;
static DWORD own_style;
/* hires2: the windowed client size (physical pixels) and position to come back to; the window's dpi. */
static int win_cw, win_ch, win_x = 0x7fffffff, win_y, own_dpi = 96;
static void own_outer(DWORD style, int dpi, int cw, int ch, RECT *r) { /* the window rectangle for a client of cw x ch at dpi */
	typedef BOOL (WINAPI *AdjF)(LPRECT, DWORD, BOOL, DWORD, UINT);
	static AdjF f;
	static int tried;
	if (!tried) { f = (AdjF)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi"); tried = 1; }
	r->left = r->top = 0; r->right = cw; r->bottom = ch;
	if (!(f && f(r, style, FALSE, 0, (UINT)dpi))) AdjustWindowRect(r, style, FALSE);
}
/* the client size cw x ch, shrunk (aspect kept) until the window with its frame fits the monitor's work area */
static void own_fit(const XrcMon *m, DWORD style, int *cw, int *ch) {
	RECT r;
	int aw = m->ww > 0 ? m->ww : m->w, ah = m->wh > 0 ? m->wh : m->h;
	own_outer(style, m->dpi, *cw, *ch, &r);
	if (r.right - r.left > aw || r.bottom - r.top > ah) {
		int fw = (r.right - r.left) - *cw, fh = (r.bottom - r.top) - *ch;
		double s1 = (double)(aw - fw) / *cw, s2 = (double)(ah - fh) / *ch, sc = s1 < s2 ? s1 : s2;
		if (sc < 0.1) sc = 0.1;
		*cw = (int)(*cw * sc) & ~1; *ch = (int)(*ch * sc) & ~1;
	}
}
/* borderless fullscreen: the window covers the monitor it is on (physical pixels); the display the game is told follows it */
static void own_refit(HWND h) {
	XrcMon m;
	RECT r;
	if (!own_fs || !h) return;
	xrc_mon_get(h, &m);
	GetWindowRect(h, &r);
	if (r.left != m.x || r.top != m.y || r.right - r.left != m.w || r.bottom - r.top != m.h)
		SetWindowPos(h, HWND_TOP, m.x, m.y, m.w, m.h, SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}
static void own_set_fs(HWND h, int on) {
	XrcMon m;
	RECT r;
	int cw, ch;
	if (!h || on == own_fs) return;
	xrc_mon_get(h, &m);
	if (on) {
		if (GetClientRect(h, &r) && r.right - r.left >= 64) { win_cw = r.right - r.left; win_ch = r.bottom - r.top; }
		GetWindowRect(h, &r); win_x = r.left; win_y = r.top;
		own_fs = 1; xrc_disp_fs = 1;
		SetWindowLongA(h, GWL_STYLE, (LONG)(WS_POPUP | WS_VISIBLE));
		SetWindowPos(h, HWND_TOP, m.x, m.y, m.w, m.h, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
	} else {
		int ow, oh, x, y;
		own_fs = 0; xrc_disp_fs = 0;
		cw = win_cw; ch = win_ch;
		if (cw < 64 || ch < 64) xrc_default_client(&m, &cw, &ch);
		own_fit(&m, own_style, &cw, &ch);
		own_outer(own_style, m.dpi, cw, ch, &r);
		ow = r.right - r.left; oh = r.bottom - r.top;
		x = m.wx + (m.ww - ow) / 2; y = m.wy + (m.wh - oh) / 2;
		if (win_x != 0x7fffffff && win_x >= m.wx && win_y >= m.wy && win_x + ow <= m.wx + m.ww && win_y + oh <= m.wy + m.wh) { x = win_x; y = win_y; }
		if (x < m.wx) x = m.wx;
		if (y < m.wy) y = m.wy;
		SetWindowLongA(h, GWL_STYLE, (LONG)(own_style | WS_VISIBLE));
		SetWindowPos(h, HWND_NOTOPMOST, x, y, ow, oh, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
	}
	InterlockedIncrement((volatile LONG *)&xrc_disp_gen);
	GetClientRect(h, &r);
	fprintf(stderr, "xrc: vulkan: window %s (client %ldx%ld on the %dx%d monitor, dpi %d)\n", on ? "borderless fullscreen" : "windowed", (long)(r.right - r.left),
	        (long)(r.bottom - r.top), m.w, m.h, m.dpi);
}
void xrc_vk_set_borderless(int on) { if (window) PostMessageA((HWND)window, OWN_WM_BORDERLESS, (WPARAM)(on != 0), 0); }
/* a test (XRC_TEST_SEQ="secs:toggle,secs:fake=WxH@dpi,..."): F11 at that time, or the window moves to another monitor / dpi
 * (XRC_FAKE_MONITOR changes, and WM_DPICHANGED / WM_DISPLAYCHANGE arrive as Windows sends them) */
static DWORD WINAPI own_test_thread(LPVOID arg) {
	char *q = (char *)arg, *tok;
	DWORD t0 = GetTickCount();
	for (tok = strtok(q, ","); tok; tok = strtok(0, ",")) {
		int secs = atoi(tok);
		char *act = strchr(tok, ':');
		DWORD el = GetTickCount() - t0;
		if (!act) continue;
		act++;
		if ((DWORD)secs * 1000u > el) Sleep((DWORD)secs * 1000u - el);
		if (!window) continue;
		if (!strcmp(act, "toggle")) {
			fprintf(stderr, "xrc: test: F11 (toggle)\n");
			PostMessageA((HWND)window, OWN_WM_BORDERLESS, (WPARAM)!own_fs, 0);
		} else if (!strncmp(act, "fake=", 5)) {
			int fw, fh, fd = 96;
			if (sscanf(act + 5, "%dx%d@%d", &fw, &fh, &fd) >= 2) {
				RECT r;
				fprintf(stderr, "xrc: test: the window moves to a %dx%d monitor at dpi %d\n", fw, fh, fd);
				_putenv_s("XRC_FAKE_MONITOR", act + 5);
				GetWindowRect((HWND)window, &r);
				if (!own_fs) { /* (windowed: as Windows scales it for the new dpi) */
					RECT sr;
					sr.left = r.left; sr.top = r.top; sr.right = r.left + (r.right - r.left) * fd / own_dpi; sr.bottom = r.top + (r.bottom - r.top) * fd / own_dpi;
					SendMessageA((HWND)window, WM_DPICHANGED, (WPARAM)(fd | (fd << 16)), (LPARAM)&sr);
				} else SendMessageA((HWND)window, WM_DPICHANGED, (WPARAM)(fd | (fd << 16)), (LPARAM)&r);
				PostMessageA((HWND)window, WM_DISPLAYCHANGE, 32, MAKELPARAM(fw, fh));
			}
		}
	}
	return 0;
}
static LRESULT CALLBACK own_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
	if (m == OWN_WM_BORDERLESS) { own_set_fs(h, (int)wp); return 0; }
	if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP) &&
	    (wp == VK_F11 || (wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)))) {
		if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(lp & (1 << 30))) own_set_fs(h, !own_fs);
		return 0; /* (not the game's: Enter with Alt held is not a confirm) */
	}
	switch (m) {
#ifdef XRC_LR
	case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
		if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) break; /* (Alt+F4 still closes) */
		if (wp == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000) && !xrc_menu_is_open()) { /* Shift+F10: the old popup */
			if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) xrc_settings_menu(h);
			return 0;
		}
		/* F10 opens the overlay; while it is open every key is its (none reaches the game) */
		if (xrc_menu_key((unsigned)wp, m == WM_KEYDOWN || m == WM_SYSKEYDOWN, (lp & (1 << 30)) != 0)) return 0;
		break;
	case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MOUSEWHEEL:
		if (xrc_menu_is_open()) {
			RECT r;
			POINT p;
			p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
			if (m == WM_MOUSEWHEEL) ScreenToClient(h, &p); /* (its point is in screen coordinates) */
			GetClientRect(h, &r);
			xrc_menu_mouse(m == WM_MOUSEMOVE ? 0 : m == WM_LBUTTONDOWN ? 1 : m == WM_LBUTTONUP ? 2 : 3, (int)p.x, (int)p.y, (int)r.right, (int)r.bottom,
			               m == WM_MOUSEWHEEL ? (short)HIWORD(wp) : 0);
			return 0;
		}
		break;
#endif
	}
	switch (m) {
	case WM_CLOSE:
		fprintf(stderr, "xrc: vulkan: the window was closed\n");
		xrc_exit_now(0);
		return 0;
	case WM_KILLFOCUS: xrc_input_release_all(); break;
	case WM_DPICHANGED: { /* another monitor / dpi: windowed takes Windows' suggested rectangle (the same logical size), fullscreen covers the new monitor */
		RECT *sr = (RECT *)lp;
		own_dpi = (int)LOWORD(wp);
		if (!own_fs && sr) SetWindowPos(h, 0, sr->left, sr->top, sr->right - sr->left, sr->bottom - sr->top, SWP_NOZORDER | SWP_NOACTIVATE);
		else own_refit(h);
		InterlockedIncrement((volatile LONG *)&xrc_disp_gen);
		fprintf(stderr, "xrc: vulkan: dpi changed to %d (%d%%)\n", own_dpi, own_dpi * 100 / 96);
		return 0;
	}
	case WM_DISPLAYCHANGE: own_refit(h); InterlockedIncrement((volatile LONG *)&xrc_disp_gen); break;
	case WM_MOVE: own_refit(h); InterlockedIncrement((volatile LONG *)&xrc_disp_gen); break;
	case WM_SIZE:
		if (!own_fs && wp != SIZE_MINIMIZED && LOWORD(lp) >= 64 && HIWORD(lp) >= 64) { win_cw = LOWORD(lp); win_ch = HIWORD(lp); }
		InterlockedIncrement((volatile LONG *)&xrc_disp_gen);
		break;
	case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
		if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) break; /* Alt+F4: closes (WM_CLOSE) */
		if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && (lp & (1 << 30))) return 0; /* (auto-repeat: still down) */
		xrc_input_dik((int)((lp >> 16) & 0x7f) | ((lp & (1 << 24)) ? 0x80 : 0), m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
		return 0;
	case WM_SYSCHAR: return 0; /* (no beep for Alt+key) */
	}
	return DefWindowProcA(h, m, wp, lp);
}
static DWORD WINAPI own_thread(LPVOID unused) {
	WNDCLASSA wc;
	RECT r;
	MSG msg;
	XrcMon m;
	DWORD st = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
	int fs = !(getenv("XRC_WINDOW") && !strcmp(getenv("XRC_WINDOW"), "windowed"));
	int cw, ch, x, y, w, h;
	(void)unused;
	xrc_mon_get(0, &m); /* (the monitor under the cursor, else the primary: where the window is made) */
	own_style = st; own_dpi = m.dpi;
	xrc_default_client(&m, &cw, &ch);
	own_fit(&m, st, &cw, &ch);
	win_cw = cw; win_ch = ch;
	if (fs) { x = m.x; y = m.y; w = m.w; h = m.h; }
	else {
		own_outer(st, m.dpi, cw, ch, &r);
		w = r.right - r.left; h = r.bottom - r.top;
		x = m.wx + (m.ww - w) / 2; y = m.wy + (m.wh - h) / 2;
	}
	memset(&wc, 0, sizeof wc);
	wc.lpfnWndProc = own_proc;
	wc.hInstance = GetModuleHandleA(0);
	wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = "xrc_vk_window";
	RegisterClassA(&wc);
	own_fs = fs; xrc_disp_fs = fs;
	window = CreateWindowExA(0, wc.lpszClassName, "LIGHTNING RETURNS: FINAL FANTASY XIII (recomp)", (fs ? (DWORD)WS_POPUP : st) | WS_VISIBLE, x, y, w, h, 0, 0, wc.hInstance, 0);
	xrc_disp_hwnd = window;
	InterlockedIncrement((volatile LONG *)&xrc_disp_gen);
	if (window) {
		RECT cr;
		GetClientRect((HWND)window, &cr);
		fprintf(stderr, "xrc: vulkan: window %s, client %ldx%ld at %d,%d on the %dx%d monitor (dpi %d = %d%%; monitor from: %s)\n", fs ? "borderless fullscreen" : "windowed",
		        (long)(cr.right - cr.left), (long)(cr.bottom - cr.top), x, y, m.w, m.h, m.dpi, m.dpi * 100 / 96, m.src);
		if (getenv("XRC_TEST_SEQ")) CloseHandle(CreateThread(0, 0, own_test_thread, _strdup(getenv("XRC_TEST_SEQ")), 0, 0));
	}
	SetEvent(own_ready);
	if (!window) return 0;
	while (GetMessageA(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
	return 0;
}
static void *plat_device_window(void) { return window; }
static int plat_window(const char **iext, u32 *niext) {
	own_ready = CreateEventA(0, TRUE, FALSE, 0);
	CloseHandle(CreateThread(0, 0, own_thread, 0, 0, 0));
	WaitForSingleObject(own_ready, INFINITE);
	if (!window) { fprintf(stderr, "xrc: vulkan: no window (%lu): drawing without one\n", GetLastError()); return 0; }
	iext[(*niext)++] = "VK_KHR_surface";
	iext[(*niext)++] = "VK_KHR_win32_surface";
	return 1;
}
/* hires2: the window's client is what the game's display follows; only with XRC_WIDTH / XRC_HEIGHT set (the game's back buffer is
 * then not the window's) a windowed window is made that size, as before (fitted to the work area) */
static void plat_resize(int w, int h) {
	XrcMon m;
	RECT r;
	int cw = w, ch = h;
	if (w < 64 || h < 64 || own_fs || !window || !getenv("XRC_WIDTH") || !getenv("XRC_HEIGHT")) return;
	xrc_mon_get((HWND)window, &m);
	own_fit(&m, own_style, &cw, &ch);
	win_cw = cw; win_ch = ch;
	own_outer(own_style, m.dpi, cw, ch, &r);
	SetWindowPos((HWND)window, 0, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
}
#endif
static int plat_surface(VkInstance in, VkSurfaceKHR *out) {
	Win32SurfaceInfo si;
	PFN_CreateWin32Surface f = (PFN_CreateWin32Surface)vkGetInstanceProcAddr_(in, "vkCreateWin32SurfaceKHR");
	memset(&si, 0, sizeof si);
	si.sType = (VkStructureType)1000009000; /* VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR */
	si.hinstance = GetModuleHandleA(0);
	si.hwnd = (HWND)window;
	return f && f(in, &si, 0, out) >= 0;
}
static void plat_size(int *w, int *h) { RECT r; GetClientRect((HWND)window, &r); *w = r.right - r.left; *h = r.bottom - r.top; }
static void pump_events(void) {} /* (the game's own message loop, or our window's thread) */
#else
static void *sdl_lib;
static int (*SDL_Init_)(unsigned);
static void *(*SDL_CreateWindow_)(const char *, int, int, int, int, unsigned);
static int (*SDL_Vulkan_GetInstanceExtensions_)(void *, unsigned *, const char **);
static int (*SDL_Vulkan_CreateSurface_)(void *, VkInstance, VkSurfaceKHR *);
static void (*SDL_Vulkan_GetDrawableSize_)(void *, int *, int *);
static void (*SDL_SetWindowSize_)(void *, int, int);
static int (*SDL_PollEvent_)(void *);
static const char *(*SDL_GetError_)(void);
static void (*SDL_GetWindowSize_)(void *, int *, int *);
static int (*SDL_SetRelativeMouseMode_)(int);
static int (*SDL_SetWindowFullscreen_)(void *, unsigned);

static int sdl_load(void) {
	const char *names[] = {"libSDL2-2.0.so.0", "libSDL2.so", "libSDL2-2.0.0.dylib", 0};
	int k;
	for (k = 0; names[k] && !sdl_lib; k++) sdl_lib = dlopen(names[k], RTLD_NOW | RTLD_LOCAL);
	if (!sdl_lib) return 0;
#define SYM(n) if (!(*(void **)&n##_ = dlsym(sdl_lib, #n))) return 0;
	SYM(SDL_Init) SYM(SDL_CreateWindow) SYM(SDL_Vulkan_GetInstanceExtensions) SYM(SDL_Vulkan_CreateSurface)
	SYM(SDL_Vulkan_GetDrawableSize) SYM(SDL_PollEvent) SYM(SDL_GetError) SYM(SDL_SetWindowSize)
	*(void **)&SDL_GetWindowSize_ = dlsym(sdl_lib, "SDL_GetWindowSize"); /* (optional: the mouse scaled, captured) */
	*(void **)&SDL_SetRelativeMouseMode_ = dlsym(sdl_lib, "SDL_SetRelativeMouseMode");
	*(void **)&SDL_SetWindowFullscreen_ = dlsym(sdl_lib, "SDL_SetWindowFullscreen"); /* (optional: F11) */
#undef SYM
	return 1;
}
/* the window's events: closing it ends the game; keyboard and mouse go to the DirectInput HLE (SDL2 event layouts) */
static void pump_events(void) {
	union { unsigned type; unsigned char b[64]; int i[16]; } ev;
	static int rel = -1, focused = 1;
	int ww = 0, wh = 0, gw = (int)dev.pp[0], gh = (int)dev.pp[1], vx0 = 0, vy0 = 0;
	if (!window) return;
	/* while the game's camera reads the cursor (mouse look: it recentres it every frame) the pointer is captured, as on
	 * Windows; in menus, the title and cutscenes it is free (the window can be moved). XRC_NO_MOUSE_GRAB=1: never. */
	if (SDL_SetRelativeMouseMode_) {
		int want = xrc_user32_mouse_look() && focused && !getenv("XRC_NO_MOUSE_GRAB");
		if (want != rel) { rel = want; SDL_SetRelativeMouseMode_(want); }
	}
	if (SDL_GetWindowSize_) SDL_GetWindowSize_(window, &ww, &wh);
	if (ww <= 0 || wh <= 0 || gw <= 0 || gh <= 0) { ww = gw = 1; wh = gh = 1; }
	{ /* the picture is scaled to fit the window, aspect kept (be_present): the pointer is mapped from its part of the
	   * window (window points: the drawable may be larger, the proportions are the same) */
		int vw, vh;
		if (xrc_view_w == gw && xrc_view_h == gh && xrc_view_map(ww, wh, &vx0, &vy0, &vw, &vh)) { ww = vw; wh = vh; }
	}
	while (SDL_PollEvent_(&ev)) {
		switch (ev.type) {
		case 0x100: /* SDL_QUIT */
			fprintf(stderr, "xrc: vulkan: the window was closed\n");
			xrc_exit_now(0); /* (sound stopped first) */
		case 0x200: /* SDL_WINDOWEVENT: focus lost (13) -> nothing held */
			if (ev.b[12] == 13) { xrc_input_release_all(); xrc_user32_mouse_release_all(); focused = 0; }
			if (ev.b[12] == 12) focused = 1; /* (focus gained) */
			break;
		case 0x300: case 0x301: /* SDL_KEYDOWN / UP: keysym.scancode at 16 (repeats change nothing) */
			if (ev.i[4] == 65) { /* F8 (SDL_SCANCODE_F8): the HD texture set drawn -- the originals, XRC_TEX_DIR, then each XRC_TEX_DIRS set */
				if (ev.type == 0x300 && !ev.b[13]) xrc_vk_tex_cycle();
				break;
			}
			if (ev.i[4] == 64) { /* F7 (SDL_SCANCODE_F7): the meshes of the next frame saved (md_frame_end) */
				if (ev.type == 0x300 && !ev.b[13]) xrc_vk_mesh_request();
				break;
			}
			if (ev.i[4] == 68) { /* F11 (SDL_SCANCODE_F11): fullscreen (the desktop's mode) on / off -- not the game's */
				static int fs;
				if (ev.type == 0x300 && !ev.b[13] && SDL_SetWindowFullscreen_ && SDL_SetWindowFullscreen_(window, fs ? 0 : 0x1001u) == 0) fs = !fs;
				break;
			}
#ifndef XRC_NO_TOOLS
			if (ev.type == 0x300 && ev.i[4] == 41) { /* Escape (SDL_SCANCODE_ESCAPE), Debug & Profile Tools on: ends the game at once (ref_user32.c) */
				fprintf(stderr, "xrc: Escape: ending the game\n");
				xrc_exit_now(0);
			}
#endif
			xrc_input_key(ev.i[4], ev.type == 0x300);
			break;
		case 0x400: /* SDL_MOUSEMOTION: x y at 20, xrel yrel at 28 (window points: scaled to the game's pixels) */
			xrc_input_mouse(ev.i[7], ev.i[8], 0);
			xrc_user32_mouse_move((ev.i[7] - vx0) * gw / ww, (ev.i[8] - vy0) * gh / wh, rel == 1 ? -1 : ev.i[5] * gw / ww, rel == 1 ? -1 : ev.i[6] * gh / wh);
			break;
		case 0x401: case 0x402: /* SDL_MOUSEBUTTONDOWN / UP: the button at 16 */
			xrc_input_button(ev.b[16], ev.type == 0x401);
			xrc_user32_mouse_button(ev.b[16], ev.type == 0x401);
			break;
		case 0x403: /* SDL_MOUSEWHEEL: y at 20 (WHEEL_DELTA a notch) */
			xrc_input_mouse(0, 0, ev.i[5] * 120);
			xrc_user32_mouse_wheel(ev.i[5] * 120);
			break;
		}
	}
}
static int plat_window(const char **iext, u32 *niext) {
	int sc = getenv("XRC_VK_SCALE") ? atoi(getenv("XRC_VK_SCALE")) : 1;
	unsigned ne = 0, k;
	const char *names[8];
	setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0); /* (SIGINT / SIGTERM stay ours: SDL would make them an event) */
	if (!(getenv("DISPLAY") || getenv("WAYLAND_DISPLAY")) || !sdl_load() || SDL_Init_(0x20) != 0) return 0;
	if (sc < 1) sc = 1;
	window = SDL_CreateWindow_("FINAL FANTASY XIII", 0x2FFF0000, 0x2FFF0000, (int)dev.pp[0] * sc, (int)dev.pp[1] * sc, 0x10000000u | 0x20u);
	if (!window) { fprintf(stderr, "xrc: vulkan: no window (%s): drawing without one\n", SDL_GetError_()); return 0; }
	if (SDL_Vulkan_GetInstanceExtensions_(window, &ne, 0) && ne <= 8 && SDL_Vulkan_GetInstanceExtensions_(window, &ne, names))
		for (k = 0; k < ne; k++) iext[(*niext)++] = names[k];
	return 1;
}
static int plat_surface(VkInstance in, VkSurfaceKHR *out) { return SDL_Vulkan_CreateSurface_(window, in, out) != 0; }
static void plat_size(int *w, int *h) { SDL_Vulkan_GetDrawableSize_(window, w, h); }
static void *plat_device_window(void) { return window; } /* (our own window, for every device) */
static void plat_resize(int w, int h) {
	int sc = getenv("XRC_VK_SCALE") ? atoi(getenv("XRC_VK_SCALE")) : 1;
	if (sc < 1) sc = 1;
	SDL_SetWindowSize_(window, w * sc, h * sc);
}
#endif

/* ============================================================================================ the device */
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice vd;
static VkQueue queue;
static u32 qfam;
static VkPhysicalDeviceProperties props;
static VkPhysicalDeviceMemoryProperties memprops;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static int swap_ok;
static int swap_plat_w, swap_plat_h; /* the window size the swap chain was made for (make_swapchain) */
static VkFormat swap_fmt;
static VkExtent2D swap_ext;
static VkImage swap_img[8];
static VkSemaphore swap_done[8];
#define VK_COMMA_ ,
#include "vk_cs.h"
static u32 nswap;
static int have_aniso, have_bc, have_independent_blend, have_fill_nonsolid;
static int have_pstats __attribute__((unused)); /* (XRC_VK_DRAWTIME) */
static VkFormat fmt_d24s8, fmt_d24x8;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout playout;
static VkPipelineCache pcache;
static double pc_new_ms;     /* when the last new pipeline was made (0: none since the last save) */
static volatile int pc_saving;
static VkCommandPool cpool;

/* ---------------------------------------------------------------------------------------- memory */
#define BLOCK (64u << 20)
typedef struct { VkDeviceSize off, size; } Seg;
typedef struct { VkDeviceMemory mem; VkDeviceSize size; u32 type; int linear; void *map; Seg *free; u32 nfree, capfree; } Block;
typedef struct { int block; VkDeviceSize off, size; } Alloc;
static Block *blocks;
static int nblocks;

static u32 mem_type(u32 bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
	u32 k;
	for (k = 0; k < memprops.memoryTypeCount; k++)
		if ((bits & (1u << k)) && (memprops.memoryTypes[k].propertyFlags & want) == want && !(memprops.memoryTypes[k].propertyFlags & avoid)) return k;
	for (k = 0; k < memprops.memoryTypeCount; k++)
		if ((bits & (1u << k)) && (memprops.memoryTypes[k].propertyFlags & want) == want) return k;
	return 0xffffffffu;
}
static void seg_insert(Block *b, VkDeviceSize off, VkDeviceSize size) {
	u32 k, i;
	for (i = 0; i < b->nfree && b->free[i].off < off; i++) {}
	if (b->nfree == b->capfree) { b->capfree = b->capfree ? b->capfree * 2 : 16; b->free = (Seg *)realloc(b->free, b->capfree * sizeof(Seg)); }
	for (k = b->nfree; k > i; k--) b->free[k] = b->free[k - 1];
	b->free[i].off = off; b->free[i].size = size;
	b->nfree++;
	/* merge with the neighbours */
	if (i + 1 < b->nfree && b->free[i].off + b->free[i].size == b->free[i + 1].off) {
		b->free[i].size += b->free[i + 1].size;
		for (k = i + 1; k + 1 < b->nfree; k++) b->free[k] = b->free[k + 1];
		b->nfree--;
	}
	if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == b->free[i].off) {
		b->free[i - 1].size += b->free[i].size;
		for (k = i; k + 1 < b->nfree; k++) b->free[k] = b->free[k + 1];
		b->nfree--;
	}
}
/* the spare blocks: per memory type and linear-ness, one BLOCK allocated (and mapped) ahead by a thread.
 * state 0 none, 1 being allocated, 2 ready (mem / map valid) */
typedef struct { VkDeviceMemory mem; void *map; volatile int state; u32 type; } Spare;
static Spare spares[VK_MAX_MEMORY_TYPES][2];
static unsigned n_spare_used, n_block_sync;
static void *spare_alloc(void *arg) {
#if !defined(_WIN32) && !defined(XRC_NO_TOOLS)
	pthread_setname_np(pthread_self(), "vk-spare");
#endif
	Spare *sp = (Spare *)arg;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = BLOCK;
	ai.memoryTypeIndex = sp->type;
	sp->map = 0;
	if (vkAllocateMemory(vd, &ai, 0, &sp->mem) != VK_SUCCESS) { sp->mem = VK_NULL_HANDLE; __atomic_store_n(&sp->state, 0, __ATOMIC_RELEASE); return 0; }
	if ((memprops.memoryTypes[sp->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
	    && vkMapMemory(vd, sp->mem, 0, BLOCK, 0, &sp->map) != VK_SUCCESS) {
		vkFreeMemory(vd, sp->mem, 0); sp->mem = VK_NULL_HANDLE;
		__atomic_store_n(&sp->state, 0, __ATOMIC_RELEASE); return 0;
	}
	__atomic_store_n(&sp->state, 2, __ATOMIC_RELEASE);
	return 0;
}
static void spare_start(u32 type, int linear) {
	Spare *sp = &spares[type][linear != 0];
	pthread_t t;
	if (__atomic_load_n(&sp->state, __ATOMIC_ACQUIRE) != 0) return;
	sp->type = type;
	sp->state = 1;
	if (pthread_create(&t, 0, spare_alloc, sp) != 0) { sp->state = 0; return; }
	pthread_detach(t);
}
static Alloc mem_alloc(VkMemoryRequirements rq, VkMemoryPropertyFlags want, int linear) {
	Alloc a = {-1, 0, 0};
	u32 type = mem_type(rq.memoryTypeBits, want, want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? 0 : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	int k;
	u32 i;
	if (type == 0xffffffffu) die("finding a memory type", VK_ERROR_OUT_OF_DEVICE_MEMORY);
	for (k = 0; k <= nblocks; k++) {
		Block *b;
		if (k == nblocks) { /* a new block */
			VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			blocks = (Block *)realloc(blocks, (size_t)(nblocks + 1) * sizeof(Block));
			b = &blocks[nblocks];
			memset(b, 0, sizeof *b);
			b->size = rq.size > BLOCK ? (rq.size + 0xffff) & ~(VkDeviceSize)0xffff : BLOCK;
			b->type = type;
			b->linear = linear;
			ai.allocationSize = b->size;
			ai.memoryTypeIndex = type;
			if (b->size == BLOCK && __atomic_load_n(&spares[type][linear != 0].state, __ATOMIC_ACQUIRE) == 2) { /* the spare, made ahead */
				Spare *sp = &spares[type][linear != 0];
				b->mem = sp->mem; b->map = sp->map;
				sp->mem = VK_NULL_HANDLE; sp->map = 0;
				__atomic_store_n(&sp->state, 0, __ATOMIC_RELEASE);
				n_spare_used++;
			} else {
				double ta = now_ms();
				VKC(vkAllocateMemory(vd, &ai, 0, &b->mem));
				if (memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) VKC(vkMapMemory(vd, b->mem, 0, b->size, 0, &b->map));
				n_block_sync++;
				HC(alloc, now_ms() - ta); HC(nalloc, 1); (void)ta;
			}
			seg_insert(b, 0, b->size);
			nblocks++;
			LOG("xrc: vulkan: memory block %d (type %u%s, %llu MB; %u from spares, %u allocated in the frame)\n", nblocks - 1, type,
			    linear ? " linear" : "", (unsigned long long)(b->size >> 20), n_spare_used, n_block_sync);
			spare_start(type, linear); /* the next one, ahead */
		}
		b = &blocks[k];
		if (b->type != type || b->linear != linear) continue;
		for (i = 0; i < b->nfree; i++) {
			VkDeviceSize al = rq.alignment ? rq.alignment : 1, off = (b->free[i].off + al - 1) / al * al;
			if (off + rq.size <= b->free[i].off + b->free[i].size) {
				Seg s = b->free[i];
				u32 j;
				for (j = i; j + 1 < b->nfree; j++) b->free[j] = b->free[j + 1];
				b->nfree--;
				if (off > s.off) seg_insert(b, s.off, off - s.off);
				if (off + rq.size < s.off + s.size) seg_insert(b, off + rq.size, s.off + s.size - off - rq.size);
				a.block = k; a.off = off; a.size = rq.size;
				return a;
			}
		}
	}
	return a;
}
static void mem_free(Alloc a) { if (a.block >= 0) seg_insert(&blocks[a.block], a.off, a.size); }

/* ---------------------------------------------------------------------------------------- frames */
#define NFRAMES 3 /* frames in flight: the GPU may be up to 2 frames behind before the CPU waits (begin_frame) */
#ifndef XRC_NO_TOOLS
typedef char hitch_slots_ok[sizeof hitch / sizeof hitch[0] == NFRAMES ? 1 : -1];
#endif
#define RING (64u << 20)
typedef struct Res Res;
typedef struct {
	VkCommandBuffer cb;
	VkFence fence;
	VkSemaphore acquired;
	VkBuffer ring;
	Alloc ring_mem;
	unsigned char *ring_map;
	VkDeviceSize ring_used;
	Res **dead; u32 ndead, capdead;      /* released while this frame may use them */
	VkBuffer *dead_buf; Alloc *dead_buf_mem; u32 ndeadb, capdeadb;
	int submitted;
	unsigned long long serial; /* (the render thread: the stream serial of its last submit) */
	int ts_written; /* (tools builds: the GPU timestamps of this frame's command buffer) */
} Frame;
static Frame frames[NFRAMES];
static int fi;            /* the frame being recorded */
#define F (&frames[fi])
static VkCommandBuffer cb;
static unsigned long long frame_no;

/* ---------------------------------------------------------------------------------------- resources */
struct Res {
	int is_buf;
	/* buffers */
	VkBuffer buf;
	u32 size, dlo, dhi;        /* the dirty range (dhi > dlo: to upload) */
	unsigned long long lock_frame; u32 lock_frames; int hot; /* locked in many frames: streamed like a dynamic buffer */
	/* images */
	VkImage img;
	VkImageView view;          /* the sampled view (D3D's channel mapping) */
	VkImageView *att;          /* per face * level: attachment views (identity), made when first needed */
	VkFormat fmt;
	VkComponentMapping sw;
	u32 d3dfmt, w, h, d, levels, layers, bpp_block;
	int depth, stencil, cube, vol, blend_ok, kind;
	u32 dirty[6];              /* per face: levels to upload */
	Alloc mem;
	u32 samples;               /* 1, or a multisampled render target / depth surface (D3D's MultiSample: -MSAA=N) */
	struct Res *resolved;      /* (multisampled) its one-sample twin, resolved into for copies and read-backs */
	VkImageLayout lay;         /* the layout it is in (all its subresources) */
	u8 pw, pr, tch;            /* written / read since the last sync; in tch[] */
	u8 can_att;                /* made with the color attachment usage */
	u8 narrow;                 /* R32F made R16F (see dist16) */
	u8 unpack;                 /* a DXT / ATI2 texture on a GPU without BC: RGBA8, decoded at upload (dxt_decode) */
	unsigned long long zread;  /* (depth) the frame its contents were last read (+1; 0: never): sampled, copied, locked, or a
	                            * rendering loading them -- see zout_drop */
	/* HD texture replacement (tex_check): its top level's hash, the state (0 not seen, 1 none, 2 loading, 3 replaced,
	 * 4 not replaced: rewritten too often or not a kind replaced), how often its contents changed, the replacement */
	unsigned long long th;
	u8 tstate, tchanges, tui; /* (tui: one mip level -- UI, fonts, logos: shown only with tex_ui_show) */
	struct Res *repl;
	struct TJob *job;
	struct Res *repl_x[8]; /* (XRC_TEX_DIRS / XRC_TEX_DIR_B: more sets, set k drawn while tex_alt == k + 1 -- F8, texb shots) */
	struct TJob *job_x[8];
};
#define ZREAD(r) ((r)->zread = frame_no + 1)

static void image_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = from; b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange.aspectMask = aspect;
	b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}
static VkImageAspectFlags img_aspect(const Res *r) {
	return r->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (r->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
}
static double n_relayout, n_split, n_bar; /* (XRC_VK_LOG: layout changes, renderings split for one, barriers) */

/* ---- hazards. An image's uses since the last sync: pw / pr (and in tch[]); the stages and writes of every use since
 * (acc_*). A use that depends on an earlier one (a read or write after a write, a write after a read) asks for a sync
 * (sync_need): a memory barrier from those stages to the use's. Layout changes (bq_add) are image barriers, which sync
 * their image. Both wait in the batch until bq_go records them -- as one barrier -- and then the batch's uses are
 * marked (pending: pm[]). Every use: img_use / img_access, then bq_go, then its command (outside a rendering). */
#define TCH_MAX 512
static Res *tch[TCH_MAX];
static u32 ntch;
static VkPipelineStageFlags acc_st;   /* the stages of the uses since the last sync */
static VkAccessFlags acc_wr;          /* their writes */
static int bufs_written, draws_since; /* a buffer copied into / a draw (its vertex reads) since the last sync */
static VkImageMemoryBarrier bq[64];
static u32 nbq;
static VkPipelineStageFlags bq_src, bq_dst, bq_gdst;
static VkAccessFlags bq_gdacc;
static int bq_global, bq_full, full_barriers = -1;
typedef struct { Res *r; int w; VkPipelineStageFlags st; VkAccessFlags acc; } PMark;
static PMark pm[64];
static u32 npm;
static void sync_need(VkPipelineStageFlags dst, VkAccessFlags dacc) { bq_global = 1; bq_gdst |= dst; bq_gdacc |= dacc; }
static void lay_scope(VkImageLayout l, int dst, VkPipelineStageFlags *st, VkAccessFlags *ac) {
	switch (l) {
	case VK_IMAGE_LAYOUT_UNDEFINED: *st = dst ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; *ac = 0; break;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		*st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		*ac = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (dst ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT : 0);
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		*st = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		*ac = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | (dst ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT : 0);
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		*st = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		*ac = dst ? VK_ACCESS_SHADER_READ_BIT : 0;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: *st = VK_PIPELINE_STAGE_TRANSFER_BIT; *ac = dst ? VK_ACCESS_TRANSFER_READ_BIT : 0; break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: *st = VK_PIPELINE_STAGE_TRANSFER_BIT; *ac = VK_ACCESS_TRANSFER_WRITE_BIT; break;
	default: *st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; *ac = VK_ACCESS_MEMORY_WRITE_BIT | (dst ? VK_ACCESS_MEMORY_READ_BIT : 0); break;
	}
}
static void bq_go(void);
/* image r into layout `to` (batched: recorded by bq_go; from UNDEFINED -- a new image -- its contents go) */
static void bq_add(Res *r, VkImageLayout to) {
	VkImageMemoryBarrier *b;
	VkPipelineStageFlags ss, ds;
	VkAccessFlags sa, da;
	if (r->lay == to) return;
	if (nbq == sizeof bq / sizeof bq[0]) bq_go();
	lay_scope(r->lay, 0, &ss, &sa);
	lay_scope(to, 1, &ds, &da);
	if (r->lay == VK_IMAGE_LAYOUT_GENERAL) ss |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	b = &bq[nbq++];
	memset(b, 0, sizeof *b);
	b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b->srcAccessMask = sa; b->dstAccessMask = da;
	b->oldLayout = r->lay; b->newLayout = to;
	b->srcQueueFamilyIndex = b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b->image = r->img;
	b->subresourceRange.aspectMask = img_aspect(r);
	b->subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	b->subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
	bq_src |= ss; bq_dst |= ds;
	r->lay = to;
	r->pw = r->pr = 0; /* (this barrier syncs it) */
	n_relayout++;
}
/* a use of image r (read or write) in stages st, its accesses acc: a sync first when it depends on an earlier one */
static void img_access(Res *r, int w, VkPipelineStageFlags st, VkAccessFlags acc) {
	if (full_barriers < 0) full_barriers = getenv("XRC_VK_FULL_BARRIERS") && atoi(getenv("XRC_VK_FULL_BARRIERS"));
	if (r->pw || (w && r->pr) || full_barriers) sync_need(st, acc);
	if (npm == sizeof pm / sizeof pm[0]) bq_go();
	pm[npm].r = r; pm[npm].w = w; pm[npm].st = st; pm[npm].acc = acc;
	npm++;
}
static void bq_go(void) {
	u32 k;
	if (bq_global || nbq) {
		VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		VkPipelineStageFlags src = bq_src, dst = bq_dst;
		if (bq_global) {
			mb.srcAccessMask = bq_full ? VK_ACCESS_MEMORY_WRITE_BIT : acc_wr;
			mb.dstAccessMask = bq_full ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : bq_gdacc;
			src |= bq_full ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : acc_st;
			dst |= bq_full ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : bq_gdst;
		}
		if (!src) src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		if (!dst) dst = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		vkCmdPipelineBarrier(cb, src, dst, 0, bq_global ? 1 : 0, &mb, 0, 0, nbq, bq);
		n_bar++;
#ifndef XRC_NO_TOOLS
		if (ppool && pp_rec[fi] && !hitch_ms) { /* (XRC_VK_PASSES: the frame's barriers, with their layout changes) */
			fprintf(stderr, "xrc: vulkan: (the frame of the renderings below) barrier before rendering %u: src %x dst %x%s", pp_n[fi], (u32)src, (u32)dst, bq_global ? (bq_full ? " full" : " global") : "");
			for (k = 0; k < nbq; k++) fprintf(stderr, " | img %p %d->%d", (void *)bq[k].image, (int)bq[k].oldLayout, (int)bq[k].newLayout);
			fprintf(stderr, "\n");
		}
#endif
		if (bq_global) { /* (everything before is synced) */
			for (k = 0; k < ntch; k++) { tch[k]->pw = tch[k]->pr = 0; tch[k]->tch = 0; }
			ntch = 0;
			acc_st = 0; acc_wr = 0;
			bufs_written = draws_since = 0;
		}
		nbq = 0; bq_src = bq_dst = bq_gdst = 0; bq_gdacc = 0; bq_global = bq_full = 0;
	}
	for (k = 0; k < npm; k++) { /* the batch's uses, after its barrier */
		Res *r = pm[k].r;
		if (pm[k].w) r->pw = 1; else r->pr = 1;
		if (!r->tch) {
			if (ntch == TCH_MAX) { /* (too many: all synced, by the next barrier -- a full one) */
				bq_global = bq_full = 1;
			} else { tch[ntch++] = r; r->tch = 1; }
		}
		acc_st |= pm[k].st;
		if (pm[k].w) acc_wr |= pm[k].acc;
	}
	npm = 0;
}
/* a full barrier: everything before, everything after */
static void barrier(void) { bq_global = bq_full = 1; bq_go(); }
/* image r into layout `to` now (outside a rendering) */
static void img_use(Res *r, VkImageLayout to) { bq_add(r, to); bq_go(); }
/* r is destroyed: out of the tracking */
static void img_untrack(Res *r) {
	u32 k;
	if (r->tch) for (k = 0; k < ntch; k++) if (tch[k] == r) { tch[k] = tch[--ntch]; break; }
	for (k = 0; k < npm; k++) if (pm[k].r == r) { pm[k] = pm[--npm]; k--; }
}
#define IMG_R_XFER(r) img_access((r), 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT)
#define IMG_W_XFER(r) img_access((r), 1, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)

/* ---------------------------------------------------------------------------------------- rendering state */
static int in_render;
static u32 cur_rt[4], cur_ds;     /* the attachments of the rendering in progress (guest surfaces) */
static VkExtent2D cur_extent;
/* A rendering is begun on the GPU (vkCmdBeginRendering: render_go) only at its first draw or partial clear: a clear of
 * the whole target before that becomes the attachments' load op (CLEAR instead of LOAD: the old contents are not read,
 * and no separate clearing pass), and a rendering with nothing in it is not made at all. */
static VkRenderingAttachmentInfo r_ca[4], r_da, r_sa;
static VkRenderingInfo r_ri;
static int r_pending, r_cleared, r_folded;
static void render_go(void);
/* Deferred clears: a rendering that only cleared (its load ops) is not made; its clears wait on their images (pcl) and
 * become the load ops of the next rendering of the whole image -- or are made by themselves (clear_emit) before anything
 * else uses the image: sampling, a copy, a fill, an upload, a read-back, the present. (The game clears targets well
 * before it draws to them: each was a pass of its own, ~0.4 ms at 720p.) XRC_VK_DEFER_CLEARS=0 (tools builds): off. */
typedef struct { Res *r; u32 face, level, w, h; VkImageAspectFlags asp; VkClearValue cv, dv; } PClear;
static PClear pcl[16];
static int npcl, defer_clears = -1;
static Res *r_res[4], *r_dres;   /* the attachments of the rendering being set up (begin_render), as images */
static u32 r_fl[4][2], r_dfl[2]; /* (their face, level) */
static int pcl_find(Res *r, u32 face, u32 level) {
	int i;
	for (i = 0; i < npcl; i++) if (pcl[i].r == r && pcl[i].face == face && pcl[i].level == level) return i;
	return -1;
}
static void pcl_drop(int i) { pcl[i] = pcl[--npcl]; }
static void clear_emit(int i) { /* (outside a rendering) */
	PClear *c = &pcl[i];
	VkImageSubresourceRange sr;
	sr.baseMipLevel = c->level; sr.levelCount = 1; sr.baseArrayLayer = c->face; sr.layerCount = 1;
	if (c->asp & VK_IMAGE_ASPECT_COLOR_BIT) {
		sr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		bq_add(c->r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); IMG_W_XFER(c->r); bq_go();
		vkCmdClearColorImage(cb, c->r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c->cv.color, 1, &sr);
	}
	if (c->asp & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
		sr.aspectMask = c->asp & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
		bq_add(c->r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); IMG_W_XFER(c->r); bq_go();
		vkCmdClearDepthStencilImage(cb, c->r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c->dv.depthStencil, 1, &sr);
	}
	pcl_drop(i);
}
static void end_render(void);
static void barrier(void);
static unsigned long long dyn_gen = 1; /* (bumped by everything that may set dynamic state / push descriptors besides be_draw2: the draw state caches (L, dc) are dropped when it moves) */
/* the deferred clears of image r (0: all) made now */
static void clears_flush(Res *r) {
	int i, any = 0;
	for (i = 0; i < npcl && r && pcl[i].r != r; i++) {}
	if (i == npcl) return; /* (none of r's) */
	if (in_render) end_render();
	for (i = npcl - 1; i >= 0; i--) if (!r || pcl[i].r == r) { clear_emit(i); any = 1; }
	(void)any;
}
static void clears_forget(Res *r) { int i; for (i = npcl - 1; i >= 0; i--) if (pcl[i].r == r) pcl_drop(i); }
static void pcl_add(Res *r, u32 face, u32 level, VkImageAspectFlags asp, const VkClearValue *v) {
	int i = pcl_find(r, face, level);
	if (i < 0) {
		if (npcl == (int)(sizeof pcl / sizeof pcl[0])) { if (in_render) return; clear_emit(0); } /* (full: the oldest made) */
		i = npcl++;
		memset(&pcl[i], 0, sizeof pcl[i]);
		pcl[i].r = r; pcl[i].face = face; pcl[i].level = level;
	}
	pcl[i].asp |= asp;
	pcl[i].w = r_ri.renderArea.extent.width; pcl[i].h = r_ri.renderArea.extent.height;
	if (asp & VK_IMAGE_ASPECT_COLOR_BIT) pcl[i].cv = *v;
	if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) pcl[i].dv.depthStencil.depth = v->depthStencil.depth;
	if (asp & VK_IMAGE_ASPECT_STENCIL_BIT) pcl[i].dv.depthStencil.stencil = v->depthStencil.stencil;
}
/* every attachment the pending rendering clears is wholly in it */
static int pcl_whole(void) {
	u32 k, w = r_ri.renderArea.extent.width, h = r_ri.renderArea.extent.height;
	for (k = 0; k < r_ri.colorAttachmentCount; k++)
		if (r_ca[k].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && (dim(r_res[k]->w, r_fl[k][1]) != w || dim(r_res[k]->h, r_fl[k][1]) != h)) return 0;
	if (r_ri.pDepthAttachment && (r_da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || r_sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) &&
	    (dim(r_dres->w, r_dfl[1]) != w || dim(r_dres->h, r_dfl[1]) != h)) return 0;
	return 1;
}
static void end_render(void) {
	if (!in_render) return;
	dyn_gen++; /* (an ended rendering: post passes and resolves may have set their own state) */
	if (r_pending && !r_cleared) { r_pending = 0; in_render = 0; return; } /* (nothing drawn or cleared: no rendering) */
	if (defer_clears < 0) defer_clears = !xrc_tool_env("XRC_VK_DEFER_CLEARS") || atoi(xrc_tool_env("XRC_VK_DEFER_CLEARS"));
	if (r_pending && defer_clears && npcl + 5 <= (int)(sizeof pcl / sizeof pcl[0]) && pcl_whole()) { /* only clears: deferred */
		u32 k;
		for (k = 0; k < r_ri.colorAttachmentCount; k++)
			if (r_ca[k].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) pcl_add(r_res[k], r_fl[k][0], r_fl[k][1], VK_IMAGE_ASPECT_COLOR_BIT, &r_ca[k].clearValue);
		if (r_ri.pDepthAttachment && r_da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) pcl_add(r_dres, r_dfl[0], r_dfl[1], VK_IMAGE_ASPECT_DEPTH_BIT, &r_da.clearValue);
		if (r_ri.pStencilAttachment && r_sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) pcl_add(r_dres, r_dfl[0], r_dfl[1], VK_IMAGE_ASPECT_STENCIL_BIT, &r_sa.clearValue);
		r_pending = 0; in_render = 0;
		return;
	}
	render_go();
	vkCmdEndRendering(cb);
#ifndef XRC_NO_TOOLS
	if (pp_open) {
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ppool, (u32)fi * 2u * PP_MAX + 2u * pp_n[fi] + 1u);
		pp_n[fi]++;
		pp_open = 0;
	}
#endif
	in_render = 0;
}

static void submit_frame(int wait_acquired, VkSemaphore signal);
static void flush_wait(void);

/* ring allocation (the current frame's): size bytes aligned; when full, everything so far is submitted and waited for */
static VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align, void **p) {
	VkDeviceSize off = (F->ring_used + align - 1) / align * align;
	if (size > RING) die("a ring allocation larger than the ring", VK_ERROR_OUT_OF_DEVICE_MEMORY);
	if (off + size > RING) {
		LOG("xrc: vulkan: the ring is full: waiting for the GPU\n");
		flush_wait();
		off = 0;
	}
	F->ring_used = off + size;
	*p = F->ring_map + off;
	return off;
}

/* ============================================================================================ formats */
static VkComponentMapping SW(VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b, VkComponentSwizzle a) {
	VkComponentMapping m = {r, g, b, a};
	return m;
}
#define I_ VK_COMPONENT_SWIZZLE_IDENTITY
#define ONE_ VK_COMPONENT_SWIZZLE_ONE
#define ZERO_ VK_COMPONENT_SWIZZLE_ZERO
#define R_ VK_COMPONENT_SWIZZLE_R
#define G_ VK_COMPONENT_SWIZZLE_G
#define B_ VK_COMPONENT_SWIZZLE_B
#define A__ VK_COMPONENT_SWIZZLE_A
#define FOURCC(a, b, c_, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c_) << 16 | (u32)(d) << 24)
static VkFormat vk_format(u32 f, VkComponentMapping *sw, int *depth, int *stencil) {
	*sw = SW(I_, I_, I_, I_);
	*depth = *stencil = 0;
	switch (f) {
	case 21: return VK_FORMAT_B8G8R8A8_UNORM;
	case 22: *sw = SW(I_, I_, I_, ONE_); return VK_FORMAT_B8G8R8A8_UNORM;
	case 23: return VK_FORMAT_R5G6B5_UNORM_PACK16;
	case 24: *sw = SW(I_, I_, I_, ONE_); return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
	case 25: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
	case 26: *sw = SW(G_, B_, A__, R_); return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
	case 28: *sw = SW(ZERO_, ZERO_, ZERO_, R_); return VK_FORMAT_R8_UNORM;
	case 31: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
	case 32: return VK_FORMAT_R8G8B8A8_UNORM;
	case 33: *sw = SW(I_, I_, I_, ONE_); return VK_FORMAT_R8G8B8A8_UNORM;
	case 34: *sw = SW(I_, I_, ONE_, ONE_); return VK_FORMAT_R16G16_UNORM;
	case 35: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	case 36: return VK_FORMAT_R16G16B16A16_UNORM;
	case 50: *sw = SW(R_, R_, R_, ONE_); return VK_FORMAT_R8_UNORM;
	case 51: *sw = SW(R_, R_, R_, G_); return VK_FORMAT_R8G8_UNORM;
	case 111: *sw = SW(I_, ONE_, ONE_, ONE_); return VK_FORMAT_R16_SFLOAT;
	case 112: *sw = SW(I_, I_, ONE_, ONE_); return VK_FORMAT_R16G16_SFLOAT;
	case 113: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case 114: *sw = SW(I_, ONE_, ONE_, ONE_); return VK_FORMAT_R32_SFLOAT;
	case 115: *sw = SW(I_, I_, ONE_, ONE_); return VK_FORMAT_R32G32_SFLOAT;
	case 116: return VK_FORMAT_R32G32B32A32_SFLOAT;
	case FOURCC('D', 'X', 'T', '1'): return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case FOURCC('D', 'X', 'T', '2'): case FOURCC('D', 'X', 'T', '3'): return VK_FORMAT_BC2_UNORM_BLOCK;
	case FOURCC('D', 'X', 'T', '4'): case FOURCC('D', 'X', 'T', '5'): return VK_FORMAT_BC3_UNORM_BLOCK;
	case FOURCC('A', 'T', 'I', '2'): return VK_FORMAT_BC5_UNORM_BLOCK;
	case 75: case 73: *depth = *stencil = 1; return fmt_d24s8;          /* D24S8 D24X4S4 */
	case 77: *depth = 1; return fmt_d24x8;                              /* D24X8 */
	case 71: case 79: *depth = 1; *stencil = 1; return fmt_d24s8;       /* D32 / D15S1: approximations */
	case 80: *depth = 1; return VK_FORMAT_D16_UNORM;
	case 82: *depth = 1; return VK_FORMAT_D32_SFLOAT;                   /* D32F_LOCKABLE */
	case 83: *depth = *stencil = 1; return VK_FORMAT_D32_SFLOAT_S8_UINT; /* D24FS8 */
	default: return VK_FORMAT_UNDEFINED;
	}
}
static int fmt_supports(VkFormat f, VkFormatFeatureFlags want) {
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties(phys, f, &fp);
	return (fp.optimalTilingFeatures & want) == want;
}

/* ============================================================================================ resources */
static Res *res_of(Obj *o) { return (Res *)o->host; }

static VkImageView make_view(Res *r, VkImageViewType type, VkComponentMapping sw, u32 level, u32 nlevels, u32 layer, u32 nlayers, int attach) {
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	VkImageView v;
	vi.image = r->img;
	vi.viewType = type;
	vi.format = r->fmt;
	vi.components = sw;
	vi.subresourceRange.aspectMask = r->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (attach && r->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
	vi.subresourceRange.baseMipLevel = level;
	vi.subresourceRange.levelCount = nlevels;
	vi.subresourceRange.baseArrayLayer = layer;
	vi.subresourceRange.layerCount = nlayers;
	VKC(vkCreateImageView(vd, &vi, 0, &v));
	return v;
}

/* the image of a texture or a standalone surface (made when first needed) */
/* the most samples, up to n, every kind of attachment has (a power of two; 1: none) */
static u32 ms_supported(u32 n) {
	VkSampleCountFlags have = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts & props.limits.framebufferStencilSampleCounts;
	u32 c = 1;
	while (c * 2 <= n && c * 2 <= 64 && (have & (c * 2))) c *= 2;
	return c;
}
/* The game's scene distance (1280x720 R32F: a full-screen pass turns the depth pre-pass into distance from the camera,
 * for its soft particles and the bloom's distance weighting) is made R16F: half the bytes written by that pass and read
 * by the others, ~3 significant digits kept -- for fades and weights, enough. It is known by its first use: an R32F
 * render target drawn into with no depth buffer (the pre-pass and the shadow maps, R32F too, have one). Not read back
 * (GetRenderTargetData fails on it). XRC_VK_DIST16=0: not done. */
static int dist16(const Obj *o) {
	static int on = -1;
	Obj *r0 = obj_of(dev.rt[0]);
	if (on < 0) on = !(getenv("XRC_VK_DIST16") && !atoi(getenv("XRC_VK_DIST16")));
	return on && r0 && (r0 == o || r0->parent == o->guest) && !obj_of(dev.ds) && !(o->usage & 2);
}
static int no_bc_forced(void);
static Res *image_of(Obj *o) {
	Res *r = res_of(o);
	VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	VkMemoryRequirements rq;
	int rt = (o->usage & 1) != 0, ds = (o->usage & 2) != 0;
	if (r) return r;
	HC(nimg, 1);
	r = (Res *)calloc(1, sizeof *r);
	r->kind = o->kind;
	r->d3dfmt = o->fmt;
	r->fmt = vk_format(o->fmt, &r->sw, &r->depth, &r->stencil);
	if (o->fmt == 114 && rt && dist16(o)) { r->fmt = VK_FORMAT_R16_SFLOAT; r->narrow = 1; LOG("xrc: vulkan: R32F render target %08x %ux%u: made R16F (XRC_VK_DIST16)\n", o->guest, o->w, o->h); }
	if (is_dxt(o->fmt) && r->fmt != VK_FORMAT_UNDEFINED && (!have_bc || no_bc_forced())) { /* (decoded at upload) */
		static int said;
		if (!said++) fprintf(stderr, "xrc: vulkan: DXT textures decoded to RGBA8 (%s)\n", have_bc ? "XRC_VK_NO_BC" : "the GPU has no BC formats");
		r->fmt = VK_FORMAT_R8G8B8A8_UNORM;
		r->unpack = 1;
	}
	if (r->fmt == VK_FORMAT_UNDEFINED) {
		static u32 warned[32];
		int k;
		for (k = 0; k < 32 && warned[k] && warned[k] != o->fmt; k++) {}
		if (k < 32 && !warned[k]) { warned[k] = o->fmt; fprintf(stderr, "xrc: vulkan: texture format %u (%08x) not handled: drawn as black\n", o->fmt, o->fmt); }
		free(r);
		return 0;
	}
	if (r->depth) ds = 1;
	r->w = o->w; r->h = o->h; r->d = o->kind == K_VTEX ? o->d : 1;
	r->levels = o->kind == K_SURF ? 1 : o->levels;
	r->cube = o->kind == K_CUBE;
	r->vol = o->kind == K_VTEX;
	r->layers = r->cube ? 6 : 1;
	r->bpp_block = fmt_bytes(o->fmt);
	r->blend_ok = fmt_supports(r->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
	ci.flags = r->cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
	ci.imageType = r->vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	ci.format = r->fmt;
	ci.extent.width = r->w; ci.extent.height = r->h; ci.extent.depth = r->d;
	ci.mipLevels = r->levels;
	ci.arrayLayers = r->layers;
	r->samples = o->kind == K_SURF && (rt || ds) && o->ms >= 2 && o->guest != dev.backbuf ? ms_supported(o->ms) : 1;
	ci.samples = (VkSampleCountFlagBits)r->samples;
	if (r->samples > 1 || (o->ms >= 2 && (rt || ds))) LOG("xrc: vulkan: surface %08x: %u samples (asked %u)\n", o->guest, r->samples, o->ms);
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = (r->samples > 1 ? 0 : VK_IMAGE_USAGE_SAMPLED_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (ds) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	else if (rt || (o->kind == K_SURF && o->pool == 0 && !is_dxt(o->fmt))) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if ((ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !fmt_supports(r->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) ci.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	r->can_att = (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKC(vkCreateImage(vd, &ci, 0, &r->img));
	vkGetImageMemoryRequirements(vd, r->img, &rq);
	r->mem = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	if (r->mem.block < 0) die("allocating image memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
	VKC(vkBindImageMemory(vd, r->img, blocks[r->mem.block].mem, r->mem.off));
	r->view = make_view(r, r->cube ? VK_IMAGE_VIEW_TYPE_CUBE : r->vol ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, r->sw, 0, r->levels, 0, r->layers, 0);
	r->att = (VkImageView *)calloc((size_t)r->layers * r->levels, sizeof(VkImageView));
	/* cleared (D3D's new resources: zero), then the guest's contents where it has them */
	end_render();
	r->lay = VK_IMAGE_LAYOUT_UNDEFINED;
	bq_add(r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); IMG_W_XFER(r); bq_go();
	{
		VkImageSubresourceRange sr = {r->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (r->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
		if (r->depth) { VkClearDepthStencilValue z = {1.0f, 0}; vkCmdClearDepthStencilImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
		else if (!is_dxt(o->fmt) || r->unpack) { VkClearColorValue z; memset(&z, 0, sizeof z); vkCmdClearColorImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
	}
	/* what the guest wrote before (D3DX loads lock before the first draw; render targets' guest memory is not theirs) */
	{
		u32 f;
		for (f = 0; f < r->layers; f++) {
			u32 l;
			for (l = 0; l < r->levels; l++)
				if (o->kind == K_SURF ? o->mem != 0 : o->lvl[f][l] != 0) r->dirty[f] |= 1u << l;
		}
		if (ds || rt) memset(r->dirty, 0, sizeof r->dirty);
	}
	o->host = r;
	return r;
}
/* a surface's image: its container's (face, level) or its own */
static Res *surface_image(Obj *s, u32 *face, u32 *level) {
	if (s->parent) {
		Obj *t = obj_of(s->parent);
		*face = s->face; *level = s->level;
		return t ? image_of(t) : 0;
	}
	*face = 0; *level = 0;
	return image_of(s);
}
/* a multisampled color image resolved into its one-sample twin (made the first time): what copies and read-backs use
 * (0 for depth: not resolved) */
static void end_render(void);
static Res *resolved_of(Res *r) {
	Res *t = r->resolved;
	VkImageResolve rv;
	if (r->depth) return 0;
	end_render();
	if (!t) {
		VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		VkMemoryRequirements rq;
		t = (Res *)calloc(1, sizeof *t);
		t->kind = r->kind; t->d3dfmt = r->d3dfmt; t->fmt = r->fmt; t->sw = r->sw; t->w = r->w; t->h = r->h; t->d = 1;
		t->levels = 1; t->layers = 1; t->bpp_block = r->bpp_block; t->blend_ok = r->blend_ok; t->samples = 1;
		ci.imageType = VK_IMAGE_TYPE_2D; ci.format = t->fmt;
		ci.extent.width = t->w; ci.extent.height = t->h; ci.extent.depth = 1;
		ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
		ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VKC(vkCreateImage(vd, &ci, 0, &t->img));
		vkGetImageMemoryRequirements(vd, t->img, &rq);
		t->mem = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
		if (t->mem.block < 0) die("allocating image memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
		VKC(vkBindImageMemory(vd, t->img, blocks[t->mem.block].mem, t->mem.off));
		t->view = make_view(t, VK_IMAGE_VIEW_TYPE_2D, t->sw, 0, 1, 0, 1, 0);
		t->att = (VkImageView *)calloc(1, sizeof(VkImageView));
		t->lay = VK_IMAGE_LAYOUT_UNDEFINED;
		r->resolved = t;
	}
	memset(&rv, 0, sizeof rv);
	rv.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rv.srcSubresource.layerCount = 1;
	rv.dstSubresource = rv.srcSubresource;
	rv.extent.width = r->w; rv.extent.height = r->h; rv.extent.depth = 1;
	bq_add(r, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); bq_add(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	IMG_R_XFER(r); IMG_W_XFER(t); bq_go();
	vkCmdResolveImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rv);
	return t;
}
/* the samples of the device's render targets (the first color one, else the depth surface) */
static u32 rt_samples(void) {
	u32 k, f, l;
	Res *r;
	for (k = 0; k < 4; k++) {
		Obj *o = obj_of(dev.rt[k]);
		if (o && (r = surface_image(o, &f, &l)) && !r->depth) return r->samples;
	}
	{
		Obj *o = obj_of(dev.ds);
		if (o && (r = surface_image(o, &f, &l)) && r->depth) return r->samples;
	}
	return 1;
}
static VkImageView attach_view(Res *r, u32 face, u32 level) {
	VkImageView *v = &r->att[face * r->levels + level];
	if (!*v) *v = make_view(r, VK_IMAGE_VIEW_TYPE_2D, SW(I_, I_, I_, I_), level, 1, face, 1, 1);
	return *v;
}

/* ---- DXT without the GPU's BC formats (most phone GPUs: Mali, PowerVR, many Adreno drivers). The texture is an RGBA8
 * image and each upload decodes the guest's 4x4 blocks into it -- 4 bytes a texel (4x DXT5's, 8x DXT1's memory) --
 * as the BC1 / BC2 / BC3 / BC5 formats define them, what the GPU's own decoder would give (BC1's three-colour mode's
 * fourth entry transparent black; BC5: red and green, blue 0, alpha 1). XRC_VK_NO_BC=1: this on any GPU (testing). */
static void bc_colors(const u8 *b, u8 c[4][4], int bc1) {
	u32 c0 = b[0] | (u32)b[1] << 8, c1 = b[2] | (u32)b[3] << 8, k;
	for (k = 0; k < 2; k++) {
		u32 v = k ? c1 : c0, r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
		c[k][0] = (u8)((r5 << 3) | (r5 >> 2)); c[k][1] = (u8)((g6 << 2) | (g6 >> 4)); c[k][2] = (u8)((b5 << 3) | (b5 >> 2)); c[k][3] = 255;
	}
	for (k = 0; k < 3; k++) {
		if (c0 > c1 || !bc1) { c[2][k] = (u8)((2u * c[0][k] + c[1][k] + 1) / 3); c[3][k] = (u8)((c[0][k] + 2u * c[1][k] + 1) / 3); }
		else { c[2][k] = (u8)((c[0][k] + c[1][k] + 1) / 2); c[3][k] = 0; }
	}
	c[2][3] = 255; c[3][3] = c0 > c1 || !bc1 ? 255 : 0;
}
static void bc_alpha8(const u8 *b, u8 a[8]) { /* (BC3's alpha, BC4/5's channels) */
	u32 a0 = b[0], a1 = b[1], k;
	a[0] = (u8)a0; a[1] = (u8)a1;
	if (a0 > a1) for (k = 1; k < 7; k++) a[k + 1] = (u8)(((7 - k) * a0 + k * a1 + 3) / 7);
	else { for (k = 1; k < 5; k++) a[k + 1] = (u8)(((5 - k) * a0 + k * a1 + 2) / 5); a[6] = 0; a[7] = 255; }
}
static u32 bc_bits48(const u8 *b, u32 i) { /* the 3-bit index of texel i (0..15) after the two endpoints */
	unsigned long long v = 0;
	u32 k;
	for (k = 0; k < 6; k++) v |= (unsigned long long)b[2 + k] << (8 * k);
	return (u32)(v >> (3 * i)) & 7;
}
/* one w x h level (of blocks src, block rows spitch bytes apart) into RGBA8 rows of w * 4 bytes */
static void dxt_decode(u32 fmt, const u8 *src, u32 spitch, u32 w, u32 h, u8 *dst) {
	u32 bx, by, bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == FOURCC('D', 'X', 'T', '1') ? 8 : 16;
	for (by = 0; by < bh; by++)
		for (bx = 0; bx < bw; bx++) {
			const u8 *b = src + by * spitch + bx * bs;
			u8 px[16][4], c[4][4], a[8];
			u32 i;
			if (fmt == FOURCC('A', 'T', 'I', '2')) { /* BC5: red block, green block */
				u8 g8[8];
				bc_alpha8(b, a); bc_alpha8(b + 8, g8);
				for (i = 0; i < 16; i++) { px[i][0] = a[bc_bits48(b, i)]; px[i][1] = g8[bc_bits48(b + 8, i)]; px[i][2] = 0; px[i][3] = 255; }
			} else {
				const u8 *cb_ = b + (bs == 16 ? 8 : 0);
				u32 idx = cb_[4] | (u32)cb_[5] << 8 | (u32)cb_[6] << 16 | (u32)cb_[7] << 24;
				bc_colors(cb_, c, bs == 8);
				for (i = 0; i < 16; i++) memcpy(px[i], c[(idx >> (2 * i)) & 3], 4);
				if (fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3')) /* BC2: 4-bit alphas */
					for (i = 0; i < 16; i++) px[i][3] = (u8)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
				else if (bs == 16) { /* BC3 */
					bc_alpha8(b, a);
					for (i = 0; i < 16; i++) px[i][3] = a[bc_bits48(b, i)];
				}
			}
			for (i = 0; i < 16; i++) {
				u32 x = bx * 4 + (i & 3), y = by * 4 + i / 4;
				if (x < w && y < h) memcpy(dst + ((size_t)y * w + x) * 4, px[i], 4);
			}
		}
}
static int no_bc_forced(void) { static int f = -1; if (f < 0) f = xrc_tool_env("XRC_VK_NO_BC") && atoi(xrc_tool_env("XRC_VK_NO_BC")); return f; }

/* guest memory -> the image: the levels the guest changed */
static void upload_image(Obj *o, Res *r) {
	u32 f, l;
	clears_flush(r);
	for (f = 0; f < r->layers; f++) {
		if (!r->dirty[f]) continue;
		for (l = 0; l < r->levels; l++) {
			u32 src, size, w = dim(r->w, l), h = dim(r->h, l), d = r->vol ? dim(r->d, l) : 1;
			void *p;
			VkDeviceSize off;
			VkBufferImageCopy bc;
			if (!(r->dirty[f] & (1u << l))) continue;
			u32 spitch = 0;
			if (o->kind == K_SURF) { u32 pitch; src = surf_bits(o, &pitch); size = pitch * rows_of(o->fmt, o->h); spitch = pitch; }
			else { src = level_mem(o, f, l); size = level_size(o, l); }
			end_render();
			if (r->unpack) { /* (decoded: RGBA8 rows; a volume's slices one after another) */
				u32 bw = (w + 3) / 4, bh = (h + 3) / 4, bs = o->fmt == FOURCC('D', 'X', 'T', '1') ? 8 : 16, z;
				if (!spitch) spitch = bw * bs;
				off = ring_alloc((VkDeviceSize)w * h * 4 * d, 16, &p);
				for (z = 0; z < d; z++) dxt_decode(o->fmt, xrc_mem + src + (size_t)z * spitch * bh, spitch, w, h, (u8 *)p + (size_t)z * w * h * 4);
				size = w * h * 4 * d;
			} else {
				off = ring_alloc(size, 16, &p);
				memcpy(p, xrc_mem + src, size);
			}
			up_bytes += size;
			HC(upkb, size / 1024.0);
			memset(&bc, 0, sizeof bc);
			bc.bufferOffset = off;
			bc.imageSubresource.aspectMask = r->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			bc.imageSubresource.mipLevel = l;
			bc.imageSubresource.baseArrayLayer = f;
			bc.imageSubresource.layerCount = 1;
			bc.imageExtent.width = w; bc.imageExtent.height = h; bc.imageExtent.depth = d;
			if (r->depth) { /* (depth uploads: not done; D3D games rarely lock depth) */ continue; }
			PP_OP(2);
			bq_add(r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); IMG_W_XFER(r); bq_go();
			vkCmdCopyBufferToImage(cb, F->ring, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
		}
		r->dirty[f] = 0;
	}
}
/* ---- HD textures (LR). A 2D texture's top level (DXT1/3/5, A8R8G8B8, X8R8G8B8; 32 to 4096 texels a side) is hashed
 * when the game fills it (its format, size and bytes): XRC_TEX_DIR=<dir> holding <hash>.dds (BC1 / BC2 / BC3 / BC7,
 * any size, its mips in the file) is sampled in its place, with the original's channel mapping; read by a thread, so
 * the first sight of one costs no frame (the original is drawn until it is in). A texture whose contents change is
 * hashed again (the replacement dropped, or another taken); one changed more than 3 times is left alone (streamed or
 * drawn into). XRC_TEX_DUMP=<dir>: each one's top level written there as <hash>.dds (once), for the offline upscaler
 * (tools\upscale\upscale_textures.py). XRC_TEX=0: neither. */
typedef struct TJob {
	unsigned long long key;
	int state, orphan, b; /* (state 0 waiting, 1 read, 2 failed; under tj_lock. b: 0 tex_dir, k from tex_dirs[k - 1]) */
	VkFormat fmt;
	u32 w, h, levels;
	u8 *data;
	size_t off[16], size[16];
	struct TJob *next;
} TJob;
static void res_destroy(Res *r);
static int tex_alt; /* (0: XRC_TEX_DIR; k: the set tex_dirs[k - 1]. F8 cycles; XRC_SHOT_AB=texb's B shot is 1) */
#define TEX_NX 8
static int tex_show = 1; /* (the F10 menu's HD textures: off, the originals are drawn; replacements are still made) */
void xrc_vk_set_tex_hd(int on) { tex_show = on != 0; }
static int tex_ui_show = 0; /* (HD UI, fonts and logos: one-level textures; XRC_TEX_UI / the F10 menu) */
void xrc_vk_set_tex_ui(int on) { tex_ui_show = on != 0; }
#if defined(XRC_LR) && defined(_WIN32)
static pthread_mutex_t tj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tj_cond = PTHREAD_COND_INITIALIZER;
static TJob *tj_head, *tj_tail;
static char tex_dir[MAX_PATH], dump_dir[MAX_PATH];
static char tex_dirs[TEX_NX][MAX_PATH];
static int n_tex_dirs;
/* F8: the next texture set -- the originals, XRC_TEX_DIR, then each XRC_TEX_DIRS set -- named in the window title */
void xrc_vk_tex_cycle(void) {
	char t[MAX_PATH + 64];
	const char *d, *sl;
	if (!tex_show) { tex_show = 1; tex_alt = 0; }
	else if (tex_alt < n_tex_dirs) tex_alt++;
	else { tex_show = 0; tex_alt = 0; }
	d = !tex_show ? "original textures" : tex_alt ? tex_dirs[tex_alt - 1] : tex_dir;
	sl = strrchr(d, 92); if (!sl) sl = strrchr(d, '/'); /* (92: a backslash) */
	snprintf(t, sizeof t, "LIGHTNING RETURNS - textures: %s (F8: next)", sl ? sl + 1 : d);
	SetWindowTextA(GetForegroundWindow(), t);
	fprintf(stderr, "xrc: textures: F8: %s\n", d);
}
static unsigned long long *tex_have;
static u32 ntex_have;
static int tex_on = -1, tex_dump_on, tex_bc7;
static unsigned n_tex_hashed, n_tex_dumped, n_tex_replaced;
static int u64cmp(const void *a, const void *b) { unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b; return x < y ? -1 : x > y; }
static u32 rd32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
/* a .dds of BC blocks: its levels in j (0: not one of those) */
static int dds_read(const char *path, TJob *j) {
	FILE *f = fopen(path, "rb");
	long n;
	u8 *p;
	size_t hdr = 128, at;
	u32 four, bs, l, w, h, mips;
	if (!f) return 0;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n < 148 || n > (256l << 20) || !(p = (u8 *)malloc((size_t)n))) { fclose(f); return 0; }
	if (fread(p, 1, (size_t)n, f) != (size_t)n || memcmp(p, "DDS ", 4)) { fclose(f); free(p); return 0; }
	fclose(f);
	h = rd32(p + 12); w = rd32(p + 16); mips = rd32(p + 28); four = rd32(p + 84);
	if (!(rd32(p + 80) & 4)) four = 0; /* (DDPF_FOURCC) */
	j->fmt = VK_FORMAT_UNDEFINED; bs = 16;
	if (four == FOURCC('D', 'X', 'T', '1')) { j->fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; bs = 8; }
	else if (four == FOURCC('D', 'X', 'T', '3')) j->fmt = VK_FORMAT_BC2_UNORM_BLOCK;
	else if (four == FOURCC('D', 'X', 'T', '5')) j->fmt = VK_FORMAT_BC3_UNORM_BLOCK;
	else if (four == FOURCC('D', 'X', '1', '0')) {
		u32 dx = rd32(p + 128);
		hdr = 148;
		if (dx == 71 || dx == 72) { j->fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; bs = 8; }
		else if (dx == 74 || dx == 75) j->fmt = VK_FORMAT_BC2_UNORM_BLOCK;
		else if (dx == 77 || dx == 78) j->fmt = VK_FORMAT_BC3_UNORM_BLOCK;
		else if ((dx == 98 || dx == 99) && tex_bc7) j->fmt = VK_FORMAT_BC7_UNORM_BLOCK; /* (sRGB read as UNORM: as the original) */
	}
	if (j->fmt == VK_FORMAT_UNDEFINED || w < 1 || h < 1 || w > 8192 || h > 8192) { free(p); return 0; }
	if (mips < 1) mips = 1;
	if (mips > 16) mips = 16;
	for (at = hdr, l = 0; l < mips; l++) {
		u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
		size_t sz = (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * bs;
		if (at + sz > (size_t)n) break;
		j->off[l] = at; j->size[l] = sz; at += sz;
		if (lw == 1 && lh == 1) { l++; break; }
	}
	if (!l) { free(p); return 0; }
	j->w = w; j->h = h; j->levels = l; j->data = p;
	return 1;
}
static void *tj_thread(void *arg) {
	(void)arg;
	for (;;) {
		TJob *j;
		char path[MAX_PATH + 32];
		int ok;
		pthread_mutex_lock(&tj_lock);
		while (!tj_head) pthread_cond_wait(&tj_cond, &tj_lock);
		j = tj_head;
		tj_head = j->next;
		if (!tj_head) tj_tail = 0;
		pthread_mutex_unlock(&tj_lock);
		snprintf(path, sizeof path, "%s\\%016llx.dds", j->b ? tex_dirs[j->b - 1] : tex_dir, j->key);
		ok = !j->orphan && dds_read(path, j);
		pthread_mutex_lock(&tj_lock);
		if (j->orphan) { free(j->data); free(j); }
		else j->state = ok ? 1 : 2;
		pthread_mutex_unlock(&tj_lock);
	}
	return 0;
}
static void tex_init(void) {
	const char *e = getenv("XRC_TEX");
	WIN32_FIND_DATAA fd;
	HANDLE fh;
	char pat[MAX_PATH + 8];
	u32 cap = 0;
	tex_on = 0;
	if (e && !atoi(e)) return;
	if ((e = getenv("XRC_TEX_DUMP")) && *e && strlen(e) < sizeof dump_dir) { strcpy(dump_dir, e); CreateDirectoryA(dump_dir, 0); tex_dump_on = 1; tex_on = 1; }
	if ((e = getenv("XRC_TEX_HD"))) tex_show = atoi(e) != 0;
	if ((e = getenv("XRC_TEX_UI"))) tex_ui_show = atoi(e) != 0;
	/* more sets to compare: XRC_TEX_DIR_B=<dir>, or XRC_TEX_DIRS=<dir>;<dir>;... (up to 8; F8 cycles original, XRC_TEX_DIR, these) */
	if ((e = getenv("XRC_TEX_DIR_B")) && *e && strlen(e) < MAX_PATH) strcpy(tex_dirs[n_tex_dirs++], e);
	if ((e = getenv("XRC_TEX_DIRS")) && *e) {
		while (*e && n_tex_dirs < TEX_NX) {
			const char *sc = strchr(e, ';');
			size_t n = sc ? (size_t)(sc - e) : strlen(e);
			if (n && n < MAX_PATH) { memcpy(tex_dirs[n_tex_dirs], e, n); tex_dirs[n_tex_dirs++][n] = 0; }
			e += n + (sc != 0);
		}
	}
	if ((e = getenv("XRC_TEX_DIR")) && *e && strlen(e) < sizeof tex_dir) strcpy(tex_dir, e);
	else { /* (default: hd_textures beside the program) */
		DWORD n = GetModuleFileNameA(0, tex_dir, sizeof tex_dir - 16);
		char *sl;
		tex_dir[n < sizeof tex_dir - 16 ? n : 0] = 0;
		if ((sl = strrchr(tex_dir, '\\'))) strcpy(sl, "\\hd_textures"); else tex_dir[0] = 0;
		if (!tex_dir[0] || GetFileAttributesA(tex_dir) == INVALID_FILE_ATTRIBUTES) { if (tex_on) fprintf(stderr, "xrc: textures: dumping to %s\n", dump_dir); return; }
	}
	if (!have_bc || no_bc_forced()) { fprintf(stderr, "xrc: textures: HD textures need the GPU's BC formats: not used\n"); return; }
	tex_bc7 = fmt_supports(VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
	snprintf(pat, sizeof pat, "%s\\*.dds", tex_dir);
	if ((fh = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
		do {
			char *end;
			unsigned long long k = strtoull(fd.cFileName, &end, 16);
			if (end != fd.cFileName + 16 || _stricmp(end, ".dds")) continue;
			if (ntex_have == cap) { cap = cap ? cap * 2 : 1024; tex_have = (unsigned long long *)realloc(tex_have, cap * sizeof *tex_have); }
			tex_have[ntex_have++] = k;
		} while (FindNextFileA(fh, &fd));
		FindClose(fh);
	}
	qsort(tex_have, ntex_have, sizeof *tex_have, u64cmp);
	fprintf(stderr, "xrc: textures: %u HD textures in %s%s\n", ntex_have, tex_dir, tex_bc7 ? "" : " (no BC7 on this GPU: those not used)");
	if (ntex_have) { pthread_t t; if (pthread_create(&t, 0, tj_thread, 0) == 0) pthread_detach(t); else ntex_have = 0; }
	tex_on = ntex_have || tex_dump_on;
}
/* 4 lanes of 8 bytes; the format and size in the seed */
static unsigned long long tex_hash(const u8 *p, size_t n, unsigned long long seed) {
	unsigned long long a = seed ^ 0x9E3779B97F4A7C15ull, b = seed + n, c = ~seed, d = seed * 0xff51afd7ed558ccdull, w;
	size_t i = 0;
	for (; i + 32 <= n; i += 32) {
		memcpy(&w, p + i, 8); a = (a ^ w) * 0xff51afd7ed558ccdull; a ^= a >> 31;
		memcpy(&w, p + i + 8, 8); b = (b ^ w) * 0xc4ceb9fe1a85ec53ull; b ^= b >> 29;
		memcpy(&w, p + i + 16, 8); c = (c ^ w) * 0xff51afd7ed558ccdull; c ^= c >> 33;
		memcpy(&w, p + i + 24, 8); d = (d ^ w) * 0xc4ceb9fe1a85ec53ull; d ^= d >> 30;
	}
	for (; i < n; i++) a = (a ^ p[i]) * 0x100000001b3ull;
	a ^= b * 0x9E3779B97F4A7C15ull; a ^= a >> 32; a ^= c * 0xff51afd7ed558ccdull; a ^= a >> 29; a ^= d * 0xc4ceb9fe1a85ec53ull;
	a ^= a >> 33; a *= 0xff51afd7ed558ccdull; a ^= a >> 33;
	return a;
}
/* the original's top level as a .dds, for the offline upscaler */
static void tex_dump(Obj *o, const u8 *src, u32 size, unsigned long long key) {
	char path[MAX_PATH + 32];
	u8 h[128];
	FILE *f;
	snprintf(path, sizeof path, "%s\\%016llx.dds", dump_dir, key);
	if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
	memset(h, 0, sizeof h);
	memcpy(h, "DDS ", 4);
#define W32(o_, v) do { u32 v_ = (v); memcpy(h + (o_), &v_, 4); } while (0)
	W32(4, 124); W32(8, 0x1007 | (is_dxt(o->fmt) ? 0x80000 : 0x8)); W32(12, o->h); W32(16, o->w);
	W32(20, is_dxt(o->fmt) ? size : o->w * 4); W32(28, 1); W32(76, 32);
	if (is_dxt(o->fmt)) { W32(80, 4); W32(84, o->fmt); }
	else { W32(80, o->fmt == 21 ? 0x41 : 0x40); W32(88, 32); W32(92, 0xff0000); W32(96, 0xff00); W32(100, 0xff); W32(104, o->fmt == 21 ? 0xff000000u : 0); }
	W32(108, 0x1000);
	W32(32, o->levels); W32(36, FOURCC('X', 'R', 'C', 'L')); /* (reserved words: the game's mip count; UI and fonts have 1) */
#undef W32
	if (!(f = fopen(path, "wb"))) return;
	fwrite(h, 1, 128, f); fwrite(src, 1, size, f);
	if (fclose(f)) DeleteFileA(path); else n_tex_dumped++;
}
static void tex_job_drop1(TJob **pj) {
	TJob *j = *pj;
	if (!j) return;
	*pj = 0;
	pthread_mutex_lock(&tj_lock);
	if (j->state) { free(j->data); free(j); } else j->orphan = 1; /* (the thread frees it) */
	pthread_mutex_unlock(&tj_lock);
}
static void tex_job_drop(Res *r) { int k; tex_job_drop1(&r->job); for (k = 0; k < TEX_NX; k++) tex_job_drop1(&r->job_x[k]); }
static TJob *tex_job_add(unsigned long long key, int b) {
	TJob *j = (TJob *)calloc(1, sizeof *j);
	j->key = key; j->b = b;
	pthread_mutex_lock(&tj_lock);
	if (tj_tail) tj_tail->next = j; else tj_head = j;
	tj_tail = j;
	pthread_cond_signal(&tj_cond);
	pthread_mutex_unlock(&tj_lock);
	return j;
}
static void res_defer(Res *r) {
	Frame *fr = F;
	if (fr->ndead == fr->capdead) { fr->capdead = fr->capdead ? fr->capdead * 2 : 64; fr->dead = (Res **)realloc(fr->dead, fr->capdead * sizeof(Res *)); }
	fr->dead[fr->ndead++] = r;
}
/* the game is filling the top level */
static void tex_check(Obj *o, Res *r) {
	u32 size, src;
	unsigned long long key;
	/* (one level: UI, fonts, logos, title art and lookup tables. Their replacements are drawn only with tex_ui_show;
	 * lookup tables never get one -- the upscaler keeps those. World surfaces have mips.) */
	r->tui = o->levels < 2;
	if (o->kind != K_TEX || r->unpack || r->samples > 1 || (o->usage & 3) || o->w < 32 || o->h < 32 || o->w > 4096 || o->h > 4096 ||
	    !(o->fmt == 21 || o->fmt == 22 || o->fmt == FOURCC('D', 'X', 'T', '1') || o->fmt == FOURCC('D', 'X', 'T', '3') || o->fmt == FOURCC('D', 'X', 'T', '5'))) {
		static int nlog = -1;
		if (nlog < 0) nlog = xrc_tool_env("XRC_TEX_LOG") ? 0 : 1000;
		if (nlog < 200 && o->kind == K_TEX) { nlog++; fprintf(stderr, "xrc: textures: not replaceable: fmt %08x %ux%u, %u levels, usage %x\n", o->fmt, o->w, o->h, o->levels, o->usage); }
		r->tstate = 4;
		return;
	}
	src = level_mem(o, 0, 0); size = level_size(o, 0);
	if (!src || !size) return;
	key = tex_hash(xrc_mem + src, size, (unsigned long long)o->fmt << 32 ^ (unsigned long long)o->w << 16 ^ o->h);
	if (++n_tex_hashed % 250 == 0) fprintf(stderr, "xrc: textures: %u hashed, %u dumped, %u replaced\n", n_tex_hashed, n_tex_dumped, n_tex_replaced);
	if (r->tstate == 3 && key == r->th) { r->dirty[0] = 0; return; } /* (the same contents again: the replacement stays) */
	if (r->tstate) { /* changed */
		tex_job_drop(r);
		if (r->repl) { res_defer(r->repl); r->repl = 0; }
		{ int k; for (k = 0; k < TEX_NX; k++) if (r->repl_x[k]) { res_defer(r->repl_x[k]); r->repl_x[k] = 0; } }
		if (++r->tchanges > 3) { r->tstate = 4; return; }
	}
	r->th = key;
	r->tstate = 1;
	if (tex_dump_on) tex_dump(o, xrc_mem + src, size, key);
	if (ntex_have && bsearch(&key, tex_have, ntex_have, sizeof key, u64cmp)) {
		r->job = tex_job_add(key, 0); r->tstate = 2;
		{ int k; for (k = 0; k < n_tex_dirs; k++) r->job_x[k] = tex_job_add(key, k + 1); }
	}
}
/* a replacement read: its image made and filled */
static void tex_take(Res *r, int b) {
	TJob *j = b ? r->job_x[b - 1] : r->job;
	int st;
	pthread_mutex_lock(&tj_lock);
	st = j->state;
	pthread_mutex_unlock(&tj_lock);
	if (!st) return;
	if (b) r->job_x[b - 1] = 0; else r->job = 0;
	if (st == 1) {
		Res *t = (Res *)calloc(1, sizeof *t);
		VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		VkMemoryRequirements rq;
		u32 l;
		t->kind = r->kind; t->d3dfmt = r->d3dfmt; t->fmt = j->fmt; t->sw = r->sw; t->w = j->w; t->h = j->h; t->d = 1;
		t->levels = j->levels; t->layers = 1; t->bpp_block = j->fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16; t->samples = 1;
		ci.imageType = VK_IMAGE_TYPE_2D; ci.format = t->fmt;
		ci.extent.width = t->w; ci.extent.height = t->h; ci.extent.depth = 1;
		ci.mipLevels = t->levels; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
		ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VKC(vkCreateImage(vd, &ci, 0, &t->img));
		vkGetImageMemoryRequirements(vd, t->img, &rq);
		t->mem = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
		if (t->mem.block < 0) { /* (out of video memory: the original stays) */
			vkDestroyImage(vd, t->img, 0); free(t); free(j->data); free(j); if (!b) r->tstate = 1;
			fprintf(stderr, "xrc: textures: out of video memory: %016llx not replaced\n", r->th);
			return;
		}
		VKC(vkBindImageMemory(vd, t->img, blocks[t->mem.block].mem, t->mem.off));
		t->view = make_view(t, VK_IMAGE_VIEW_TYPE_2D, t->sw, 0, t->levels, 0, 1, 0);
		t->att = (VkImageView *)calloc(t->levels, sizeof(VkImageView));
		t->lay = VK_IMAGE_LAYOUT_UNDEFINED;
		end_render();
		for (l = 0; l < t->levels; l++) {
			VkBufferImageCopy bc;
			void *p;
			VkDeviceSize off = ring_alloc(j->size[l], 16, &p);
			memcpy(p, j->data + j->off[l], j->size[l]);
			up_bytes += j->size[l];
			memset(&bc, 0, sizeof bc);
			bc.bufferOffset = off;
			bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			bc.imageSubresource.mipLevel = l;
			bc.imageSubresource.layerCount = 1;
			bc.imageExtent.width = t->w >> l ? t->w >> l : 1; bc.imageExtent.height = t->h >> l ? t->h >> l : 1; bc.imageExtent.depth = 1;
			bq_add(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); IMG_W_XFER(t); bq_go();
			vkCmdCopyBufferToImage(cb, F->ring, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
		}
		if (b) { r->repl_x[b - 1] = t; free(j->data); free(j); return; }
		r->repl = t; r->tstate = 3;
		if (!n_tex_replaced++ || n_tex_replaced % 100 == 0)
			fprintf(stderr, "xrc: textures: %u replaced (%016llx: %ux%u -> %ux%u)\n", n_tex_replaced, r->th, r->w, r->h, t->w, t->h);
	} else if (!b) r->tstate = 1;
	free(j->data); free(j);
}
#else
void xrc_vk_tex_cycle(void) {}
#endif
static Res *texture_ready(Obj *o) {
	Res *r = image_of(o);
	u32 f, any = 0;
	if (!r) return 0;
#if defined(XRC_LR) && defined(_WIN32)
	if (tex_on < 0) tex_init();
	if (tex_on && r->tstate != 4 && (r->dirty[0] & 1)) tex_check(o, r);
	if (r->job) tex_take(r, 0);
	{ int k; for (k = 0; k < n_tex_dirs; k++) if (r->job_x[k]) tex_take(r, k + 1); }
#endif
	for (f = 0; f < r->layers; f++) any |= r->dirty[f];
	if (any) upload_image(o, r);
	return r;
}

/* static vertex / index buffers */
static Res *buffer_of(Obj *o) {
	Res *r = res_of(o);
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	VkMemoryRequirements rq;
	if (r) return r;
	r = (Res *)calloc(1, sizeof *r);
	r->is_buf = 1;
	r->size = o->size ? o->size : 4;
	bi.size = (r->size + 3) & ~3u;
	bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VKC(vkCreateBuffer(vd, &bi, 0, &r->buf));
	vkGetBufferMemoryRequirements(vd, r->buf, &rq);
	r->mem = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
	if (r->mem.block < 0) die("allocating buffer memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
	VKC(vkBindBufferMemory(vd, r->buf, blocks[r->mem.block].mem, r->mem.off));
	r->dlo = 0; r->dhi = o->mem ? r->size : 0;
	o->host = r;
	return r;
}
static Res *buffer_ready(Obj *o) {
	Res *r = buffer_of(o);
	if (r->dhi > r->dlo && o->mem) {
		u32 lo = r->dlo & ~3u, hi = (r->dhi + 3) & ~3u, n;
		void *p;
		VkDeviceSize off;
		VkBufferCopy bc;
		if (hi > ((r->size + 3) & ~3u)) hi = (r->size + 3) & ~3u;
		n = hi - lo;
		end_render();
		off = ring_alloc(n, 16, &p);
		memcpy(p, xrc_mem + o->mem + lo, lo + n <= o->size ? n : o->size - lo);
		up_bytes += n;
		HC(upkb, n / 1024.0);
		bc.srcOffset = off; bc.dstOffset = lo; bc.size = n;
		PP_OP(3);
		if (draws_since || full_barriers > 0) { sync_need(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT); bq_go(); } /* (after the draws reading it) */
		vkCmdCopyBuffer(cb, F->ring, r->buf, 1, &bc);
		bufs_written = 1; acc_st |= VK_PIPELINE_STAGE_TRANSFER_BIT; acc_wr |= VK_ACCESS_TRANSFER_WRITE_BIT;
		r->dlo = r->dhi = 0;
	}
	return r;
}
/* D3DUSAGE_DYNAMIC, D3DPOOL_SYSTEMMEM, or a static buffer the game rewrites all the time (a managed sprite batch locked
 * before every draw: uploading it whole would end the rendering each time) */
static int streamed(Obj *o) { Res *r = res_of(o); return (o->usage & 0x200) || o->pool == 2 || (r && r->hot); }

static void res_destroy(Res *r) {
	u32 k;
	if (r->is_buf) { vkDestroyBuffer(vd, r->buf, 0); mem_free(r->mem); free(r); return; }
	if (r->resolved) { res_destroy(r->resolved); r->resolved = 0; }
	if (r->repl) { res_destroy(r->repl); r->repl = 0; }
	{ int k; for (k = 0; k < 8; k++) if (r->repl_x[k]) { res_destroy(r->repl_x[k]); r->repl_x[k] = 0; } }
#if defined(XRC_LR) && defined(_WIN32)
	tex_job_drop(r);
#endif
	img_untrack(r);
	for (k = 0; k < r->layers * r->levels; k++) if (r->att[k]) vkDestroyImageView(vd, r->att[k], 0);
	free(r->att);
	vkDestroyImageView(vd, r->view, 0);
	vkDestroyImage(vd, r->img, 0);
	mem_free(r->mem);
	free(r);
}

/* ============================================================================================ shaders */
/* mod_noat: a pixel shader's variant without the alpha test (vk_sm3.h SM3_NO_ALPHA_TEST), for the draws with the test
 * off -- a shader that may discard keeps the GPU from testing depth before it runs (most of the scene's draws) */
/* mod_nz, mod_noat_nz: the same without the oDepth write (SM3_NO_DEPTH_OUT; made for the shaders writing it) -- see zout_drop */
typedef struct { Sm3Info info; VkShaderModule mod, mod_noat, mod_nz, mod_noat_nz; u32 serial; int bad; unsigned long long hash; } Shader;
static void pw_reg(Shader *s); /* (the pipeline prewarm's shader registry, below) */
static void pw_unreg(Shader *s);
static VkShaderModule module_ex(const u32 *code, u32 nw, int flags, const Sm3Info *same) {
	Sm3Info n2;
	char err[256];
	VkShaderModule m = VK_NULL_HANDLE;
	if (sm3_translate_ex(code, nw, &n2, err, sizeof err, flags)) {
		if (!same || n2.nwords != same->nwords || memcmp(n2.spirv, same->spirv, n2.nwords * 4)) { /* (else: the same code) */
			VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			mi.codeSize = n2.nwords * 4;
			mi.pCode = n2.spirv;
			VKC(vkCreateShaderModule(vd, &mi, 0, &m));
		}
		sm3_free(&n2);
	}
	return m;
}
static u32 shader_serial;
static Shader *shader_of(Obj *o) {
	Shader *s = (Shader *)o->host;
	char err[256];
	if (s) return s->bad ? 0 : s;
	s = (Shader *)calloc(1, sizeof *s);
	o->host = s;
	s->serial = ++shader_serial;
	if (!sm3_translate((const uint32_t *)(xrc_mem + o->mem), o->size / 4, &s->info, err, sizeof err)) {
		fprintf(stderr, "xrc: vulkan: shader %u not translated: %s (its draws are skipped)\n", M32(o->guest + 4), err);
		s->bad = 1;
		return 0;
	}
	{
		VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		mi.codeSize = s->info.nwords * 4;
		mi.pCode = s->info.spirv;
		VKC(vkCreateShaderModule(vd, &mi, 0, &s->mod));
	}
	{ /* (the content hash: a pipeline key's shader identity that outlasts the run -- see the prewarm) */
		const u32 *w = (const uint32_t *)(xrc_mem + o->mem);
		unsigned long long h = 0xcbf29ce484222325ull;
		u32 q;
		for (q = 0; q < o->size / 4; q++) h = (h ^ w[q]) * 0x100000001b3ull;
		s->hash = h ? h : 1;
	}
	if (s->info.ps) {
		const u32 *code = (const uint32_t *)(xrc_mem + o->mem);
		s->mod_noat = module_ex(code, o->size / 4, SM3_NO_ALPHA_TEST, &s->info); /* (none: no test in it) */
		if (s->info.writes_depth) {
			s->mod_nz = module_ex(code, o->size / 4, SM3_NO_DEPTH_OUT, 0);
			if (s->mod_nz && s->mod_noat) s->mod_noat_nz = module_ex(code, o->size / 4, SM3_NO_DEPTH_OUT | SM3_NO_ALPHA_TEST, 0);
		}
	}
	sm3_free(&s->info);
	pw_reg(s);
	return s;
}

/* ============================================================================================ pipelines */
typedef struct {
	u32 vs, ps;                          /* shader serials */
	u8 topo, cull, fill, nattr, nbind, nrt, atoc, dbias;
	u8 at;                               /* the alpha test on (the pixel shader with it: Shader.mod / mod_noat) */
	u8 ms, a2c;                          /* samples (0/1: one), alpha to coverage (multisampled only) */
	u8 nz;                               /* the pixel shader without its oDepth write (Shader.mod_nz) -- see zout_drop */
	u8 zt, zw, zf, st, s2;               /* depth test / write / func, stencil enable / two-sided */
	u8 sf[2][4];                         /* stencil fail zfail pass func: front, back */
	u8 be, sb, db, bo, sba, dba, boa;    /* blend */
	u8 wm[4];
	u32 rtf[4], dsf;
	struct { u8 loc, bind, fmt, pad; u16 off, pad2; } attr[16];
	struct { u16 stride; u8 slot, pad; } bind[16];
} PKey;
typedef struct { PKey k; VkPipeline p; u32 hash; int pending; } PEnt;
/* The game's depth pre-pass (a 1280x720 R32F target of linear depth, and the shadow maps) has pixel shaders writing
 * oDepth: a per-pixel linear depth -- the one its vertex shader also makes the rasterized depth (z*w, so z/w is the
 * linear depth; the field's shaders seen), only interpolated correctly (the rasterized one: linearly on the screen). A shader writing depth keeps the GPU from testing depth before it runs,
 * and that pass draws ~3x over. Its depth buffer is cleared before the pass and read by nothing after it: only its
 * own draws' tests use it. For such a buffer the shaders run without the oDepth write (Res.zread: no read of the
 * buffer in the last 120 frames -- one now and then is a rendering split while the field loads) -- the tests between
 * its draws then use the rasterized depth, a close match; the color written (the linear depth sampled later) is the
 * same. XRC_VK_ZOUT_DROP=0: not done. */
static int zout_drop(void) {
	static int on = -1;
	if (on < 0) on = !(getenv("XRC_VK_ZOUT_DROP") && !atoi(getenv("XRC_VK_ZOUT_DROP")));
	return on;
}
static VkShaderModule ps_module(const Shader *ps, const PKey *k) {
	if (k->nz && ps->mod_nz) return !k->at && ps->mod_noat_nz ? ps->mod_noat_nz : ps->mod_nz;
	return !k->at && ps->mod_noat ? ps->mod_noat : ps->mod;
}
static PEnt *ptab;
static u32 pcap, pcount;
static u32 hash_key(const PKey *k) {
	/* 8 bytes a step (once a draw: the byte-wise FNV was a ~260-step multiply chain) */
	const unsigned char *p = (const unsigned char *)k;
	unsigned long long h = 0x9e3779b97f4a7c15ull, w;
	size_t i;
	for (i = 0; i + 8 <= sizeof *k; i += 8) { memcpy(&w, p + i, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
	for (; i < sizeof *k; i++) h = (h ^ p[i]) * 0x100000001b3ull;
	h ^= h >> 32;
	return (u32)h ? (u32)h : 1;
}
static const VkFormat decl_fmt[17] = {
	VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
	VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED,
	VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16_UNORM,
	VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
	VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
static VkBlendFactor blend_factor(u32 b, int alpha) {
	switch (b) {
	case 1: return VK_BLEND_FACTOR_ZERO;
	case 2: return VK_BLEND_FACTOR_ONE;
	case 3: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
	case 4: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
	case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case 7: return VK_BLEND_FACTOR_DST_ALPHA;
	case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case 9: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
	case 10: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case 11: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	case 12: return VK_BLEND_FACTOR_SRC_ALPHA;              /* BOTHSRCALPHA (as source; the destination below) */
	case 13: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case 14: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
	case 15: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
	default: return VK_BLEND_FACTOR_ONE;
	}
}
static VkBlendOp blend_op(u32 o) {
	switch (o) {
	case 2: return VK_BLEND_OP_SUBTRACT;
	case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
	case 4: return VK_BLEND_OP_MIN;
	case 5: return VK_BLEND_OP_MAX;
	default: return VK_BLEND_OP_ADD;
	}
}
static VkCompareOp cmp_op(u32 f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp st_op(u32 o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }

static VkPipeline make_pipeline(const PKey *k, VkShaderModule vsm, VkShaderModule psm) {
	VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkVertexInputAttributeDescription at[16];
	VkVertexInputBindingDescription bd[16];
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState ba[4];
	VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
	                        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
	                        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BIAS};
	VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	VkFormat cf[4];
	VkPipeline p;
	u32 i;
	VkResult r;
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vsm; st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = psm; st[1].pName = "main";
	for (i = 0; i < k->nattr; i++) {
		at[i].location = k->attr[i].loc; at[i].binding = k->attr[i].bind;
		at[i].format = k->attr[i].fmt == 255 ? VK_FORMAT_R32G32B32A32_SFLOAT : decl_fmt[k->attr[i].fmt];
		at[i].offset = k->attr[i].off;
	}
	for (i = 0; i < k->nbind; i++) { bd[i].binding = k->bind[i].slot; bd[i].stride = k->bind[i].stride; bd[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX; }
	vi.vertexAttributeDescriptionCount = k->nattr; vi.pVertexAttributeDescriptions = at;
	vi.vertexBindingDescriptionCount = k->nbind; vi.pVertexBindingDescriptions = bd;
	ia.topology = (VkPrimitiveTopology)k->topo;
	vp.viewportCount = 1; vp.scissorCount = 1;
	rs.polygonMode = k->fill == 1 && have_fill_nonsolid ? VK_POLYGON_MODE_POINT : k->fill == 2 && have_fill_nonsolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	rs.cullMode = k->cull == 2 ? VK_CULL_MODE_FRONT_BIT : k->cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.depthBiasEnable = k->dbias;
	rs.lineWidth = 1.0f;
	ms.rasterizationSamples = (VkSampleCountFlagBits)(k->ms > 1 ? k->ms : 1);
	ms.alphaToCoverageEnable = k->a2c; /* (one sample: alpha to coverage is done as an alpha test, see draw) */
	dss.depthTestEnable = k->zt;
	dss.depthWriteEnable = k->zw;
	dss.depthCompareOp = cmp_op(k->zf);
	dss.stencilTestEnable = k->st;
	dss.front.failOp = st_op(k->sf[0][0]); dss.front.depthFailOp = st_op(k->sf[0][1]); dss.front.passOp = st_op(k->sf[0][2]); dss.front.compareOp = cmp_op(k->sf[0][3]);
	dss.back.failOp = st_op(k->sf[1][0]); dss.back.depthFailOp = st_op(k->sf[1][1]); dss.back.passOp = st_op(k->sf[1][2]); dss.back.compareOp = cmp_op(k->sf[1][3]);
	for (i = 0; i < k->nrt; i++) {
		memset(&ba[i], 0, sizeof ba[i]);
		ba[i].blendEnable = k->be && (i == 0 || have_independent_blend) ? 1 : 0;
		if (k->be & (1 << (4 + i))) ba[i].blendEnable = 0; /* (the format cannot blend) */
		ba[i].srcColorBlendFactor = blend_factor(k->sb, 0);
		ba[i].dstColorBlendFactor = k->sb == 12 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : k->sb == 13 ? VK_BLEND_FACTOR_SRC_ALPHA : blend_factor(k->db, 0);
		ba[i].colorBlendOp = blend_op(k->bo);
		ba[i].srcAlphaBlendFactor = blend_factor(k->sba, 1);
		ba[i].dstAlphaBlendFactor = k->sba == 12 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : k->sba == 13 ? VK_BLEND_FACTOR_SRC_ALPHA : blend_factor(k->dba, 1);
		ba[i].alphaBlendOp = blend_op(k->boa);
		ba[i].colorWriteMask = have_independent_blend ? k->wm[i] : k->wm[0];
		cf[i] = (VkFormat)k->rtf[i];
	}
	cbs.attachmentCount = k->nrt; cbs.pAttachments = ba;
	dy.dynamicStateCount = sizeof dyn / sizeof dyn[0]; dy.pDynamicStates = dyn;
	ri.colorAttachmentCount = k->nrt; ri.pColorAttachmentFormats = cf;
	ri.depthAttachmentFormat = (VkFormat)k->dsf;
	ri.stencilAttachmentFormat = k->dsf == VK_FORMAT_D24_UNORM_S8_UINT || k->dsf == VK_FORMAT_D32_SFLOAT_S8_UINT || k->dsf == VK_FORMAT_D16_UNORM_S8_UINT ? (VkFormat)k->dsf : VK_FORMAT_UNDEFINED;
	gi.pNext = &ri;
	gi.stageCount = 2; gi.pStages = st;
	gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs;
	gi.pMultisampleState = &ms; gi.pDepthStencilState = &dss; gi.pColorBlendState = &cbs; gi.pDynamicState = &dy;
	gi.layout = playout;
	r = vkCreateGraphicsPipelines(vd, pcache, 1, &gi, 0, &p);
	if (r < 0) { fprintf(stderr, "xrc: vulkan: a pipeline failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
	return p;
}
/* the pipeline cache file (XRC_VK_PCACHE) */
static const char *pc_path(void) {
	const char *e = getenv("XRC_VK_PCACHE");
	if (e && (!*e || !strcmp(e, "-"))) return 0;
	return e ? e : "vk_pipelines.bin";
}
/* its contents when they are this driver's and GPU's (the header: length, version 1, vendor, device, cache UUID) */
static void *pc_load(size_t *n) {
	const char *path = pc_path();
	FILE *f;
	unsigned char *d = 0;
	long len;
	u32 h[4];
	*n = 0;
	if (!path || !(f = fopen(path, "rb"))) return 0;
	if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 32 && fseek(f, 0, SEEK_SET) == 0 && (d = (unsigned char *)malloc((size_t)len)) != 0
	    && fread(d, 1, (size_t)len, f) == (size_t)len) {
		memcpy(h, d, 16);
		if (h[0] >= 32 && h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h[2] == props.vendorID && h[3] == props.deviceID
		    && !memcmp(d + 16, props.pipelineCacheUUID, VK_UUID_SIZE)) *n = (size_t)len;
		else LOG("xrc: vulkan: %s is another driver's or GPU's pipeline cache: not used\n", path);
	}
	fclose(f);
	if (!*n) { free(d); d = 0; }
	return d;
}
typedef struct { void *d; size_t n; } PcSave;
static void *pc_writer(void *arg) {
#if !defined(_WIN32) && !defined(XRC_NO_TOOLS)
	pthread_setname_np(pthread_self(), "vk-pcache");
#endif
	PcSave *w = (PcSave *)arg;
	const char *path = pc_path();
	char tmp[1024];
	FILE *f;
	/* the cache's contents, here and not on the main thread: serializing it (megabytes) and the checksum took
	 * several ms of a frame (a hiccup in the profile: pc_maybe_save in Present). The cache is internally synchronized:
	 * the builders may add to it meanwhile */
	if (vkGetPipelineCacheData(vd, pcache, &w->n, 0) != VK_SUCCESS || !w->n || !(w->d = malloc(w->n))
	    || vkGetPipelineCacheData(vd, pcache, &w->n, w->d) < 0) goto done;
	{ /* unchanged (the new pipelines were all in the cache already): not written again (one writer at a time: pc_saving) */
		static size_t last_n;
		static u32 last_sum;
		u32 sum = 2166136261u;
		size_t i;
		for (i = 0; i < w->n; i++) sum = (sum ^ ((unsigned char *)w->d)[i]) * 16777619u;
		if (w->n == last_n && sum == last_sum) goto done;
		last_n = w->n; last_sum = sum;
	}
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if ((f = fopen(tmp, "wb")) != 0) {
		int ok = fwrite(w->d, 1, w->n, f) == w->n;
		ok = fclose(f) == 0 && ok;
#ifdef _WIN32
		if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
		if (ok) ok = rename(tmp, path) == 0;
#endif
		if (!ok) { fprintf(stderr, "xrc: vulkan: cannot write the pipeline cache %s\n", path); remove(tmp); }
		else LOG("xrc: vulkan: pipeline cache saved (%zu KB)\n", w->n / 1024);
	}
done:
	free(w->d); free(w);
	__atomic_store_n(&pc_saving, 0, __ATOMIC_RELEASE);
	return 0;
}
/* once per frame: the cache saved when pipelines were made and none for PC_QUIET ms (not in the middle of a burst) */
#define PC_QUIET 3000.0
static void pc_maybe_save(void) {
	PcSave *w;
	pthread_t t;
	if (!pc_new_ms || !pc_path() || __atomic_load_n(&pc_saving, __ATOMIC_ACQUIRE) || now_ms() - pc_new_ms < PC_QUIET) return;
	pc_new_ms = 0;
	w = (PcSave *)calloc(1, sizeof *w); /* (filled by the writer) */
	pc_saving = 1;
	if (pthread_create(&t, 0, pc_writer, w) != 0) { pc_saving = 0; free(w->d); free(w); return; }
	pthread_detach(t);
}
/* the pipeline builder: a thread makes the pipelines (vkCreateGraphicsPipelines, through the cache: compiling, and the
 * driver putting the code in GPU memory -- milliseconds even on a cache hit) while the game goes on; a draw whose
 * pipeline is not made yet is skipped (a frame or two). The table (ptab) is the main thread's: made pipelines come back
 * on the done list and are put in at the next lookup. Shader modules a queued job uses are destroyed only once the
 * queue is empty (dead_mods). XRC_VK_SYNC_PIPELINES=1: made in the draw instead (as before).
 * XRC_VK_PIPE_THREADS=N: N builders taking jobs from the one queue (default half the CPU threads, 1 to 8: the
 * launcher's Pipeline Build Threads "Best". Measured 2026-11-18 from an empty cache on the user's Ryzen, 3 builders
 * skipped no fewer draws than 1 -- each pipeline is made within the frame that first needs it -- so more mostly help
 * slower CPUs; idle builders cost nothing). make_pipeline uses only its locals, and the pipeline cache is internally synchronized (no
 * EXTERNALLY_SYNCHRONIZED flag), so they need nothing else; pj_pending is the main thread's alone. */
typedef struct PJob { PKey k; u32 hash; VkShaderModule vm, pm; VkPipeline p; struct PJob *next; int pre; } PJob;
static int pw_out;                           /* prewarm jobs queued or being made */
static pthread_mutex_t pj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pj_cond = PTHREAD_COND_INITIALIZER;
static PJob *pj_head, *pj_tail, *pj_done;
static int pj_started, pj_sync = -1;
static volatile int pj_pending;             /* queued or being made, not yet put in the table */
static unsigned long long n_pipe_waits;      /* draws skipped while their pipeline was being made */
static unsigned n_pipe_async;
static VkShaderModule *dead_mods; static u32 ndead_mods, capdead_mods;
#if defined(_WIN32) && !defined(XRC_NO_TOOLS)
void xrc_watch_snapshot(void);
void xrc_prof_snapshot(void);
static double hs_ms;
static void *hs_thread(void *arg) {
	unsigned long long seen = ~0ull;
	double t_seen = 0;
	int shots = 0, n_shot = 0;
	(void)arg;
	for (;;) {
		unsigned long long f = frame_no;
		double t = now_ms();
		Sleep(4);
		if (f != seen) { seen = f; t_seen = t; shots = 0; continue; }
		if (n_shot < 120 && shots < 8 && t - t_seen >= hs_ms + 40.0 * shots) {
			fprintf(stderr, "xrc: hitch stack: frame %llu not presented for %.0f ms (snapshot %d)\n", f, t - t_seen, shots + 1);
			if (shots == 0) xrc_watch_snapshot();
			xrc_prof_snapshot();
			shots++; n_shot++;
		}
	}
	return 0;
}
#endif
static void *pj_thread(void *arg) {
#if !defined(_WIN32) && !defined(XRC_NO_TOOLS)
	pthread_setname_np(pthread_self(), "vk-pipelines");
#endif
	(void)arg;
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); /* (the builders, prewarm included, never take time from the game's threads) */
#endif
	for (;;) {
		PJob *j;
		pthread_mutex_lock(&pj_lock);
		while (!pj_head) pthread_cond_wait(&pj_cond, &pj_lock);
		j = pj_head;
		pj_head = j->next;
		if (!pj_head) pj_tail = 0;
		pthread_mutex_unlock(&pj_lock);
		j->p = make_pipeline(&j->k, j->vm, j->pm);
		pthread_mutex_lock(&pj_lock);
		j->next = pj_done;
		pj_done = j;
		pthread_mutex_unlock(&pj_lock);
	}
	return 0;
}
/* the builders: XRC_VK_PIPE_THREADS (1..16), else half the CPU threads, 1 to 8 (the launcher's Best) */
static int pj_threads(void) {
	const char *e = getenv("XRC_VK_PIPE_THREADS");
	int n;
#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	n = (int)si.dwNumberOfProcessors;
#else
	n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
	if (e && atoi(e) > 0) return atoi(e) > 16 ? 16 : atoi(e);
	n /= 2;
	return n < 1 ? 1 : n > 8 ? 8 : n;
}
static PEnt *ptab_find(const PKey *k, u32 h) {
	u32 i;
	for (i = h & (pcap - 1); ptab[i].hash; i = (i + 1) & (pcap - 1))
		if (ptab[i].hash == h && !memcmp(&ptab[i].k, k, sizeof *k)) return &ptab[i];
	return 0;
}
/* the made pipelines put in the table; the deferred shader modules destroyed once nothing is queued */
static void pj_collect(void) {
	PJob *d;
	if (!__atomic_load_n(&pj_done, __ATOMIC_ACQUIRE) && (pj_pending || !ndead_mods)) return;
	pthread_mutex_lock(&pj_lock);
	d = pj_done;
	pj_done = 0;
	pthread_mutex_unlock(&pj_lock);
	while (d) {
		PJob *n = d->next;
		PEnt *e = ptab_find(&d->k, d->hash);
		if (e) { e->p = d->p; e->pending = 0; }
		pj_pending--;
		if (d->pre) pw_out--;
		pc_new_ms = now_ms();
		free(d);
		d = n;
	}
	if (!pj_pending && ndead_mods) {
		u32 k;
		for (k = 0; k < ndead_mods; k++) vkDestroyShaderModule(vd, dead_mods[k], 0);
		ndead_mods = 0;
	}
}
/* a released shader's module: destroyed now, or once the queued pipelines that may use it are made */
static void shader_module_free(VkShaderModule m) {
	if (!m) return;
	if (!pj_pending) { vkDestroyShaderModule(vd, m, 0); return; }
	if (ndead_mods == capdead_mods) { capdead_mods = capdead_mods ? capdead_mods * 2 : 64; dead_mods = (VkShaderModule *)realloc(dead_mods, capdead_mods * sizeof *dead_mods); }
	dead_mods[ndead_mods++] = m;
}
/* a draw whose pipeline is being made waits for it, up to a budget per frame (XRC_VK_PIPE_WAIT_MS, default 2; 0: never
 * wait): from the pipeline cache a builder makes one in a fraction of a millisecond, so most draws that were skipped
 * (an object missing for a frame or two -- a flicker as a zone streams in) are drawn now, and a burst of new
 * pipelines (a cold cache) still costs a frame no more than the budget. The rest are skipped, as before. */
static double pw_budget = -1, pw_used;
static unsigned long long pw_frame = ~0ull, n_pipe_waited;
static int pj_wait(PEnt *e) {
	double t0, end;
	if (pw_budget < 0) { const char *s = getenv("XRC_VK_PIPE_WAIT_MS"); pw_budget = s && *s ? atof(s) : 2.0; }
	if (pw_frame != frame_no) { pw_frame = frame_no; pw_used = 0; }
	if (pw_used >= pw_budget) return 0;
	t0 = now_ms(); end = t0 + pw_budget - pw_used;
	while (e->pending) {
		pj_collect();
		if (!e->pending || now_ms() >= end) break;
		sched_yield();
	}
	pw_used += now_ms() - t0;
	if (!e->pending) n_pipe_waited++;
	return !e->pending;
}
/* the table grown (rehashed) when half full */
static void ptab_grow(void) {
	if (pcount * 2 >= pcap) {
		u32 nc = pcap ? pcap * 2 : 1024, j;
		PEnt *nt = (PEnt *)calloc(nc, sizeof(PEnt));
		for (j = 0; j < pcap; j++) if (ptab[j].hash) {
			u32 x = ptab[j].hash & (nc - 1);
			while (nt[x].hash) x = (x + 1) & (nc - 1);
			nt[x] = ptab[j];
		}
		free(ptab);
		ptab = nt; pcap = nc;
	}
}
static void pj_start(void) {
	int n, b;
	if (pj_started) return;
	n = pj_threads();
	for (b = 0; b < n; b++) {
		pthread_t t;
		if (pthread_create(&t, 0, pj_thread, 0) != 0) { if (!b) die("starting the pipeline builder", VK_ERROR_INITIALIZATION_FAILED); break; }
		pthread_detach(t);
	}
	LOG("xrc: vulkan: %d pipeline builder thread%s\n", b, b == 1 ? "" : "s");
	pj_started = 1;
}
static void pj_push(PJob *j) {
	pthread_mutex_lock(&pj_lock);
	if (pj_tail) pj_tail->next = j; else pj_head = j;
	pj_tail = j;
	pthread_cond_signal(&pj_cond);
	pthread_mutex_unlock(&pj_lock);
}
static void pw_record(const PKey *k, const Shader *vs, const Shader *ps);
/* the pipeline for a draw: 0 and *waiting = 1 while it is being made */
static VkPipeline pipeline_for(const PKey *k, Shader *vs, Shader *ps, int *waiting) {
	u32 h = hash_key(k), i;
	PEnt *e;
	*waiting = 0;
	if (pj_sync < 0) pj_sync = getenv("XRC_VK_SYNC_PIPELINES") && atoi(getenv("XRC_VK_SYNC_PIPELINES")) ? 1 : 0;
	ptab_grow();
	pj_collect();
	if ((e = ptab_find(k, h)) != 0) {
		if (e->pending && !pj_wait(e)) { *waiting = 1; n_pipe_waits++; }
		return e->p;
	}
	for (i = h & (pcap - 1); ptab[i].hash; i = (i + 1) & (pcap - 1)) {}
	ptab[i].k = *k;
	ptab[i].hash = h;
	pcount++;
	pw_record(k, vs, ps);
	if (pj_sync) {
		HC(npipe, 1);
		ptab[i].p = make_pipeline(k, vs->mod, ps_module(ps, k));
		pc_new_ms = now_ms();
		return ptab[i].p;
	}
	pj_start();
	{
		PJob *j = (PJob *)calloc(1, sizeof *j);
		j->k = *k; j->hash = h; j->vm = vs->mod; j->pm = ps_module(ps, k);
		ptab[i].p = VK_NULL_HANDLE;
		ptab[i].pending = 1;
		pj_pending++;
		n_pipe_async++;
		HC(npipe, 1);
		pj_push(j);
	}
	if (pj_wait(&ptab[i])) return ptab[i].p; /* (made within the frame's wait budget) */
	*waiting = 1;
	n_pipe_waits++;
	return VK_NULL_HANDLE;
}

/* ============================================================================================ pipeline prewarm
 * Every pipeline key the game needs is recorded (once) to <XRC_VK_PCACHE>.keys; at the next boot a pump makes them all
 * on the builder threads, a few at a time and only while no draw's own pipeline is queued, so the first visit to a zone
 * finds its ~185 pipelines made instead of building them in one frame. A key names its shaders by a hash of their
 * bytecode (the serials are per run); a shader is only made when the game first draws with it, so the pump also makes
 * the translations itself (the game's shader objects not yet drawn with, ~1 ms a frame), and a key is queued once both
 * its shaders exist. Bounded: PW_MAX keys; a key invalid here (samples the GPU lacks, bad counts) is dropped. The file
 * has a header (magic, version incl. sizeof PKey, GPU, driver UUID): anything else is ignored and rewritten.
 * XRC_VK_PREWARM=0: no prewarm (the keys are still recorded). */
#define PW_MAX 4096
#define PW_VER 1u
typedef struct { unsigned long long vh, ph; PKey k; } PwRec;       /* (the file's record; k.vs = k.ps = 0) */
typedef struct { PwRec r; u8 state; u32 qvs, qps; } PwKey;          /* state: 0 waiting for its shaders, 1 queued (for the shader serials qvs, qps: a reloaded shader is queued again), 2 invalid */
typedef struct { u32 magic, ver, size, vendor, device, n; u8 uuid[16]; } PwHdr;
static PwKey *pw_keys; static u32 pw_n;
static u32 *pw_set; static u32 pw_setcap;                           /* key index + 1, open-addressed by record hash */
static int pw_on = -1, pw_dirty, pw_saving;
static double pw_dirty_ms;
static int pw_more; static u32 pw_loaded;                            /* keys read from the file at boot */
static u32 pw_gen, pw_seen_gen = ~0u, pw_cursor, pw_queued, pw_invalid, pw_logged = ~0u;
static struct { unsigned long long h; Shader *s; u32 serial; } *pw_reg_t; static u32 pw_regcap = 8192;
static char pw_path_buf[1100];
static const char *pw_path(void) {
	const char *p = pc_path();
	if (!p) return 0;
	snprintf(pw_path_buf, sizeof pw_path_buf, "%s.keys", p);
	return pw_path_buf;
}
static u32 pw_rec_hash(const PwRec *r) {
	return hash_key(&r->k) ^ (u32)(r->vh * 0x9e3779b97f4a7c15ull >> 32) ^ (u32)(r->ph * 0xc2b2ae3d27d4eb4full >> 29);
}
static int pw_find(const PwRec *r, u32 *slot) {
	u32 i;
	for (i = pw_rec_hash(r) & (pw_setcap - 1); pw_set[i]; i = (i + 1) & (pw_setcap - 1)) {
		const PwRec *q = &pw_keys[pw_set[i] - 1].r;
		if (q->vh == r->vh && q->ph == r->ph && !memcmp(&q->k, &r->k, sizeof r->k)) return 1;
	}
	*slot = i;
	return 0;
}
static int pw_add(const PwRec *r) {
	u32 slot;
	if (pw_n >= PW_MAX || pw_find(r, &slot)) return 0;
	pw_keys[pw_n].r = *r; pw_keys[pw_n].state = 0;
	pw_set[slot] = ++pw_n;
	return 1;
}
static void pw_init(void) {
	const char *e = getenv("XRC_VK_PREWARM"), *path;
	FILE *f;
	PwHdr h;
	pw_on = !(e && *e && !atoi(e));
	pw_keys = (PwKey *)calloc(PW_MAX, sizeof *pw_keys);
	pw_setcap = 16384;
	pw_set = (u32 *)calloc(pw_setcap, sizeof *pw_set);
	pw_reg_t = calloc(pw_regcap, sizeof *pw_reg_t);
	if (!(path = pw_path()) || !(f = fopen(path, "rb"))) return;
	if (fread(&h, sizeof h, 1, f) == 1 && h.magic == 0x314b5058u && h.ver == PW_VER && h.size == sizeof(PKey) && h.vendor == props.vendorID
	    && h.device == props.deviceID && !memcmp(h.uuid, props.pipelineCacheUUID, 16) && h.n <= PW_MAX) {
		PwRec r;
		u32 i;
		for (i = 0; i < h.n && fread(&r, sizeof r, 1, f) == 1; i++) pw_add(&r);
		if (i < h.n || fgetc(f) != EOF) { /* (truncated or with trailing bytes: not trusted, started fresh) */
			memset(pw_keys, 0, PW_MAX * sizeof *pw_keys); memset(pw_set, 0, pw_setcap * sizeof *pw_set);
			pw_n = 0;
			LOG("xrc: vulkan: %s is damaged: dropped\n", path);
		}
		pw_loaded = pw_n;
		LOG("xrc: vulkan: %u pipeline keys recorded in %s\n", pw_n, path);
	} else LOG("xrc: vulkan: %s is from another version, driver or GPU: dropped\n", path);
	fclose(f);
}
static void pw_reg(Shader *s) {
	u32 i;
	if (pw_on < 0) pw_init();
	u32 n;
	/* a shader with the bytecode of an earlier one (the game re-creates its shaders as zones stream) takes that one's serial: its
	 * pipeline keys are then equal, so the pipelines already made are found (not made again), and a recorded key is made once a run */
	for (i = (u32)(s->hash >> 7) & (pw_regcap - 1), n = 0; pw_reg_t[i].h && pw_reg_t[i].h != s->hash && n < pw_regcap; i = (i + 1) & (pw_regcap - 1), n++) {}
	if (n >= pw_regcap) return; /* (full: not registered) */
	if (pw_reg_t[i].h) s->serial = pw_reg_t[i].serial; else pw_reg_t[i].serial = s->serial;
	pw_reg_t[i].h = s->hash; pw_reg_t[i].s = s;
	pw_gen++;
}
static void pw_unreg(Shader *s) {
	u32 i, n = 0;
	if (pw_on < 0 || !s->hash) return;
	for (i = (u32)(s->hash >> 7) & (pw_regcap - 1); pw_reg_t[i].h && n++ < pw_regcap; i = (i + 1) & (pw_regcap - 1))
		if (pw_reg_t[i].h == s->hash) { if (pw_reg_t[i].s == s) pw_reg_t[i].s = 0; return; }
}
static Shader *pw_shader(unsigned long long h) {
	u32 i, n = 0;
	for (i = (u32)(h >> 7) & (pw_regcap - 1); pw_reg_t[i].h && n++ < pw_regcap; i = (i + 1) & (pw_regcap - 1))
		if (pw_reg_t[i].h == h) return pw_reg_t[i].s;
	return 0;
}
/* a pipeline made for a draw: recorded (once) */
static void pw_record(const PKey *k, const Shader *vs, const Shader *ps) {
	PwRec r;
	if (pw_on < 0) pw_init();
	if (!pw_path()) return;
	memset(&r, 0, sizeof r);
	r.k = *k; r.k.vs = r.k.ps = 0;
	r.vh = vs->hash; r.ph = ps->hash;
	if (pw_add(&r)) { PwKey *q = &pw_keys[pw_n - 1]; q->state = 1; q->qvs = vs->serial; q->qps = ps->serial; pw_dirty = 1; pw_dirty_ms = now_ms(); }
}
/* the snapshot to write: 16 bytes ahead of the records hold their count */
static void *pw_writer(void *arg) {
	PwRec *recs = (PwRec *)arg;
	PwHdr h;
	u32 n = ((u32 *)recs)[-4];
	char tmp[1200];
	FILE *f;
	const char *path = pw_path();
	memset(&h, 0, sizeof h);
	h.magic = 0x314b5058u; h.ver = PW_VER; h.size = sizeof(PKey); h.vendor = props.vendorID; h.device = props.deviceID; h.n = n;
	memcpy(h.uuid, props.pipelineCacheUUID, 16);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if ((f = fopen(tmp, "wb")) != 0) {
		int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(recs, sizeof *recs, n, f) == n;
		ok = fclose(f) == 0 && ok;
#ifdef _WIN32
		if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
		if (ok) ok = rename(tmp, path) == 0;
#endif
		if (!ok) remove(tmp);
		else LOG("xrc: vulkan: %u pipeline keys saved\n", n);
	}
	free((u32 *)recs - 4);
	__atomic_store_n(&pw_saving, 0, __ATOMIC_RELEASE);
	return 0;
}
/* the file rewritten when keys were added and none for PC_QUIET ms (a snapshot, written on a thread) */
static void pw_maybe_save(void) {
	u32 *blk, i;
	pthread_t t;
	if (!pw_dirty || __atomic_load_n(&pw_saving, __ATOMIC_ACQUIRE) || now_ms() - pw_dirty_ms < PC_QUIET) return;
	pw_dirty = 0;
	blk = (u32 *)malloc(16 + (size_t)pw_n * sizeof(PwRec));
	if (!blk) return;
	blk[0] = pw_n;
	for (i = 0; i < pw_n; i++) memcpy((PwRec *)(blk + 4) + i, &pw_keys[i].r, sizeof(PwRec));
	pw_saving = 1;
	if (pthread_create(&t, 0, pw_writer, blk + 4) != 0) { pw_saving = 0; free(blk); return; }
	pthread_detach(t);
}
static int pw_valid(const PKey *k, const Shader *vs, const Shader *ps) {
	u32 sc = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
	if (k->nrt > 4 || k->nattr > 16 || k->nbind > 16 || (!k->nrt && !k->dsf)) return 0;
	if (k->ms > 1 && (!(sc & k->ms) || (k->ms & (k->ms - 1)))) return 0;
	if (k->a2c && k->ms <= 1) return 0;
	if (k->nz && !ps->mod_nz) return 0;
	if (!vs->mod || !ps->mod) return 0;
	return 1;
}
/* once a frame: the game's shaders not yet drawn with made (budget), then the keys whose shaders exist queued (a few at a
 * time, only when no draw's own pipeline is queued) */
static void pw_pump(void) {
	u32 nthr, maxout, scanned = 0, i, start;
	double t0;
	if (pw_on < 0) pw_init();
	pw_maybe_save();
	if (!pw_on || !pw_n) return;
	if (pj_sync < 0) pj_sync = getenv("XRC_VK_SYNC_PIPELINES") && atoi(getenv("XRC_VK_SYNC_PIPELINES")) ? 1 : 0;
	if (pj_sync) return;
	pj_collect();
	t0 = now_ms();
	{ /* the shader objects: 256 a frame round and round, translating the ones with no host yet */
		static u32 cur = 1;
		for (; scanned < 256 && now_ms() - t0 < 1.0; scanned++) {
			Obj *o;
			if (cur >= d3_nobjs) cur = 1;
			o = &d3_ochunks[cur / 1024u][cur % 1024u];
			cur++;
			if ((o->kind == K_VS || o->kind == K_PS) && !o->host && o->size >= 8) shader_of(o);
		}
	}
	nthr = (u32)pj_threads();
	maxout = nthr < 2 ? 2 : nthr > 4 ? 4 : nthr;
	if (pw_seen_gen != pw_gen || pw_more) {
		if ((u32)pj_pending > (u32)pw_out) return; /* (a draw's own pipeline is queued: it goes first; tried again next frame) */
		if (!pw_more) pw_seen_gen = pw_gen;
		start = pw_more ? pw_cursor : 0;
		pw_more = 0;
		for (i = start; i < pw_n; i++) {
			PwKey *q = &pw_keys[i];
			Shader *vs, *ps;
			PKey k;
			u32 h, x;
			PEnt *e;
			if (q->state == 2) continue;
			if (!(vs = pw_shader(q->r.vh)) || !(ps = pw_shader(q->r.ph))) continue; /* (waits for them) */
			if (q->state == 1 && q->qvs == vs->serial && q->qps == ps->serial) continue;
			if ((u32)pw_out >= maxout) { pw_cursor = i; pw_more = 1; goto logit; }
			if (!pw_valid(&q->r.k, vs, ps)) { q->state = 2; pw_invalid++; continue; }
			k = q->r.k; k.vs = vs->serial; k.ps = ps->serial;
			h = hash_key(&k);
			q->state = 1; q->qvs = vs->serial; q->qps = ps->serial;
			ptab_grow();
			if (ptab_find(&k, h)) continue; /* (a draw got there first) */
			for (x = h & (pcap - 1); ptab[x].hash; x = (x + 1) & (pcap - 1)) {}
			e = &ptab[x];
			e->k = k; e->hash = h; e->p = VK_NULL_HANDLE; e->pending = 1;
			pcount++;
			pj_start();
			{
				PJob *j = (PJob *)calloc(1, sizeof *j);
				j->k = k; j->hash = h; j->vm = vs->mod; j->pm = ps_module(ps, &k); j->pre = 1;
				pj_pending++; pw_out++; pw_queued++; n_pipe_async++;
				pj_push(j);
			}
		}
		pw_cursor = 0;
	}
logit:
	if (pw_queued != pw_logged && !pw_out && !pw_more) {
		u32 waiting = 0;
		for (i = 0; i < pw_n; i++) waiting += pw_keys[i].state == 0;
		pw_logged = pw_queued;
		LOG("xrc: vulkan: pipeline prewarm: %u of %u recorded keys built (%u invalid here, %u waiting for their shaders)\n", pw_queued, pw_loaded, pw_invalid, waiting);
	}
}

/* ============================================================================================ samplers */
typedef struct { u32 k[9]; VkSampler s; } SEnt;
/* forced anisotropic filtering on mipmapped linear samplers (XRC_ANISO, default 16; 0 = only where the game asks): the game
   asks for it on few surfaces, so floors and walls seen at an angle blur. Live from the F10 menu (part of the sampler key). */
static int aniso_force = -1;
void xrc_vk_set_aniso(int n) { aniso_force = n < 0 ? 0 : n > 16 ? 16 : n; }
static SEnt stab[512];
static u32 nsamp;
static VkSampler sampler_for(u32 s) {
	const u32 *ss = dev.ss[s];
	u32 key[9];
	int want;
	if (aniso_force < 0) { const char *e = getenv("XRC_ANISO"); xrc_vk_set_aniso(e && *e ? atoi(e) : 16); }
	want = (ss[5] == 3 || ss[6] == 3) ? (ss[10] < 1 ? 1 : (int)ss[10]) : 0;
	if (aniso_force > want && ss[7] != 0 && ss[6] >= 2) want = aniso_force;
	key[0] = ss[1]; key[1] = ss[2]; key[2] = ss[3]; key[3] = ss[4]; key[4] = ss[5] | ss[6] << 8 | ss[7] << 16;
	key[5] = ss[8]; key[6] = ss[9]; key[7] = ss[10]; key[8] = (u32)want;
	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	VkSamplerAddressMode am[6] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
	                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
	u32 i;
	float bias;
	{ static u32 last_i[32]; if (s < 32 && last_i[s] < nsamp && !memcmp(stab[last_i[s]].k, key, sizeof key)) return stab[last_i[s]].s;
	  for (i = 0; i < nsamp; i++) if (!memcmp(stab[i].k, key, sizeof key)) { if (s < 32) last_i[s] = i; return stab[i].s; } }
	if (nsamp == 512) return stab[0].s;
	si.magFilter = ss[5] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	si.minFilter = ss[6] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	si.mipmapMode = ss[7] == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = am[ss[1] < 6 ? ss[1] : 0];
	si.addressModeV = am[ss[2] < 6 ? ss[2] : 0];
	si.addressModeW = am[ss[3] < 6 ? ss[3] : 0];
	if (si.addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE || si.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE || si.addressModeW == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE) {
		if (si.addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE) si.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		if (si.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE) si.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		if (si.addressModeW == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE) si.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	}
	memcpy(&bias, &ss[8], 4);
	si.mipLodBias = bias;
	si.minLod = (float)ss[9];
	si.maxLod = ss[7] == 0 ? (float)ss[9] : VK_LOD_CLAMP_NONE;
	if (want && have_aniso) {
		si.anisotropyEnable = 1;
		si.maxAnisotropy = (float)(want > 16 ? 16 : want);
		if (si.maxAnisotropy > props.limits.maxSamplerAnisotropy) si.maxAnisotropy = props.limits.maxSamplerAnisotropy;
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	}
	si.borderColor = ss[4] == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : ss[4] == 0xff000000u ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
	               : (ss[4] >> 24) >= 0x80 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	memcpy(stab[nsamp].k, key, sizeof key);
	VKC(vkCreateSampler(vd, &si, 0, &stab[nsamp].s));
	return stab[nsamp++].s;
}

/* placeholder images for samplers with nothing (or the wrong kind) bound: 1x1, (0, 0, 0, 1) */
static VkImageView dummy_view[5];
static Res *dummy_res[5];
static void make_dummies(void) {
	int k;
	for (k = 2; k <= 4; k++) {
		Res *r = (Res *)calloc(1, sizeof *r);
		VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		VkMemoryRequirements rq;
		VkClearColorValue col;
		VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
		r->fmt = VK_FORMAT_R8G8B8A8_UNORM;
		r->cube = k == 3; r->vol = k == 4; r->layers = r->cube ? 6 : 1; r->levels = 1;
		ci.flags = r->cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
		ci.imageType = r->vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
		ci.format = r->fmt;
		ci.extent.width = ci.extent.height = ci.extent.depth = 1;
		ci.mipLevels = 1; ci.arrayLayers = r->layers; ci.samples = VK_SAMPLE_COUNT_1_BIT;
		ci.tiling = VK_IMAGE_TILING_OPTIMAL;
		ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VKC(vkCreateImage(vd, &ci, 0, &r->img));
		vkGetImageMemoryRequirements(vd, r->img, &rq);
		r->mem = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
		VKC(vkBindImageMemory(vd, r->img, blocks[r->mem.block].mem, r->mem.off));
		r->view = make_view(r, r->cube ? VK_IMAGE_VIEW_TYPE_CUBE : r->vol ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, SW(I_, I_, I_, I_), 0, 1, 0, r->layers, 0);
		img_use(r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		memset(&col, 0, sizeof col);
		col.float32[3] = 1.0f;
		vkCmdClearColorImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &sr);
		dummy_res[k] = r;
		dummy_view[k] = r->view;
	}
	barrier();
	for (k = 0; k < 5; k++) if (dummy_res[k]) img_use(dummy_res[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* ============================================================================================ rendering */
static float f32(u32 b) { float f; memcpy(&f, &b, 4); return f; }

static void ms_mismatch(void) {
	static int said;
	if (!said++) fprintf(stderr, "xrc: vulkan: render targets of different sample counts together: the others are left out\n");
}
#ifdef XRC_LR
static void fxaa_scene(u32 prev);
#endif
/* the attachments: the device's render targets and depth surface; a rendering begins when they changed (on the GPU:
 * render_go) */
static int begin_render(void) {
	VkRenderingAttachmentInfo *ca = r_ca, *da = &r_da, *sa = &r_sa;
	VkRenderingInfo *ri = &r_ri;
	u32 k, w = 0xffffffffu, h = 0xffffffffu, n = 0, sm;
	Res *dr = 0;
	if (in_render && !memcmp(cur_rt, dev.rt, sizeof cur_rt) && cur_ds == dev.ds) return 1;
	dyn_gen++; /* (fxaa_scene below draws with its own state) */
	end_render();
#ifdef XRC_LR
	if (cur_rt[0] && !cur_ds && !dev.ds && cur_rt[0] != dev.rt[0]) fxaa_scene(cur_rt[0]);
#endif
	sm = rt_samples(); /* (attachments of other sample counts are left out: Vulkan wants them all alike) */
	memset(ca, 0, sizeof r_ca);
	for (k = 0; k < 4; k++) {
		Obj *s = obj_of(dev.rt[k]);
		u32 face, level;
		Res *r;
		if (!s) continue;
		r = surface_image(s, &face, &level);
		if (!r || r->depth) continue;
		if (r->samples != sm) { ms_mismatch(); continue; }
		ca[n].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		r_res[n] = r; r_fl[n][0] = face; r_fl[n][1] = level;
		ca[n].imageView = attach_view(r, face, level);
		ca[n].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		ca[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		if (dim(r->w, level) < w) w = dim(r->w, level);
		if (dim(r->h, level) < h) h = dim(r->h, level);
		n++;
	}
	memset(da, 0, sizeof *da);
	memset(sa, 0, sizeof *sa);
	{
		Obj *s = obj_of(dev.ds);
		u32 face, level;
		if (s && (dr = surface_image(s, &face, &level)) && dr->depth && dr->samples != sm) { ms_mismatch(); dr = 0; }
		if (dr && dr->depth) {
			da->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			r_dres = dr; r_dfl[0] = face; r_dfl[1] = level;
			da->imageView = attach_view(dr, face, level);
			da->imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			da->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			da->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			*sa = *da;
			if (dim(dr->w, level) < w) w = dim(dr->w, level);
			if (dim(dr->h, level) < h) h = dim(dr->h, level);
		} else dr = 0;
	}
	if (!n && !dr) return 0;
	memset(ri, 0, sizeof *ri);
	ri->sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	ri->renderArea.extent.width = w; ri->renderArea.extent.height = h;
	ri->layerCount = 1;
	ri->colorAttachmentCount = n;
	ri->pColorAttachments = ca;
	if (dr) { ri->pDepthAttachment = da; if (dr->stencil) ri->pStencilAttachment = sa; }
	r_pending = 1; r_cleared = 0; r_folded = 0;
	if (npcl) { /* deferred clears of these attachments: the load ops when the rendering covers them, else made now */
		int i;
		for (k = 0; k < n; k++) {
			if ((i = pcl_find(r_res[k], r_fl[k][0], r_fl[k][1])) < 0) continue;
			if (pcl[i].w != w || pcl[i].h != h || pcl[i].asp != VK_IMAGE_ASPECT_COLOR_BIT) { clear_emit(i); continue; }
			ca[k].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[k].clearValue = pcl[i].cv;
			r_cleared = 1; r_folded++;
			pcl_drop(i);
		}
		if (dr && (i = pcl_find(dr, r_dfl[0], r_dfl[1])) >= 0) {
			if (pcl[i].w != w || pcl[i].h != h || (pcl[i].asp & VK_IMAGE_ASPECT_COLOR_BIT)) { clear_emit(i); }
			else {
				if (pcl[i].asp & VK_IMAGE_ASPECT_DEPTH_BIT) { da->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da->clearValue = pcl[i].dv; }
				if (pcl[i].asp & VK_IMAGE_ASPECT_STENCIL_BIT) { sa->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; sa->clearValue = pcl[i].dv; }
				r_cleared = 1; r_folded++;
				pcl_drop(i);
			}
		}
	}
	in_render = 1;
	memcpy(cur_rt, dev.rt, sizeof cur_rt);
	cur_ds = dev.ds;
	cur_extent = ri->renderArea.extent;
	return 1;
}
static void render_go(void) {
	u32 k;
	if (!r_pending) return;
	r_pending = 0;
	for (k = 0; k < r_ri.colorAttachmentCount; k++) {
		bq_add(r_res[k], r_ca[k].imageLayout);
		img_access(r_res[k], 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	}
	if (r_ri.pDepthAttachment) {
		if (r_da.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) ZREAD(r_dres);
		bq_add(r_dres, r_da.imageLayout);
		img_access(r_dres, 1, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	}
	bq_go();
#ifndef XRC_NO_TOOLS
	if (ppool && pp_rec[fi] && pp_n[fi] < PP_MAX) {
		PPass *q = &pp[fi][pp_n[fi]];
		Obj *s0 = obj_of(cur_rt[0]), *sd = obj_of(cur_ds);
		u32 f0, l0;
		Res *r0 = s0 ? surface_image(s0, &f0, &l0) : 0, *rd = sd ? surface_image(sd, &f0, &l0) : 0;
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ppool, (u32)fi * 2u * PP_MAX + 2u * pp_n[fi]);
		memset(q, 0, sizeof *q);
		q->w = r_ri.renderArea.extent.width; q->h = r_ri.renderArea.extent.height; q->ncol = r_ri.colorAttachmentCount;
		q->depth = r_ri.pDepthAttachment != 0; q->rt0 = cur_rt[0];
		q->fmt = r0 && !r0->depth ? (u32)r0->fmt : rd ? (u32)rd->fmt : 0;
		q->clears = (u32)r_folded;
		q->blits = pp_ops[0]; q->fills = pp_ops[1]; q->ups = pp_ops[2]; q->upb = pp_ops[3]; q->reads = pp_ops[4];
		memset(pp_ops, 0, sizeof pp_ops);
		pp_open = 1;
		if (!hitch_ms)
			fprintf(stderr, "xrc: vulkan: (the frame of the renderings below) rendering %u: rt0 %08x img %p load %d store %d, depth %08x img %p load %d store %d\n", pp_n[fi], cur_rt[0],
			        r0 ? (void *)r0->img : 0, (int)r_ca[0].loadOp, (int)r_ca[0].storeOp, cur_ds, rd ? (void *)rd->img : 0, (int)r_da.loadOp, (int)r_da.storeOp);
	}
#endif
	vkCmdBeginRendering(cb, &r_ri);
#ifndef XRC_NO_TOOLS
	if (dt_rec[fi] && dt_n[fi] < DT_MAX) {
		DtDraw *d = &dt[fi][dt_n[fi]];
		memset(d, 0, sizeof *d);
		d->pass = ++dt_pass;
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dtpool, (u32)fi * DT_MAX + dt_n[fi]);
		if (dspool) { /* (an empty one: every query read back must have been written) */
			vkCmdBeginQuery(cb, dspool, (u32)fi * DT_MAX + dt_n[fi], 0);
			vkCmdEndQuery(cb, dspool, (u32)fi * DT_MAX + dt_n[fi]);
		}
		dt_n[fi]++;
	}
#endif
	n_renders++; n_px += (double)r_ri.renderArea.extent.width * r_ri.renderArea.extent.height;
}

static void set_viewport_scissor(void) {
	VkViewport v;
	VkRect2D sc;
	float x = (float)(int)dev.vp[0], y = (float)(int)dev.vp[1], w = (float)dev.vp[2], h = (float)dev.vp[3];
	v.x = x; v.y = y + h; v.width = w; v.height = -h;
	v.minDepth = f32(dev.vp[4]); v.maxDepth = f32(dev.vp[5]);
	vkCmdSetViewport(cb, 0, 1, &v);
	sc.offset.x = 0; sc.offset.y = 0; sc.extent = cur_extent;
	if (dev.rs[174]) { /* SCISSORTESTENABLE: RECT left top right bottom */
		int l = (int)dev.scissor[0], t = (int)dev.scissor[1], r = (int)dev.scissor[2], b = (int)dev.scissor[3];
		if (l < 0) l = 0;
		if (t < 0) t = 0;
		if (r > (int)cur_extent.width) r = (int)cur_extent.width;
		if (b > (int)cur_extent.height) b = (int)cur_extent.height;
		sc.offset.x = l; sc.offset.y = t;
		sc.extent.width = r > l ? (u32)(r - l) : 0; sc.extent.height = b > t ? (u32)(b - t) : 0;
	}
	vkCmdSetScissor(cb, 0, 1, &sc);
}

/* vertices per primitive count */
static u32 vcount(u32 type, u32 n) {
	switch (type) {
	case 1: return n;
	case 2: return 2 * n;
	case 3: return n + 1;
	case 4: return 3 * n;
	case 5: case 6: return n + 2;
	default: return 0;
	}
}
static VkPrimitiveTopology topo_of(u32 type) {
	switch (type) {
	case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; /* (fans become lists) */
	}
}

#if defined(XRC_LR) && defined(_WIN32)
/* ============================================================================================ mesh dump
 * F7 / XRC_MESH_DUMP_AT=<seconds since the process started>: the meshes drawn in the next whole frame -- each draw with a
 * POSITION + TEXCOORD0 declaration and a hashed texture on sampler 0: its vertices (the index range), triangles, the
 * sampler 0-3 content hashes (tex_check's) and the vertex shader constants -- saved (a thread) as
 * XRC_MESH_DIR/mesh_<frame>.glb (default: mesh_dumps beside the exe) and mesh_<frame>.json (a summary). Positions,
 * UVs, joints and weights as stored (decoded from the declaration's type); normals made unit length. */
typedef struct { char *p; size_t n, cap; } MdStr;
typedef struct {
	unsigned long long key, th[4];
	u32 draw, nv, ntri, vs, prim, has_n, has_j;
	float *pos, *nrm, *uv, *uv1, *wt;
	u16 *jnt;
	u32 *idx;
	float vsc[128], vsx[64]; /* (vsx: c169-c184: bbox scale and offset, the UV transform c180-c183) */
	MdStr decl, named; /* (json text: the declaration's elements; the shader's named constants, from its CTAB) */
} MDraw;
typedef struct { MDraw *d; u32 n; unsigned long long frame; u32 seq, sk_decl, sk_tex, sk_dup, sk_shadow, sk_other; } MdJob;
static volatile int md_req;
static int md_on; /* (this frame's draws are taken) */
static MDraw *md_d;
static u32 md_n, md_max, md_seq, md_sk_decl, md_sk_tex, md_sk_dup, md_sk_shadow, md_sk_other;
void xrc_vk_mesh_request(void) { md_req = 1; }
static void md_printf(MdStr *b, const char *fmt, ...) {
	va_list ap;
	int k;
	for (;;) {
		size_t room = b->cap - b->n;
		char *q;
		va_start(ap, fmt); k = vsnprintf(b->p ? b->p + b->n : 0, room, fmt, ap); va_end(ap);
		if (k < 0) return;
		if ((size_t)k < room) { b->n += (size_t)k; return; }
		q = (char *)realloc(b->p, (b->cap + (size_t)k + 1) * 2);
		if (!q) return;
		b->p = q; b->cap = (b->cap + (size_t)k + 1) * 2;
	}
}
static int md_fin(float f) { u32 u; memcpy(&u, &f, 4); return (u & 0x7f800000u) != 0x7f800000u; } /* (-ffast-math: no isfinite) */
static void md_floats(MdStr *b, const float *v, u32 n) {
	u32 i;
	md_printf(b, "[");
	for (i = 0; i < n; i++) md_printf(b, i ? ",%.9g" : "%.9g", md_fin(v[i]) ? (double)v[i] : 0.0);
	md_printf(b, "]");
}
static float md_half(u16 h) {
	u32 s = (u32)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
	float f;
	if (!e) { f = (float)m * (1.0f / 16777216.0f); return s ? -f : f; } /* (subnormal: m * 2^-24) */
	u = e == 31 ? s | 0x7f800000u | (m << 13) : s | ((e + 112) << 23) | (m << 13);
	memcpy(&f, &u, 4);
	return f;
}
static u32 md_tbytes(u32 t) { return t <= 3 ? 4 * (t + 1) : (t == 7 || t == 10 || t == 12 || t == 16) ? 8 : 4; }
static u32 md_ncomp(u32 t) { return t <= 3 ? t + 1 : (t == 6 || t == 9 || t == 11 || t == 15) ? 2 : (t == 13 || t == 14) ? 3 : 4; }
/* one element at a (D3DDECLTYPE t): x y z w as the shader reads it (missing ones 0 0 0 1) */
static void md_elem(u32 a, u32 t, float *o) {
	u32 i, n = md_ncomp(t);
	o[0] = o[1] = o[2] = 0; o[3] = 1;
	switch (t) {
	case 0: case 1: case 2: case 3: for (i = 0; i < n; i++) { u32 u = M32(a + 4 * i); memcpy(&o[i], &u, 4); } break;
	case 4: o[0] = M8(a + 2) / 255.0f; o[1] = M8(a + 1) / 255.0f; o[2] = M8(a) / 255.0f; o[3] = M8(a + 3) / 255.0f; break; /* (D3DCOLOR: B G R A) */
	case 5: for (i = 0; i < 4; i++) o[i] = (float)M8(a + i); break;
	case 8: for (i = 0; i < 4; i++) o[i] = M8(a + i) / 255.0f; break;
	case 6: case 7: for (i = 0; i < n; i++) o[i] = (float)(int16_t)M16(a + 2 * i); break;
	case 9: case 10: for (i = 0; i < n; i++) { int v = (int16_t)M16(a + 2 * i); o[i] = v < -32767 ? -1.0f : v / 32767.0f; } break;
	case 11: case 12: for (i = 0; i < n; i++) o[i] = M16(a + 2 * i) / 65535.0f; break;
	case 13: case 14: {
		u32 u = M32(a);
		for (i = 0; i < 3; i++) {
			int c = (int)((u >> (10 * i)) & 1023);
			if (t == 13) o[i] = (float)c;
			else { if (c & 512) c -= 1024; o[i] = c < -511 ? -1.0f : c / 511.0f; }
		}
		break;
	}
	case 15: case 16: for (i = 0; i < n; i++) o[i] = md_half(M16(a + 2 * i)); break;
	}
}
static const char *const md_tname[17] = {"FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR", "UBYTE4", "SHORT2", "SHORT4", "UBYTE4N",
	"SHORT2N", "SHORT4N", "USHORT2N", "USHORT4N", "UDEC3", "DEC3N", "FLOAT16_2", "FLOAT16_4"};
static const char *const md_uname[14] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
	"TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};
/* a texture's content hash: tex_check's (r->th), else made the same way (no HD textures loaded: tex_check not run) */
static unsigned long long md_tex_hash(Obj *o) {
	Res *r;
	u32 src, size;
	if (!o || o->kind != K_TEX) return 0;
	r = res_of(o);
	if (r && r->th) return r->th;
	if ((o->usage & 3) || o->w < 32 || o->h < 32 || o->w > 4096 || o->h > 4096 ||
	    !(o->fmt == 21 || o->fmt == 22 || o->fmt == FOURCC('D', 'X', 'T', '1') || o->fmt == FOURCC('D', 'X', 'T', '3') || o->fmt == FOURCC('D', 'X', 'T', '5')))
		return 0;
	src = level_mem(o, 0, 0); size = level_size(o, 0);
	if (!src || !size) return 0;
	return tex_hash(xrc_mem + src, size, (unsigned long long)o->fmt << 32 ^ (unsigned long long)o->w << 16 ^ o->h);
}
/* the vertex shader's named constants (its CTAB comment): {"name":{"set":"f","reg":r,"n":n,"v":[values]}, ...} */
static void md_ctab(Obj *vso, MdStr *b) {
	u32 p = vso->mem, end = vso->mem + vso->size, t, first = 1;
	md_printf(b, "{");
	if (vso->size < 8) { md_printf(b, "}"); return; }
	for (p += 4; p + 8 <= end; p += 4 + 4 * (t >> 16)) { /* (the comments after the version token) */
		u32 c, sz, n, ci, k;
		t = M32(p);
		if ((t & 0xffff) != 0xfffe) break;
		if (M32(p + 4) != FOURCC('C', 'T', 'A', 'B') || (t >> 16) < 8) continue;
		c = p + 8; sz = 4 * (t >> 16) - 4; /* (the table: offsets from c) */
		n = M32(c + 12); ci = M32(c + 16);
		for (k = 0; k < n && ci + 20 * (k + 1) <= sz; k++) {
			u32 e = c + ci + 20 * k, nm = M32(e), set = M16(e + 4), reg = M16(e + 6), cnt = M16(e + 8), j;
			if (set > 2 || nm >= sz) continue; /* (samplers left out) */
			md_printf(b, first ? "\"" : ",\"");
			first = 0;
			for (j = 0; nm + j < sz && j < 64; j++) {
				u32 ch = M8(c + nm + j);
				if (!ch) break;
				if (ch > 32 && ch < 127 && ch != '"' && ch != 92) md_printf(b, "%c", (int)ch);
			}
			md_printf(b, "\":{\"set\":\"%c\",\"reg\":%u,\"n\":%u,\"v\":", "bif"[set], reg, cnt);
			if (set == 2) { if (reg >= 256) cnt = 0; else if (reg + cnt > 256) cnt = 256 - reg; md_floats(b, dev.vsf + 4 * reg, 4 * cnt); }
			else {
				md_printf(b, "[");
				for (j = 0; j < cnt && reg + j < 16; j++) {
					if (set == 0) md_printf(b, j ? ",%d" : "%d", dev.vsb[reg + j]);
					else md_printf(b, j ? ",%d,%d,%d,%d" : "%d,%d,%d,%d", dev.vsi[4 * (reg + j)], dev.vsi[4 * (reg + j) + 1], dev.vsi[4 * (reg + j) + 2], dev.vsi[4 * (reg + j) + 3]);
				}
				md_printf(b, "]");
			}
			md_printf(b, "}");
		}
		break;
	}
	md_printf(b, "}");
}
static void md_free(MDraw *d) { free(d->pos); free(d->nrm); free(d->uv); free(d->uv1); free(d->wt); free(d->jnt); free(d->idx); free(d->decl.p); free(d->named.p); }
/* a draw of the frame taken (be_draw2, its textures ready): the vertices its indices reach, as triangle lists */
static void md_draw(int method, const u32 *a, u32 np, u32 base_vertex, u32 min_index, u32 first, Obj *vso, Shader *vs, Obj *decl) {
	u32 type = a[0], nel = decl->size ? decl->size - 1 : 0, e, k, n, i, vmin = 0xffffffffu, vmax = 0, nvx, isz = 2, isrc = 0, ntri;
	int el[6] = {-1, -1, -1, -1, -1, -1}; /* (POSITION NORMAL TEXCOORD0 BLENDINDICES BLENDWEIGHT: element numbers) */
	u32 sbase[15], sstride[15], ssize[15], used = 0, ev[6][3]; /* (ev: stream offset type) */
	unsigned long long th[4], key;
	u32 *vix;
	MDraw *d;
	md_seq++;
	{ /* (the shadow maps: like XRC_NO_SHADOWS) */
		Obj *s0 = obj_of(dev.rt[0]);
		u32 f, l, sm = M32(0x026e4f3cu);
		Res *r0 = s0 ? surface_image(s0, &f, &l) : 0;
		if (r0 && r0->d3dfmt == 114 && dim(r0->w, l) == sm && dim(r0->h, l) == sm) { md_sk_shadow++; return; }
	}
	for (e = 0; e < nel; e++) {
		u32 p = decl->mem + 8 * e, s = M16(p), t = M8(p + 4), u = M8(p + 6), ui = M8(p + 7);
		int w = u == 0 ? 0 : u == 3 ? 1 : u == 5 ? 2 : u == 2 ? 3 : u == 1 ? 4 : -1;
		if (u == 5 && ui == 7) w = 3; /* (LR: joint indices are TEXCOORD7 (UBYTE4), weights TEXCOORD6 (UBYTE4N)) */
		else if (u == 5 && ui == 6) w = 4;
		else if (u == 5 && ui == 1) w = 5; /* (second UV set) */
		else if (ui) w = -1;
		if (s >= 15) break;
		if (t > 16 || w < 0 || el[w] >= 0) continue;
		el[w] = (int)e; ev[w][0] = s; ev[w][1] = M16(p + 2); ev[w][2] = t;
	}
	if (el[0] < 0 || el[2] < 0) { md_sk_decl++; return; }
	for (k = 0; k < 4; k++) th[k] = md_tex_hash(obj_of(dev.tex[k]));
	if (!th[0]) { md_sk_tex++; return; }
	if ((type != 4 && type != 5) || md_n >= 16384) { md_sk_other++; return; }
	if (el[3] < 0 || el[4] < 0) el[3] = el[4] = -1; /* (joints and weights: both or neither) */
	for (k = 0; k < 6; k++) if (el[k] >= 0) used |= 1u << ev[k][0];
	/* the streams: vertex v of stream s at sbase + v * stride */
	for (k = 0; k < 15; k++) {
		if (!(used & (1u << k))) continue;
		if (method >= 2) {
			if (k) { md_sk_other++; return; }
			sbase[k] = method == 2 ? a[2] : a[6]; sstride[k] = method == 2 ? a[3] : a[7]; ssize[k] = 0xffffffffu;
		} else {
			Obj *vb = obj_of(dev.stream[k][0]);
			if (!vb || vb->kind != K_VB || !vb->mem || dev.stream[k][1] >= vb->size) { md_sk_other++; return; }
			sbase[k] = vb->mem + dev.stream[k][1]; sstride[k] = dev.stream[k][2]; ssize[k] = vb->size - dev.stream[k][1];
		}
	}
	/* the vertex numbers, in draw order */
	n = vcount(type, np);
	if (n < 3) { md_sk_other++; return; }
	if (method == 1) {
		Obj *ib = obj_of(dev.ib);
		if (!ib || ib->kind != K_IB || !ib->mem) { md_sk_other++; return; }
		isz = ib->fmt == 102 ? 4 : 2;
		if ((unsigned long long)first * isz + (unsigned long long)n * isz > ib->size) { md_sk_other++; return; }
		isrc = ib->mem + first * isz;
	} else if (method == 3) { isrc = a[4]; isz = a[5] == 102 ? 4 : 2; }
	vix = (u32 *)malloc((size_t)n * 4);
	if (!vix) return;
	for (i = 0; i < n; i++) {
		u32 v = method == 0 ? first + i : method == 2 ? i : isz == 4 ? M32(isrc + 4 * i) : M16(isrc + 2 * i);
		if (method == 1) v += base_vertex;
		vix[i] = v;
		if (v < vmin) vmin = v;
		if (v > vmax) vmax = v;
	}
	(void)min_index;
	nvx = vmax - vmin + 1;
	if (nvx > (1u << 20)) { free(vix); md_sk_other++; return; }
	for (k = 0; k < 6; k++) if (el[k] >= 0) { /* (every element of every vertex inside its buffer) */
		u32 s = ev[k][0];
		unsigned long long hi = (unsigned long long)vmax * sstride[s] + ev[k][1] + md_tbytes(ev[k][2]);
		if (method < 2 && hi > ssize[s]) { free(vix); md_sk_other++; return; }
	}
	{ /* (the same buffer, range and texture again in this frame: once) */
		u32 w[12];
		key = 0xcbf29ce484222325ull;
		w[0] = (u32)method; w[1] = type; w[2] = np; w[3] = sbase[ev[0][0]]; w[4] = sstride[ev[0][0]]; w[5] = method == 1 ? dev.ib : 0;
		w[6] = first; w[7] = base_vertex; w[8] = isrc; w[9] = vmin; w[10] = (u32)th[0]; w[11] = (u32)(th[0] >> 32);
		for (i = 0; i < 12; i++) key = (key ^ w[i]) * 0x100000001b3ull;
		for (i = 0; i < md_n; i++) if (md_d[i].key == key) { free(vix); md_sk_dup++; return; }
	}
	if (md_n == md_max) {
		MDraw *q = (MDraw *)realloc(md_d, sizeof *md_d * (md_max ? md_max * 2 : 256));
		if (!q) { free(vix); return; }
		md_d = q; md_max = md_max ? md_max * 2 : 256;
	}
	d = &md_d[md_n];
	memset(d, 0, sizeof *d);
	d->key = key; memcpy(d->th, th, sizeof th);
	d->draw = md_seq - 1; d->nv = nvx; d->vs = vs->serial; d->prim = type;
	d->has_n = el[1] >= 0; d->has_j = el[3] >= 0;
	d->pos = (float *)malloc((size_t)nvx * 12); d->uv = (float *)malloc((size_t)nvx * 8); if (el[5] >= 0) d->uv1 = (float *)malloc((size_t)nvx * 8);
	if (d->has_n) d->nrm = (float *)malloc((size_t)nvx * 12);
	if (d->has_j) { d->jnt = (u16 *)malloc((size_t)nvx * 8); d->wt = (float *)malloc((size_t)nvx * 16); }
	d->idx = (u32 *)malloc((size_t)n * 3 * 4);
	if (!d->pos || !d->uv || !d->idx || (d->has_n && !d->nrm) || (d->has_j && (!d->jnt || !d->wt))) { md_free(d); free(vix); return; }
	for (i = 0; i < nvx; i++) {
		float o[4];
		u32 v = vmin + i, c;
#define MD_AT(w) (sbase[ev[w][0]] + v * sstride[ev[w][0]] + ev[w][1])
		md_elem(MD_AT(0), ev[0][2], o);
		for (c = 0; c < 3; c++) d->pos[3 * i + c] = md_fin(o[c]) ? o[c] : 0.0f;
		md_elem(MD_AT(2), ev[2][2], o);
		for (c = 0; c < 2; c++) d->uv[2 * i + c] = md_fin(o[c]) ? o[c] : 0.0f;
		if (d->uv1) { md_elem(MD_AT(5), ev[5][2], o); for (c = 0; c < 2; c++) d->uv1[2 * i + c] = md_fin(o[c]) ? o[c] : 0.0f; }
		if (d->has_n) {
			float l;
			u32 t = ev[1][2];
			md_elem(MD_AT(1), t, o);
			if (t == 4 || t == 8 || t == 11 || t == 12) for (c = 0; c < 3; c++) o[c] = o[c] * 2 - 1; /* (unsigned: biased) */
			for (c = 0; c < 3; c++) if (!md_fin(o[c])) o[c] = 0;
			l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
			if (l > 1e-12f) for (c = 0; c < 3; c++) d->nrm[3 * i + c] = o[c] / l;
			else { d->nrm[3 * i] = 0; d->nrm[3 * i + 1] = 0; d->nrm[3 * i + 2] = 1; }
		}
		if (d->has_j) {
			u32 t = ev[3][2], nc;
			md_elem(MD_AT(3), t, o);
			for (c = 0; c < 4; c++) {
				float x = (t == 4 || t == 8) ? o[c] * 255.0f + 0.5f : o[c] + 0.5f; /* (normalized bytes: back to 0-255) */
				d->jnt[4 * i + c] = (u16)(md_fin(x) && x > 0 ? (x > 65535 ? 65535 : x) : 0);
			}
			t = ev[4][2]; nc = md_ncomp(t);
			md_elem(MD_AT(4), t, o);
			for (c = 0; c < 4; c++) d->wt[4 * i + c] = c < nc && md_fin(o[c]) ? o[c] : 0.0f; /* (as stored: missing ones 0) */
		}
#undef MD_AT
	}
	ntri = 0;
	for (i = 0; i + 2 < n; i += type == 4 ? 3 : 1) {
		u32 x = vix[i] - vmin, y = vix[i + 1] - vmin, z = vix[i + 2] - vmin;
		if (type == 5) { if (x == y || y == z || x == z) continue; if (i & 1) { u32 s = x; x = y; y = s; } } /* (strips: odd ones turned back) */
		d->idx[3 * ntri] = x; d->idx[3 * ntri + 1] = y; d->idx[3 * ntri + 2] = z;
		ntri++;
	}
	free(vix);
	d->ntri = ntri;
	if (!ntri) { md_free(d); md_sk_other++; return; }
	memcpy(d->vsc, dev.vsf, sizeof d->vsc);
	memcpy(d->vsx, dev.vsf + 4 * 169, sizeof d->vsx);
	md_printf(&d->decl, "[");
	for (e = 0; e < nel; e++) {
		u32 p = decl->mem + 8 * e, s = M16(p), t = M8(p + 4), u = M8(p + 6);
		if (s >= 15) break;
		md_printf(&d->decl, "%s{\"stream\":%u,\"offset\":%u,\"type\":\"%s\",\"usage\":\"%s\",\"index\":%u}", e ? "," : "", s, M16(p + 2),
		          t <= 16 ? md_tname[t] : "UNUSED", u < 14 ? md_uname[u] : "?", M8(p + 7));
	}
	md_printf(&d->decl, "]");
	md_ctab(vso, &d->named);
	md_n++;
}
/* the GLB: one buffer; per draw one view + accessor for each attribute and the indices, a mesh, a node, a material */
typedef struct { unsigned char *b; size_t o; MdStr bv, ac; u32 n; } MdOut;
static u32 md_put(MdOut *x, const void *src, size_t len, int target, int ctype, u32 count, const char *at, const float *mn, const float *mx) {
	memcpy(x->b + x->o, src, len);
	md_printf(&x->bv, "%s{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":%llu,\"target\":%d}", x->n ? "," : "", (unsigned long long)x->o, (unsigned long long)len, target);
	md_printf(&x->ac, "%s{\"bufferView\":%u,\"componentType\":%d,\"count\":%u,\"type\":\"%s\"", x->n ? "," : "", x->n, ctype, count, at);
	if (mn) { md_printf(&x->ac, ",\"min\":"); md_floats(&x->ac, mn, 3); md_printf(&x->ac, ",\"max\":"); md_floats(&x->ac, mx, 3); }
	md_printf(&x->ac, "}");
	x->o += (len + 3) & ~(size_t)3;
	return x->n++;
}
static void md_dir(char *dir, size_t n) {
	const char *e = getenv("XRC_MESH_DIR");
	if (e && *e && strlen(e) < n) strcpy(dir, e);
	else {
		DWORD k = GetModuleFileNameA(0, dir, (DWORD)n - 16);
		char *sl;
		dir[k < n ? k : 0] = 0;
		sl = strrchr(dir, 92); /* (92: a backslash) */
		if (sl) sl[1] = 0; else dir[0] = 0;
		strcat(dir, "mesh_dumps");
	}
	CreateDirectoryA(dir, 0);
}
static void *md_write(void *arg) {
	MdJob *j = (MdJob *)arg;
	MdOut x;
	MdStr js = {0}, me = {0}, no = {0}, ma = {0}, sc = {0};
	char dir[MAX_PATH], fn[MAX_PATH + 64];
	size_t bin = 0;
	u32 i, k, totv = 0, tott = 0;
	FILE *f;
	memset(&x, 0, sizeof x);
	md_dir(dir, sizeof dir);
	for (i = 0; i < j->n; i++) {
		MDraw *d = &j->d[i];
		bin += (size_t)d->nv * (12 + 8 + (d->has_n ? 12 : 0) + (d->uv1 ? 8 : 0) + (d->has_j ? 24 : 0)) + (size_t)d->ntri * 12;
	}
	x.b = (unsigned char *)malloc(bin ? bin : 4);
	if (j->n && x.b) {
		for (i = 0; i < j->n; i++) {
			MDraw *d = &j->d[i];
			float mn[3], mx[3];
			u32 ap, an = 0, at, au1 = 0, aj = 0, aw = 0, ai, c;
			for (c = 0; c < 3; c++) { mn[c] = d->pos[c]; mx[c] = d->pos[c]; }
			for (k = 1; k < d->nv; k++) for (c = 0; c < 3; c++) { float v = d->pos[3 * k + c]; if (v < mn[c]) mn[c] = v; if (v > mx[c]) mx[c] = v; }
			ap = md_put(&x, d->pos, (size_t)d->nv * 12, 34962, 5126, d->nv, "VEC3", mn, mx);
			if (d->has_n) an = md_put(&x, d->nrm, (size_t)d->nv * 12, 34962, 5126, d->nv, "VEC3", 0, 0);
			at = md_put(&x, d->uv, (size_t)d->nv * 8, 34962, 5126, d->nv, "VEC2", 0, 0);
			if (d->uv1) au1 = md_put(&x, d->uv1, (size_t)d->nv * 8, 34962, 5126, d->nv, "VEC2", 0, 0);
			if (d->has_j) {
				aj = md_put(&x, d->jnt, (size_t)d->nv * 8, 34962, 5123, d->nv, "VEC4", 0, 0);
				aw = md_put(&x, d->wt, (size_t)d->nv * 16, 34962, 5126, d->nv, "VEC4", 0, 0);
			}
			ai = md_put(&x, d->idx, (size_t)d->ntri * 12, 34963, 5125, d->ntri * 3, "SCALAR", 0, 0);
			md_printf(&me, "%s{\"name\":\"draw_%u\",\"primitives\":[{\"attributes\":{\"POSITION\":%u,\"TEXCOORD_0\":%u", i ? "," : "", d->draw, ap, at);
			if (d->has_n) md_printf(&me, ",\"NORMAL\":%u", an);
			if (d->uv1) md_printf(&me, ",\"TEXCOORD_1\":%u", au1);
			if (d->has_j) md_printf(&me, ",\"_LR_JOINTS\":%u,\"_LR_WEIGHTS\":%u", aj, aw);
			md_printf(&me, "},\"indices\":%u,\"material\":%u,\"mode\":4}]}", ai, i);
			md_printf(&ma, "%s{\"name\":\"tex_%016llx\",\"doubleSided\":true,\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1},\"extras\":{\"lr_tex_hash\":\"%016llx\"",
			          i ? "," : "", d->th[0], d->th[0]);
			for (k = 1; k < 4; k++) if (d->th[k]) md_printf(&ma, ",\"lr_tex_hash_%u\":\"%016llx\"", k, d->th[k]);
			md_printf(&ma, "}}");
			md_printf(&no, "%s{\"name\":\"draw_%u\",\"mesh\":%u,\"extras\":{\"draw\":%u,\"vs\":%u,\"vs_c\":", i ? "," : "", d->draw, i, d->draw, d->vs);
			md_floats(&no, d->vsc, 128);
			md_printf(&no, ",\"vs_c169\":");
			md_floats(&no, d->vsx, 64);
			md_printf(&no, ",\"vs_named\":%s}}", d->named.p ? d->named.p : "{}");
			totv += d->nv; tott += d->ntri;
		}
		md_printf(&js, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"xrc mesh dump\"},\"scene\":0,\"scenes\":[{\"nodes\":[");
		for (i = 0; i < j->n; i++) md_printf(&js, i ? ",%u" : "%u", i);
		md_printf(&js, "]}],\"nodes\":[%s],\"meshes\":[%s],\"materials\":[%s],\"buffers\":[{\"byteLength\":%llu}],\"bufferViews\":[%s],\"accessors\":[%s]}",
		          no.p, me.p, ma.p, (unsigned long long)x.o, x.bv.p, x.ac.p);
		while (js.n & 3) md_printf(&js, " ");
		snprintf(fn, sizeof fn, "%s\\mesh_%llu.glb", dir, j->frame);
		if (js.p && (f = fopen(fn, "wb")) != 0) {
			u32 h[5];
			h[0] = 0x46546c67u; h[1] = 2; h[2] = (u32)(12 + 8 + js.n + 8 + x.o); h[3] = (u32)js.n; h[4] = 0x4e4f534au; /* (glTF, JSON) */
			fwrite(h, 4, 5, f);
			fwrite(js.p, 1, js.n, f);
			h[0] = (u32)x.o; h[1] = 0x004e4942u; /* (BIN) */
			fwrite(h, 4, 2, f);
			fwrite(x.b, 1, x.o, f);
			fclose(f);
			fprintf(stderr, "xrc: mesh: wrote %s (%u draws, %u vertices)\n", fn, j->n, totv);
		} else fprintf(stderr, "xrc: mesh: %s not written\n", fn);
	} else fprintf(stderr, "xrc: mesh: frame %llu: no textured draws taken\n", j->frame);
	/* the summary */
	md_printf(&sc, "{\"frame\":%llu,\"glb\":\"mesh_%llu.glb\",\"draws_seen\":%u,\"draws\":%u,\"vertices\":%u,\"triangles\":%u,"
	          "\"skipped\":{\"no_position_or_texcoord0\":%u,\"no_texture_hash\":%u,\"duplicate\":%u,\"shadow_map\":%u,\"other\":%u},\"list\":[",
	          j->frame, j->frame, j->seq, j->n, totv, tott, j->sk_decl, j->sk_tex, j->sk_dup, j->sk_shadow, j->sk_other);
	for (i = 0; i < j->n; i++) {
		MDraw *d = &j->d[i];
		md_printf(&sc, "%s\n{\"draw\":%u,\"tex_hash\":\"%016llx\",\"vertices\":%u,\"triangles\":%u,\"prim\":\"%s\",\"vs\":%u,\"decl\":%s}",
		          i ? "," : "", d->draw, d->th[0], d->nv, d->ntri, d->prim == 5 ? "strip" : "list", d->vs, d->decl.p ? d->decl.p : "[]");
	}
	md_printf(&sc, "]}\n");
	snprintf(fn, sizeof fn, "%s\\mesh_%llu.json", dir, j->frame);
	if (sc.p && (f = fopen(fn, "wb")) != 0) { fwrite(sc.p, 1, sc.n, f); fclose(f); }
	for (i = 0; i < j->n; i++) md_free(&j->d[i]);
	free(j->d); free(x.b); free(x.bv.p); free(x.ac.p); free(js.p); free(me.p); free(no.p); free(ma.p); free(sc.p); free(j);
	return 0;
}
static double md_uptime(void) { /* (seconds since the process started) */
	FILETIME c, x, k, u, now;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u)) return 0;
	GetSystemTimeAsFileTime(&now);
	return (double)((((unsigned long long)now.dwHighDateTime << 32 | now.dwLowDateTime) - ((unsigned long long)c.dwHighDateTime << 32 | c.dwLowDateTime))) * 1e-7;
}
/* be_present, before the frame ends: the frame taken is written; F7 / XRC_MESH_DUMP_AT start the next one's */
static void md_frame_end(void) {
	static double at = -2;
	if (md_on) {
		MdJob *j = (MdJob *)calloc(1, sizeof *j);
		pthread_t t;
		md_on = 0;
		if (j) {
			j->d = md_d; j->n = md_n; j->frame = frame_no; j->seq = md_seq;
			j->sk_decl = md_sk_decl; j->sk_tex = md_sk_tex; j->sk_dup = md_sk_dup; j->sk_shadow = md_sk_shadow; j->sk_other = md_sk_other;
			md_d = 0; md_n = md_max = 0;
			if (pthread_create(&t, 0, md_write, j) == 0) pthread_detach(t);
			else md_write(j);
		}
	}
	if (at == -2) { const char *e = getenv("XRC_MESH_DUMP_AT"); at = e && *e ? atof(e) : -1; }
	if (at >= 0 && md_uptime() >= at) { at = -1; md_req = 1; }
	if (md_req) {
		u32 i;
		md_req = 0; md_on = 1;
		for (i = 0; i < md_n; i++) md_free(&md_d[i]);
		md_n = 0; md_seq = md_sk_decl = md_sk_tex = md_sk_dup = md_sk_shadow = md_sk_other = 0;
		fprintf(stderr, "xrc: mesh: capturing frame %llu\n", frame_no + 1);
	}
}
#else
void xrc_vk_mesh_request(void) {}
#endif

static unsigned long long nskipped;
static void skip(const char *why) {
	static unsigned long long warned;
	nskipped++;
	if (warned < 20) { warned++; fprintf(stderr, "xrc: vulkan: a draw skipped: %s\n", why); }
}


/* XRC_PROF_DRAW=1: QueryPerformanceCounter accumulators of the draw path (per-frame totals logged every 300 frames) */
#include <windows.h>
double xrc_pd_acc[40]; unsigned long long xrc_pd_cnt[40]; int xrc_pd_on = -1;
static LARGE_INTEGER pd_freq;
unsigned long long xrc_pd_now(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (unsigned long long)q.QuadPart; }
void xrc_pd_add(int i, unsigned long long t0) { unsigned long long t = xrc_pd_now(); if (!pd_freq.QuadPart) QueryPerformanceFrequency(&pd_freq); xrc_pd_acc[i] += (double)(t - t0) * 1000.0 / (double)pd_freq.QuadPart; xrc_pd_cnt[i]++; }
static const char *const pd_names[40] = {"hle_draw_total","","tex_first","vtx_key","upload","rt_key","pipeline_for","constants","begin_render+layouts","descriptors","cmds_draw","", "SetTexture","SetRenderState","SetSamplerState","SetVSConstF","SetPSConstF","SetStreamSource","SetIndices","SetVShader","SetPShader","SetVDecl","SetViewport/Scissor/RT"};
static unsigned long long pd_last;
#define PD_INIT unsigned long long pd_m = xrc_pd_on > 0 ? xrc_pd_now() : 0
#define PD_MARK(i) do { if (pd_m) { unsigned long long t_ = xrc_pd_now(); if (!pd_freq.QuadPart) QueryPerformanceFrequency(&pd_freq); xrc_pd_acc[i] += (double)(t_ - pd_m) * 1000.0 / (double)pd_freq.QuadPart; xrc_pd_cnt[i]++; pd_m = t_; } } while (0)
static void pd_report(void) {
	int i; static unsigned long long f0;
	unsigned long long n = frame_no - f0;
	if (!n) return;
	fprintf(stderr, "xrc: pd frames %llu:", n);
	for (i = 0; i < 40; i++) if (xrc_pd_cnt[i]) fprintf(stderr, " %s=%.3fms/%.1fc", pd_names[i] && *pd_names[i] ? pd_names[i] : "x", xrc_pd_acc[i] / (double)n, (double)xrc_pd_cnt[i] / (double)n);
	fprintf(stderr, "\n");
	memset(xrc_pd_acc, 0, sizeof xrc_pd_acc); memset(xrc_pd_cnt, 0, sizeof xrc_pd_cnt); f0 = frame_no;
}
static int dc_off; /* (XRC_SHOT_AB=dc: the B shot with the draw state caches off; dc0: no change, the noise floor) */
static struct { unsigned long long gen; VkPipeline pipe; VkImageView view[20]; VkSampler smp[20]; VkImageLayout lay[20]; VkBuffer vb[16]; VkDeviceSize vo[16]; VkBuffer ib; VkDeviceSize io; VkIndexType it; } dc; /* (what the last draw bound on this command buffer; zeroed when dyn_gen moves) */
static void be_draw2(int method, const u32 *a);
static void be_draw(int method, const u32 *a) {
	double t = now_ms();
	be_draw2(method, a);
#ifndef XRC_NO_TOOLS
	if (pp_open) pp[fi][pp_n[fi]].draws++;
#endif
	draw_ms += now_ms() - t;
	HC(draw, now_ms() - t); HC(ndraw, 1);
}
#ifdef XRC_SHCOUNT /* test-only counter (compile-time off): shadow-map draws per frame and the np=624 nv=396 group; one "xrc: shc" line per frame */
static void shc_draw(u32 np, u32 nv) {
	static unsigned long long lf = ~0ull; static unsigned n, g; Obj *s0; Res *r0; u32 f, l;
	if (lf != frame_no) { if (lf != ~0ull && n) fprintf(stderr, "xrc: shc f%llu n=%u g=%u\n", lf, n, g); lf = frame_no; n = g = 0; }
	s0 = obj_of(dev.rt[0]); r0 = s0 ? surface_image(s0, &f, &l) : 0;
	if (r0 && r0->d3dfmt == 114 && r0->w == r0->h && r0->w >= 512) { n++; if (np == 624 && nv == 396) g++; }
}
#endif
static void be_draw2(int method, const u32 *a) {
	u32 type = a[0], np, nv, base_vertex = 0, first = 0, nidx = 0, min_index = 0, ib_off = 0, k;
	int indexed = method == 1 || method == 3, isz = 2;
	Obj *vso = obj_of(dev.vs), *pso = obj_of(dev.ps), *decl = obj_of(dev.decl);
	Shader *vs, *ps;
	PKey key;
	VkPipeline pipe;
	VkBuffer vbuf[16];
	VkDeviceSize voff[16];
	u32 used_streams = 0;
	VkBuffer ibuf = VK_NULL_HANDLE;
	VkDeviceSize ioff = 0;
	VkIndexType itype = VK_INDEX_TYPE_UINT16;
	PD_INIT;
	if (dc_off) dyn_gen++;
	switch (method) {
	case 0: np = a[2]; nv = vcount(type, np); first = a[1]; break;                               /* type start count */
	case 1: np = a[5]; nidx = vcount(type, np); base_vertex = a[1]; min_index = a[2]; nv = a[3]; first = a[4]; break; /* type base min nv start count */
	case 2: np = a[1]; nv = vcount(type, np); break;                                              /* type count data stride */
	default: np = a[3]; nidx = vcount(type, np); min_index = a[1]; nv = a[2]; isz = a[5] == 102 ? 4 : 2; break; /* type min nv count idata ifmt vdata stride */
	}
	if (!np || (!nv && !indexed)) return;
	PD_MARK(1);
	{ /* XRC_NO_SHADOWS=1 (the launcher's Shadow Resolution: Off): nothing drawn into the shadow maps -- the game's square
	   * R32F targets of its -Shadow size (its global at 026e4f3c); their clears stay (no shadow anywhere) */
		static int no_shadows = -1;
		if (no_shadows < 0) no_shadows = getenv("XRC_NO_SHADOWS") && atoi(getenv("XRC_NO_SHADOWS")) ? 1 : 0;
		if (no_shadows) {
			Obj *s0 = obj_of(dev.rt[0]);
			u32 f, l, n = M32(0x026e4f3cu);
			Res *r0 = s0 ? surface_image(s0, &f, &l) : 0;
			if (r0 && r0->d3dfmt == 114 /* R32F */ && dim(r0->w, l) == n && dim(r0->h, l) == n) return;
		}
	}
	{ static int nd = -1; if (nd < 0) nd = xrc_tool_env("XRC_VK_NODRAW") != 0; if (nd) return; } /* (debugging) */
	if (!vso || !pso) { skip("fixed-function (no shader)"); return; }
	if (!decl) { skip("FVF (no declaration)"); return; }
	vs = shader_of(vso);
	ps = shader_of(pso);
	if (!vs || !ps) return;
#ifdef XRC_SHCOUNT
	shc_draw(np, nv);
#endif
	if (dev.freq[0] & 0xc0000000u) { skip("instancing"); return; }
	if (type == 6 && np > 21845) { skip("a fan too large"); return; }
	/* textures first (their uploads end a rendering) */
	{
		for (k = 0; k < 20; k++) {
			Obj *t = obj_of(dev.tex[k]);
			Res *tr = t && (t->kind == K_TEX || t->kind == K_CUBE || t->kind == K_VTEX) ? texture_ready(t) : 0;
			if (npcl && tr) clears_flush(tr);
			if (tr && tr->depth) ZREAD(tr);
		}
	}
	PD_MARK(2);
#if defined(XRC_LR) && defined(_WIN32)
	if (md_on) md_draw(method, a, np, base_vertex, min_index, first, vso, vs, decl); /* (F7: the mesh dump) */
#endif
	memset(&key, 0, sizeof key);
	key.vs = vs->serial; key.ps = ps->serial;
	/* vertex input: the shader's v# from the declaration's elements of the same semantic */
	{
		const Sm3Info *vi = &vs->info;
		u32 nel = decl->size ? decl->size - 1 : 0, v, zero_bind = 0;
		for (v = 0; v < 16; v++) {
			u32 e;
			int found = 0;
			if (!(vi->in_mask & (1u << v))) continue;
			for (e = 0; e < nel; e++) {
				u32 p = decl->mem + 8 * e;
				if (M8(p + 6) == vi->in_usage[v] && M8(p + 7) == vi->in_index[v] && M8(p + 4) <= 16) {
					u32 s = M16(p);
					if (s >= 15) break;
					key.attr[key.nattr].loc = (u8)v;
					key.attr[key.nattr].bind = (u8)s;
					key.attr[key.nattr].fmt = M8(p + 4);
					key.attr[key.nattr].off = M16(p + 2);
					key.nattr++;
					used_streams |= 1u << s;
					found = 1;
					break;
				}
			}
			if (!found) { /* (not in the declaration: (0,0,0,0) from binding 15) */
				key.attr[key.nattr].loc = (u8)v; key.attr[key.nattr].bind = 15; key.attr[key.nattr].fmt = 255; key.attr[key.nattr].off = 0;
				key.nattr++;
				zero_bind = 1;
			}
		}
		for (k = 0; k < 15; k++) if (used_streams & (1u << k)) {
			key.bind[key.nbind].slot = (u8)k;
			key.bind[key.nbind].stride = (u16)(method >= 2 ? (k == 0 ? (method == 2 ? a[3] : a[7]) : dev.stream[k][2]) : dev.stream[k][2]);
			key.nbind++;
		}
		if (zero_bind) { key.bind[key.nbind].slot = 15; key.bind[key.nbind].stride = 0; key.nbind++; }
	}
	PD_MARK(3);
	/* vertex data: streamed (dynamic buffers, UP data: copied from the draw's first vertex on) or the static buffer
	 * (bound at that vertex): every stream starts at first_v, the draw numbers its vertices from there */
	{
		u32 first_v = method == 1 ? base_vertex + min_index : method == 0 ? first : 0;
		if (F->ring_used > RING / 4 * 3) flush_wait(); /* (a draw's ring allocations never wrap) */
		for (k = 0; k < 15; k++) {
			Obj *vb;
			u32 stride, off;
			if (!(used_streams & (1u << k))) continue;
			if (method >= 2) {
				u32 data = method == 2 ? a[2] : a[6];
				void *p;
				stride = method == 2 ? a[3] : a[7];
				if (k != 0) { skip("an UP draw with more than one stream"); return; }
				vbuf[k] = F->ring;
				voff[k] = ring_alloc((VkDeviceSize)stride * (nv + min_index), 16, &p);
				memcpy(p, xrc_mem + data, (size_t)stride * (nv + min_index));
				continue;
			}
			vb = obj_of(dev.stream[k][0]);
			if (!vb || vb->kind != K_VB || !vb->mem) { skip("a stream without a vertex buffer"); return; }
			off = dev.stream[k][1];
			stride = dev.stream[k][2];
			if (streamed(vb)) {
				u32 lo = off + first_v * stride, n = nv * stride;
				void *p;
				if (lo >= vb->size) { skip("vertices outside their buffer"); return; }
				if (lo + n > vb->size) n = vb->size - lo;
				vbuf[k] = F->ring;
				voff[k] = ring_alloc(n ? n : 4, 16, &p);
				memcpy(p, xrc_mem + vb->mem + lo, n);
			} else {
				Res *r = buffer_ready(vb);
				vbuf[k] = r->buf;
				voff[k] = off;
			}
		}
		/* (the static buffers: bound at vertex first_v, like the streamed copies) */
		if (method <= 1)
			for (k = 0; k < 15; k++) if ((used_streams & (1u << k)) && vbuf[k] != F->ring) voff[k] += (VkDeviceSize)first_v * dev.stream[k][2];
		if (key.nbind && key.bind[key.nbind - 1].slot == 15) {
			void *p;
			VkDeviceSize zoff = ring_alloc(16, 16, &p);
			memset(p, 0, 16);
			vbuf[15] = F->ring; voff[15] = zoff;
			used_streams |= 1u << 15;
		}
		/* indices */
		if (indexed) {
			u32 isrc, n = nidx * (u32)isz;
			void *p;
			if (method == 1) {
				Obj *ib = obj_of(dev.ib);
				if (!ib || ib->kind != K_IB || !ib->mem) { skip("no index buffer"); return; }
				isz = ib->fmt == 102 ? 4 : 2;
				n = nidx * (u32)isz;
				ib_off = first * (u32)isz;
				if (ib_off + n > ib->size) { skip("indices outside their buffer"); return; }
				isrc = ib->mem + ib_off;
				if (!streamed(ib) && type != 6) {
					Res *r = buffer_ready(ib);
					ibuf = r->buf; ioff = ib_off;
				}
			} else isrc = a[4];
			itype = isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
			if (!ibuf) {
				if (type == 6) { /* a fan: as a list */
					u32 t;
					n = np * 3 * (u32)isz;
					ioff = ring_alloc(n, 16, &p);
					for (t = 0; t < np; t++) {
						if (isz == 4) {
							u32 *o = (u32 *)p + 3 * t;
							o[0] = M32(isrc); o[1] = M32(isrc + 4 * (t + 1)); o[2] = M32(isrc + 4 * (t + 2));
						} else {
							uint16_t *o = (uint16_t *)p + 3 * t;
							o[0] = M16(isrc); o[1] = M16(isrc + 2 * (t + 1)); o[2] = M16(isrc + 2 * (t + 2));
						}
					}
					nidx = np * 3;
				} else {
					ioff = ring_alloc(n, 16, &p);
					memcpy(p, xrc_mem + isrc, n);
				}
				ibuf = F->ring;
			}
		} else if (type == 6) { /* a non-indexed fan: list indices */
			void *p;
			u32 t;
			ioff = ring_alloc((VkDeviceSize)np * 3 * 2, 16, &p);
			for (t = 0; t < np; t++) { uint16_t *o = (uint16_t *)p + 3 * t; o[0] = 0; o[1] = (uint16_t)(t + 1); o[2] = (uint16_t)(t + 2); }
			ibuf = F->ring;
			itype = VK_INDEX_TYPE_UINT16;
			nidx = np * 3;
			indexed = 2;
		}
		PD_MARK(4);
		key.topo = (u8)topo_of(type);
		{
			/* render targets */
			u32 n = 0;
			for (k = 0; k < 4; k++) {
				Obj *s = obj_of(dev.rt[k]);
				u32 face, level;
				Res *r;
				if (!s) continue;
				r = surface_image(s, &face, &level);
				if (!r || r->depth) continue;
				key.rtf[n] = r->fmt;
				if (!r->blend_ok) key.be |= (u8)(1 << (4 + n));
				key.wm[n] = (u8)((k == 0 ? dev.rs[168] : dev.rs[190 + k - 1]) & 0xf);
				n++;
			}
			key.nrt = (u8)n;
			{
				Obj *s = obj_of(dev.ds);
				u32 face, level;
				Res *r = s ? surface_image(s, &face, &level) : 0;
				key.dsf = r && r->depth ? r->fmt : 0;
			}
		}
		key.cull = (u8)dev.rs[22];
		key.fill = (u8)dev.rs[8];
		key.dbias = dev.rs[195] || dev.rs[175];
		key.zt = dev.rs[7] && key.dsf;
		key.zw = key.zt && dev.rs[14];
		key.zf = (u8)dev.rs[23];
		key.st = dev.rs[52] && key.dsf;
		if (key.st) {
			key.sf[0][0] = (u8)dev.rs[53]; key.sf[0][1] = (u8)dev.rs[54]; key.sf[0][2] = (u8)dev.rs[55]; key.sf[0][3] = (u8)dev.rs[56];
			if (dev.rs[185]) { key.sf[1][0] = (u8)dev.rs[186]; key.sf[1][1] = (u8)dev.rs[187]; key.sf[1][2] = (u8)dev.rs[188]; key.sf[1][3] = (u8)dev.rs[189]; }
			else memcpy(key.sf[1], key.sf[0], 4);
		}
		if (dev.rs[27]) {
			key.be |= 1;
			key.sb = (u8)dev.rs[19]; key.db = (u8)dev.rs[20]; key.bo = (u8)dev.rs[171];
			if (dev.rs[206]) { key.sba = (u8)dev.rs[207]; key.dba = (u8)dev.rs[208]; key.boa = (u8)dev.rs[209]; }
			else { key.sba = key.sb; key.dba = key.db; key.boa = key.bo; }
		}
		if (!key.nrt && !key.dsf) { skip("no render target"); return; }
		{
			int waiting;
			{ /* the alpha test (as the push constants below say it) */
				int af = dev.rs[15] ? (int)dev.rs[25] : 8;
				key.ms = (u8)rt_samples();
				if (af == 8 && (dev.rs[154] == FOURCC('A', '2', 'M', '1') || dev.rs[181] == FOURCC('A', 'T', 'O', 'C'))) {
					if (key.ms > 1) key.a2c = 1; else af = 7; /* (alpha to coverage: real when multisampled, else a test) */
				}
				static int at_all = -1; /* (XRC_VK_ALPHA_ALWAYS=1, tools builds: every draw with the test, as before -- to compare) */
				if (at_all < 0) at_all = xrc_tool_env("XRC_VK_ALPHA_ALWAYS") != 0;
				key.at = af != 8 || at_all;
			}
			if (ps->mod_nz && zout_drop()) { /* (see zout_drop) */
				Obj *so = obj_of(dev.ds);
				u32 f, l;
				Res *dr = so ? surface_image(so, &f, &l) : 0;
				if (dr && dr->depth && (!dr->zread || frame_no > dr->zread + 120)) key.nz = 1;
				{ /* shadow-map target: pin the choice instead of keying on zread age (XRC_POP_ZSHADOW: default keep | drop | old) */
				  static int pz = -1;
				  if (pz < 0) { const char *v = getenv("XRC_POP_ZSHADOW"); pz = !v ? 1 : !strcmp(v, "drop") ? 2 : !strcmp(v, "old") ? 0 : 1; }
				  if (pz) { Obj *s0 = obj_of(dev.rt[0]); u32 f2, l2; Res *r0 = s0 ? surface_image(s0, &f2, &l2) : 0; if (r0 && r0->d3dfmt == 114 && r0->w == r0->h && r0->w >= 512) key.nz = (pz == 2 && ps->mod_nz) ? 1 : 0; } }
			}
			PD_MARK(5);
			pipe = pipeline_for(&key, vs, ps, &waiting);
			if (!pipe) { if (waiting) { nskipped++; HC(nskip, 1); } else skip("the pipeline failed"); return; }
			PD_MARK(6);
#ifndef XRC_NO_TOOLS
			if (pp_open) { /* (what the draws of a pass do that costs: XRC_VK_PASSES) */
				PPass *q = &pp[fi][pp_n[fi]];
				q->n_at += key.at; q->n_zout += ps->info.writes_depth && !key.nz; q->n_blend += key.be & 1; q->n_zt += key.zt; q->n_zw += key.zw;
				if (!hitch_ms && pp_rec[fi]) { /* (the pixel shaders of a rendering and what they sample, once a shader a rendering) */
					static u32 seen_pass = ~0u, seen[64], nseen;
					u32 k2, t2;
					if (seen_pass != pp_n[fi] + 1000u * (u32)fi) { seen_pass = pp_n[fi] + 1000u * (u32)fi; nseen = 0; }
					for (k2 = 0; k2 < nseen && seen[k2] != ps->serial; k2++) {}
					if (k2 == nseen && nseen < 64) {
						char tl[600];
						int o2 = 0;
						seen[nseen++] = ps->serial;
						tl[0] = 0;
						for (t2 = 0; t2 < 20 && o2 < 500; t2++) {
							Obj *to = obj_of(dev.tex[t2]);
							Res *tr = to && to->host ? res_of(to) : 0;
							if (tr) o2 += snprintf(tl + o2, sizeof tl - (size_t)o2, " s%u=%08x(%ux%u f%u%s)", t2, dev.tex[t2], tr->w, tr->h, tr->d3dfmt, tr->depth ? " depth" : "");
						}
						fprintf(stderr, "xrc: vulkan: (the frame of the renderings below) rendering %u uses ps %u vs %u blend %d %u/%u zt %d wm %x rtf %u:%s\n", pp_n[fi], ps->serial, vs->serial,
						        key.be & 1, key.sb, key.db, key.zt, key.wm[0], key.rtf[0], tl);
					}
				}
			}
			{ /* XRC_VK_DUMP_SHADERS=dir: each vertex + pixel shader pair's bytecode, once (vs_<serial>.bin, ps_<serial>.bin) */
				static const char *dd = (const char *)1;
				static u8 done[8192];
				if (dd == (const char *)1) dd = xrc_tool_env("XRC_VK_DUMP_SHADERS");
				if (dd && ps->serial < sizeof done * 8 / 2 && !(done[ps->serial / 8] & (1 << (ps->serial % 8)))) {
					char fn[1024];
					FILE *f;
					Obj *ob[2];
					int k3;
					ob[0] = obj_of(dev.vs); ob[1] = obj_of(dev.ps);
					done[ps->serial / 8] |= (u8)(1 << (ps->serial % 8));
					for (k3 = 0; k3 < 2; k3++) {
						if (!ob[k3]) continue;
						snprintf(fn, sizeof fn, "%s/%s_%u_%u%s.bin", dd, k3 ? "ps" : "vs", ps->serial, k3 ? ps->serial : vs->serial, ps->info.writes_depth ? "_zout" : "");
						if ((f = fopen(fn, "wb")) != 0) { fwrite(xrc_mem + ob[k3]->mem, 1, ob[k3]->size, f); fclose(f); }
					}
				}
			}
#endif
		}
		/* constants: the shader's block (c, i, b) */
		{
			VkDeviceSize uoff[2];
			int st;
			Res *tx[20];
			u32 txm = 0, wantm = 0;
			for (st = 0; st < 2; st++) {
				unsigned char *p;
				u32 j;
				uoff[st] = ring_alloc(SM3_UBO_SIZE, props.limits.minUniformBufferOffsetAlignment > 16 ? props.limits.minUniformBufferOffsetAlignment : 16, (void **)&p);
				memcpy(p, st ? dev.psf : dev.vsf, st ? 224 * 16 : 256 * 16);
				if (st) memset(p + 224 * 16, 0, 32 * 16);
				memcpy(p + 4096, st ? dev.psi : dev.vsi, 256);
				for (j = 0; j < 16; j++) { int b = st ? dev.psb[j] : dev.vsb[j]; memset(p + 4352 + 16 * j, 0, 16); memcpy(p + 4352 + 16 * j, &b, 4); }
			}
			PD_MARK(7);
			if (!begin_render()) { skip("no attachment"); return; }
			{ /* the textures' layouts: read-only, or GENERAL when this rendering draws into it (changed outside it: a
			   * rendering under way is ended, and begun again) */
				int pass2, need = 0;
				for (k = 0; k < 20; k++) {
					u32 want = k < 16 ? ps->info.sampler[k] : vs->info.sampler[k];
					Obj *t = want ? obj_of(dev.tex[k]) : 0;
					Res *r = t && (t->kind == K_TEX || t->kind == K_CUBE || t->kind == K_VTEX) ? image_of(t) : 0;
					if (r && ((want == 2 && (r->cube || r->vol)) || (want == 3 && !r->cube) || (want == 4 && !r->vol))) r = 0;
					if (r && r->repl && tex_show && (!r->tui || tex_ui_show)) r = tex_alt && r->repl_x[tex_alt - 1] ? r->repl_x[tex_alt - 1] : r->repl; /* (an HD texture) */
					tx[k] = r;
					if (want) wantm |= 1u << k;
					if (r) txm |= 1u << k;
				}
				for (pass2 = 0; pass2 < 2; pass2++) {
					for (u32 mm = txm; mm; mm &= mm - 1) {
						Res *r; u32 j;
						k = (u32)__builtin_ctz(mm); r = tx[k];
						int att = 0;
						if (!r) continue;
						for (j = 0; j < r_ri.colorAttachmentCount; j++)
							if (r_res[j] == r) {
								att = 1;
								if (r_ca[j].imageLayout != VK_IMAGE_LAYOUT_GENERAL) { if (pass2) r_ca[j].imageLayout = VK_IMAGE_LAYOUT_GENERAL; else need = 1; }
							}
						if (r_ri.pDepthAttachment && r_dres == r) {
							att = 1;
							if (r_da.imageLayout != VK_IMAGE_LAYOUT_GENERAL) { if (pass2) r_da.imageLayout = r_sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL; else need = 1; }
						}
						if (!att) {
							VkPipelineStageFlags st = k < 16 ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
							if (!pass2) { if (r->lay != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || r->pw || full_barriers > 0) need = 1; }
							else { bq_add(r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); img_access(r, 0, st, VK_ACCESS_SHADER_READ_BIT); }
						}
					}
					if (!pass2) {
						if (bufs_written || bq_global || nbq) need = 1; /* (static vertex / index data copied since the last sync; a barrier waiting) */
						if (need && !r_pending) { /* (under way: split -- a barrier only outside a rendering) */
							end_render();
							n_split++;
							if (!begin_render()) { skip("no attachment"); return; }
						}
					} else if (bufs_written) sync_need(VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT);
				}
			}
			render_go();
			bq_go(); /* (a rendering under way: the uses marked; nothing to record) */
			draws_since = 1; acc_st |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
			PD_MARK(8);
			if (dc.gen != dyn_gen) { memset(&dc, 0, sizeof dc); dc.gen = dyn_gen; }
			if (dc.pipe != pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); dc.pipe = pipe; }
			{
				VkWriteDescriptorSet wr[22];
				VkDescriptorBufferInfo bi[2];
				VkDescriptorImageInfo ii[20];
				u32 nw = 0;
				for (st = 0; st < 2; st++) {
					bi[st].buffer = F->ring; bi[st].offset = uoff[st]; bi[st].range = SM3_UBO_SIZE;
					memset(&wr[nw], 0, sizeof wr[nw]);
					wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					wr[nw].dstBinding = (u32)st;
					wr[nw].descriptorCount = 1;
					wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
					wr[nw].pBufferInfo = &bi[st];
					nw++;
				}
				for (u32 mm = wantm; mm; mm &= mm - 1) {
					u32 want;
					Res *r;
					k = (u32)__builtin_ctz(mm); want = k < 16 ? ps->info.sampler[k] : vs->info.sampler[k]; r = tx[k];
					ii[k].sampler = sampler_for(k);
					ii[k].imageView = r ? r->view : dummy_view[want];
					ii[k].imageLayout = r ? r->lay : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					if (dc.view[k] == ii[k].imageView && dc.smp[k] == ii[k].sampler && dc.lay[k] == ii[k].imageLayout) continue; /* (still what the push set holds) */
					dc.view[k] = ii[k].imageView; dc.smp[k] = ii[k].sampler; dc.lay[k] = ii[k].imageLayout;
					memset(&wr[nw], 0, sizeof wr[nw]);
					wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					wr[nw].dstBinding = 2 + k;
					wr[nw].descriptorCount = 1;
					wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					wr[nw].pImageInfo = &ii[k];
					nw++;
				}
				vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, nw, wr);
			}
			PD_MARK(9);
		}
		/* push constants + dynamic state: each only when it differs from the last draw's on this command buffer (see dyn_gen) */
		{
			typedef struct { float fix[4]; int afunc; float aref; float pad[2]; } PC_;
			static struct { unsigned long long gen; int pc_ok, vs_ok, st_ok, bc_ok, db_ok; PC_ pc; u32 vp[6], sc[5], ext, st[3], bf, db[2]; } L;
			PC_ pc;
			float vw = (float)dev.vp[2], vh = (float)dev.vp[3];
			if (L.gen != dyn_gen) { memset(&L, 0, sizeof L); L.gen = dyn_gen; }
			memset(&pc, 0, sizeof pc);
			pc.fix[0] = vw > 0 ? 1.0f / vw : 0; pc.fix[1] = vh > 0 ? -1.0f / vh : 0;
			pc.afunc = dev.rs[15] ? (int)dev.rs[25] : 8;
			pc.aref = (float)(dev.rs[24] & 0xff) / 255.0f;
			/* alpha to coverage (ATI: POINTSIZE 'A2M1'; NVIDIA: ADAPTIVETESS_Y 'ATOC') without multisampling: a test at 0.5 */
			if (pc.afunc == 8 && !key.a2c && (dev.rs[154] == FOURCC('A', '2', 'M', '1') || dev.rs[181] == FOURCC('A', 'T', 'O', 'C'))) { pc.afunc = 7; pc.aref = 0.5f; }
			if (!L.pc_ok || memcmp(&L.pc, &pc, sizeof pc)) { vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, SM3_PUSH_SIZE, &pc); L.pc = pc; L.pc_ok = 1; }
			{
				u32 sc[5], ext = cur_extent.width * 65536u + cur_extent.height;
				sc[0] = dev.rs[174]; sc[1] = sc[2] = sc[3] = sc[4] = 0;
				if (dev.rs[174]) memcpy(&sc[1], dev.scissor, 16);
				if (!L.vs_ok || L.ext != ext || memcmp(L.vp, dev.vp, sizeof L.vp) || memcmp(L.sc, sc, sizeof sc)) {
					set_viewport_scissor();
					memcpy(L.vp, dev.vp, sizeof L.vp); memcpy(L.sc, sc, sizeof sc); L.ext = ext; L.vs_ok = 1;
				}
			}
			{
				u32 st[3], bf = dev.rs[193], db[2] = {dev.rs[195], dev.rs[175]};
				if (key.st) { st[0] = dev.rs[57] & 0xff; st[1] = dev.rs[58] & 0xff; st[2] = dev.rs[59] & 0xff; } else { st[0] = 0; st[1] = 0xff; st[2] = 0xff; }
				if (!L.st_ok || memcmp(L.st, st, sizeof st)) {
					vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, st[0]);
					vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, st[1]);
					vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, st[2]);
					memcpy(L.st, st, sizeof st); L.st_ok = 1;
				}
				if (!L.bc_ok || L.bf != bf) {
					float bc[4] = {((bf >> 16) & 0xff) / 255.0f, ((bf >> 8) & 0xff) / 255.0f, (bf & 0xff) / 255.0f, (bf >> 24) / 255.0f};
					vkCmdSetBlendConstants(cb, bc);
					L.bf = bf; L.bc_ok = 1;
				}
				if (!L.db_ok || memcmp(L.db, db, sizeof db)) {
					vkCmdSetDepthBias(cb, f32(dev.rs[195]) * 16777216.0f, 0.0f, f32(dev.rs[175]));
					memcpy(L.db, db, sizeof db); L.db_ok = 1;
				}
			}
		}
		for (k = 0; k < 16; k++) if (used_streams & (1u << k)) { if (dc.vb[k] != vbuf[k] || dc.vo[k] != voff[k]) { vkCmdBindVertexBuffers(cb, k, 1, &vbuf[k], &voff[k]); dc.vb[k] = vbuf[k]; dc.vo[k] = voff[k]; } }
		/* vertex numbering: every bound stream starts at first_v (the static ones were offset to it) */
#ifndef XRC_NO_TOOLS
		int dt_i = dt_rec[fi] && dt_n[fi] < DT_MAX ? (int)dt_n[fi]++ : -1;
		if (dt_i >= 0 && dspool) vkCmdBeginQuery(cb, dspool, (u32)fi * DT_MAX + (u32)dt_i, 0);
#endif
		if (indexed) {
			if (dc.ib != ibuf || dc.io != ioff || dc.it != itype) { vkCmdBindIndexBuffer(cb, ibuf, ioff, itype); dc.ib = ibuf; dc.io = ioff; dc.it = itype; }
			if (method == 1) vkCmdDrawIndexed(cb, nidx, 1, 0, (int32_t)base_vertex - (int32_t)first_v, 0);
			else if (method == 3) vkCmdDrawIndexed(cb, nidx, 1, 0, 0, 0);
			else vkCmdDrawIndexed(cb, nidx, 1, 0, 0, 0); /* (a non-indexed fan, from vertex first_v = first) */
		} else if (method == 0) vkCmdDraw(cb, nv, 1, 0, 0);
		else vkCmdDraw(cb, nv, 1, 0, 0);
		PD_MARK(10);
#ifndef XRC_NO_TOOLS
		if (dt_i >= 0) {
			DtDraw *d = &dt[fi][dt_i];
			u32 j;
			if (dspool) vkCmdEndQuery(cb, dspool, (u32)fi * DT_MAX + (u32)dt_i);
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dtpool, (u32)fi * DT_MAX + (u32)dt_i);
			d->ps = ps->serial; d->vs = vs->serial; d->pass = dt_pass; d->blend = key.be & 1; d->at = key.at;
			d->nv = indexed ? nidx : nv; d->w = cur_extent.width; d->h = cur_extent.height;
			d->nsmp = 0;
			for (j = 0; j < 16; j++) d->nsmp += ps->info.sampler[j] != 0;
			d->words = pso ? pso->size / 4 : 0;
		}
#endif
	}
}

/* ============================================================================================ clears and copies */
static void be_clear(const u32 *a) { dyn_gen++;
	u32 n = a[0], rects = a[1], flags = a[2], color = a[3], st = a[5], k, na = 0;
	float z = f32(a[4]);
	VkClearAttachment ca[5];
	VkClearRect cr[64];
	u32 ncr = 0;
	int x0, y0, x1, y1;
	if (!begin_render()) return;
	if (flags & 1) {
		u32 j = 0;
		for (k = 0; k < 4; k++) {
			Obj *s = obj_of(dev.rt[k]);
			u32 face, level;
			Res *r;
			if (!s) continue;
			r = surface_image(s, &face, &level);
			if (!r || r->depth) continue;
			ca[na].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			ca[na].colorAttachment = j++;
			ca[na].clearValue.color.float32[0] = ((color >> 16) & 0xff) / 255.0f;
			ca[na].clearValue.color.float32[1] = ((color >> 8) & 0xff) / 255.0f;
			ca[na].clearValue.color.float32[2] = (color & 0xff) / 255.0f;
			ca[na].clearValue.color.float32[3] = (color >> 24) / 255.0f;
			na++;
		}
	}
	if (flags & 6) {
		Obj *s = obj_of(dev.ds);
		u32 face, level;
		Res *r = s ? surface_image(s, &face, &level) : 0;
		if (r && r->depth) {
			ca[na].aspectMask = (flags & 2 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (flags & 4 && r->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
			ca[na].colorAttachment = 0;
			ca[na].clearValue.depthStencil.depth = z;
			ca[na].clearValue.depthStencil.stencil = st & 0xff;
			if (ca[na].aspectMask) na++;
		}
	}
	if (!na) return;
	/* the viewport (and the scissor rect, when on), and the rects */
	x0 = (int)dev.vp[0]; y0 = (int)dev.vp[1]; x1 = x0 + (int)dev.vp[2]; y1 = y0 + (int)dev.vp[3];
	if (dev.rs[174]) {
		if ((int)dev.scissor[0] > x0) x0 = (int)dev.scissor[0];
		if ((int)dev.scissor[1] > y0) y0 = (int)dev.scissor[1];
		if ((int)dev.scissor[2] < x1) x1 = (int)dev.scissor[2];
		if ((int)dev.scissor[3] < y1) y1 = (int)dev.scissor[3];
	}
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > (int)cur_extent.width) x1 = (int)cur_extent.width;
	if (y1 > (int)cur_extent.height) y1 = (int)cur_extent.height;
	for (k = 0; k < (rects && n ? n : 1) && ncr < 64; k++) {
		int l = x0, t = y0, r = x1, b = y1;
		if (rects && n) {
			int rl = (int)M32(rects + 16 * k), rt = (int)M32(rects + 16 * k + 4), rr = (int)M32(rects + 16 * k + 8), rb = (int)M32(rects + 16 * k + 12);
			if (rl > l) l = rl;
			if (rt > t) t = rt;
			if (rr < r) r = rr;
			if (rb < b) b = rb;
		}
		if (r <= l || b <= t) continue;
		cr[ncr].rect.offset.x = l; cr[ncr].rect.offset.y = t;
		cr[ncr].rect.extent.width = (u32)(r - l); cr[ncr].rect.extent.height = (u32)(b - t);
		cr[ncr].baseArrayLayer = 0; cr[ncr].layerCount = 1;
		ncr++;
	}
	if (!ncr) return;
	if (r_pending && ncr == 1 && cr[0].rect.offset.x == 0 && cr[0].rect.offset.y == 0 && cr[0].rect.extent.width == cur_extent.width &&
	    cr[0].rect.extent.height == cur_extent.height) {
		/* the whole target, before anything was drawn: the load ops (see r_pending) */
		for (k = 0; k < na; k++) {
			if (ca[k].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
				r_ca[ca[k].colorAttachment].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				r_ca[ca[k].colorAttachment].clearValue = ca[k].clearValue;
			}
			if (ca[k].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) { r_da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; r_da.clearValue = ca[k].clearValue; }
			if (ca[k].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) { r_sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; r_sa.clearValue = ca[k].clearValue; }
		}
		r_cleared = 1; r_folded++;
		return;
	}
	render_go();
	vkCmdClearAttachments(cb, na, ca, ncr, cr);
#ifndef XRC_NO_TOOLS
	if (pp_open) pp[fi][pp_n[fi]].clears++;
#endif
}

/* StretchRect(src, srcRect, dst, dstRect, filter): a blit */
static void rect_of(u32 p, u32 w, u32 h, VkOffset3D o[2]) {
	o[0].x = 0; o[0].y = 0; o[0].z = 0; o[1].x = (int)w; o[1].y = (int)h; o[1].z = 1;
	if (p) { o[0].x = (int)M32(p); o[0].y = (int)M32(p + 4); o[1].x = (int)M32(p + 8); o[1].y = (int)M32(p + 12); }
}
static Res *surface_ready(Obj *s, u32 *face, u32 *level) {
	Res *r = surface_image(s, face, level);
	Obj *t = s->parent ? obj_of(s->parent) : s;
	if (r && t) { u32 f, any = 0; for (f = 0; f < r->layers; f++) any |= r->dirty[f]; if (any) upload_image(t, r); }
	return r;
}
/* A scaling StretchRect drawn: the source sampled (a one-level view, in the shader-read layout) into the destination
 * as an attachment, instead of vkCmdBlitImage -- which an AMD GPU (Ryzen integrated) runs slowly: its 1280x720 ->
 * 640x360 one cost ~0.65 ms, a draw ~0.05. The shaders are D3D9 bytecode put through vk_sm3 like the game's: a vertex
 * shader passing position and texcoord, a pixel shader sampling s0. One pipeline per destination format.
 * XRC_VK_BLIT_DRAW=0: always vkCmdBlitImage (A/B). */
#define SM3_REG(t, n) (0x80000000u | (((t) & 7u) << 28) | (((t) & 0x18u) << 8) | (n))
static const u32 blit_vs_code[] = {
	0xfffe0300u,
	0x0200001fu, 0x80000000u, SM3_REG(1, 0) | 0xf0000u, /* dcl_position v0 */
	0x0200001fu, 0x80000005u, SM3_REG(1, 1) | 0xf0000u, /* dcl_texcoord v1 */
	0x0200001fu, 0x80000000u, SM3_REG(6, 0) | 0xf0000u, /* dcl_position o0 */
	0x0200001fu, 0x80000005u, SM3_REG(6, 1) | 0xf0000u, /* dcl_texcoord o1 */
	0x02000001u, SM3_REG(6, 0) | 0xf0000u, SM3_REG(1, 0) | 0xe40000u, /* mov o0, v0 */
	0x02000001u, SM3_REG(6, 1) | 0xf0000u, SM3_REG(1, 1) | 0xe40000u, /* mov o1, v1 */
	0x0000ffffu};
static const u32 blit_ps_code[] = {
	0xffff0300u,
	0x0200001fu, 0x80000005u, SM3_REG(1, 0) | 0xf0000u,  /* dcl_texcoord v0 */
	0x0200001fu, 0x90000000u, SM3_REG(10, 0) | 0xf0000u, /* dcl_2d s0 */
	0x03000042u, SM3_REG(0, 0) | 0xf0000u, SM3_REG(1, 0) | 0xe40000u, SM3_REG(10, 0) | 0xe40000u, /* texld r0, v0, s0 */
	0x02000001u, SM3_REG(8, 0) | 0xf0000u, SM3_REG(0, 0) | 0xe40000u, /* mov oC0, r0 */
	0x0000ffffu};
/* FXAA (Lottes' FXAA 2, the fast variant: 9 samples) -- assembled from:
 *   c0 = (1/w, 1/h, 0, 0); c5 = (reduce_mul / 4, reduce_min, span_max, -span_max); c1 luma weights, c3 / c4 steps
 *   NW NE SW SE M = s0 at v0 + (-1,-1) (1,-1) (-1,1) (1,1) texels, and v0; their lumas (dp3 with c1)
 *   dir = ((SW+SE) - (NW+NE), (NW+SW) - (NE+SE)); reduce = max((sum of the 4) * c5.x, c5.y)
 *   dir = clamp(dir / (min(|dir.x|, |dir.y|) + reduce), -span, span) * c0.xy
 *   A = (s0(v0 + dir * -1/6) + s0(v0 + dir * 1/6)) / 2; B = A / 2 + (s0(v0 - dir / 2) + s0(v0 + dir / 2)) / 4
 *   out.rgb = luma(B) inside [min, max] of the 5 lumas ? B : A; out.a = M.a */
static const u32 fxaa_ps_code[] = {
	0xffff0300u, 0x05000051u, 0xa00f0001u, 0x3e991687u, 0x3f1645a2u, 0x3de978d5u, 0x00000000u, 0x05000051u,
	0xa00f0003u, 0xbe2aaaabu, 0x3e2aaaabu, 0xbf000000u, 0x3f000000u, 0x05000051u, 0xa00f0004u, 0xbf800000u,
	0x3f800000u, 0x3f000000u, 0x3e800000u, 0x0200001fu, 0x80000005u, 0x900f0000u, 0x0200001fu, 0x90000000u,
	0xa00f0800u, 0x02000001u, 0x800f0001u, 0xa0aa0004u, 0x04000004u, 0x80030001u, 0xa0e40000u, 0xa0000004u,
	0x90e40000u, 0x03000042u, 0x800f0002u, 0x80e40001u, 0xa0e40800u, 0x03000008u, 0x8001000au, 0x80e40002u,
	0xa0e40001u, 0x04000004u, 0x80030001u, 0xa0e40000u, 0xa0010004u, 0x90e40000u, 0x03000042u, 0x800f0002u,
	0x80e40001u, 0xa0e40800u, 0x03000008u, 0x8002000au, 0x80e40002u, 0xa0e40001u, 0x04000004u, 0x80030001u,
	0xa0e40000u, 0xa0540004u, 0x90e40000u, 0x03000042u, 0x800f0002u, 0x80e40001u, 0xa0e40800u, 0x03000008u,
	0x8004000au, 0x80e40002u, 0xa0e40001u, 0x04000004u, 0x80030001u, 0xa0e40000u, 0xa0550004u, 0x90e40000u,
	0x03000042u, 0x800f0002u, 0x80e40001u, 0xa0e40800u, 0x03000008u, 0x8008000au, 0x80e40002u, 0xa0e40001u,
	0x03000042u, 0x800f0000u, 0x90e40000u, 0xa0e40800u, 0x03000008u, 0x8001000bu, 0x80e40000u, 0xa0e40001u,
	0x0300000au, 0x80030003u, 0x8054000au, 0x80fe000au, 0x0300000au, 0x80010003u, 0x80000003u, 0x80550003u,
	0x0300000au, 0x8002000bu, 0x80000003u, 0x8000000bu, 0x0300000bu, 0x80030003u, 0x8054000au, 0x80fe000au,
	0x0300000bu, 0x80010003u, 0x80000003u, 0x80550003u, 0x0300000bu, 0x8004000bu, 0x80000003u, 0x8000000bu,
	0x03000002u, 0x80010004u, 0x8000000au, 0x8055000au, 0x03000002u, 0x80020004u, 0x80aa000au, 0x80ff000au,
	0x03000002u, 0x80040004u, 0x8000000au, 0x80aa000au, 0x03000002u, 0x80080004u, 0x8055000au, 0x80ff000au,
	0x03000002u, 0x80010005u, 0x80550004u, 0x81000004u, 0x03000002u, 0x80020005u, 0x80aa0004u, 0x81ff0004u,
	0x03000002u, 0x80010006u, 0x80000004u, 0x80550004u, 0x03000005u, 0x80010006u, 0x80000006u, 0xa0000005u,
	0x0300000bu, 0x80010006u, 0x80000006u, 0xa0550005u, 0x0300000au, 0x80020006u, 0x8b000005u, 0x8b550005u,
	0x03000002u, 0x80020006u, 0x80550006u, 0x80000006u, 0x02000006u, 0x80020006u, 0x80550006u, 0x03000005u,
	0x80030005u, 0x80e40005u, 0x80550006u, 0x0300000au, 0x80030005u, 0x80e40005u, 0xa0aa0005u, 0x0300000bu,
	0x80030005u, 0x80e40005u, 0xa0ff0005u, 0x03000005u, 0x80030005u, 0x80e40005u, 0xa0e40000u, 0x04000004u,
	0x80030001u, 0x80e40005u, 0xa0000003u, 0x90e40000u, 0x03000042u, 0x800f0002u, 0x80e40001u, 0xa0e40800u,
	0x04000004u, 0x80030001u, 0x80e40005u, 0xa0550003u, 0x90e40000u, 0x03000042u, 0x800f0003u, 0x80e40001u,
	0xa0e40800u, 0x03000002u, 0x800f0002u, 0x80e40002u, 0x80e40003u, 0x03000005u, 0x800f0002u, 0x80e40002u,
	0xa0aa0004u, 0x04000004u, 0x80030001u, 0x80e40005u, 0xa0aa0003u, 0x90e40000u, 0x03000042u, 0x800f0003u,
	0x80e40001u, 0xa0e40800u, 0x04000004u, 0x80030001u, 0x80e40005u, 0xa0ff0003u, 0x90e40000u, 0x03000042u,
	0x800f0007u, 0x80e40001u, 0xa0e40800u, 0x03000002u, 0x800f0003u, 0x80e40003u, 0x80e40007u, 0x03000005u,
	0x800f0003u, 0x80e40003u, 0xa0ff0004u, 0x04000004u, 0x800f0003u, 0x80e40002u, 0xa0aa0004u, 0x80e40003u,
	0x03000008u, 0x80040006u, 0x80e40003u, 0xa0e40001u, 0x03000002u, 0x80080006u, 0x80aa0006u, 0x8155000bu,
	0x03000002u, 0x80080007u, 0x80aa000bu, 0x81aa0006u, 0x0300000au, 0x80080006u, 0x80ff0006u, 0x80ff0007u,
	0x04000058u, 0x800f0008u, 0x80ff0006u, 0x80e40003u, 0x80e40002u, 0x02000001u, 0x80080008u, 0x80ff0000u,
	0x02000001u, 0x800f0800u, 0x80e40008u, 0x0000ffffu};
static VkShaderModule blit_vsm, blit_psm, fxaa_psm;
static VkSampler blit_smp[2];
static struct { VkFormat f; int prog; VkPipeline p; } blit_pipes[16];
static int blit_state; /* 0 not set up, 1 ready, -1 off / failed */
static int blit_all;   /* (XRC_VK_BLIT_DRAW=2, a test: same-size copies drawn too) */
static int blit_draw_on = -1;
static int blit_setup(void) {
	Sm3Info vi, pi;
	char err[256];
	VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	int k;
	const char *e = getenv("XRC_VK_BLIT_DRAW");
	if (blit_state) return blit_state > 0;
	blit_state = -1;
	blit_all = e && atoi(e) == 2;
	(void)e;
	memset(&vi, 0, sizeof vi); memset(&pi, 0, sizeof pi);
	if (!sm3_translate_ex(blit_vs_code, sizeof blit_vs_code / 4, &vi, err, sizeof err, 0)) { fprintf(stderr, "xrc: vulkan: the blit vertex shader: %s\n", err); return 0; }
	if (!sm3_translate_ex(blit_ps_code, sizeof blit_ps_code / 4, &pi, err, sizeof err, SM3_NO_ALPHA_TEST)) { fprintf(stderr, "xrc: vulkan: the blit pixel shader: %s\n", err); sm3_free(&vi); return 0; }
	mi.codeSize = vi.nwords * 4; mi.pCode = vi.spirv;
	VKC(vkCreateShaderModule(vd, &mi, 0, &blit_vsm));
	mi.codeSize = pi.nwords * 4; mi.pCode = pi.spirv;
	VKC(vkCreateShaderModule(vd, &mi, 0, &blit_psm));
	sm3_free(&vi); sm3_free(&pi);
	memset(&pi, 0, sizeof pi);
	if (sm3_translate_ex(fxaa_ps_code, sizeof fxaa_ps_code / 4, &pi, err, sizeof err, SM3_NO_ALPHA_TEST)) {
		mi.codeSize = pi.nwords * 4; mi.pCode = pi.spirv;
		VKC(vkCreateShaderModule(vd, &mi, 0, &fxaa_psm));
		sm3_free(&pi);
	} else fprintf(stderr, "xrc: vulkan: the FXAA pixel shader: %s\n", err);
	for (k = 0; k < 2; k++) {
		si.magFilter = si.minFilter = k ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.maxLod = 0.25f;
		VKC(vkCreateSampler(vd, &si, 0, &blit_smp[k]));
	}
	blit_state = 1;
	return 1;
}
static VkPipeline blit_pipe(VkFormat f, int prog) {
	u32 k;
	PKey key;
	for (k = 0; k < sizeof blit_pipes / sizeof blit_pipes[0] && blit_pipes[k].p; k++) if (blit_pipes[k].f == f && blit_pipes[k].prog == prog) return blit_pipes[k].p;
	if (prog == 1 && !fxaa_psm) return VK_NULL_HANDLE;
	if (k == sizeof blit_pipes / sizeof blit_pipes[0]) return VK_NULL_HANDLE;
	memset(&key, 0, sizeof key);
	key.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; key.cull = 1; key.fill = 3;
	key.nattr = 2; key.nbind = 1; key.nrt = 1; key.ms = 1;
	key.zf = 8; key.sf[0][0] = key.sf[0][1] = key.sf[0][2] = key.sf[1][0] = key.sf[1][1] = key.sf[1][2] = 1; key.sf[0][3] = key.sf[1][3] = 8;
	key.sb = key.sba = 2; key.db = key.dba = 1; key.bo = key.boa = 1;
	key.wm[0] = 0xf;
	if (prog == 2) { key.be = 1; key.sb = key.sba = 2; key.db = key.dba = 6; } /* (2: a copy blended over, premultiplied alpha -- the settings overlay) */
	key.rtf[0] = (u32)f; key.dsf = (u32)VK_FORMAT_UNDEFINED;
	key.attr[0].loc = 0; key.attr[0].fmt = 255; key.attr[0].off = 0;
	key.attr[1].loc = 1; key.attr[1].fmt = 255; key.attr[1].off = 16;
	key.bind[0].slot = 0; key.bind[0].stride = 32;
	blit_pipes[k].p = make_pipeline(&key, blit_vsm, prog == 1 ? fxaa_psm : blit_psm);
	if (blit_pipes[k].p) { blit_pipes[k].f = f; blit_pipes[k].prog = prog; }
	return blit_pipes[k].p;
}
/* 1: drawn; 0: not this way (the caller blits). prog 0: a copy; 1: FXAA, its constants c0..c5 in pc6; 2: a copy blended
 * over the destination (premultiplied alpha) */
static int blit_draw(Res *s, u32 sf, u32 sl, Res *d, u32 df, u32 dl, const VkImageBlit *b, int linear, int prog, const float *pc6) {
	VkPipeline pipe;
	VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
	VkWriteDescriptorSet wr[3];
	VkDescriptorBufferInfo bi;
	VkDescriptorImageInfo ii;
	VkViewport vp;
	VkRect2D sc;
	VkDeviceSize voff, uoff;
	float *v;
	void *u;
	u32 dw = dim(d->w, dl), dh = dim(d->h, dl), sw = dim(s->w, sl), sh = dim(s->h, sl), k;
	struct { float fix[4]; int afunc; float aref; float pad[2]; } pc;
	float x0, y0, x1, y1, u0, v0, u1, v1;
	if (s == d || s->depth || d->depth || s->samples > 1 || d->samples > 1 || s->vol || d->vol || !d->can_att || !blit_setup()) return 0;
	if (!fmt_supports(s->fmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | (linear ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0))) return 0;
	if (!(pipe = blit_pipe(d->fmt, prog))) return 0;
	/* (ring space first: getting it may submit what is recorded) */
	voff = ring_alloc(6 * 32, 16, (void **)&v);
	uoff = ring_alloc(SM3_UBO_SIZE, props.limits.minUniformBufferOffsetAlignment > 16 ? props.limits.minUniformBufferOffsetAlignment : 16, &u);
	memset(u, 0, SM3_UBO_SIZE);
	if (pc6) memcpy(u, pc6, 6 * 16); /* (the pixel shader's c0..c5) */
	x0 = 2.0f * (float)b->dstOffsets[0].x / (float)dw - 1.0f; x1 = 2.0f * (float)b->dstOffsets[1].x / (float)dw - 1.0f;
	y0 = 2.0f * (float)b->dstOffsets[0].y / (float)dh - 1.0f; y1 = 2.0f * (float)b->dstOffsets[1].y / (float)dh - 1.0f;
	u0 = (float)b->srcOffsets[0].x / (float)sw; u1 = (float)b->srcOffsets[1].x / (float)sw;
	v0 = (float)b->srcOffsets[0].y / (float)sh; v1 = (float)b->srcOffsets[1].y / (float)sh;
	{
		const float q[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1}, {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
		for (k = 0; k < 6; k++) {
			v[k * 8 + 0] = q[k][0]; v[k * 8 + 1] = q[k][1]; v[k * 8 + 2] = 0.0f; v[k * 8 + 3] = 1.0f;
			v[k * 8 + 4] = q[k][2]; v[k * 8 + 5] = q[k][3]; v[k * 8 + 6] = 0.0f; v[k * 8 + 7] = 1.0f;
		}
	}
	bq_add(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); bq_add(d, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	img_access(s, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	img_access(d, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
	bq_go();
	ca.imageView = attach_view(d, df, dl);
	ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	ri.renderArea.extent.width = dw; ri.renderArea.extent.height = dh;
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
	vkCmdBeginRendering(cb, &ri);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	memset(wr, 0, sizeof wr);
	bi.buffer = F->ring; bi.offset = uoff; bi.range = SM3_UBO_SIZE;
	for (k = 0; k < 2; k++) {
		wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstBinding = k; wr[k].descriptorCount = 1;
		wr[k].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[k].pBufferInfo = &bi;
	}
	ii.sampler = blit_smp[linear ? 1 : 0];
	ii.imageView = attach_view(s, sf, sl);
	ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	wr[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[2].dstBinding = 2; wr[2].descriptorCount = 1;
	wr[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[2].pImageInfo = &ii;
	vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 3, wr);
	memset(&pc, 0, sizeof pc);
	pc.afunc = 8;
	vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, SM3_PUSH_SIZE, &pc);
	vp.x = 0; vp.y = 0; vp.width = (float)dw; vp.height = (float)dh; vp.minDepth = 0; vp.maxDepth = 1;
	dyn_gen++;
	vkCmdSetViewport(cb, 0, 1, &vp);
	sc.offset.x = sc.offset.y = 0; sc.extent.width = dw; sc.extent.height = dh;
	vkCmdSetScissor(cb, 0, 1, &sc);
	{
		float bc[4] = {0, 0, 0, 0};
		vkCmdSetBlendConstants(cb, bc);
		vkCmdSetDepthBias(cb, 0, 0, 0);
		vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
		vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
		vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
	}
	vkCmdBindVertexBuffers(cb, 0, 1, &F->ring, &voff);
	vkCmdDraw(cb, 6, 1, 0, 0);
	vkCmdEndRendering(cb);
	return 1;
}
/* copies held back (see be_stretch_rect): full-size StretchRects not made yet */
#define NPEND 4
typedef struct { u32 a[5]; Res *s, *d; } PCopy;
static PCopy pend[NPEND];
static int npend, defer_on = -1;
/* XRC_FXAA=2|4|8 (the launcher's Anti-alias Method FXAA, Quality 2x / 4x / 8x): FXAA (the shader above) before the
 * HUD. The game ends a frame by copying its finished 3D image into a surface, drawing the HUD over it, and copying
 * that to the back buffer. The surface copied to the back buffer (hud_next, then hud_rt from the next frame on) is
 * that one: the frame's first full-size copy into it is drawn through FXAA instead. A frame without one gets none
 * (some menus). Quality: the longest edge smoothed (span 4 / 8 / 16 texels) and how little contrast makes one. */
static u32 hud_rt, hud_next;
static unsigned long long fxaa_frame = ~0ull;
static int fxaa_q = -1;
void xrc_vk_set_fxaa(int q) { fxaa_q = q < 0 ? 0 : q; } /* (the settings menu: from the next frame on) */
static int fxaa_copy(Res *s, Res *d, const VkImageBlit *b) {
	float pc[24];
	if (fxaa_q < 0) { fxaa_q = getenv("XRC_FXAA") ? atoi(getenv("XRC_FXAA")) : 16; /* (integ3: SMAA by default) */ if (fxaa_q > 1) LOG("xrc: vulkan: FXAA %dx\n", fxaa_q); }
	if (fxaa_q <= 1 || fxaa_q == 16 || fxaa_frame == frame_no) return 0; /* (16: SMAA, made in fxaa_scene) */
	memset(pc, 0, sizeof pc);
	pc[0] = 1.0f / (float)s->w; pc[1] = 1.0f / (float)s->h;
	pc[20] = fxaa_q >= 8 ? 1.0f / 64.0f : fxaa_q >= 4 ? 1.0f / 32.0f : 1.0f / 16.0f; /* reduce_mul / 4: reduce_mul 1/16, 1/8, 1/4 */
	pc[21] = fxaa_q >= 8 ? 1.0f / 256.0f : 1.0f / 128.0f;                             /* reduce_min */
	pc[22] = fxaa_q >= 8 ? 16.0f : fxaa_q >= 4 ? 8.0f : 4.0f; pc[23] = -pc[22];        /* span */
	{
		static int said;
		if (said++ < 3) LOG("xrc: vulkan: FXAA copy %ux%u (%d,%d)-(%d,%d) -> %ux%u (%d,%d)-(%d,%d)\n", s->w, s->h, b->srcOffsets[0].x, b->srcOffsets[0].y, b->srcOffsets[1].x, b->srcOffsets[1].y,
		                    d->w, d->h, b->dstOffsets[0].x, b->dstOffsets[0].y, b->dstOffsets[1].x, b->dstOffsets[1].y);
	}
	if (!blit_draw(s, 0, 0, d, 0, 0, b, 1, 1, pc)) return 0;
	fxaa_frame = frame_no;
	return 1;
}
#if defined(XRC_LR) && defined(_WIN32)
/* SMAA 1x (XRC_FXAA=16, the menu's "SMAA"): the shaders of smaa/ (Jorge Jimenez et al., MIT; smaa/build.sh compiles them),
 * three passes on the finished 3D image where fxaa_scene makes FXAA: edges (luma) -> blending weights (the area and search
 * tables) -> the image blended with its neighbors into hud_rt. Own images: edges RG8, weights RGBA8, the two tables. */
#include "smaa/smaa_spv.h"
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"
static VkShaderModule smaa_mod[6];
static struct { VkFormat f; int pass; VkPipeline p; } smaa_pipes[8];
static Res *smaa_area, *smaa_search, *smaa_edges, *smaa_wts;
static int smaa_state; /* 0 not set up, 1 ready, -1 failed */
static Res *smaa_image(u32 w, u32 h, VkFormat f, int att) {
	VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	VkMemoryRequirements rq;
	Res *t = (Res *)calloc(1, sizeof *t);
	t->fmt = f; t->sw = SW(I_, I_, I_, I_); t->w = w; t->h = h; t->d = 1; t->levels = 1; t->layers = 1; t->samples = 1;
	t->can_att = (u8)(att != 0); t->blend_ok = 1;
	ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f;
	ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
	ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (att ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKC(vkCreateImage(vd, &ci, 0, &t->img));
	vkGetImageMemoryRequirements(vd, t->img, &rq);
	t->mem = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	if (t->mem.block < 0) { vkDestroyImage(vd, t->img, 0); free(t); return 0; }
	VKC(vkBindImageMemory(vd, t->img, blocks[t->mem.block].mem, t->mem.off));
	t->view = make_view(t, VK_IMAGE_VIEW_TYPE_2D, t->sw, 0, 1, 0, 1, 0);
	t->att = (VkImageView *)calloc(1, sizeof(VkImageView));
	t->lay = VK_IMAGE_LAYOUT_UNDEFINED;
	return t;
}
static void smaa_upload(Res *r, const void *src, u32 pitch) {
	VkBufferImageCopy c;
	void *p;
	VkDeviceSize off = ring_alloc((VkDeviceSize)pitch * r->h, 16, &p);
	memcpy(p, src, (size_t)pitch * r->h);
	img_use(r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	memset(&c, 0, sizeof c);
	c.bufferOffset = off;
	c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
	c.imageExtent.width = r->w; c.imageExtent.height = r->h; c.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(cb, F->ring, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
	r->pw = 1;
	img_use(r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
static int smaa_setup(void) {
	static const u32 *code[6] = {smaa_edge_vert, smaa_edge_frag, smaa_blend_vert, smaa_blend_frag, smaa_nb_vert, smaa_nb_frag};
	static const size_t size[6] = {sizeof smaa_edge_vert, sizeof smaa_edge_frag, sizeof smaa_blend_vert, sizeof smaa_blend_frag, sizeof smaa_nb_vert, sizeof smaa_nb_frag};
	VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	int k;
	if (smaa_state) return smaa_state > 0;
	smaa_state = -1;
	if (!blit_setup()) return 0;
	if (!fmt_supports(VK_FORMAT_R8G8_UNORM, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return 0;
	for (k = 0; k < 6; k++) {
		mi.codeSize = size[k]; mi.pCode = code[k];
		if (vkCreateShaderModule(vd, &mi, 0, &smaa_mod[k]) != VK_SUCCESS) { fprintf(stderr, "xrc: vulkan: SMAA shader %d failed\n", k); return 0; }
	}
	if (!(smaa_area = smaa_image(AREATEX_WIDTH, AREATEX_HEIGHT, VK_FORMAT_R8G8_UNORM, 0)) || !(smaa_search = smaa_image(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, VK_FORMAT_R8_UNORM, 0))) return 0;
	smaa_upload(smaa_area, areaTexBytes, AREATEX_PITCH);
	smaa_upload(smaa_search, searchTexBytes, SEARCHTEX_PITCH);
	LOG("xrc: vulkan: SMAA ready\n");
	smaa_state = 1;
	return 1;
}
static VkPipeline smaa_pipe(int pass, VkFormat f) {
	u32 k;
	PKey key;
	for (k = 0; k < sizeof smaa_pipes / sizeof smaa_pipes[0] && smaa_pipes[k].p; k++) if (smaa_pipes[k].f == f && smaa_pipes[k].pass == pass) return smaa_pipes[k].p;
	if (k == sizeof smaa_pipes / sizeof smaa_pipes[0]) return VK_NULL_HANDLE;
	memset(&key, 0, sizeof key);
	key.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; key.cull = 1; key.fill = 3;
	key.nattr = 2; key.nbind = 1; key.nrt = 1; key.ms = 1;
	key.zf = 8; key.sf[0][0] = key.sf[0][1] = key.sf[0][2] = key.sf[1][0] = key.sf[1][1] = key.sf[1][2] = 1; key.sf[0][3] = key.sf[1][3] = 8;
	key.sb = key.sba = 2; key.db = key.dba = 1; key.bo = key.boa = 1;
	key.wm[0] = 0xf;
	key.rtf[0] = (u32)f; key.dsf = (u32)VK_FORMAT_UNDEFINED;
	key.attr[0].loc = 0; key.attr[0].fmt = 255; key.attr[0].off = 0;
	key.attr[1].loc = 1; key.attr[1].fmt = 255; key.attr[1].off = 16;
	key.bind[0].slot = 0; key.bind[0].stride = 32;
	smaa_pipes[k].p = make_pipeline(&key, smaa_mod[pass * 2], smaa_mod[pass * 2 + 1]);
	if (smaa_pipes[k].p) { smaa_pipes[k].f = f; smaa_pipes[k].pass = pass; }
	return smaa_pipes[k].p;
}
/* one pass over the whole of d: src[] at bindings 2.. (lin: linear filtered); d cleared first when clear */
static int smaa_pass(int pass, Res *d, int clear, Res **src, const int *lin, int nsrc, const Res *size_of) {
	VkPipeline pipe;
	VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
	VkWriteDescriptorSet wr[4];
	VkDescriptorImageInfo ii[4];
	VkViewport vp;
	VkRect2D sc;
	VkDeviceSize voff;
	float *v, pc[8];
	int k;
	if (!(pipe = smaa_pipe(pass, d->fmt))) return 0;
	voff = ring_alloc(6 * 32, 16, (void **)&v); /* (first: getting ring space may submit what is recorded) */
	{
		const float q[6][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {-1, 1, 0, 1}, {1, -1, 1, 0}, {1, 1, 1, 1}, {-1, 1, 0, 1}};
		for (k = 0; k < 6; k++) {
			v[k * 8 + 0] = q[k][0]; v[k * 8 + 1] = q[k][1]; v[k * 8 + 2] = 0.0f; v[k * 8 + 3] = 1.0f;
			v[k * 8 + 4] = q[k][2]; v[k * 8 + 5] = q[k][3]; v[k * 8 + 6] = 0.0f; v[k * 8 + 7] = 1.0f;
		}
	}
	for (k = 0; k < nsrc; k++) bq_add(src[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	bq_add(d, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	for (k = 0; k < nsrc; k++) img_access(src[k], 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	img_access(d, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
	bq_go();
	ca.imageView = attach_view(d, 0, 0);
	ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ca.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	memset(&ca.clearValue, 0, sizeof ca.clearValue);
	ri.renderArea.extent.width = d->w; ri.renderArea.extent.height = d->h;
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
	vkCmdBeginRendering(cb, &ri);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	memset(wr, 0, sizeof wr);
	for (k = 0; k < nsrc; k++) {
		ii[k].sampler = blit_smp[lin[k] ? 1 : 0];
		ii[k].imageView = src[k]->view;
		ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstBinding = 2 + (u32)k; wr[k].descriptorCount = 1;
		wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[k].pImageInfo = &ii[k];
	}
	vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, (u32)nsrc, wr);
	memset(pc, 0, sizeof pc);
	pc[0] = 1.0f / (float)size_of->w; pc[1] = 1.0f / (float)size_of->h; pc[2] = (float)size_of->w; pc[3] = (float)size_of->h;
	vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, SM3_PUSH_SIZE, pc);
	vp.x = 0; vp.y = 0; vp.width = (float)d->w; vp.height = (float)d->h; vp.minDepth = 0; vp.maxDepth = 1;
	vkCmdSetViewport(cb, 0, 1, &vp);
	sc.offset.x = sc.offset.y = 0; sc.extent.width = d->w; sc.extent.height = d->h;
	vkCmdSetScissor(cb, 0, 1, &sc);
	{
		float bc[4] = {0, 0, 0, 0};
		vkCmdSetBlendConstants(cb, bc);
		vkCmdSetDepthBias(cb, 0, 0, 0);
		vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
		vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
		vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
	}
	vkCmdBindVertexBuffers(cb, 0, 1, &F->ring, &voff);
	vkCmdDraw(cb, 6, 1, 0, 0);
	vkCmdEndRendering(cb);
	return 1;
}
/* s (the 3D image) through SMAA into d (same size and format); 1: done */
static int smaa_copy(Res *s, Res *d) {
	Res *in[3];
	int lin[3];
	if (!d->can_att || !smaa_setup()) return 0;
	if (!smaa_edges || smaa_edges->w != s->w || smaa_edges->h != s->h) {
		if (smaa_edges) { res_defer(smaa_edges); smaa_edges = 0; }
		if (smaa_wts) { res_defer(smaa_wts); smaa_wts = 0; }
		if (!(smaa_edges = smaa_image(s->w, s->h, VK_FORMAT_R8G8_UNORM, 1)) || !(smaa_wts = smaa_image(s->w, s->h, VK_FORMAT_R8G8B8A8_UNORM, 1))) return 0;
		LOG("xrc: vulkan: SMAA %ux%u\n", s->w, s->h);
	}
	in[0] = s; lin[0] = 1;
	if (!smaa_pass(0, smaa_edges, 1, in, lin, 1, s)) return 0;
	in[0] = smaa_edges; in[1] = smaa_area; in[2] = smaa_search; lin[0] = 1; lin[1] = 1; lin[2] = 0;
	if (!smaa_pass(1, smaa_wts, 1, in, lin, 3, s)) return 0;
	in[0] = s; in[1] = smaa_wts; lin[0] = lin[1] = 1;
	return smaa_pass(2, d, 0, in, lin, 2, s);
}
#endif
/* vk_post.c: s (TRANSFER_SRC) drawn into b's rectangle of dst (TRANSFER_DST) where a copy would rescale or end on the
 * screen -- stage 0: the game's StretchRect into the back buffer; 1: the back buffer onto the screen. 1: done */
static VkFormat post_fmt; /* (present_copy's destination format when it is not the swap chain's: post_headless) */
static int post_blit(Res *s, const VkImageBlit *b, VkImage dst, VkFormat dfmt, int stage) {
	static int ready;
	if (!ready) {
		XrcPostVk v;
		memset(&v, 0, sizeof v);
		v.dev = vd; v.gdpa = vkGetDeviceProcAddr; v.mem = &memprops; v.ts_period = props.limits.timestampPeriod;
		v.barrier = vkCmdPipelineBarrier; v.begin = vkCmdBeginRendering; v.end = vkCmdEndRendering; v.bind = vkCmdBindPipeline;
		v.push_desc = vkCmdPushDescriptorSetKHR; v.push_const = vkCmdPushConstants; v.viewport = vkCmdSetViewport;
		v.scissor = vkCmdSetScissor; v.draw = vkCmdDraw; v.copy = vkCmdCopyImage; v.reset_q = vkCmdResetQueryPool; v.timestamp = vkCmdWriteTimestamp;
		xrc_post_init(&v);
		ready = 1;
	}
	if (!s || s->samples > 1 || s->depth || !s->view || s->bpp_block != 4) return 0;
	return xrc_post_blit(cb, s->img, s->view, s->w, s->h, dst, dfmt, b, stage, frame_no);
}
static void stretch_do(const u32 *a, int may_defer);
static void pend_flush(void);
#ifdef XRC_LR
/* LR ends a frame differently from FF13: one draw writes the finished 3D image (tone mapping, glare) into a surface of
 * the frame's size; the next rendering, into the surface copied to the back buffer (hud_rt), draws that image and the HUD
 * over it. So FXAA is made on that image as that rendering begins: from it into hud_rt (whose contents the next draw
 * replaces), then copied back -- the HUD stays sharp. prev: the rendering just ended (its render target, no depth). */
static void fxaa_scene(u32 prev) {
	Obj *so, *dso;
	Res *s, *d;
	u32 sf, sl, df, dl, a[5];
	VkImageBlit b;
	if (fxaa_q < 0) { fxaa_q = getenv("XRC_FXAA") ? atoi(getenv("XRC_FXAA")) : 16; fprintf(stderr, "xrc: vulkan: anti-aliasing %d (%s)\n", fxaa_q, getenv("XRC_FXAA") ? "set by the user" : "default"); }
	if (fxaa_q <= 1 && fxaa_q >= 0) return;
	if (!hud_rt || dev.rt[0] != hud_rt || dev.rt[1] || fxaa_frame == frame_no) return;
	if (!(so = obj_of(prev)) || !(dso = obj_of(hud_rt)) || !(s = surface_image(so, &sf, &sl)) || !(d = surface_image(dso, &df, &dl))) return;
	if (s == d || s->depth || d->depth || s->samples > 1 || d->samples > 1 || sf || sl || df || dl || s->w != d->w || s->h != d->h || s->fmt != d->fmt) return;
	pend_flush();
	memset(&b, 0, sizeof b);
	b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
	b.srcOffsets[1].x = b.dstOffsets[1].x = (int)s->w; b.srcOffsets[1].y = b.dstOffsets[1].y = (int)s->h; b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
	if (fxaa_q == 16) {
#if defined(_WIN32)
		if (!smaa_copy(s, d)) return;
		{ static int logged; if (!logged) { logged = 1; fprintf(stderr, "xrc: vulkan: SMAA active (frame %llu)\n", (unsigned long long)frame_no); } }
		fxaa_frame = frame_no;
#else
		return;
#endif
	} else if (!fxaa_copy(s, d, &b)) return;
	a[0] = hud_rt; a[1] = 0; a[2] = prev; a[3] = 0; a[4] = 1; /* (back: the whole surface, point filtered) */
	stretch_do(a, 0);
}
#endif
static void stretch_do(const u32 *a, int may_defer) {
	Obj *so = obj_of(a[0]), *dso = obj_of(a[2]);
	u32 sf, sl, df, dl;
	Res *s, *d;
	VkImageBlit b;
	if (!so || !dso) return;
	s = surface_ready(so, &sf, &sl);
	d = surface_image(dso, &df, &dl);
	if (!s || !d) return;
	if (s->depth) ZREAD(s);
	end_render(); /* (first: it turns a pending clear-only rendering into deferred clears -- flushed next, before the copy. The other way round a
	               * game Clear of the back buffer before its pillarboxed copy (21:9) was made at Present, after the copy: black) */
	clears_flush(s); clears_flush(d);
	memset(&b, 0, sizeof b);
	b.srcSubresource.aspectMask = s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (s->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
	b.srcSubresource.mipLevel = sl; b.srcSubresource.baseArrayLayer = sf; b.srcSubresource.layerCount = 1;
	b.dstSubresource = b.srcSubresource;
	b.dstSubresource.aspectMask = d->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (d->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
	b.dstSubresource.mipLevel = dl; b.dstSubresource.baseArrayLayer = df;
	rect_of(a[1], dim(s->w, sl), dim(s->h, sl), b.srcOffsets);
	rect_of(a[3], dim(d->w, dl), dim(d->h, dl), b.dstOffsets);
	if (s->depth != d->depth || (s->depth && s->fmt != d->fmt)) return;
	if (s->samples > 1) { /* multisampled: resolved (straight into d when it is the same size and format) */
		if (d->samples == 1 && s->fmt == d->fmt && s->w == dim(d->w, dl) && s->h == dim(d->h, dl) && b.srcOffsets[0].x == 0 && b.srcOffsets[0].y == 0 &&
		    b.srcOffsets[1].x == (int)s->w && b.srcOffsets[1].y == (int)s->h && !memcmp(b.srcOffsets, b.dstOffsets, sizeof b.srcOffsets) && !s->depth) {
			VkImageResolve rv;
			memset(&rv, 0, sizeof rv);
			rv.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rv.srcSubresource.layerCount = 1;
			rv.dstSubresource = b.dstSubresource;
			rv.extent.width = s->w; rv.extent.height = s->h; rv.extent.depth = 1;
			PP_OP(0);
			bq_add(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); bq_add(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			IMG_R_XFER(s); IMG_W_XFER(d); bq_go();
			vkCmdResolveImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rv);
			return;
		}
		if (!(s = resolved_of(s))) return;
		b.srcSubresource.mipLevel = 0; b.srcSubresource.baseArrayLayer = 0;
	}
	if (d->samples > 1) { /* (into a multisampled surface: not done) */
		static int said;
		if (!said++) fprintf(stderr, "xrc: vulkan: StretchRect into a multisampled surface: skipped\n");
		return;
	}
	if (a[2] == hud_rt && hud_rt && s != d && !s->depth && s->samples == 1 && sf == 0 && sl == 0 && dl == 0 && df == 0 && s->w == d->w && s->h == d->h &&
	    b.srcOffsets[0].x == 0 && b.srcOffsets[0].y == 0 && b.srcOffsets[1].x == (int)s->w && b.srcOffsets[1].y == (int)s->h &&
	    !memcmp(b.srcOffsets, b.dstOffsets, sizeof b.srcOffsets) && fxaa_copy(s, d, &b))
		return; /* (the 3D image into the HUD's surface: through FXAA) */
	if (may_defer && npend < NPEND) { /* (be_stretch_rect: a full-size copy) */
		memcpy(pend[npend].a, a, sizeof pend[npend].a);
		/* (no rectangles: the whole surfaces, as full_copy checked. The game's are pointers into its stack, gone by
		 * the time a held-back copy is made -- the copy into the back buffer that a movie frame's texture locks
		 * made came out black: the 1920x1080 movies flickered) */
		pend[npend].a[1] = pend[npend].a[3] = 0;
		pend[npend].s = s; pend[npend].d = d;
		npend++;
		return;
	}
	if (a[2] == dev.backbuf && s != d && !s->depth && sl == 0 && dl == 0 && b.srcOffsets[1].x > b.srcOffsets[0].x && b.srcOffsets[1].y > b.srcOffsets[0].y &&
	    xrc_post_active(0, (u32)(b.srcOffsets[1].x - b.srcOffsets[0].x), (u32)(b.srcOffsets[1].y - b.srcOffsets[0].y),
	                    (u32)abs(b.dstOffsets[1].x - b.dstOffsets[0].x), (u32)abs(b.dstOffsets[1].y - b.dstOffsets[0].y))) {
		/* (the game's picture into the back buffer, rescaled: vk_post.c's Lanczos, not a two-tap blit) */
		PP_OP(0);
		bq_add(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); bq_add(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		IMG_R_XFER(s); IMG_W_XFER(d); bq_go();
		if (post_blit(s, &b, d->img, d->fmt, 0)) return;
	}
	PP_OP(0);
#ifndef XRC_NO_TOOLS
	if (ppool && pp_rec[fi] && !hitch_ms)
		fprintf(stderr, "xrc: vulkan: (the frame of the renderings below) blit %08x %ux%u fmt %u%s -> %08x %ux%u fmt %u, rects %d,%d-%d,%d -> %d,%d-%d,%d, filter %u, before rendering %u\n",
		        a[0], s->w, s->h, (u32)s->fmt, s->depth ? " (depth)" : "", a[2], d->w, d->h, (u32)d->fmt, b.srcOffsets[0].x, b.srcOffsets[0].y, b.srcOffsets[1].x,
		        b.srcOffsets[1].y, b.dstOffsets[0].x, b.dstOffsets[0].y, b.dstOffsets[1].x, b.dstOffsets[1].y, a[4], pp_n[fi]);
#endif
	if (blit_draw_on < 0) blit_draw_on = !(getenv("XRC_VK_BLIT_DRAW") && !atoi(getenv("XRC_VK_BLIT_DRAW")));
	if (blit_draw_on && ((blit_setup() && blit_all) || !(s->fmt == d->fmt && b.srcOffsets[1].x - b.srcOffsets[0].x == b.dstOffsets[1].x - b.dstOffsets[0].x &&
	                                    b.srcOffsets[1].y - b.srcOffsets[0].y == b.dstOffsets[1].y - b.dstOffsets[0].y)) &&
	    blit_draw(s, b.srcSubresource.baseArrayLayer, b.srcSubresource.mipLevel, d, df, dl, &b, !(s->depth || a[4] == 1), 0, 0))
		return; /* (scaled or converted: drawn) */
	{ /* (the same image: GENERAL) */
		VkImageLayout sl2 = s == d ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dl2 = s == d ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		bq_add(s, sl2); bq_add(d, dl2);
		IMG_R_XFER(s); IMG_W_XFER(d); bq_go();
	}
	if (s->fmt == d->fmt && b.srcOffsets[1].x - b.srcOffsets[0].x == b.dstOffsets[1].x - b.dstOffsets[0].x &&
	    b.srcOffsets[1].y - b.srcOffsets[0].y == b.dstOffsets[1].y - b.dstOffsets[0].y && b.srcOffsets[1].x > b.srcOffsets[0].x &&
	    b.srcOffsets[1].y > b.srcOffsets[0].y && b.dstOffsets[0].x >= 0 && b.dstOffsets[0].y >= 0 && b.srcOffsets[0].x >= 0 && b.srcOffsets[0].y >= 0) {
		/* the same size and format: a copy (no filtering to do; a blit may take a slower path -- on an AMD GPU two
		 * 1280x720 blits took ~2.8 ms) */
		VkImageCopy ic;
		ic.srcSubresource = b.srcSubresource; ic.dstSubresource = b.dstSubresource;
		ic.srcOffset = b.srcOffsets[0]; ic.dstOffset = b.dstOffsets[0];
		ic.extent.width = (u32)(b.srcOffsets[1].x - b.srcOffsets[0].x);
		ic.extent.height = (u32)(b.srcOffsets[1].y - b.srcOffsets[0].y);
		ic.extent.depth = 1;
		vkCmdCopyImage(cb, s->img, s->lay, d->img, d->lay, 1, &ic);
	} else
		vkCmdBlitImage(cb, s->img, s->lay, d->img, d->lay, 1, &b,
		               s->depth || a[4] == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
}
static void be_color_fill(const u32 *a) { dyn_gen++;
	Obj *so = (PP_OP(1), obj_of(a[0]));
	u32 f, l, c = a[2];
	Res *r;
	VkClearColorValue col;
	VkImageSubresourceRange sr;
	if (!so || !(r = surface_image(so, &f, &l)) || r->depth || is_dxt(r->d3dfmt)) return;
	end_render();
	clears_flush(r);
	col.float32[0] = ((c >> 16) & 0xff) / 255.0f; col.float32[1] = ((c >> 8) & 0xff) / 255.0f;
	col.float32[2] = (c & 0xff) / 255.0f; col.float32[3] = (c >> 24) / 255.0f;
	sr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; sr.baseMipLevel = l; sr.levelCount = 1; sr.baseArrayLayer = f; sr.layerCount = 1;
	if (a[1]) { /* a rect: as a clear in a rendering of that surface */
		VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
		VkClearAttachment cl;
		VkClearRect cr;
		VkOffset3D o[2];
		if (!fmt_supports(r->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return;
		rect_of(a[1], dim(r->w, l), dim(r->h, l), o);
		bq_add(r, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		img_access(r, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		bq_go();
		ca.imageView = attach_view(r, f, l); ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ri.renderArea.extent.width = dim(r->w, l); ri.renderArea.extent.height = dim(r->h, l);
		ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
		vkCmdBeginRendering(cb, &ri);
		cl.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; cl.colorAttachment = 0; cl.clearValue.color = col;
		cr.rect.offset.x = o[0].x; cr.rect.offset.y = o[0].y;
		cr.rect.extent.width = (u32)(o[1].x - o[0].x); cr.rect.extent.height = (u32)(o[1].y - o[0].y);
		cr.baseArrayLayer = 0; cr.layerCount = 1;
		if (o[1].x > o[0].x && o[1].y > o[0].y) vkCmdClearAttachments(cb, 1, &cl, 1, &cr);
		vkCmdEndRendering(cb);
	} else {
		bq_add(r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); IMG_W_XFER(r); bq_go();
		vkCmdClearColorImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &sr);
	}
}

/* an image level read back into memory (waits for the GPU) */
static int read_back(Res *r, u32 face, u32 level, void *dst, u32 size) {
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	VkMemoryRequirements rq;
	VkBuffer b;
	Alloc m;
	VkBufferImageCopy bc;
	end_render(); /* (before the flush: see stretch_do) */
	clears_flush(r);
	if (r->samples > 1) { if (!(r = resolved_of(r))) return 0; face = level = 0; }
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VKC(vkCreateBuffer(vd, &bi, 0, &b));
	vkGetBufferMemoryRequirements(vd, b, &rq);
	m = mem_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
	if (m.block < 0) { vkDestroyBuffer(vd, b, 0); return 0; }
	VKC(vkBindBufferMemory(vd, b, blocks[m.block].mem, m.off));
	end_render();
	memset(&bc, 0, sizeof bc);
	bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	bc.imageSubresource.mipLevel = level; bc.imageSubresource.baseArrayLayer = face; bc.imageSubresource.layerCount = 1;
	bc.imageExtent.width = dim(r->w, level); bc.imageExtent.height = dim(r->h, level); bc.imageExtent.depth = 1;
	PP_OP(4);
	bq_add(r, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); IMG_R_XFER(r); bq_go();
	vkCmdCopyImageToBuffer(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &bc);
	barrier();
	flush_wait();
	memcpy(dst, (unsigned char *)blocks[m.block].map + m.off, size);
	vkDestroyBuffer(vd, b, 0);
	mem_free(m);
	return 1;
}
static u32 be_rt_data(const u32 *a) { dyn_gen++;
	Obj *so = obj_of(a[0]), *d = obj_of(a[1]);
	u32 f, l, pitch, bits;
	Res *r;
	if (!so || !d || !(r = surface_image(so, &f, &l)) || r->depth || r->narrow || so->fmt != d->fmt) {
		LOG("xrc: vulkan: GetRenderTargetData %x -> %x failed (formats %u %u)\n", a[0], a[1], so ? so->fmt : 0, d ? d->fmt : 0);
		return 0x8876086cu;
	}
	bits = surf_bits(d, &pitch);
	if (pitch * rows_of(d->fmt, d->h) < r->bpp_block * dim(r->w, l) * dim(r->h, l)) { LOG("xrc: vulkan: GetRenderTargetData: too small\n"); return 0x8876086cu; }
	LOG("xrc: vulkan: GetRenderTargetData %ux%u\n", dim(r->w, l), dim(r->h, l));
	read_back(r, f, l, xrc_mem + bits, r->bpp_block * dim(r->w, l) * dim(r->h, l));
	return 0;
}

/* ============================================================================================ frames */
static void begin_frame(void) { dyn_gen++;
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	u32 k;
	if (F->submitted) {
		double t = now_ms();
		cs_wait(F->serial); /* (the render thread has submitted it) */
		xrc_gil_release();
		VKC(vkWaitForFences(vd, 1, &F->fence, 1, ~0ull));
		xrc_gil_acquire();
		wait_ms += now_ms() - t;
		HC(fwait, now_ms() - t);
		F->submitted = 0;
	}
	VKC(vkResetFences(vd, 1, &F->fence));
#ifndef XRC_NO_TOOLS
	int h_show = 0;
	double h_gpu = -1;
	if (tspool && F->ts_written) {
		uint64_t ts[2];
		if (vkGetQueryPoolResults(vd, tspool, (u32)fi * 2u, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
			double ms = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
			gpu_ms += ms;
			h_gpu = ms;
			{ extern double xrc_gpu_ms_total; xrc_gpu_ms_total += ms; }
		}
		F->ts_written = 0;
	}
	if (hitch_ms && hitch[fi].valid) {
		Hitch *h = &hitch[fi];
		h_show = h->interval > hitch_ms || h_gpu > hitch_ms;
		if (h_show) {
			h_reported++;
			hprint("xrc: HITCH frame %llu (ends at %llu us): %.2f ms present to present (avg %.2f), GPU %.2f ms (avg %.2f) | main thread: %.2f ms CPU; %.2f in %u draws, %.2f in vkQueueSubmit, %.2f in acquire+present, %.2f waiting for the GPU (frame fence), %.2f frame limiter, %.2f waiting for the GIL, %.2f in %u flushes (ring full / read-back), %.2f in %u memory allocations, %.2f the rest (the game's own work) | %u images made, %u pipelines requested, %u draws skipped (pipeline not ready), %.0f KB uploaded\n",
			        h->frame, h->t_end, h->interval, h_avg_int, h_gpu, h_avg_gpu, h->cpu, h->draw, h->ndraw, h->submit, h->present, h->fwait, h->limit, h->gil, h->flush, h->nflush, h->alloc, h->nalloc,
			        h->interval - h->draw - h->submit - h->present - h->fwait - h->limit - h->gil - h->flush - h->alloc, h->nimg, h->npipe, h->nskip, h->upkb);
		}
		if (h_gpu >= 0) h_avg_gpu = h_avg_gpu ? h_avg_gpu * 0.98 + h_gpu * 0.02 : h_gpu;
		h_avg_int = h_avg_int ? h_avg_int * 0.98 + h->interval * 0.02 : h->interval;
		h->valid = 0;
	}
	if (dt_rec[fi]) {
		static uint64_t ts[DT_MAX], fr[DT_MAX];
		u32 n = dt_n[fi], j;
		if (n && vkGetQueryPoolResults(vd, dtpool, (u32)fi * DT_MAX, n, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS
		    && (!dspool || vkGetQueryPoolResults(vd, dspool, (u32)fi * DT_MAX, n, sizeof fr, fr, sizeof fr[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)) {
			double per = props.limits.timestampPeriod / 1e6;
			uint64_t prev = 0;
			dt_frames++;
			for (j = 0; j < n; j++) {
				DtDraw *d = &dt[fi][j];
				if (!d->ps) { prev = ts[j]; continue; }
				if (d->ps < 8192) {
					DtSum *q = &dt_sum[d->ps];
					q->ms += prev && ts[j] >= prev ? (double)(ts[j] - prev) * per : 0;
					q->frags += dspool ? (double)fr[j] : 0;
					q->draws++;
					q->blend = d->blend; q->at = d->at; q->w = d->w; q->h = d->h; q->nsmp = d->nsmp; q->words = d->words; q->vs = d->vs;
				}
				prev = ts[j];
			}
		}
		dt_rec[fi] = 0;
		if (dt_frames >= 20) { /* (the report: the costliest pixel shaders over the frames sampled since the last) */
			static u32 idx[8192];
			u32 m = 0, a, b2;
			double tot = 0;
			for (j = 1; j < 8192; j++) if (dt_sum[j].draws) { idx[m++] = j; tot += dt_sum[j].ms; }
			for (a = 1; a < m; a++) for (b2 = a; b2 > 0 && dt_sum[idx[b2]].ms > dt_sum[idx[b2 - 1]].ms; b2--) { u32 t = idx[b2]; idx[b2] = idx[b2 - 1]; idx[b2 - 1] = t; }
			hprint("xrc: vulkan: draw times at frame %llu, %u frames sampled, %.2f ms GPU per frame in draws; per pixel shader (ms per frame, draws per frame, Mpx shaded per frame, ns per pixel, blend, alpha test, target, samplers, bytecode words, vs):\n", frame_no, dt_frames, tot / dt_frames);
			for (a = 0; a < m && a < 30; a++) {
				DtSum *q = &dt_sum[idx[a]];
				hprint("xrc:   ps %4u  %6.3f ms  %6.1f draws  %6.2f Mpx  %6.2f ns/px  blend %u at %u  %4ux%-4u  smp %u  words %4u  vs %u\n", idx[a], q->ms / dt_frames, (double)q->draws / dt_frames,
				       q->frags / dt_frames / 1e6, q->frags > 0 ? q->ms * 1e6 / q->frags : 0, q->blend, q->at, q->w, q->h, q->nsmp, q->words, q->vs);
			}
			memset(dt_sum, 0, sizeof dt_sum);
			dt_frames = 0;
		}
	}
	if (ppool && pp_rec[fi] && hitch_ms && !h_show) pp_rec[fi] = 0; /* (an ordinary frame: its renderings not shown) */
	if (ppool && pp_rec[fi]) {
		static uint64_t ts[2 * PP_MAX];
		u32 n = pp_n[fi], j;
		if (n && vkGetQueryPoolResults(vd, ppool, (u32)fi * 2u * PP_MAX, 2u * n, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
			double sum = 0, per = props.limits.timestampPeriod / 1e6;
			hprint("xrc: vulkan: a frame's renderings (GPU ms, the gap before it, size, color attachments + depth, format, draws, clears, rt0; work outside renderings before it: blits, fills, image / buffer uploads, read-backs; draws with the alpha test, depth written by the shader, blending, depth test, depth write):\n");
			for (j = 0; j < n; j++) {
				PPass *q = &pp[fi][j];
				double ms = ts[2 * j + 1] >= ts[2 * j] ? (double)(ts[2 * j + 1] - ts[2 * j]) * per : 0;
				sum += ms;
				double gap = j && ts[2 * j] >= ts[2 * j - 1] ? (double)(ts[2 * j] - ts[2 * j - 1]) * per : 0;
				hprint("xrc:   %3u  %6.3f ms (gap %6.3f)  %4ux%-4u  %u%s  fmt %3u  %4u draws  %u clears  %08x   %u %u %u %u %u   at %u zout %u blend %u zt %u zw %u\n", j, ms, gap, q->w, q->h, q->ncol,
				        q->depth ? "+d" : "  ", q->fmt, q->draws, q->clears, q->rt0, q->blits, q->fills, q->ups, q->upb, q->reads, q->n_at, q->n_zout, q->n_blend, q->n_zt, q->n_zw);
			}
			hprint("xrc:   %.3f ms in %u renderings; first to last %.3f ms\n", sum, n, (double)(ts[2 * n - 1] - ts[0]) * per);
		}
		pp_rec[fi] = 0;
	}
#endif
	for (k = 0; k < F->ndead; k++) res_destroy(F->dead[k]);
	F->ndead = 0;
	F->ring_used = 0;
	cb = F->cb;
	VKC(vkResetCommandBuffer(cb, 0));
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKC(vkBeginCommandBuffer(cb, &bi));
#ifndef XRC_NO_TOOLS
	if (tspool) {
		vkCmdResetQueryPool(cb, tspool, (u32)fi * 2u, 2);
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, tspool, (u32)fi * 2u);
	}
	if (dtpool && !cs_on && frame_no % dt_every == 0) {
		vkCmdResetQueryPool(cb, dtpool, (u32)fi * DT_MAX, DT_MAX);
		if (dspool) vkCmdResetQueryPool(cb, dspool, (u32)fi * DT_MAX, DT_MAX);
		dt_rec[fi] = 1; dt_n[fi] = 0;
	}
	if (ppool && (hitch_ms || frame_no % 600 == 300)) {
		vkCmdResetQueryPool(cb, ppool, (u32)fi * 2u * PP_MAX, 2u * PP_MAX);
		pp_rec[fi] = 1; pp_n[fi] = 0; pp_open = 0;
	}
#endif
	in_render = 0; r_pending = 0;
}
static void submit_frame(int wait_acquired, VkSemaphore signal) {
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	end_render();
#ifndef XRC_NO_TOOLS
	if (tspool) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tspool, (u32)fi * 2u + 1u); F->ts_written = 1; }
#endif
	VKC(vkEndCommandBuffer(cb));
	si.commandBufferCount = 1; si.pCommandBuffers = &cb;
	if (wait_acquired) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &F->acquired; si.pWaitDstStageMask = &ws; }
	if (signal) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &signal; }
	{
#ifndef XRC_NO_TOOLS
		double ts = now_ms();
#endif
		VKC(vkQueueSubmit(queue, 1, &si, F->fence));
		HC(submit, now_ms() - ts);
	}
	F->submitted = 1;
	F->serial = cs_serial;
}
/* everything recorded so far: submitted and waited for; recording goes on in the same frame (its ring from 0) */
static void flush_wait(void) {
	int k;
#ifndef XRC_NO_TOOLS
	double tf = now_ms();
#endif
	submit_frame(0, VK_NULL_HANDLE);
	for (k = 0; k < NFRAMES; k++) if (k != fi && frames[k].submitted) { /* (the other frames too: what dies may be theirs) */
		cs_wait(frames[k].serial);
		xrc_gil_release();
		VKC(vkWaitForFences(vd, 1, &frames[k].fence, 1, ~0ull));
		xrc_gil_acquire();
	}
	begin_frame(); /* (the next draw begins a rendering again) */
	HC(flush, now_ms() - tf); HC(nflush, 1);
}

static void make_swapchain(void) {
	VkSurfaceCapabilitiesKHR caps;
	VkSurfaceFormatKHR sf[32];
	VkPresentModeKHR pm[16];
	u32 n = 32, k, npm = 16;
	VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	VkSwapchainKHR old = swapchain;
	int w, h;
	VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
	VKC(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, sf));
	swap_fmt = sf[0].format;
	for (k = 0; k < n; k++) if (sf[k].format == VK_FORMAT_B8G8R8A8_UNORM || sf[k].format == VK_FORMAT_R8G8B8A8_UNORM) { swap_fmt = sf[k].format; break; }
	plat_size(&w, &h);
	swap_plat_w = w; swap_plat_h = h; /* (compared with the window's at each Present: a resize. Not swap_ext, which a
	                                   * driver may report in other units than the window's) */
	swap_ext = caps.currentExtent;
	if (swap_ext.width == 0xffffffffu) { swap_ext.width = (u32)w; swap_ext.height = (u32)h; }
	if (!swap_ext.width || !swap_ext.height) { swap_ok = 0; return; } /* (minimised: no swap chain until it has a size) */
	swap_ok = 1;
	ci.surface = surface;
	ci.minImageCount = caps.minImageCount < 3 ? (caps.maxImageCount && caps.maxImageCount < 3 ? caps.maxImageCount : 3) : caps.minImageCount;
	/* (not two without present wait, though that keeps at most one finished frame waiting for the display: at 1080p,
	 * 10-12 ms of GPU and 12 ms of main thread per frame, a missed refresh made the next frame wait a whole one too --
	 * stretches at 30 fps) */
	if (xrc_tool_env("XRC_VK_SWAP_IMAGES")) { /* (a test: the swap chain's length) */
		u32 n = (u32)atoi(xrc_tool_env("XRC_VK_SWAP_IMAGES"));
		if (n >= caps.minImageCount && (!caps.maxImageCount || n <= caps.maxImageCount)) ci.minImageCount = n;
	}
	ci.imageFormat = swap_fmt;
	ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	ci.imageExtent = swap_ext;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = caps.currentTransform;
	ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	/* FIFO (vsync: no frame is drawn that the display does not show) whenever the driver has it -- Vulkan requires it,
	 * so this is every driver -- else IMMEDIATE. Not MAILBOX: it does not work on every system (and draws frames that
	 * are never shown). */
	VKC(vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &npm, pm));
	{
		int fifo = 0, immediate = 0;
		for (k = 0; k < npm; k++) {
			if (pm[k] == VK_PRESENT_MODE_FIFO_KHR) fifo = 1;
			if (pm[k] == VK_PRESENT_MODE_IMMEDIATE_KHR) immediate = 1;
		}
		ci.presentMode = fifo || !immediate ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
		if (immediate && xrc_tool_env("XRC_UNLOCKED")) ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR; /* (a benchmark: ovr_game.c) */
		{ /* hifps1: XRC_FPS_CAP above the display's refresh (or unlocked): FIFO would hold the game at the refresh, so
		   * IMMEDIATE (vsync off; with VRR the display follows the frames). A cap at or below the refresh keeps FIFO. */
			extern int xrc_hifps_want_immediate(void);
			if (immediate && xrc_hifps_want_immediate()) ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
		}
		{ /* XRC_VK_PRESENT=fifo|mailbox|immediate: that one, if the driver has it */
			const char *want = getenv("XRC_VK_PRESENT");
			VkPresentModeKHR wm = !want ? ci.presentMode : !strcmp(want, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR : !strcmp(want, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
			for (k = 0; k < npm; k++) if (pm[k] == wm) ci.presentMode = wm;
		}
		{
			static const char *nm[] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};
			char have[64] = "";
			for (k = 0; k < npm; k++) if ((u32)pm[k] < 4) { strcat(have, " "); strcat(have, nm[pm[k]]); }
			LOG("xrc: vulkan: present mode %s (the driver has:%s)\n", (u32)ci.presentMode < 4 ? nm[ci.presentMode] : "?", have);
		}
	}
	ci.clipped = 1;
	ci.oldSwapchain = old;
	VKC(vkCreateSwapchainKHR(vd, &ci, 0, &swapchain));
	pw_first = pw_last + 1; /* (ids presented before are the old swap chain's) */
	if (old) vkDestroySwapchainKHR(vd, old, 0);
	nswap = 8;
	VKC(vkGetSwapchainImagesKHR(vd, swapchain, &nswap, swap_img));
	for (k = 0; k < nswap; k++) if (!swap_done[k]) {
		VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VKC(vkCreateSemaphore(vd, &si, 0, &swap_done[k]));
	}
	xrc_g_out_w = (int)swap_ext.width; xrc_g_out_h = (int)swap_ext.height;
	xrc_size_note("output set (swap chain)");
	LOG("xrc: vulkan: swap chain %ux%u, %u images, format %d\n", swap_ext.width, swap_ext.height, nswap, (int)swap_fmt);
}

/* XRC_SHOT: the back buffer as a PNG */
void xrc_png_write(const char *path, const unsigned char *bgrx, int pitch, unsigned w, unsigned h);
static u32 shot_src; /* (be_present) */
static int ab_mode, ab_val, ab_phase; /* (XRC_SHOT_AB) */
/* xrc_shot_request(path) (ovr_debug.c's photo key): the next presented frame as a PNG at path, full back buffer size */
static char shot_req[700];
void xrc_shot_request(const char *path) { snprintf(shot_req, sizeof shot_req, "%s", path); }
static void shot_req_write(Res *bb) {
	unsigned char *buf = (unsigned char *)malloc((size_t)bb->w * bb->h * 4);
	if (buf && read_back(bb, 0, 0, buf, bb->w * bb->h * 4)) {
		if (bb->fmt == VK_FORMAT_R8G8B8A8_UNORM) { size_t i; for (i = 0; i < (size_t)bb->w * bb->h; i++) { unsigned char t = buf[4 * i]; buf[4 * i] = buf[4 * i + 2]; buf[4 * i + 2] = t; } }
		xrc_png_write(shot_req, buf, (int)bb->w * 4, bb->w, bb->h);
		fprintf(stderr, "xrc: vulkan: %s (%ux%u)\n", shot_req, bb->w, bb->h);
	}
	free(buf);
	shot_req[0] = 0;
}
static void maybe_shot(Res *bb) {
	static double next = -2, t0;
	static const char *p;
	if (shot_req[0] && bb && bb->bpp_block == 4) shot_req_write(bb);
	struct timespec ts;
	double now;
	char path[600];
	const char *sd = xrc_tool_env("XRC_SHOT_DIR"); /* (XRC_SHOT_DIR=dir: the shots there, not in the run directory) */
	unsigned char *buf;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
	if (next == -2) { p = xrc_tool_env("XRC_SHOT"); next = p ? atof(p) : -1; t0 = now; }
	if (next < 0 || now - t0 < next || !bb || bb->bpp_block != 4) return;
	/* XRC_SHOT_AB=tex|texb|ui|<anti-aliasing value>: each shot twice, on two frames in a row -- shot_<secs>_a.png as the run is
	 * set, shot_<secs>_b.png with HD textures / the XRC_TEX_DIR_B set (texb) / HD UI / that anti-aliasing on -- so an A/B pair
	 * shows the same moment */
	if (!ab_mode) { const char *e = xrc_tool_env("XRC_SHOT_AB"); ab_mode = !e ? -1 : !strcmp(e, "dc") ? 7 : !strcmp(e, "dc0") ? 8 : !strcmp(e, "tex") ? 1 : !strcmp(e, "ui") ? 2 : !strcmp(e, "texb") ? 5 : !strcmp(e, "post") ? 6 : !strncmp(e, "aniso", 5) ? 4 : 3; ab_val = !e ? 0 : atoi(e + (ab_mode == 4 ? 5 : 0)); if (ab_mode == 6) xrc_post_bypass(1); }
	if (ab_mode > 0) snprintf(path, sizeof path, "%s%sshot_%g_%c.png", sd ? sd : "", sd ? "/" : "", next, ab_phase ? 'b' : 'a');
	else
	snprintf(path, sizeof path, "%s%sshot_%g.png", sd ? sd : "", sd ? "/" : "", next);
	buf = (unsigned char *)malloc((size_t)bb->w * bb->h * 4);
	if (buf && read_back(bb, 0, 0, buf, bb->w * bb->h * 4)) {
		if (bb->fmt == VK_FORMAT_R8G8B8A8_UNORM) { size_t i; for (i = 0; i < (size_t)bb->w * bb->h; i++) { unsigned char t = buf[4 * i]; buf[4 * i] = buf[4 * i + 2]; buf[4 * i + 2] = t; } }
		xrc_png_write(path, buf, (int)bb->w * 4, bb->w, bb->h);
		fprintf(stderr, "xrc: vulkan: %s (%ux%u)\n", path, bb->w, bb->h);
#if defined(XRC_LR) && defined(_WIN32)
		{ const char *ms = getenv("XRC_MESH_AT_SHOT"); if (ms && next >= atof(ms)) md_req = 1; } /* (mesh dump of the frame after each shot from that second on) */
#endif
	}
	free(buf);
	if (xrc_tool_env("XRC_SHOT_SRC") && shot_src) { /* (debug: the surface the game copied into the back buffer) */
		Obj *so = obj_of(shot_src);
		u32 f, l;
		Res *s = so ? surface_image(so, &f, &l) : 0;
		if (s && s->bpp_block == 4 && s->samples == 1 && !s->depth && (buf = (unsigned char *)malloc((size_t)s->w * s->h * 4))) {
			snprintf(path, sizeof path, "%s%sshot_%g_src.png", sd ? sd : "", sd ? "/" : "", next);
			if (read_back(s, 0, 0, buf, s->w * s->h * 4)) {
				if (s->fmt == VK_FORMAT_R8G8B8A8_UNORM) { size_t i; for (i = 0; i < (size_t)s->w * s->h; i++) { unsigned char t = buf[4 * i]; buf[4 * i] = buf[4 * i + 2]; buf[4 * i + 2] = t; } }
				xrc_png_write(path, buf, (int)s->w * 4, s->w, s->h);
				fprintf(stderr, "xrc: vulkan: %s (%ux%u fmt %u)\n", path, s->w, s->h, (u32)s->fmt);
			}
			free(buf);
		} else fprintf(stderr, "xrc: vulkan: shot source %x not saved (%s)\n", shot_src, s ? "format" : "no image");
	}
	if (ab_mode > 0) {
		ab_phase = !ab_phase;
		if (ab_mode == 1) { static int was; if (ab_phase) { was = tex_show; tex_show = 1; } else tex_show = was; }
		else if (ab_mode == 2) { static int was; if (ab_phase) { was = tex_ui_show; tex_ui_show = 1; } else tex_ui_show = was; }
		else if (ab_mode == 5) tex_alt = ab_phase; /* (A: XRC_TEX_DIR, B: XRC_TEX_DIR_B; run with HD textures on) */
		else if (ab_mode == 4) { static int was; if (ab_phase) { was = aniso_force; aniso_force = ab_val; } else aniso_force = was; }
		else if (ab_mode == 7) dc_off = ab_phase;
		else if (ab_mode == 8) {}
		else if (ab_mode == 6) xrc_post_bypass(!ab_phase); /* (post: A with vk_post.c's passes off, B as set) */
		else { static int was; if (ab_phase) { was = fxaa_q; fxaa_q = ab_val; } else fxaa_q = was; }
		if (ab_phase) return; /* (the B shot: the next present) */
	}
	p = strchr(p, ',');
	next = p ? atof(++p) : -1;
}

/* XRC_REC=start,secs: from <start> s for <secs> s, each moment twice on two frames in a row -- rec_a.raw as the run is
 * set, rec_b.raw with HD textures on (the XRC_SHOT_AB=tex pair as video) -- raw BGRA frames, size and count in the log
 * ("xrc: rec: ..."), for ffmpeg -f rawvideo. Run with XRC_TEX_HD=0. */
static void maybe_rec(Res *bb) {
	static int st, phase, was;
	static double t0, t1, start, dur;
	static FILE *fa, *fb;
	static unsigned char *buf;
	static unsigned n, rw, rh;
	struct timespec ts;
	double now;
	if (st < 0 || !bb || bb->bpp_block != 4) return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
	if (!st) {
		const char *e = xrc_tool_env("XRC_REC"), *k;
		if (!e) { st = -1; return; }
		start = atof(e);
		k = strchr(e, ',');
		dur = k ? atof(k + 1) : 10;
		t0 = now;
		st = 1;
	}
	if (now - t0 < start) return;
	if (!fa) {
		rw = bb->w; rh = bb->h;
		fa = fopen("rec_a.raw", "wb");
		fb = fopen("rec_b.raw", "wb");
		buf = (unsigned char *)malloc((size_t)rw * rh * 4);
		if (!fa || !fb || !buf) { fprintf(stderr, "xrc: rec: cannot open rec_a.raw / rec_b.raw\n"); st = -1; return; }
		t1 = now;
	}
	if (!phase && now - t1 >= dur) {
		fclose(fa); fclose(fb); free(buf);
		fprintf(stderr, "xrc: rec: %u frame pairs %ux%u in %.2f s (%.2f per second)\n", n, rw, rh, now - t1, n / (now - t1));
		st = -1;
		return;
	}
	if (bb->w != rw || bb->h != rh || !read_back(bb, 0, 0, buf, rw * rh * 4)) return;
	if (bb->fmt == VK_FORMAT_R8G8B8A8_UNORM) { size_t i; for (i = 0; i < (size_t)rw * rh; i++) { unsigned char t = buf[4 * i]; buf[4 * i] = buf[4 * i + 2]; buf[4 * i + 2] = t; } }
	fwrite(buf, 4, (size_t)rw * rh, phase ? fb : fa);
	if (!phase) { was = tex_show; tex_show = 1; phase = 1; }
	else { tex_show = was; phase = 0; n++; }
}

/* ============================================================================================ the frame rate counter
 * (debug tools builds): the presents per second, averaged over half a second and shown twice a second in the swap chain image's
 * top right corner -- white 5x7 digits on a dark box, made on the CPU and copied in after the back buffer (no shader;
 * 8-bit 4-channel swap chain formats only, the ones make_swapchain prefers). */
#ifndef XRC_NO_TOOLS
static const unsigned char ft_font[13][7] = { /* 0-9 F P S: 5 bits a row, the left column in bit 4 */
	{14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {31, 2, 4, 2, 1, 17, 14},
	{2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
	{14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}, {31, 16, 16, 30, 16, 16, 16}, {30, 17, 17, 30, 16, 16, 16},
	{15, 16, 16, 14, 1, 1, 30}};
static void draw_frame_time(VkImage img) {
	static double last, acc, shown = -1, since;
	static int n, off = -1;
	double t = now_ms();
	char text[24], *ch;
	int scale, nch, bw, bh, x0, y0, x, y;
	u32 *px;
	VkDeviceSize ro;
	VkBufferImageCopy bc;
	if (off < 0) off = xrc_tool_env("XRC_FRAME_TIME") && atoi(xrc_tool_env("XRC_FRAME_TIME")) == 0;
	if (off || (swap_fmt != VK_FORMAT_B8G8R8A8_UNORM && swap_fmt != VK_FORMAT_R8G8B8A8_UNORM && swap_fmt != VK_FORMAT_B8G8R8A8_SRGB && swap_fmt != VK_FORMAT_R8G8B8A8_SRGB)) return;
	if (last > 0) { acc += t - last; n++; }
	last = t;
	if (t - since >= 500 && n) { shown = acc / n; acc = 0; n = 0; since = t; }
	if (shown < 0) return;
	snprintf(text, sizeof text, "%.0f FPS", shown > 0 ? 1000.0 / shown : 0.0);
	nch = (int)strlen(text);
	scale = (int)swap_ext.height / 360; if (scale < 1) scale = 1;
	bw = (nch * 6 + 3) * scale; bh = 11 * scale;
	x0 = (int)swap_ext.width - bw - 4 * scale; y0 = 4 * scale;
	if (x0 < 0 || bh + y0 > (int)swap_ext.height) return;
	ro = ring_alloc((VkDeviceSize)bw * bh * 4, 16, (void **)&px);
	for (y = 0; y < bh; y++) for (x = 0; x < bw; x++) px[y * bw + x] = 0xff181818u; /* (gray: the same in BGRA and RGBA) */
	for (ch = text, x = 2 * scale; *ch; ch++, x += 6 * scale) {
		int g = *ch >= '0' && *ch <= '9' ? *ch - '0' : *ch == 'F' ? 10 : *ch == 'P' ? 11 : *ch == 'S' ? 12 : -1, r, c, i, j;
		if (g < 0) continue;
		for (r = 0; r < 7; r++) for (c = 0; c < 5; c++) {
			if (!(ft_font[g][r] >> (4 - c) & 1)) continue;
			for (j = 0; j < scale; j++) for (i = 0; i < scale; i++) px[((2 * scale) + r * scale + j) * bw + x + c * scale + i] = 0xffffffffu;
		}
	}
	memset(&bc, 0, sizeof bc);
	bc.bufferOffset = ro;
	bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bc.imageSubresource.layerCount = 1;
	bc.imageOffset.x = x0; bc.imageOffset.y = y0;
	bc.imageExtent.width = (u32)bw; bc.imageExtent.height = (u32)bh; bc.imageExtent.depth = 1;
	barrier(); /* (after the blit into the same image) */
	vkCmdCopyBufferToImage(cb, F->ring, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
}
#endif

/* the back buffer into the swapchain image: a copy when it is the same size and format (a blit -- the scaling path --
 * is slow on an AMD GPU: ~0.6 ms more for a 1280x720 one, as the game's own showed) */
static void present_copy(Res *bb, const VkImageBlit *b, VkImage dst);

#if defined(XRC_LR) && defined(_WIN32)
/* ============================================================================================ the settings overlay
 * (F10, xrc_menu.c): while it is open the frame is put together in an image of the swap chain's size -- the back buffer
 * scaled in, the whole picture dimmed, the menu's panel (drawn on the CPU by xrc_menu.c, premultiplied BGRA, uploaded
 * only when it changed) blended over -- and that image copied into the swap chain image. Headless (no swap chain) the
 * same is made at the back buffer's size, for XRC_SHOT. */
int xrc_menu_is_open(void);
const unsigned *xrc_menu_frame(int sw, int sh, int *x, int *y, int *w, int *h, int *dirty);
static Res *ovl_comp, *ovl_tex;
static struct { Res *r; unsigned long long frame; } ovl_dead[4]; /* (freed some frames later: the GPU may still use them) */
static void ovl_drop(Res *r) {
	int k;
	if (!r) return;
	for (k = 0; k < 4; k++) if (!ovl_dead[k].r) { ovl_dead[k].r = r; ovl_dead[k].frame = frame_no; return; }
	/* (no slot: leaked -- only when the size changes four times within a few frames) */
}
static Res *ovl_image(u32 w, u32 h, VkFormat f, int att) {
	VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	VkMemoryRequirements rq;
	Res *t = (Res *)calloc(1, sizeof *t);
	t->fmt = f; t->sw = SW(I_, I_, I_, I_); t->w = w; t->h = h; t->d = 1; t->levels = 1; t->layers = 1; t->samples = 1;
	t->can_att = (u8)(att != 0); t->blend_ok = 1; t->bpp_block = 4;
	ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f;
	ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
	ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (att ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(vd, &ci, 0, &t->img) != VK_SUCCESS) { free(t); return 0; }
	vkGetImageMemoryRequirements(vd, t->img, &rq);
	t->mem = mem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	if (t->mem.block < 0) { vkDestroyImage(vd, t->img, 0); free(t); return 0; }
	VKC(vkBindImageMemory(vd, t->img, blocks[t->mem.block].mem, t->mem.off));
	t->view = make_view(t, VK_IMAGE_VIEW_TYPE_2D, t->sw, 0, 1, 0, 1, 0);
	t->att = (VkImageView *)calloc(1, sizeof(VkImageView));
	t->lay = VK_IMAGE_LAYOUT_UNDEFINED;
	return t;
}
/* the overlaid frame: bb (already in the transfer source layout) scaled by b into an image W x H of format f, the menu
 * over it. Returns that image (in the transfer source layout), or 0: the menu is closed (or it could not be drawn). */
static Res *ovl_compose(Res *bb, const VkImageBlit *b, u32 W, u32 H, VkFormat f) {
	const unsigned *px;
	int px_x, px_y, pw, ph, dirty, k;
	VkImageBlit q;
	for (k = 0; k < 4; k++)
		if (ovl_dead[k].r && frame_no > ovl_dead[k].frame + NFRAMES + 4) { res_destroy(ovl_dead[k].r); ovl_dead[k].r = 0; }
	if (!xrc_menu_is_open() || !blit_setup()) return 0;
	if (!(px = xrc_menu_frame((int)W, (int)H, &px_x, &px_y, &pw, &ph, &dirty)) || pw <= 0 || ph <= 0) return 0;
	if (ovl_comp && (ovl_comp->w != W || ovl_comp->h != H || ovl_comp->fmt != f)) { ovl_drop(ovl_comp); ovl_comp = 0; }
	if (!ovl_comp && !(ovl_comp = ovl_image(W, H, f, 1))) return 0;
	if (ovl_tex && (ovl_tex->w != (u32)pw || ovl_tex->h != (u32)ph + 2)) { ovl_drop(ovl_tex); ovl_tex = 0; }
	if (!ovl_tex) { if (!(ovl_tex = ovl_image((u32)pw, (u32)ph + 2, VK_FORMAT_B8G8R8A8_UNORM, 0))) return 0; dirty = 1; }
	if (dirty) { /* (the panel, and below it two rows of the dimming colour) */
		VkBufferImageCopy c;
		void *p;
		VkDeviceSize off = ring_alloc((VkDeviceSize)pw * (ph + 2) * 4, 16, &p);
		memcpy(p, px, (size_t)pw * (ph + 2) * 4);
		img_use(ovl_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		memset(&c, 0, sizeof c);
		c.bufferOffset = off;
		c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
		c.imageExtent.width = (u32)pw; c.imageExtent.height = (u32)ph + 2; c.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(cb, F->ring, ovl_tex->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		ovl_tex->pw = 1;
	}
	/* the game's picture, letterboxed as on the screen */
	bq_add(ovl_comp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); bq_go();
	if (b->dstOffsets[1].x - b->dstOffsets[0].x != (int)W || b->dstOffsets[1].y - b->dstOffsets[0].y != (int)H) {
		VkClearColorValue black;
		VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		memset(&black, 0, sizeof black);
		vkCmdClearColorImage(cb, ovl_comp->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &sr);
	}
	present_copy(bb, b, ovl_comp->img);
	ovl_comp->pw = 1;
	/* the dimming (the strip under the panel, stretched over everything), then the panel */
	memset(&q, 0, sizeof q);
	q.srcSubresource.aspectMask = q.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	q.srcSubresource.layerCount = q.dstSubresource.layerCount = 1;
	q.srcOffsets[0].x = 1; q.srcOffsets[0].y = ph; q.srcOffsets[1].x = pw - 1; q.srcOffsets[1].y = ph + 2; q.srcOffsets[1].z = 1;
	q.dstOffsets[1].x = (int)W; q.dstOffsets[1].y = (int)H; q.dstOffsets[1].z = 1;
	if (!blit_draw(ovl_tex, 0, 0, ovl_comp, 0, 0, &q, 0, 2, 0)) return 0;
	q.srcOffsets[0].x = 0; q.srcOffsets[0].y = 0; q.srcOffsets[1].x = pw; q.srcOffsets[1].y = ph;
	q.dstOffsets[0].x = px_x; q.dstOffsets[0].y = px_y; q.dstOffsets[1].x = px_x + pw; q.dstOffsets[1].y = px_y + ph;
	if (!blit_draw(ovl_tex, 0, 0, ovl_comp, 0, 0, &q, 0, 2, 0)) return 0;
	bq_add(ovl_comp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); IMG_R_XFER(ovl_comp); bq_go();
	return ovl_comp;
}
/* the overlaid frame into the swap chain image (in the transfer destination layout): 1, or 0 (draw it as usual) */
static int ovl_present(Res *bb, const VkImageBlit *b, VkImage dst) {
	VkFormat f = swap_fmt == VK_FORMAT_B8G8R8A8_UNORM || swap_fmt == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM :
	             swap_fmt == VK_FORMAT_R8G8B8A8_UNORM || swap_fmt == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED;
	Res *c;
	VkImageCopy ic;
	if (f == VK_FORMAT_UNDEFINED || !(c = ovl_compose(bb, b, swap_ext.width, swap_ext.height, f))) return 0;
	memset(&ic, 0, sizeof ic);
	ic.srcSubresource.aspectMask = ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ic.srcSubresource.layerCount = ic.dstSubresource.layerCount = 1;
	ic.extent.width = swap_ext.width; ic.extent.height = swap_ext.height; ic.extent.depth = 1;
	vkCmdCopyImage(cb, c->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
	return 1;
}
/* headless: the overlaid back buffer (its size) for XRC_SHOT, or bb itself while the menu is closed */
static Res *ovl_headless(Res *bb) {
	VkImageBlit b;
	Res *c;
	if (!bb || bb->bpp_block != 4 || bb->samples > 1 || !xrc_menu_is_open()) return bb;
	if (bb->fmt != VK_FORMAT_B8G8R8A8_UNORM && bb->fmt != VK_FORMAT_R8G8B8A8_UNORM) return bb;
	memset(&b, 0, sizeof b);
	b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
	b.srcOffsets[1].x = b.dstOffsets[1].x = (int)bb->w; b.srcOffsets[1].y = b.dstOffsets[1].y = (int)bb->h; b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
	end_render();
	clears_flush(bb);
	bq_add(bb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); IMG_R_XFER(bb); bq_go();
	c = ovl_compose(bb, &b, bb->w, bb->h, bb->fmt);
	return c ? c : bb;
}
#endif

static void present_copy(Res *bb, const VkImageBlit *b, VkImage dst) {
	if (post_blit(bb, b, dst, post_fmt ? post_fmt : swap_fmt, 1)) return; /* (vk_post.c: rescale / sharpen / deband, when on) */
	if (bb->fmt == swap_fmt && b->dstOffsets[1].x - b->dstOffsets[0].x == (int)bb->w && b->dstOffsets[1].y - b->dstOffsets[0].y == (int)bb->h) {
		VkImageCopy ic;
		memset(&ic, 0, sizeof ic);
		ic.srcSubresource = b->srcSubresource; ic.dstSubresource = b->dstSubresource;
		ic.dstOffset = b->dstOffsets[0];
		ic.extent.width = bb->w; ic.extent.height = bb->h; ic.extent.depth = 1;
		vkCmdCopyImage(cb, bb->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
	} else
		vkCmdBlitImage(cb, bb->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, b, VK_FILTER_LINEAR);
}
#if defined(XRC_LR) && defined(_WIN32)
/* headless (XRC_SHOT): the back buffer as present_copy would put it on a screen of XRC_POST_OUT=WxH (default: its own
 * size), so shots show vk_post.c's passes -- or bb itself when they are off and no size is asked for */
static Res *post_out;
static Res *post_headless(Res *bb) {
	static int ow = -1, oh;
	VkImageBlit b;
	u32 W, H;
	if (ow < 0) { const char *e = getenv("XRC_POST_OUT"); ow = oh = 0; if (e && sscanf(e, "%dx%d", &ow, &oh) != 2) ow = oh = 0; }
	if (!bb || bb->bpp_block != 4 || bb->samples > 1 || (bb->fmt != VK_FORMAT_B8G8R8A8_UNORM && bb->fmt != VK_FORMAT_R8G8B8A8_UNORM)) return bb;
	W = ow > 0 ? (u32)ow : bb->w; H = oh > 0 ? (u32)oh : bb->h;
	if (ow <= 0 && ab_mode != 6 && !xrc_post_active(1, bb->w, bb->h, W, H)) return bb;
	if (post_out && (post_out->w != W || post_out->h != H || post_out->fmt != bb->fmt)) { res_defer(post_out); post_out = 0; }
	if (!post_out && !(post_out = ovl_image(W, H, bb->fmt, 1))) return bb;
	memset(&b, 0, sizeof b);
	b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
	b.srcOffsets[1].x = (int)bb->w; b.srcOffsets[1].y = (int)bb->h; b.srcOffsets[1].z = 1;
	{ /* (fitted, its aspect kept -- as be_present) */
		double sx = (double)W / bb->w, sy = (double)H / bb->h, s = sx < sy ? sx : sy;
		int w = (int)(bb->w * s), h = (int)(bb->h * s), x = ((int)W - w) / 2, y = ((int)H - h) / 2;
		b.dstOffsets[0].x = x; b.dstOffsets[0].y = y; b.dstOffsets[1].x = x + w; b.dstOffsets[1].y = y + h; b.dstOffsets[1].z = 1;
	}
	end_render();
	clears_flush(bb);
	bq_add(bb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); IMG_R_XFER(bb);
	bq_add(post_out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); IMG_W_XFER(post_out); bq_go();
	if (b.dstOffsets[1].x - b.dstOffsets[0].x != (int)W || b.dstOffsets[1].y - b.dstOffsets[0].y != (int)H) {
		VkClearColorValue black;
		VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		memset(&black, 0, sizeof black);
		vkCmdClearColorImage(cb, post_out->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &sr);
	}
	post_fmt = bb->fmt;
	present_copy(bb, &b, post_out->img);
	post_fmt = VK_FORMAT_UNDEFINED;
	post_out->pw = 1;
	return post_out;
}
#endif
static void be_present(void) { dyn_gen++;
	Obj *bbo = obj_of(dev.backbuf);
	u32 f, l, img = 0;
	Res *bb = bbo ? surface_image(bbo, &f, &l) : 0;
	VkResult r;
#if defined(XRC_LR) && defined(_WIN32)
	md_frame_end(); /* (F7: the mesh dump) */
#endif
	end_render();
#ifdef XRC_LR
	{ void xrc_settings_poll(double secs); xrc_settings_poll((double)xrc_now_us() / 1e6); }
#endif
	{ /* a held-back copy into the back buffer: with DISCARD (its contents undefined after Present) the copy's source is
	   * shown instead and the copy dropped; else it is made (see be_stretch_rect) */
		int k;
		for (k = 0; k < npend; k++)
			if (pend[k].d == bb) {
				PCopy c = pend[k];
				pend[k] = pend[--npend];
				if (dev.pp[6] == 1) { clears_flush(c.s); bb = c.s; } else stretch_do(c.a, 0);
				break;
			}
	}
	shot_src = hud_next; /* (XRC_SHOT_SRC: maybe_shot also saves the surface copied into the back buffer) */
	hud_rt = hud_next; hud_next = 0; /* (FXAA: what next frame's HUD is drawn into -- see fxaa_copy) */
	clears_flush(bb);
#if defined(XRC_LR) && defined(_WIN32)
	maybe_shot(swapchain ? bb : post_headless(ovl_headless(bb))); /* (headless: the settings overlay drawn in for the shot; vk_post.c's passes) */
#else
	maybe_shot(bb);
#endif
	maybe_rec(bb);
	pump_events();
	if (bb) { xrc_view_w = (int)bb->w; xrc_view_h = (int)bb->h; } /* (the input side's mapping: xrc_view_map) */
#ifdef _WIN32
	xrc_view_window = window;
#endif
	if (surface && swapchain && __atomic_load_n(&swap_ok, __ATOMIC_ACQUIRE)) {
		/* the window resized (it is resizable: ref_user32.c / SDL): a swap chain of the new size -- not every driver
		 * reports the old one out of date. The game's picture stays its size, scaled to fit (below) */
		int w, h;
		plat_size(&w, &h);
		if (w > 0 && h > 0 && (w != swap_plat_w || h != swap_plat_h)) __atomic_store_n(&swap_ok, 0, __ATOMIC_RELEASE);
	}
	if (surface && !__atomic_load_n(&swap_ok, __ATOMIC_ACQUIRE)) { cs_drain(); vkDeviceWaitIdle(vd); make_swapchain(); }
	if (!swapchain || !bb || !swap_ok) {
		submit_frame(0, VK_NULL_HANDLE);
	} else if (cs_on) { /* (the render thread acquires and presents: the swap chain image is a placeholder here) */
		static unsigned long long prev;
		VkImage si = CS_SWAP_IMG;
		VkImageBlit b;
		end_render();
		cs_acquire(swapchain, F->acquired);
		image_barrier(si, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		memset(&b, 0, sizeof b);
		b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
		b.srcOffsets[1].x = (int)bb->w; b.srcOffsets[1].y = (int)bb->h; b.srcOffsets[1].z = 1;
		{
			double sx = (double)swap_ext.width / bb->w, sy = (double)swap_ext.height / bb->h, s = sx < sy ? sx : sy;
			int w = (int)(bb->w * s), h = (int)(bb->h * s), x = ((int)swap_ext.width - w) / 2, y = ((int)swap_ext.height - h) / 2;
			VkClearColorValue black;
			VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			memset(&black, 0, sizeof black);
			if (w != (int)swap_ext.width || h != (int)swap_ext.height) vkCmdClearColorImage(cb, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &sr);
			b.dstOffsets[0].x = x; b.dstOffsets[0].y = y;
			b.dstOffsets[1].x = x + w; b.dstOffsets[1].y = y + h; b.dstOffsets[1].z = 1;
			xrc_size_frame((int)bb->w, (int)bb->h, w, h);
		}
		bq_add(bb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); IMG_R_XFER(bb); bq_go();
#if defined(XRC_LR) && defined(_WIN32)
		if (!ovl_present(bb, &b, si))
#endif
		present_copy(bb, &b, si);
#ifndef XRC_NO_TOOLS
		draw_frame_time(si);
#endif
		image_barrier(si, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		submit_frame(1, CS_SWAP_DONE);
		cs_present(queue, swapchain);
		/* at most a frame ahead of the render thread: the previous frame's submit must be done */
		{
			double t0 = now_ms();
			cs_wait(prev);
			present_ms += now_ms() - t0;
			HC(present, now_ms() - t0);
		}
		prev = F->serial;
	} else {
		VkImageBlit b;
		end_render();
		{
			double t0 = now_ms();
			extern unsigned long long xrc_acquire_wait_us;
			xrc_gil_release();
			r = vkAcquireNextImageKHR(vd, swapchain, ~0ull, F->acquired, VK_NULL_HANDLE, &img);
			xrc_acquire_wait_us += (unsigned long long)((now_ms() - t0) * 1000.0); /* (the frame limiter: vblank lock) */
			xrc_gil_acquire();
			present_ms += now_ms() - t0;
			HC(present, now_ms() - t0);
		}
		if (r == VK_ERROR_OUT_OF_DATE_KHR) {
			submit_frame(0, VK_NULL_HANDLE);
			vkDeviceWaitIdle(vd);
			make_swapchain();
			fi = (fi + 1) % NFRAMES;
			frame_no++;
			begin_frame();
			return;
		}
		image_barrier(swap_img[img], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		memset(&b, 0, sizeof b);
		b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
		b.srcOffsets[1].x = (int)bb->w; b.srcOffsets[1].y = (int)bb->h; b.srcOffsets[1].z = 1;
		{ /* the back buffer scaled to fit, its aspect kept */
			double sx = (double)swap_ext.width / bb->w, sy = (double)swap_ext.height / bb->h, s = sx < sy ? sx : sy;
			int w = (int)(bb->w * s), h = (int)(bb->h * s), x = ((int)swap_ext.width - w) / 2, y = ((int)swap_ext.height - h) / 2;
			VkClearColorValue black;
			VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			memset(&black, 0, sizeof black);
			if (w != (int)swap_ext.width || h != (int)swap_ext.height) vkCmdClearColorImage(cb, swap_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &sr);
			b.dstOffsets[0].x = x; b.dstOffsets[0].y = y;
			b.dstOffsets[1].x = x + w; b.dstOffsets[1].y = y + h; b.dstOffsets[1].z = 1;
			xrc_size_frame((int)bb->w, (int)bb->h, w, h);
		}
		bq_add(bb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); IMG_R_XFER(bb); bq_go();
#if defined(XRC_LR) && defined(_WIN32)
		if (!ovl_present(bb, &b, swap_img[img]))
#endif
		present_copy(bb, &b, swap_img[img]);
#ifndef XRC_NO_TOOLS
		draw_frame_time(swap_img[img]);
#endif
		image_barrier(swap_img[img], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		submit_frame(1, swap_done[img]);
		{
			VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
			pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &swap_done[img];
			VkPresentIdKHR pid = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
			uint64_t id = pw_last + 1;
			pi.swapchainCount = 1; pi.pSwapchains = &swapchain; pi.pImageIndices = &img;
			if (vkWaitForPresentKHR_) { pid.swapchainCount = 1; pid.pPresentIds = &id; pi.pNext = &pid; }
			double t0 = now_ms();
			xrc_gil_release();
			r = vkQueuePresentKHR(queue, &pi);
			if (vkWaitForPresentKHR_ && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
				pw_last = id;
				/* (the frame before on the screen; a time limit: a window the display does not show -- covered,
				 * another desktop -- may never complete one) */
				if (id - 1 >= pw_first) vkWaitForPresentKHR_(vd, swapchain, id - 1, 40000000ull);
			}
			xrc_gil_acquire();
			present_ms += now_ms() - t0;
			HC(present, now_ms() - t0);
			if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(vd); make_swapchain(); }
		}
	}
#ifndef XRC_NO_TOOLS
	if (hitch_ms) {
		extern unsigned long long xrc_frame_wait_us;
		unsigned long long xrc_main_gil_wait_us(void);
		static unsigned long long lw0, gw0;
		static double cpu0;
		double t = now_ms(), cpu = h_thread_cpu_ms();
		hcur.cpu = cpu0 ? cpu - cpu0 : 0;
		cpu0 = cpu;
		{ unsigned long long xrc_now_us(void); hcur.t_end = xrc_now_us(); }
		unsigned long long lw = xrc_frame_wait_us, gw = xrc_main_gil_wait_us();
		hcur.limit = (double)(lw - lw0) / 1000.0; hcur.gil = (double)(gw - gw0) / 1000.0;
		lw0 = lw; gw0 = gw;
		hcur.interval = h_last_present ? t - h_last_present : 0;
		{ /* (XRC_HITCH_THREADS=1: for a frame over XRC_HITCH_MS, each thread's CPU / GIL held / GIL waited ms in it and its worst wake-up) */
			static double ht = -1;
			if (ht < 0) ht = xrc_tool_env("XRC_HITCH_THREADS") ? atof(xrc_tool_env("XRC_HITCH_THREADS")) : 0;
			if (ht > 0) { /* (XRC_HITCH_THREADS=ms: frames over that many ms) */
				int show = h_last_present && hcur.interval > ht;
				if (show) fprintf(stderr, "xrc: hitch threads: frame %llu, %.1f ms (cpu / GIL held / GIL waited, ms in the frame)\n", frame_no, hcur.interval);
				xrc_thread_report(show ? stderr : 0, 1);
				xrc_thread_wake_report(show ? stderr : 0);
			}
		}
		{ /* (XRC_HITCH_FROM=secs: the distribution from then on -- past the boot and the title) */
			static double from = -1, t_first;
			if (from < 0) { from = xrc_tool_env("XRC_HITCH_FROM") ? atof(xrc_tool_env("XRC_HITCH_FROM")) * 1000.0 : 0; t_first = t; }
			if (h_last_present && t - t_first >= from) h_hist_add(hcur.interval);
			/* (XRC_HITCH_DUMP=secs: the report then, for a test run that is killed rather than closed) */
			static double dump_at = -1;
			if (dump_at < 0) dump_at = xrc_tool_env("XRC_HITCH_DUMP") ? atof(xrc_tool_env("XRC_HITCH_DUMP")) * 1000.0 : 0;
			if (dump_at > 0 && t - t_first >= dump_at) { dump_at = 0; hitch_flush(); memset(h_hist, 0, sizeof h_hist); }
		}
		hcur.frame = frame_no;
		hcur.valid = h_last_present != 0;
		h_last_present = t;
		hitch[fi] = hcur;
		memset(&hcur, 0, sizeof hcur);
	}
#endif
	fi = (fi + 1) % NFRAMES;
	frame_no++;
	begin_frame();
	pc_maybe_save();
	pw_pump();
	if (xrc_pd_on < 0) xrc_pd_on = getenv("XRC_PROF_DRAW") && atoi(getenv("XRC_PROF_DRAW")) ? 1 : 0;
	if (xrc_pd_on > 0 && frame_no % 300 == 0) pd_report();
	if (vlog && frame_no % 600 == 0) {
		fprintf(stderr, "xrc: vulkan: %u pipelines (%u made by the builder), %u samplers, %llu draws skipped (%llu waiting for their pipeline; %llu drawn after a wait for it); per frame: %.2f ms GPU, %.2f ms waiting for the GPU, %.2f ms in acquire+present, %.2f ms in draws, %.0f KB uploaded, %.1f renderings (%.2f Mpx), %.1f layout changes, %.2f renderings split for one, %.1f barriers\n",
		        pcount, n_pipe_async, nsamp, nskipped, n_pipe_waits, n_pipe_waited, gpu_ms / 600, wait_ms / 600, present_ms / 600, draw_ms / 600, up_bytes / 600 / 1024, n_renders / 600, n_px / 600 / 1e6,
		        n_relayout / 600, n_split / 600, n_bar / 600);
		pace_bp += present_ms; pace_bw += wait_ms;
		wait_ms = draw_ms = up_bytes = n_renders = n_px = gpu_ms = present_ms = n_relayout = n_split = n_bar = 0;
	}
}

static void be_locked(Obj *o, u32 face, u32 level, u32 off, u32 size) {
	Res *r = res_of(o);
	if (!r || o->kind == K_VS || o->kind == K_PS) return;
	if (r->is_buf) {
		u32 hi = size ? off + size : r->size;
		if (r->lock_frame != frame_no) { r->lock_frame = frame_no; if (++r->lock_frames >= 8 && !r->hot) { r->hot = 1; LOG("xrc: vulkan: buffer %x (%u bytes) is streamed from now on\n", o->guest, o->size); } }
		if (r->dhi <= r->dlo) { r->dlo = off; r->dhi = hi; }
		else { if (off < r->dlo) r->dlo = off; if (hi > r->dhi) r->dhi = hi; }
		if (!size) r->dlo = 0;
		return;
	}
	if (face < 6 && level < 32) r->dirty[face] |= 1u << level;
}
static void be_release(Obj *o) { dyn_gen++;
	void *h = o->host;
	u32 k;
	for (k = 0; k < 4; k++) if (cur_rt[k] == o->guest) { end_render(); cur_rt[k] = 0; }
	if (cur_ds == o->guest) { end_render(); cur_ds = 0; }
	o->host = 0;
	if (!h) return;
	if (o->kind == K_VS || o->kind == K_PS) { Shader *s = (Shader *)h; pw_unreg(s); shader_module_free(s->mod); if (s->mod_noat) shader_module_free(s->mod_noat); if (s->mod_nz) shader_module_free(s->mod_nz); if (s->mod_noat_nz) shader_module_free(s->mod_noat_nz); free(s); return; }
	if (o->kind == K_DECL || o->kind == K_QUERY || o->kind == K_SB || o->kind == K_DEV) return;
	clears_forget((Res *)h);
	{ /* still in use by the frames in flight: destroyed when this frame is next begun (all the frames before it are then done) */
		Frame *fr = F;
		if (fr->ndead == fr->capdead) { fr->capdead = fr->capdead ? fr->capdead * 2 : 64; fr->dead = (Res **)realloc(fr->dead, fr->capdead * sizeof(Res *)); }
		fr->dead[fr->ndead++] = (Res *)h;
	}
}
static void be_reset(void) { dyn_gen++;
	end_render();
	memset(cur_rt, 0, sizeof cur_rt);
	cur_ds = 0;
}

/* Full-size copies (StretchRect of a whole surface into another of its size and format, the game's way of passing
 * whole frames around) are held back (pend[]), and made only when something needs them -- a draw sampling the copy or
 * rendering into either surface, a clear of either, a lock, ... -- or not at all:
 *   - a copy whose destination is overwritten by another full copy before anything read it is dropped;
 *   - a copy out of a held-back copy's destination is made from that copy's source instead;
 *   - one into the back buffer is not made: with the swap effect DISCARD (the game's: the back buffer's contents
 *     undefined after Present) Present shows its source (be_present).
 * Per frame the game copies its finished image into a surface (then made through FXAA), draws the HUD over it, and
 * copies that to the back buffer, and copies it once more into one the next frame overwrites first: two full-screen
 * copies less. XRC_VK_COPY_DEFER=0 (or the older XRC_VK_BB_DEFER=0): none held back. */
static void pend_make(int k) { PCopy c = pend[k]; pend[k] = pend[--npend]; stretch_do(c.a, 0); }
static void pend_flush(void) { while (npend) pend_make(npend - 1); }
static int is_image(Obj *o) { return o->kind == K_TEX || o->kind == K_CUBE || o->kind == K_VTEX || o->kind == K_SURF || o->kind == K_VOL; }
/* a whole-surface copy: its source and destination images, else 0 */
static int full_copy(const u32 *a, Res **rs, Res **rd) {
	Obj *so = obj_of(a[0]), *dso = obj_of(a[2]);
	u32 sf, sl, df, dl;
	VkOffset3D o1[2], o2[2];
	Res *s, *d;
	if (!so || !dso || !(s = surface_image(so, &sf, &sl)) || !(d = surface_image(dso, &df, &dl))) return 0;
	if (s == d || s->depth || d->depth || s->samples > 1 || d->samples > 1 || sf || sl || df || dl || s->fmt != d->fmt || s->w != d->w || s->h != d->h ||
	    s->layers != 1 || d->layers != 1 || s->levels != 1 || d->levels != 1)
		return 0;
	rect_of(a[1], s->w, s->h, o1); rect_of(a[3], d->w, d->h, o2);
	if (o1[0].x || o1[0].y || o1[1].x != (int)s->w || o1[1].y != (int)s->h || memcmp(o1, o2, sizeof o1)) return 0;
	*rs = s; *rd = d;
	return 1;
}
static void be_stretch_rect(const u32 *a) { dyn_gen++;
	u32 a2[5];
	Res *s = 0, *d = 0;
	int full, k;
	if (defer_on < 0)
		defer_on = !(getenv("XRC_VK_COPY_DEFER") && !atoi(getenv("XRC_VK_COPY_DEFER"))) && !(getenv("XRC_VK_BB_DEFER") && !atoi(getenv("XRC_VK_BB_DEFER")));
	if (a[2] == dev.backbuf) hud_next = a[0]; /* (FXAA: see fxaa_copy) */
#ifndef XRC_NO_TOOLS
	if (ppool && pp_rec[fi] && !hitch_ms) { /* (XRC_VK_PASSES: the frame's copies among its renderings) */
		Obj *so = obj_of(a[0]), *dso = obj_of(a[2]);
		fprintf(stderr, "xrc: vulkan: (the frame of the renderings below) StretchRect %08x %ux%u -> %08x %ux%u%s\n", a[0], so ? so->w : 0, so ? so->h : 0,
		        a[2], dso ? dso->w : 0, dso ? dso->h : 0, a[2] == dev.backbuf ? " (the back buffer)" : "");
	}
#endif
	memcpy(a2, a, sizeof a2);
	full = defer_on && full_copy(a, &s, &d);
	for (k = npend - 1; k >= 0; k--) {
		Res *ps = pend[k].s, *pd = pend[k].d;
		if (full && d == pd && s != pd) { pend[k] = pend[--npend]; continue; } /* (overwritten before any use: dropped) */
		if (full && s == pd && d != ps) { a2[0] = pend[k].a[0]; a2[1] = pend[k].a[1]; s = ps; continue; } /* (from its source) */
		if (s == pd || d == ps || d == pd || (!full && 1)) {
			/* (any other StretchRect: the held-back copies it may touch made first -- not knowing its images, all) */
			if (full) pend_make(k); else { pend_flush(); break; }
		}
	}
	if (npend == NPEND) pend_make(0);
	stretch_do(a2, full);
}
/* a draw or clear: the held-back copies made that it reads (sampling a destination) or writes (into a source or a
 * destination) */
static int pend_bound(Res *r, int tex) {
	u32 k, f, l;
	Obj *o;
	for (k = 0; k < 4; k++) if ((o = obj_of(dev.rt[k])) && surface_image(o, &f, &l) == r) return 1;
	if ((o = obj_of(dev.ds)) && surface_image(o, &f, &l) == r) return 1;
	if (tex) for (k = 0; k < 20; k++) if ((o = obj_of(dev.tex[k])) && o->host && res_of(o) == r) return 1;
	return 0;
}
static void pend_use(int tex) {
	int k;
	for (k = npend - 1; k >= 0; k--) if (pend_bound(pend[k].d, tex) || pend_bound(pend[k].s, 0)) pend_make(k);
}
static void w_draw(int method, const u32 *a) { if (npend) pend_use(1); be_draw(method, a); }
static void w_clear(const u32 *a) { if (npend) pend_use(0); be_clear(a); }
static void w_locked(Obj *o, u32 face, u32 level, u32 off, u32 size) { dyn_gen++;
	if (is_image(o)) {
		u32 f, l;
		Res *r = o->kind == K_SURF ? surface_image(o, &f, &l) : res_of(o);
		if (r && r->depth) ZREAD(r);
		pend_flush();
	}
	be_locked(o, face, level, off, size);
}
static void w_release(Obj *o) { if (is_image(o)) pend_flush(); be_release(o); }
static void w_color_fill(const u32 *a) { pend_flush(); be_color_fill(a); }
static u32 w_rt_data(const u32 *a) { pend_flush(); return be_rt_data(a); }
static void w_reset(void) { pend_flush(); be_reset(); }
static const D3Backend vk_backend = {w_draw, w_clear, be_present, w_locked, w_release, be_stretch_rect, w_color_fill, w_rt_data, w_reset};

/* ============================================================================================ start */
static int has_ext(const VkExtensionProperties *e, u32 n, const char *name) {
	u32 k;
	for (k = 0; k < n; k++) if (!strcmp(e[k].extensionName, name)) return 1;
	return 0;
}
const D3Backend *d3_be_init(void) {
	VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	const char *iext[16];
	u32 niext = 0, n, k;
	VkPhysicalDevice pd[16];
	int headless = getenv("XRC_VK_HEADLESS") != 0;
	static int inited;
	if (inited && !vd) return 0; /* (the first device found no Vulkan: none for this one either -- not a backend without a device) */
	if (inited) { /* another device (the game makes a 1x1 one first): the same Vulkan device, the window at its size */
		if (window && surface) {
			void *w = plat_device_window();
			plat_resize((int)dev.pp[0], (int)dev.pp[1]);
			pump_events();
			submit_frame(0, VK_NULL_HANDLE);
			vkDeviceWaitIdle(vd);
			F->submitted = 0;
			if (w && w != window) { /* another window (Windows: the game's first device is on a temporary one) */
				VkBool32 pr = 0;
				if (swapchain) vkDestroySwapchainKHR(vd, swapchain, 0);
				swapchain = VK_NULL_HANDLE;
				vkDestroySurfaceKHR(inst, surface, 0);
				surface = VK_NULL_HANDLE;
				window = w;
				if (!plat_surface(inst, &surface)) { fprintf(stderr, "xrc: vulkan: no surface on the new window\n"); surface = VK_NULL_HANDLE; }
				else vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, surface, &pr);
				LOG("xrc: vulkan: drawing in another window\n");
			}
			if (surface) make_swapchain();
			begin_frame();
		}
		memset(cur_rt, 0, sizeof cur_rt);
		cur_ds = 0;
		return &vk_backend;
	}
	inited = 1;
	vlog = xrc_tool_env("XRC_VK_LOG") ? atoi(xrc_tool_env("XRC_VK_LOG")) : 0;
	vk_lib = 0;
#ifdef _WIN32
	vk_lib = dlopen("vulkan-1.dll", 0);
#endif
	if (!vk_lib) vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!vk_lib) vk_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (!vk_lib) vk_lib = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
	if (!vk_lib) { fprintf(stderr, "xrc: vulkan: no Vulkan loader (vulkan-1.dll / libvulkan.so.1): nothing is drawn\n"); return 0; }
	vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)dlsym(vk_lib, "vkGetInstanceProcAddr");
	if (!vkGetInstanceProcAddr_) return 0;
#define LG(f) f = (PFN_##f)vkGetInstanceProcAddr_(0, #f);
	VK_FNS(LG)
#undef LG
	/* the window */
	if (!headless) plat_window(iext, &niext);
	ai.pApplicationName = "ff13";
	ai.pEngineName = "xrc";
	ai.apiVersion = VK_API_VERSION_1_3;
	ii.pApplicationInfo = &ai;
	ii.enabledExtensionCount = niext;
	ii.ppEnabledExtensionNames = iext;
	if (xrc_tool_env("XRC_VK_VALIDATION")) {
		static const char *layer = "VK_LAYER_KHRONOS_validation";
		ii.enabledLayerCount = 1;
		ii.ppEnabledLayerNames = &layer;
	}
	if (vkCreateInstance(&ii, 0, &inst) < 0) { fprintf(stderr, "xrc: vulkan: no Vulkan 1.3 instance: nothing is drawn\n"); return 0; }
#define LI(f) f = (PFN_##f)vkGetInstanceProcAddr_(inst, #f);
	VKI_FNS(LI)
#undef LI
	if (window && !plat_surface(inst, &surface)) { fprintf(stderr, "xrc: vulkan: no window surface: drawing without one\n"); surface = VK_NULL_HANDLE; }
	n = 16;
	VKC(vkEnumeratePhysicalDevices(inst, &n, pd));
	if (!n) { fprintf(stderr, "xrc: vulkan: no GPU: nothing is drawn\n"); return 0; }
	{ /* the device: XRC_VK_DEVICE, else the first discrete GPU, else the first */
		int pick = -1;
		if (getenv("XRC_VK_DEVICE")) pick = atoi(getenv("XRC_VK_DEVICE"));
		for (k = 0; k < n && pick < 0; k++) { vkGetPhysicalDeviceProperties(pd[k], &props); if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pick = (int)k; }
		if (pick < 0 || pick >= (int)n) pick = 0;
		phys = pd[pick];
	}
	vkGetPhysicalDeviceProperties(phys, &props);
	vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
	if (VK_API_VERSION_MINOR(props.apiVersion) < 3 && VK_API_VERSION_MAJOR(props.apiVersion) == 1) {
		fprintf(stderr, "xrc: vulkan: %s has Vulkan %u.%u (1.3 needed): nothing is drawn\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
		return 0;
	}
	{ /* the queue: graphics (and present) */
		VkQueueFamilyProperties qf[16];
		u32 nq = 16;
		vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
		qfam = 0xffffffffu;
		for (k = 0; k < nq; k++) {
			VkBool32 pr = 1;
			if (!(qf[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
			if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(phys, k, surface, &pr);
			if (pr) { qfam = k; break; }
		}
		if (qfam == 0xffffffffu) { fprintf(stderr, "xrc: vulkan: no graphics queue\n"); return 0; }
	}
	{ /* the device: features and extensions */
		VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		float prio = 1.0f;
		VkExtensionProperties *ex;
		u32 nex = 0;
		const char *dext[8];
		u32 ndext = 0;
		VkPhysicalDevicePresentIdFeaturesKHR pid_f;
		VkPhysicalDevicePresentWaitFeaturesKHR pw_f;
		int want_pw = 0;
		memset(&pid_f, 0, sizeof pid_f); memset(&pw_f, 0, sizeof pw_f);
		f2.pNext = &f13;
		vkGetPhysicalDeviceFeatures2(phys, &f2);
		if (!f13.dynamicRendering) { fprintf(stderr, "xrc: vulkan: no dynamic rendering: nothing is drawn\n"); return 0; }
		vkEnumerateDeviceExtensionProperties(phys, 0, &nex, 0);
		ex = (VkExtensionProperties *)calloc(nex ? nex : 1, sizeof *ex);
		vkEnumerateDeviceExtensionProperties(phys, 0, &nex, ex);
		if (!has_ext(ex, nex, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) { fprintf(stderr, "xrc: vulkan: no %s: nothing is drawn\n", VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME); return 0; }
		dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
		if (surface) dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
		if (has_ext(ex, nex, "VK_KHR_portability_subset")) dext[ndext++] = "VK_KHR_portability_subset";
		if (surface && has_ext(ex, nex, VK_KHR_PRESENT_ID_EXTENSION_NAME) && has_ext(ex, nex, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)
		    && !(getenv("XRC_VK_PRESENT_WAIT") && !atoi(getenv("XRC_VK_PRESENT_WAIT")))) {
			VkPhysicalDeviceFeatures2 q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			pid_f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
			pw_f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
			q.pNext = &pid_f; pid_f.pNext = &pw_f;
			vkGetPhysicalDeviceFeatures2(phys, &q);
			if (pid_f.presentId && pw_f.presentWait) {
				dext[ndext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
				dext[ndext++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
				pid_f.pNext = &pw_f; pw_f.pNext = 0;
				f13.pNext = &pid_f; /* (enabled with the device, below) */
				want_pw = 1;
			}
		}
		free(ex);
		have_aniso = f2.features.samplerAnisotropy;
		have_bc = f2.features.textureCompressionBC;
		have_independent_blend = f2.features.independentBlend;
		have_fill_nonsolid = f2.features.fillModeNonSolid;
		{ /* only what is used */
			VkPhysicalDeviceFeatures want;
			memset(&want, 0, sizeof want);
			want.samplerAnisotropy = f2.features.samplerAnisotropy;
			want.textureCompressionBC = f2.features.textureCompressionBC;
			want.independentBlend = f2.features.independentBlend;
			want.fillModeNonSolid = f2.features.fillModeNonSolid;
#ifndef XRC_NO_TOOLS
			if (xrc_tool_env("XRC_VK_DRAWTIME")) want.pipelineStatisticsQuery = have_pstats = f2.features.pipelineStatisticsQuery;
#endif
			f2.features = want;
			memset((char *)&f13 + offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess), 0,
			       sizeof f13 - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess));
			f13.dynamicRendering = 1;
		}
		qi.queueFamilyIndex = qfam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
		di.pNext = &f2;
		di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
		di.enabledExtensionCount = ndext; di.ppEnabledExtensionNames = dext;
		VKC(vkCreateDevice(phys, &di, 0, &vd));
		if (want_pw) vkWaitForPresentKHR_ = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(vd, "vkWaitForPresentKHR");
		fprintf(stderr, "xrc: vulkan: present wait %s\n", vkWaitForPresentKHR_ ? "on" : "not available");
	}
#define LD(f) f = (PFN_##f)vkGetDeviceProcAddr(vd, #f);
	VKD_FNS(LD)
#undef LD
#define LD(f) vkr_##f = (PFN_##f)vkGetDeviceProcAddr(vd, #f);
	CS_FNS(LD)
#undef LD
	vkGetDeviceQueue(vd, qfam, 0, &queue);
#ifndef XRC_NO_TOOLS
	/* (only with XRC_VK_LOG, which reports them: reading the results back can block in a driver -- lavapipe waits there) */
	hitch_ms = xrc_tool_env("XRC_HITCH_MS") ? atof(xrc_tool_env("XRC_HITCH_MS")) : 0;
#if defined(_WIN32) && !defined(XRC_NO_TOOLS)
	if (xrc_tool_env("XRC_HITCH_STACK")) { /* (XRC_HITCH_STACK=ms: a frame that takes longer prints every guest thread's stack, at ms and 100 ms later: where a 200 ms frame waits) */
		pthread_t t;
		hs_ms = atof(xrc_tool_env("XRC_HITCH_STACK"));
		if (hs_ms > 0 && pthread_create(&t, 0, hs_thread, 0) == 0) pthread_detach(t);
	}
#endif
	if (hitch_ms) atexit(hitch_flush);
	/* (also for the fps report's GPU time: on in debug builds unless XRC_VK_GPU_TIME=0; not on a CPU device, lavapipe) */
	if ((xrc_tool_env("XRC_VK_LOG") || hitch_ms || ((!xrc_tool_env("XRC_VK_GPU_TIME") || atoi(xrc_tool_env("XRC_VK_GPU_TIME"))) && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU))
	    && props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0.0f) {
		VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		extern double xrc_gpu_ms_total;
		qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2 * NFRAMES;
		if (vkCreateQueryPool(vd, &qi, 0, &tspool) != VK_SUCCESS) tspool = VK_NULL_HANDLE;
		else if (xrc_gpu_ms_total < 0) xrc_gpu_ms_total = 0;
	}
	if ((xrc_tool_env("XRC_VK_PASSES") || hitch_ms) && props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0.0f) {
		VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2 * PP_MAX * NFRAMES;
		if (vkCreateQueryPool(vd, &qi, 0, &ppool) != VK_SUCCESS) ppool = VK_NULL_HANDLE;
	}
	if (xrc_tool_env("XRC_VK_DRAWTIME") && props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0.0f) {
		VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		dt_every = (u32)atoi(xrc_tool_env("XRC_VK_DRAWTIME"));
		if (dt_every < 2) dt_every = 30;
		qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = DT_MAX * NFRAMES;
		if (vkCreateQueryPool(vd, &qi, 0, &dtpool) != VK_SUCCESS) dtpool = VK_NULL_HANDLE;
		if (dtpool && have_pstats) {
			qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
			qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
			if (vkCreateQueryPool(vd, &qi, 0, &dspool) != VK_SUCCESS) dspool = VK_NULL_HANDLE;
		}
		fprintf(stderr, "xrc: vulkan: draw times every %u frames%s\n", dt_every, dspool ? ", with pixel counts" : " (no pipeline statistics: no pixel counts)");
	}
#endif
	fprintf(stderr, "xrc: vulkan: %s (Vulkan %u.%u)%s\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
	        surface ? "" : ", no window");
	if (!have_bc) fprintf(stderr, "xrc: vulkan: no BC (DXT) textures on this GPU: decoded on the CPU instead\n");
	fmt_d24s8 = fmt_supports(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
	fmt_d24x8 = fmt_supports(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_X8_D24_UNORM_PACK32 : VK_FORMAT_D32_SFLOAT;
	/* the descriptor set layout (push descriptors) and the pipeline layout */
	{
		VkDescriptorSetLayoutBinding b[22];
		VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, SM3_PUSH_SIZE};
		VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
		memset(b, 0, sizeof b);
		for (k = 0; k < 22; k++) {
			b[k].binding = k;
			b[k].descriptorCount = 1;
			b[k].descriptorType = k < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			b[k].stageFlags = k == 0 || k >= 18 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
		}
		li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
		li.bindingCount = 22; li.pBindings = b;
		VKC(vkCreateDescriptorSetLayout(vd, &li, 0, &dsl));
		pi.setLayoutCount = 1; pi.pSetLayouts = &dsl;
		pi.pushConstantRangeCount = 1; pi.pPushConstantRanges = &pr;
		VKC(vkCreatePipelineLayout(vd, &pi, 0, &playout));
		ci.pInitialData = pc_load(&ci.initialDataSize);
		if (ci.pInitialData) LOG("xrc: vulkan: pipeline cache %s loaded (%zu KB)\n", pc_path(), ci.initialDataSize / 1024);
		VKC(vkCreatePipelineCache(vd, &ci, 0, &pcache));
		free((void *)ci.pInitialData);
	}
	/* frames */
	{
		VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pi.queueFamilyIndex = qfam;
		VKC(vkCreateCommandPool(vd, &pi, 0, &cpool));
		for (k = 0; k < NFRAMES; k++) {
			Frame *fr = &frames[k];
			VkCommandBufferAllocateInfo ai2 = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			VkMemoryRequirements rq;
			ai2.commandPool = cpool; ai2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai2.commandBufferCount = 1;
			VKC(vkAllocateCommandBuffers(vd, &ai2, &fr->cb));
			VKC(vkCreateFence(vd, &fci, 0, &fr->fence));
			VKC(vkCreateSemaphore(vd, &sci, 0, &fr->acquired));
			bi.size = RING;
			bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			VKC(vkCreateBuffer(vd, &bi, 0, &fr->ring));
			vkGetBufferMemoryRequirements(vd, fr->ring, &rq);
			fr->ring_mem = mem_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
			if (fr->ring_mem.block < 0) die("allocating the ring", VK_ERROR_OUT_OF_DEVICE_MEMORY);
			VKC(vkBindBufferMemory(vd, fr->ring, blocks[fr->ring_mem.block].mem, fr->ring_mem.off));
			fr->ring_map = (unsigned char *)blocks[fr->ring_mem.block].map + fr->ring_mem.off;
		}
	}
	fi = 0;
	begin_frame();
	make_dummies();
	if (surface) make_swapchain();
	cs_start();
	return &vk_backend;
}
/* Shadow stability (XRC_SHADOW_HOLD=<frames>, menu row "Shadow stability"; default off): an object that tested visible in the object gather
 * (00a6c5b0) within the last N frames is kept even when a later frustum test culls it (page_00a6 patch). */
static int shadow_hold_init; static unsigned long long shadow_hold_n;
void xrc_vk_set_shadow_hold(int n) { shadow_hold_init = 1; shadow_hold_n = n > 0 ? (unsigned long long)n : 0; }
u32 xrc_shadow_hold(u32 site, u32 key, u32 res) {
	typedef struct { u32 key, site; unsigned long long lv; } HE; static HE tab[8192]; int h, i;
	if (!shadow_hold_init) { const char *e = getenv("XRC_SHADOW_HOLD"); shadow_hold_init = 1; shadow_hold_n = e ? (unsigned long long)atoll(e) : 900; fprintf(stderr, "xrc: vulkan: shadow stability %llu (%s)\n", shadow_hold_n, e ? "set by the user" : "default"); } /* (integ5: On by default; XRC_SHADOW_HOLD=0 is off) */
	if (!shadow_hold_n || key < 0x1000) return res;
	h = (int)((key >> 3) & 8191);
	for (i = 0; i < 8192 && tab[h].key && (tab[h].key != key || tab[h].site != site); i++) h = (h + 1) & 8191;
	if (i == 8192) return res;
	if (!tab[h].key) { tab[h].key = key; tab[h].site = site; }
	if (res & 0xff) { tab[h].lv = frame_no; return res; }
	if (tab[h].lv && frame_no - tab[h].lv <= shadow_hold_n) return (res & 0xffffff00u) | 1u;
	return res;
}
#endif
