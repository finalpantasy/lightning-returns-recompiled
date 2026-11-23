/* ovr_game.c - hand-written replacements for functions of the game itself (compile-time options).
 *
 * XRC_FRAME_LIMIT=30 or 60 (build with XRC_DEFS=-DXRC_FRAME_LIMIT=60): the frame limiter's rate, without its snapping.
 * The game paces its frames like a vsync. A 1 ms timeSetEvent callback runs its pacer (00e8d220, on its own thread),
 * which counts fixed 1/60 s ticks ([this] = 60.0) and lets the waiting main thread go only on a tick boundary, once
 * round([this] / [this + 4]) ticks (the game's target rate) have passed since the last frame: at a 60 target a frame
 * that needs 17 ms takes 33 (fps snaps to 60, 30, 20 ...). The release (00e8d3c0) puts a QueryPerformanceCounter time
 * stamp into the frame-time ring and ups the semaphore the main thread waits on in 00e8d460 (flag [this + 0x40] ==
 * -1 while it waits).
 * With XRC_FRAME_LIMIT the pacer releases the waiting main thread once 1/XRC_FRAME_LIMIT s (or the game's own target
 * period, if longer) has passed since the last release, keeping that cadence while frames are on time:
 *   30  a steady 30 fps (a frame up to 33 ms takes 33; a longer one is not stretched to 50)
 *   60  up to 60 fps (a 20 ms frame takes 20 ms, not 33)
 * Everything else (the waits, the threads, the ring) is the game's. (The engine steps its logic by the last frame
 * time rounded to whole 1/60 s ticks, 00e8d490: a frame time between multiples of 1/60 s steps the nearest one.)
 *
 * The idle work (all builds): the frame wait first runs an idle callback ([this + 0x38]: 0040e880), which builds
 * resources -- effect nodes (00428c20) and the ExResourceManager's queue (00ccb570: in "checker mode", one item after
 * another while 00404770 says go on) -- until the frame is due: 00e8d5a0 ("the pacer has released the frame",
 * [pacer + 0x34]). The game presents without waiting (D3DPRESENT_INTERVAL_IMMEDIATE) and runs fast natively, so on a
 * PC the rest of each frame goes to that. Here a frame is often due already when its wait starts (slower code; a FIFO
 * present that waits for the vblank), and then each frame got one unit of idle work: loading a zone, whose effects
 * have thousands of instances to build, seemed to hang on its loading screen ("Effect NodeBuild wait", 00428da0).
 * xo_00e8d5a0 keeps "not due" for the first XRC_MIN_IDLE_MS (default 4; tools builds read the variable) of each
 * frame's idle work -- frames with nothing to build lose nothing: the callback stops when it has no work. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"

/* 00e8d5a0 (cdecl, no arguments): is the frame due (bool) -- the original, unless this frame's idle work has not had
 * its minimum yet. The idle period: from the frame wait's start (xrc_idle_begin, XRC_FRAME_LIMIT builds), or else
 * from a call more than 2 ms after the last one. */
static unsigned long long idle_t0, idle_last;
unsigned long long xrc_frame_wait_us; /* the main thread's time in the frame wait (XRC_FRAME_LIMIT builds; the fps report) */
unsigned long long xrc_acquire_wait_us; /* the main thread's time waiting for a swap chain image (vk_d3d9.c: the queue full) */
double xrc_gpu_ms_total = -1; /* the GPU's time on the frames so far, in ms (the fps report); -1: not measured (vk_d3d9.c) */
static int idle_new;
void xrc_idle_begin(void) { idle_new = 1; }
void XRC_CC xo_00e8d5a0(XCtx *c) {
	static XFn orig;
	static unsigned long long min_us = ~0ull;
	unsigned long long now = xrc_now_us();
	if (!orig) orig = xrc_translation_of(0x00e8d5a0u);
	if (min_us == ~0ull) {
		const char *e = xrc_tool_env("XRC_MIN_IDLE_MS");
		min_us = e ? (unsigned long long)(atof(e) * 1000.0) : 4000ull;
	}
	if (idle_new || now - idle_last > 2000ull) { idle_t0 = now; idle_new = 0; }
	idle_last = now;
	orig(c);
	if ((c->eax & 0xffu) && now - idle_t0 < min_us) c->eax = 0;
}

#ifdef XRC_FRAME_LIMIT
int xrc_vblank_start(void);
unsigned long long xrc_vblank_count(double *hz);
#if XRC_FRAME_LIMIT != 30 && XRC_FRAME_LIMIT != 60
#error "XRC_FRAME_LIMIT: 30 or 60"
#endif
/* 00e8d220 (thiscall, no arguments): the pacer's tick */
/* the release when due: from the pacer's 1 ms tick, and from the main thread as it starts to wait (00e8d560), so a
 * frame that is already late goes on at once instead of at the next tick (~1 ms a frame otherwise) */
static void try_release(XCtx *c, u32 self) {
	static int said;
	static unsigned long long last_rel;
	float fps;
	unsigned long long period = 10000000ull / XRC_FRAME_LIMIT, now;
	static unsigned long long unlock_at = 1; /* (1: not read yet; 0: never) */
	if (unlock_at == 1) {
		const char *e = xrc_tool_env("XRC_UNLOCKED");
		unlock_at = e ? xrc_now_us() + (unsigned long long)(atof(e) * 1e6) + 2 : 0;
	}
	memcpy(&fps, xrc_mem + self + 4, 4);
	if (fps >= 1.0f && fps <= 1000.0f && (unsigned long long)(1e7 / fps) > period) period = (unsigned long long)(1e7 / fps);
	/* XRC_UNLOCKED=secs (tools builds): from secs on, no frame limit at all (and an immediate present, vk_d3d9.c) -- a
	 * benchmark of what a frame costs (the game's logic steps by whole 1/60 s ticks: it may then run fast or slow) */
	if (unlock_at && xrc_now_us() >= unlock_at) period = 0;
	if (!said) { said = 1; fprintf(stderr, "xrc: XRC_FRAME_LIMIT: frames at up to %d fps, not snapped\n", XRC_FRAME_LIMIT); }
	/* locked to the display's refresh (vblank.c), when it runs at a whole multiple of the rate (60 Hz at 60, 120 at
	 * 60 ...: every k-th vertical blank): each frame released at a vertical blank -- one clock, not two drifting
	 * apart. Else (144 Hz, not measured yet, XRC_VSYNC_LOCK=0) the limiter's own clock. */
	if (period && xrc_vblank_start()) {
		static unsigned long long last_vb;
		static int was_locked = -1;
		double hz, r;
		unsigned long long vb = xrc_vblank_count(&hz);
		int k = 0;
		r = hz * (double)period / 1e7; /* (refreshes per frame) */
		if (hz > 0) {
			k = (int)(r + 0.5);
			if (k < 1 || r < k * 0.97 || r > k * 1.03) k = 0;
		}
		if ((k != 0) != was_locked) {
			was_locked = k != 0;
			if (k) fprintf(stderr, "xrc: frame limiter: locked to the display's refresh (%.2f Hz, every %d)\n", hz, k);
			else if (hz > 0) fprintf(stderr, "xrc: frame limiter: its own clock (the display's %.2f Hz is not a multiple of the rate)\n", hz);
		}
		if (k) {
			/* (when present waited for an image in each of the last frames, the swap chain's queue is full -- it
			 * filled before the lock, and at the display's own rate it would stay full: frames late, and this wait,
			 * which the game loads in, nothing. One refresh more, once, empties it. Not for a wait now and then: the
			 * GPU behind (1080p: one frame in twelve) -- a refresh more each time lost 3-4 fps) */
			static unsigned long long aw_last;
			static int aw_run;
			unsigned long long need = (unsigned long long)k + (aw_run >= 8 ? 1u : 0u);
			if (M32(self + 0x40) == 0xffffffffu && vb >= last_vb + need) {
				aw_run = need > (unsigned long long)k ? 0 : xrc_acquire_wait_us - aw_last > 2000ull ? aw_run + 1 : 0;
				aw_last = xrc_acquire_wait_us;
				last_vb = vb;
				last_rel = xrc_qpc();
				xrc_call_guest(c, 0x00e8d3c0u, 0, 0);
				M32(self + 0x40) = 0;
			}
			return;
		}
	}
	if (M32(self + 0x40) == 0xffffffffu) {
		unsigned long long due = last_rel + period;
		now = xrc_qpc();
		if (!last_rel || now + 10000ull >= due) {
			last_rel = !last_rel || now > due + 10000ull ? now : due;
			xrc_call_guest(c, 0x00e8d3c0u, 0, 0);
			M32(self + 0x40) = 0;
		}
	}
}
static void XRC_CC m_pacer_tick(XCtx *c) {
	try_release(c, c->ecx);
	xrc_cdecl_ret(c);
}
/* the main thread's frame wait (this in ecx): released first if due, then the game's wait (on the semaphore) */
static void XRC_CC m_frame_wait(XCtx *c) {
	static XFn orig;
	if (!orig) orig = xrc_translation_of(0x00e8d560u);
	if (!orig) xrc_fatal(c, "XRC_FRAME_LIMIT: no translation of 00e8d560");
	unsigned long long t0 = xrc_now_us();
	try_release(c, c->ecx);
	xrc_idle_begin();
	orig(c);
	xrc_frame_wait_us += xrc_now_us() - t0;
}
const XrcManual xrc_manual_game[] = {
	{0x00e8d220u, m_pacer_tick, "frame pacer tick (XRC_FRAME_LIMIT)"},
	{0x00e8d560u, m_frame_wait, "frame wait (XRC_FRAME_LIMIT: released at once if due)"},
	{0x00e8d5a0u, xo_00e8d5a0, "frame due (fast: a minimum of idle work)"},
};
const unsigned xrc_manual_game_n = 3;
#else
const XrcManual xrc_manual_game[] = {{0x00e8d5a0u, xo_00e8d5a0, "frame due (fast: a minimum of idle work)"}};
const unsigned xrc_manual_game_n = 1;
#endif
