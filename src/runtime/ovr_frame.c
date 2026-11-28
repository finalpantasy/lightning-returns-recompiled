/* ovr_frame.c - the game's frame-limiter spin on one precise host wait, and a counter for the dt clamp.
 *
 * Where the time goes (2026-11-20): Framework::TickFrame 00ab93f0 -> 00ac3040 FrameClock::WaitForFrame. Its limiter
 * branch (00a96110 false: the runtime has no real vblank capability) ends in a loop that calls 00a96590 until the
 * frame's target time. 00a96590 (cdecl, no args) is a vblank wait: with no device object [0x510e940] it reads QPC,
 * then polls IDirect3DDevice9::GetRasterStatus(dev [0x510e944], 0, &rs) between Sleep(0) calls until rs.InVBlank, or
 * the scanline falls below the first one read, or 20 ms (QPC freq / 50) have passed since entry. The runtime's
 * GetRasterStatus (hle_d3d9.c) always answers {InVBlank 0, ScanLine 0}, so the loop can only end at 20 ms: a Sleep(0) +
 * QPC spin of 20 ms (a quarter of the main thread's CPU). This override does the same: it makes the first poll (the
 * stub's answer is constant: anything else, or a device object, runs the translation), caches the QPC frequency as the
 * original does, waits for entry + freq/50 ONCE (a high-resolution waitable timer with the GIL released, then a spin
 * under XRC_OVR_FRAME_SPIN_US, default 300 us), and returns with eax:edx = freq/50 like the original's last compare.
 * XRC_OVR_FRAME=0: the original spin.
 *
 * 00ac33b0 FrameClock::GetQuantizedDelta (thiscall, ret 4: dt -> *arg). XRC_CLAMP_LOG=1 counts, per 600 TickFrame
 * frames, those whose raw dt is above the clamp TickFrame applies (20020 units = 66.7 ms at the default 14.985 fps)
 * and the game time that clamp removes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "xrc_rt.h"
#include "guest_types.h"


#ifdef _WIN32
static __thread HANDLE hr_timer;
static void wait_100ns(unsigned long long t) { /* host wait of t * 100 ns, GIL not held */
	if (!hr_timer) hr_timer = CreateWaitableTimerExW(0, 0, 0x2 /* HIGH_RESOLUTION */, 0x1f0003);
	if (hr_timer) {
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)t;
		if (SetWaitableTimer(hr_timer, &due, 0, 0, 0, FALSE)) { WaitForSingleObject(hr_timer, INFINITE); return; }
	}
	Sleep((DWORD)(t / 10000ull));
}
#else
static void wait_100ns(unsigned long long t) {
	struct timespec ts;
	ts.tv_sec = (time_t)(t / 10000000ull);
	ts.tv_nsec = (long)(t % 10000000ull) * 100L;
	nanosleep(&ts, 0);
}
#endif

static int ovr_on = -1;
static unsigned long long spin_100ns = 3000;
static void ovr_init(void) {
	const char *e = getenv("XRC_OVR_FRAME"), *s = getenv("XRC_OVR_FRAME_SPIN_US");
	ovr_on = !(e && e[0] == '0') && !getenv("XRC_TIME_SCALE");
	if (s) spin_100ns = (unsigned long long)atoi(s) * 10ull;
	if (ovr_on) fprintf(stderr, "xrc: frame limiter: one high-resolution wait (XRC_OVR_FRAME=0 for the game's spin), final spin %llu us\n", spin_100ns / 10);
}

/* wait until xrc_qpc() >= target (100 ns units) */
static void wait_until(unsigned long long target) {
	xrc_gil_release();
	for (;;) {
		unsigned long long now = xrc_qpc();
		if (now >= target) break;
		if (target - now > spin_100ns) wait_100ns(target - now - spin_100ns);
		/* else spin the last stretch */
	}
	xrc_gil_acquire();
}


/* ------------------------------------------------------------------------------------------ vblank model
 * IDirect3DDevice9::GetRasterStatus (hle_d3d9.c) and the wait below share one model of the display's refresh:
 * period P (the refresh rate of the monitor the game's window is on) and the QPC time of one vblank, both from
 * DwmGetCompositionTimingInfo (re-read every second, so it stays phase-locked to the real vblank). InVBlank is 1 for the
 * first 5% of each period; else 0 and ScanLine = position in the period * display height / P. No window (headless), no
 * DWM or XRC_TIME_SCALE: 60 Hz from the first call. XRC_RASTER=0: the old {0, 0} answer (the wait then ends at 20 ms). */
#ifdef _WIN32
#include <dwmapi.h>
extern void *xrc_view_window;
#endif
int xrc_raster_mode(void) {
	static int m = -1;
	if (m < 0) { const char *e = getenv("XRC_RASTER"); m = !(e && e[0] == '0'); }
	return m;
}
static unsigned long long rs_period = 166667ull, rs_anchor, rs_synced; /* 100 ns units on xrc_qpc()'s clock */
static unsigned rs_height = 1080;
static void rs_sync(unsigned long long now) {
	if (rs_anchor && now - rs_synced < 10000000ull) return;
	rs_synced = now;
#ifdef _WIN32
	if (xrc_view_window && !getenv("XRC_TIME_SCALE")) {
		DWM_TIMING_INFO ti;
		LARGE_INTEGER fq, q;
		memset(&ti, 0, sizeof ti);
		ti.cbSize = sizeof ti;
		QueryPerformanceFrequency(&fq);
		QueryPerformanceCounter(&q);
		if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &ti)) && ti.qpcRefreshPeriod && ti.qpcVBlank) {
			MONITORINFO mi;
			unsigned long long per = (unsigned long long)((double)ti.qpcRefreshPeriod * 1e7 / (double)fq.QuadPart + 0.5);
			long long off = (long long)now - (long long)((double)q.QuadPart * 1e7 / (double)fq.QuadPart); /* QPC -> xrc_qpc */
			if (per >= 40000ull && per <= 1000000ull) { /* 10 to 250 Hz */
				rs_period = per;
				rs_anchor = (unsigned long long)((long long)((double)ti.qpcVBlank * 1e7 / (double)fq.QuadPart) + off);
			}
			memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
			if (GetMonitorInfoA(MonitorFromWindow((HWND)xrc_view_window, MONITOR_DEFAULTTOPRIMARY), &mi) && mi.rcMonitor.bottom > mi.rcMonitor.top)
				rs_height = (unsigned)(mi.rcMonitor.bottom - mi.rcMonitor.top);
		}
	}
#endif
	if (!rs_anchor) rs_anchor = now;
}
/* the position in the refresh period at now (0 = the vblank starts) */
static unsigned long long rs_phase(unsigned long long now) {
	long long d;
	rs_sync(now);
	d = (long long)(now - rs_anchor) % (long long)rs_period;
	if (d < 0) d += (long long)rs_period;
	return (unsigned long long)d;
}
/* hle_d3d9.c: the D3DRASTER_STATUS now */
void xrc_raster_status(unsigned *in_vblank, unsigned *scanline) {
	unsigned long long ph = rs_phase(xrc_qpc());
	if (ph < rs_period / 20) { *in_vblank = 1; *scanline = 0; }
	else { *in_vblank = 0; *scanline = (unsigned)(ph * rs_height / rs_period); }
}
/* the xrc_qpc() time of the vblank GetRasterStatus reports next (now if one is going on) */
static unsigned long long rs_next_vblank(unsigned long long now) {
	unsigned long long ph = rs_phase(now);
	if (ph < rs_period / 20) return now;
	return now + (rs_period - ph);
}


/* XRC_OVR_FRAME_LOG=1: every 30 calls, the wait's duration (orig or override: entry to return) against 20 ms */
static int wlog = -1;
static unsigned long long w_n, w_sum, w_min = ~0ull, w_max, w_gap_sum, w_last_end, w_over20;
static u32 w_ra[8]; static unsigned long long w_ra_n[8];
static void wlog_note(unsigned long long t0, unsigned long long t1) {
	unsigned long long d = t1 - t0;
	w_n++; w_sum += d; if (d < w_min) w_min = d; if (d > w_max) w_max = d;
	if (w_last_end) w_gap_sum += t0 - w_last_end;
	w_last_end = t1;
	if (d > 200000ull + 3000ull) w_over20++; /* more than 0.3 ms late */
	if (w_n % 30 == 0) {
		fprintf(stderr, "xrc: wait00a96590 [%s]: %llu calls; duration avg %.3f min %.3f max %.3f ms (20 ms wanted); %llu over 20.3 ms; avg gap between calls %.2f ms; at %.1f s\n",
		        ovr_on ? "override" : "original", w_n, w_sum / 10000.0 / 30, w_min / 10000.0, w_max / 10000.0, w_over20, w_gap_sum / 10000.0 / 30, (double)(t1 - 600000000ull) / 1e7);
		{ int i; for (i = 0; i < 8 && w_ra[i]; i++) fprintf(stderr, "xrc: wait00a96590 caller %08x: %llu calls\n", w_ra[i], w_ra_n[i]); }
		w_sum = w_gap_sum = w_max = 0; w_min = ~0ull;
	}
}
static void XRC_CC xo_00a96590_body(XCtx *c);
void XRC_CC xo_00a96590(XCtx *c) {
	unsigned long long t0;
	if (wlog < 0) wlog = getenv("XRC_OVR_FRAME_LOG") != 0;
	if (!wlog) { xo_00a96590_body(c); return; }
	{ /* the callers (return addresses) of the wait */
		u32 ra = ret_addr(c);
		int i;
		for (i = 0; i < 8 && w_ra[i] && w_ra[i] != ra; i++);
		if (i < 8) { w_ra[i] = ra; w_ra_n[i]++; }
	}
	t0 = xrc_qpc();
	xo_00a96590_body(c);
	wlog_note(t0, xrc_qpc());
}
static int hf_live; /* (hifps1: XRC_FPS_CAP pacing ran last TickFrame: the game's own vblank wait returns at once) */
static unsigned long long hf_vb_skips;
static void XRC_CC xo_00a96590_body(XCtx *c) {
	static XFn orig;
	unsigned long long start, timeout;
	u32 dev, rs = c->esp - 0x100, args[3];
	if (!orig) orig = xrc_translation_of(0x00a96590u);
	if (ovr_on < 0) ovr_init();
	if (!ovr_on || M32(G_WAIT_FLAG) || !(dev = M32(G_WAIT_OBJ))) { orig(c); return; }
	start = xrc_qpc();
	if (!(GF(QpfCache, G_QPF_CACHE, init_flags_b0) & QPF_VALID)) { GF(QpfCache, G_QPF_CACHE, init_flags) |= QPF_VALID; GF(QpfCache, G_QPF_CACHE, freq_lo) = 10000000u; GF(QpfCache, G_QPF_CACHE, freq_hi) = 0; } /* QueryPerformanceFrequency */
	timeout = GF(QpfCache, G_QPF_CACHE, freq) / 50ull;
	if (hf_live) { /* FrameClock::WaitForFrame only calls this after a frame longer than 1/59.94 s (n == 1): no
		* wait at all, the limiter in GetQuantizedDelta owns the pacing (the answer = the 20 ms timeout, as the stub's) */
		hf_vb_skips++;
		c->eax = (u32)timeout;
		c->edx = (u32)(timeout >> 32);
		xrc_cdecl_ret(c);
		return;
	}
	if (xrc_raster_mode()) { /* the modeled vblank, else the 20 ms cap */
		unsigned long long vb = rs_next_vblank(start);
		if (vb - start < timeout) { /* the vblank exit: eax = the last poll's 0 */
			if (vb > start) wait_until(vb);
			c->eax = 0;
			xrc_cdecl_ret(c);
			return;
		}
		wait_until(start + timeout);
		c->eax = (u32)timeout;
		c->edx = (u32)(timeout >> 32);
		xrc_cdecl_ret(c);
		return;
	}
	args[0] = dev; args[1] = 0; args[2] = rs;
	GF(U32Pair, rs, w0) = 0; GF(U32Pair, rs, w1) = 0;
	{ /* the first poll: the stub's constant answer, else the original loop */
		u32 r = xrc_call_guest(c, guest_vfn(dev, 0x4c), 3, args);
		if (r || GF(U32Pair, rs, w0) || GF(U32Pair, rs, w1)) { orig(c); return; }
	}
	wait_until(start + timeout);
	c->eax = (u32)timeout;
	c->edx = (u32)(timeout >> 32);
	xrc_cdecl_ret(c);
}

/* ---------------------------------------------------------------------------------------- clamp counter */
static int clamp_on = -1;
static int cl_frames, cl_over;
static long long cl_lost, cl_max;
static unsigned long long cl_total_over, cl_total;


/* ------------------------------------------------------------------------------------------ 60 fps (Variable) pacing
 * XRC_FPS60_LOG=1: every 300 TickFrame frames print the raw dt histogram (in 1/59.94 s ticks of 5005 units: 0, 1, 2, 3+),
 * the real time between frames (avg / max ms) and FrameClock [0] / [4] (the n in 00ac3040 is (int)([0]/[4] + 0.5)).
 * XRC_FPS60 (default on since 2026-11-21, owner-verified EXAMINE fix; =0 for the game's own dt): when the game's target fps (FrameClock [4]) is above 45 (Frame rate = Variable), give the game
 * an even dt: wait to the next 1/59.94 s boundary on a fixed schedule (the high-resolution wait above) and hand TickFrame
 * dt = 5005 units; a late frame gets ceil(elapsed / tick) ticks (max 4) and the schedule restarts from it. Without it
 * the dt is the game's own floor-quantized real time (0, 1 or 2 ticks per frame, depending on when Present returns). */
static int f60_on = -1, f60_log = -1;
static unsigned long long f60_sched, f60_prev;
static int f60_hist[4], f60_n;
static double f60_sum, f60_max;
extern void xrc_pace_get(double *pr, double *fw);
extern double xrc_gpu_ms_total;
static int pt_on = -1, pt_fix = -1;
typedef struct { double t, cpu, wait, over, iv, pr, fw, gpu, mcyc, goff; int k, gw; } PTRow; /* (integ5: mcyc = main-thread Mcycles this frame, goff = ms without the GIL, gw = GIL waiters) */
static PTRow *pt_rows; static unsigned pt_n, pt_cap = 60000;
static unsigned long long pt_prev_post, pt_prev_cyc, pt_prev_goff, pt_first, pt_dump_at;
/* integ5 (XRC_STALL_DIAG=1): a 10 Hz monitor thread logs system / process CPU, process read I/O and its own wake latency (how late a 100 ms sleep returns) */
typedef struct { double t; unsigned long long si, sk, su, pk, pu, rb, ro; double late; int gw; } MSample;
volatile DWORD xrc_stall_main_tid;
typedef struct { double t, ms; const char *name; } ImpRec;
static ImpRec *imp_rows; static unsigned imp_n, imp_cap = 20000;
void xrc_stall_imp(const char *name, unsigned long long t0, unsigned long long dt) { if (imp_rows && imp_n < imp_cap) { imp_rows[imp_n].t = (double)t0 / 1e4; imp_rows[imp_n].ms = (double)dt / 1e4; imp_rows[imp_n].name = name; imp_n++; } }
static MSample *mon_rows; static unsigned mon_n, mon_cap = 20000; static int mon_on;
static unsigned long long ft64(FILETIME f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static DWORD WINAPI mon_thread(LPVOID u) {
	(void)u;
	for (;;) {
		unsigned long long a = xrc_qpc(), b2; FILETIME i, k, us, c, e, pk, pu; IO_COUNTERS io; MSample *m;
		Sleep(100);
		b2 = xrc_qpc();
		if (mon_n >= mon_cap) return 0;
		m = &mon_rows[mon_n];
		GetSystemTimes(&i, &k, &us); GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu); GetProcessIoCounters(GetCurrentProcess(), &io);
		m->t = (double)b2 / 1e4; m->late = (double)(b2 - a) / 1e4 - 100.0; m->si = ft64(i); m->sk = ft64(k); m->su = ft64(us); m->pk = ft64(pk); m->pu = ft64(pu);
		m->rb = io.ReadTransferCount; m->ro = io.ReadOperationCount; m->gw = xrc_gil_waiters;
		mon_n++;
	}
}
static double pt_pr0, pt_fw0, pt_g0;
static void pt_dump(void) {
	const char *d = getenv("XRC_PACE_CSV"); FILE *f; unsigned i;
	if (!pt_rows || !pt_n) return;
	f = fopen(d ? d : "pace_trace.csv", "w"); if (!f) return;
	fprintf(f, "frame,t_ms,cpu_ms,wait_ms,overshoot_ms,interval_ms,k,present_ms,fence_ms,gpu_ms,mcyc,goff_ms,gilw\n");
	for (i = 0; i < pt_n; i++) { PTRow *r = &pt_rows[i]; fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", i, r->t, r->cpu, r->wait, r->over, r->iv, r->k, r->pr, r->fw, r->gpu, r->mcyc, r->goff, r->gw); }
	fclose(f);
	if (mon_on && mon_n) { char p[600]; snprintf(p, sizeof p, "%s.mon.csv", d ? d : "pace_trace.csv"); f = fopen(p, "w"); if (f) {
		fprintf(f, "t_ms,wake_late_ms,sys_idle,sys_kern,sys_user,proc_kern,proc_user,read_bytes,read_ops,gil_waiters\n");
		for (i = 0; i < mon_n; i++) { MSample *m = &mon_rows[i]; fprintf(f, "%.1f,%.2f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d\n", m->t, m->late, m->si, m->sk, m->su, m->pk, m->pu, m->rb, m->ro, m->gw); }
		fclose(f); } }
	if (imp_rows && imp_n) { char p[600]; snprintf(p, sizeof p, "%s.imp.csv", d ? d : "pace_trace.csv"); f = fopen(p, "w"); if (f) {
		fprintf(f, "t_ms,ms,import\n");
		for (i = 0; i < imp_n; i++) fprintf(f, "%.3f,%.3f,%s\n", imp_rows[i].t, imp_rows[i].ms, imp_rows[i].name ? imp_rows[i].name : "?");
		fclose(f); } }
	fprintf(stderr, "xrc: pace trace: %u frames -> %s\n", pt_n, d ? d : "pace_trace.csv");
}
static void pt_row(unsigned long long pre, unsigned long long post, unsigned long long target, int k) {
	double pr, fw, g = xrc_gpu_ms_total; PTRow *r;
	if (!pt_on) return;
	if (!pt_rows) { const char *e;
		pt_rows = calloc(pt_cap, sizeof *pt_rows); atexit(pt_dump);
		e = getenv("XRC_PACE_DUMP_S"); if (e) pt_dump_at = (unsigned long long)(atof(e) * 1e7); /* (one CSV dump this many seconds after the first frame: runs killed by taskkill lose the atexit one) */
		if (getenv("XRC_STALL_DIAG")) { imp_rows = calloc(imp_cap, sizeof *imp_rows); xrc_stall_main_tid = GetCurrentThreadId(); mon_rows = calloc(mon_cap, sizeof *mon_rows); if (mon_rows && CreateThread(0, 0, mon_thread, 0, 0, 0)) mon_on = 1; }
		if (getenv("XRC_STALL_PRIO")) { int pv = atoi(getenv("XRC_STALL_PRIO")); BOOL ok = SetThreadPriority(GetCurrentThread(), pv); fprintf(stderr, "xrc: main thread priority %d (XRC_STALL_PRIO) ok %d\n", pv, (int)ok); } /* (experiment: -2..2, 15 = time critical) */
		pt_first = post; }
	xrc_pace_get(&pr, &fw);
	if (pt_n < pt_cap && pt_prev_post) {
		r = &pt_rows[pt_n++];
		r->t = (double)post / 1e4; r->cpu = (double)(pre - pt_prev_post) / 1e4; r->wait = (double)(post - pre) / 1e4;
		r->over = target ? (double)(long long)(post - target) / 1e4 : 0; r->iv = (double)(post - pt_prev_post) / 1e4; r->k = k;
		{ ULONG64 cy = 0, go = xrc_gil_off_us(); QueryThreadCycleTime(GetCurrentThread(), &cy);
		  r->mcyc = (double)(cy - pt_prev_cyc) / 1e6; r->goff = (double)(go - pt_prev_goff) / 1e3; r->gw = xrc_gil_waiters; pt_prev_cyc = cy; pt_prev_goff = go; }
		r->pr = pr - pt_pr0; r->fw = fw - pt_fw0; r->gpu = g > 0 ? g - pt_g0 : 0;
		if ((pt_n & 8191) == 0 && getenv("XRC_PACE_FLUSH")) pt_dump();
		if (pt_dump_at && post - pt_first >= pt_dump_at) { pt_dump_at = 0; pt_dump(); }
	}
	pt_prev_post = post; pt_pr0 = pr; pt_fw0 = fw; pt_g0 = g > 0 ? g : 0;
}
/* ------------------------------------------------------------------------------- hifps1: any refresh rate
 * XRC_FPS_CAP=<hz> (e.g. 60, 120, 144, 165, 180, 240) or 0 / unlocked; unset = the fps60 pacing above, unchanged.
 * Needs the game's Frame rate = Variable (FrameClock target fps > 45), like fps60.
 * - Limiter: one high-resolution wait to a fixed-period schedule (period 1/cap s, kept in 1/10 ns so 144 Hz does not drift);
 *   a late frame resyncs the schedule to now (no catch-up burst, no debt): the next frame gets a full period.
 * - dt: the real time between two limiter exits, in the game's units (1/300000 s), not rounded to 1/59.94 s ticks
 *   (GetQuantizedDelta's rounding gave 0-dt frames above 60 fps). The fraction is carried, so sum(dt) == wall time.
 *   The game still clamps one frame to 20020 units (66.7 ms) in TickFrame.
 * - The game's own wait (FrameClock::WaitForFrame -> 00a96590 after frames longer than 1/59.94 s) is skipped.
 * - XRC_FPS_WHOLEMS=1 (default on with a cap): dt rounded to whole ms (300 units) with the remainder carried. The field
 *   (FieldInterface::vf8 and the world-clock path fed by an int ms value) truncates dt to ms each frame; whole-ms dt makes
 *   that exact at any rate (at 144 fps it alternates 6 / 7 ms). =0: exact units.
 * - XRC_FPS_DTSCALE=<f>: test only: dt *= f (dt-driven things slow down, frame-driven ones keep speed); with
 *   XRC_FPS_DTSCALE_AT=t0,t1 only from t0 to t1 s after boot (=0: whatever still moves is advanced per frame).
 * - XRC_FPS_EDGE=present (default): the wait sits just before the game's Present, so frames leave on the schedule and
 *   dt = the time between presents; =tick: the wait in GetQuantizedDelta (after Present, as fps60 does).
 * - XRC_FPS_MON=1 (default on with a cap): every 5 s one line: fps, frames over 2x the period, game dt / wall, world
 *   clock rate, field ms / wall. */
static int hf_cap = -2, hf_ms = 1, hf_mon = 0;
static double hf_scale = 1.0, hf_sc_t0 = -1, hf_sc_t1 = 1e30;
static unsigned long long hf_period_q, hf_next_q; /* 1e-9 s units (QPC*100) */
static unsigned long long hf_prev;
static double hf_carry;
static void hifps_init(void) {
	const char *e = getenv("XRC_FPS_CAP");
	if (!e || !*e) hf_cap = -1;
	else if (!strcmp(e, "unlocked") || !strcmp(e, "0") || !strcmp(e, "off")) hf_cap = 0;
	else { hf_cap = atoi(e); if (hf_cap < 20) hf_cap = 20; if (hf_cap > 1000) hf_cap = 1000; }
	if (hf_cap > 0) hf_period_q = (unsigned long long)(1e9 / hf_cap + 0.5);
	e = getenv("XRC_FPS_WHOLEMS"); hf_ms = !(e && e[0] == '0');
	e = getenv("XRC_FPS_DTSCALE"); if (e) hf_scale = atof(e);
	e = getenv("XRC_FPS_DTSCALE_AT"); if (e) { hf_sc_t0 = atof(e); e = strchr(e, ','); if (e) hf_sc_t1 = atof(e + 1); } /* (t0,t1 s after boot) */
	e = getenv("XRC_FPS_MON"); hf_mon = e ? e[0] != '0' : hf_cap >= 0;
	if (hf_cap >= 0) fprintf(stderr, "xrc: hifps: cap %d Hz%s, dt %s, scale %g (XRC_FPS_CAP; needs Frame rate = Variable)\n",
	                         hf_cap, hf_cap ? "" : " (unlocked)", hf_ms ? "whole ms + carry" : "exact units + carry", hf_scale);
}
/* vk_d3d9.c (swap chain create): 1 when the cap is above the window's display refresh, or unlocked */
int xrc_hifps_want_immediate(void) {
	int hz = 0, want;
	if (hf_cap == -2) hifps_init();
	if (hf_cap < 0) return 0;
#ifdef _WIN32
	{
		MONITORINFOEXA mi; DEVMODEA dm;
		memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
		memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
		if (GetMonitorInfoA(MonitorFromWindow((HWND)xrc_view_window, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO *)&mi)
		    && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) hz = (int)dm.dmDisplayFrequency;
	}
#endif
	/* default IMMEDIATE for any cap: with FIFO + present wait, a present right after the limiter blocked 5-6 ms per frame
	 * in tests (cap 60: 46.5 fps, 12.8% of frames > 2x; IMMEDIATE cap 144: p99 7.4 ms). XRC_FPS_VSYNC=1: FIFO when the
	 * cap is at or below the display's refresh (unverified on a real high-refresh monitor). */
	{ const char *v = getenv("XRC_FPS_VSYNC"); want = !(v && v[0] == '1') || hf_cap == 0 || hz <= 1 || hf_cap > hz + 1; }
	fprintf(stderr, "xrc: hifps: display %d Hz, cap %d: %s\n", hz, hf_cap, want ? "IMMEDIATE (vsync off)" : "FIFO (vsync)");
	return want;
}
static int hf_edge = -1; /* 1: the wait is just before the game's Present (XRC_FPS_EDGE=present, default); 0: in GetQuantizedDelta */
static unsigned long long hf_exit, hf_exit_n, hf_seen_n;
static long long hf_lastU;
static DWORD hf_main_tid;
static void hifps_limit(void) {
	unsigned long long now = xrc_qpc(), pre = now, post, tgt_t = 0;
	if (hf_cap > 0) {
		unsigned long long nq = now * 100ull;
		if (!hf_next_q) hf_next_q = nq;
		hf_next_q += hf_period_q;
		if (nq < hf_next_q) { tgt_t = hf_next_q / 100ull; wait_until(tgt_t); }
		else hf_next_q = nq; /* late: resync, no catch-up */
	}
	post = xrc_qpc();
	pt_row(pre, post, tgt_t, (int)hf_lastU);
	hf_exit = post; hf_exit_n++;
}
/* hle_d3d9.c v_Present: the limiter, once per TickFrame, on the main thread, so presents leave on the schedule */
void xrc_hifps_pre_present(void) {
	if (!hf_live || hf_edge != 1 || GetCurrentThreadId() != hf_main_tid || hf_exit_n != hf_seen_n) return;
	hifps_limit();
}
static void hifps_tick(u32 out, unsigned long long now) {
	unsigned long long post;
	double u;
	long long U;
	(void)now;
	if (hf_edge < 0) { const char *e = getenv("XRC_FPS_EDGE"); hf_edge = !(e && !strcmp(e, "tick")); fprintf(stderr, "xrc: hifps: wait %s\n", hf_edge ? "before Present" : "in GetQuantizedDelta"); }
	hf_live = 1;
	hf_main_tid = GetCurrentThreadId();
	if (pt_on < 0) { pt_on = getenv("XRC_PACE_TRACE") != 0; pt_fix = 1; }
	if (!hf_edge || hf_exit_n == hf_seen_n) hifps_limit(); /* (no present since the last tick: wait here) */
	hf_seen_n = hf_exit_n;
	post = hf_exit;
	if (!hf_prev) hf_prev = post - 166833ull;
	{ double ts = (double)(post - 600000000ull) / 1e7, sc = ts >= hf_sc_t0 && ts < hf_sc_t1 ? hf_scale : 1.0;
	  u = (double)(post - hf_prev) * 0.03 * sc + hf_carry; } /* 100 ns -> 1/300000 s */
	hf_prev = post;
	if (u > 600000.0) u = 600000.0; /* (2 s: a stall; TickFrame clamps to 20020 anyway) */
	U = hf_ms ? (long long)(u / 300.0) * 300 : (long long)u;
	hf_carry = u - (double)U;
	if (U < 0) U = 0;
	M32(out) = (u32)U;
	hf_lastU = U;
}
static unsigned long long hm_t0, hm_w0, hm_n, hm_long;
static double hm_dt, hm_ms, hm_maxiv;
static unsigned long long hm_prev;
static void hf_monitor(u32 out, u32 fc) {
	unsigned long long now = xrc_qpc(), w = 0;
	u32 fom = M32(0x24c9d00u), tm = 0, fi;
	if (hf_mon < 1) return;
	if (fom) tm = M32(fom + 0x18e74u);
	if (tm) w = M64(tm + 0x50u);
	fi = M32(0x234f130u);
	if (!hm_t0) { hm_t0 = now; hm_w0 = w; hm_prev = now; return; }
	{ double iv = (double)(now - hm_prev) / 1e4, per = hf_cap > 0 ? 1000.0 / hf_cap : 1000.0 / 59.94;
	  if (iv > 2.0 * per) hm_long++; if (iv > hm_maxiv) hm_maxiv = iv; }
	hm_prev = now;
	hm_n++; hm_dt += (double)(int)M32(out) / 300.0;
	if (fi) hm_ms += (double)(int)M32(fi + 0x4cu);
	if (now - hm_t0 >= 50000000ull) {
		double wall = (double)(now - hm_t0) / 1e4; /* ms */
		long long dw = (long long)(w - hm_w0);
		unsigned long long h24 = w ? (24ull * w / 1080000000ull) % 24ull : 0, m60 = w ? (24ull * w / 18000000ull) % 60ull : 0;
		fprintf(stderr, "xrc: hifps mon: %.1f fps, %llu frames > 2x period (max %.1f ms); game dt/wall %.4f; world +%lld units = %.4f per wall unit (clock %02llu:%02llu); field ms/wall %.4f; FC0 %g FC4 %g; vb skips %llu; at %.1f s\n",
		        hm_n * 1000.0 / wall, hm_long, hm_maxiv, hm_dt / wall, dw, (double)dw / (wall * 300.0), h24, m60, hm_ms / wall,
		        fc ? *(float *)(XM + fc) : -1.0f, fc ? *(float *)(XM + fc + 4) : -1.0f, hf_vb_skips, (double)(now - 600000000ull) / 1e7);
		hm_t0 = now; hm_w0 = w; hm_n = hm_long = 0; hm_dt = hm_ms = hm_maxiv = 0;
	}
}
static void fps60_tick(u32 out) {
	u32 fc = M32(G_FRAME_CLOCK);
	float tgt = fc ? GFN(FrameClock, fc, target_fps) : 0.0f;
	unsigned long long now = xrc_qpc();
	long long raw = (int)M32(out);
	if (f60_on < 0) { const char *e = getenv("XRC_FPS60"); f60_on = !(e && e[0] == '0'); f60_log = getenv("XRC_FPS60_LOG") != 0;
		if (f60_on) fprintf(stderr, "xrc: fps60 pacing on (XRC_FPS60=0 turns it off): even dt when the target fps is above 45\n"); }
	if (f60_log) {
		long long t = raw / 5005;
		double d = f60_prev ? (double)(now - f60_prev) / 10000.0 : 0.0;
		f60_hist[t < 0 ? 0 : t > 3 ? 3 : t]++; f60_sum += d; if (d > f60_max) f60_max = d;
		if (++f60_n >= 300) {
			fprintf(stderr, "xrc: fps60: raw dt ticks 0:%d 1:%d 2:%d 3+:%d; frame period avg %.2f max %.2f ms; FC[0]=%g FC[4]=%g; at %.1f s\n",
			        f60_hist[0], f60_hist[1], f60_hist[2], f60_hist[3], f60_sum / f60_n, f60_max, fc ? GFN(FrameClock, fc, fps) : -1.0f, tgt,
			        (double)(now - 600000000ull) / 1e7);
			f60_n = 0; f60_hist[0] = f60_hist[1] = f60_hist[2] = f60_hist[3] = 0; f60_sum = f60_max = 0.0;
		}
	}
	f60_prev = now;
	if (pt_on < 0) { pt_on = getenv("XRC_PACE_TRACE") != 0; pt_fix = !(getenv("XRC_PACE_FIX") && getenv("XRC_PACE_FIX")[0] == '0'); if (pt_on || pt_fix) fprintf(stderr, "xrc: pace6: trace %d fix %d\n", pt_on, pt_fix); }
	if (hf_cap == -2) hifps_init();
	if (hf_cap >= 0 && tgt > 45.0f) { f60_sched = 0; hifps_tick(out, now); hf_monitor(out, fc); return; }
	hf_live = 0;
	if (!f60_on || tgt <= 45.0f) { f60_sched = 0; pt_row(now, now, 0, 0); hf_monitor(out, fc); return; }
	{
		const unsigned long long P = 166833ull; /* QPC (10 MHz) per 1/59.94 s */
		long long k = 1; unsigned long long pre = now, tgt_t = 0;
		if (!f60_sched) f60_sched = now;
		if (now < f60_sched + P) { tgt_t = f60_sched + P; wait_until(tgt_t); f60_sched += P; }
		else if (pt_fix) { /* catch up: whole ticks elapsed (>= 1), game time stays on the schedule */
			k = (long long)((now - f60_sched) / P); if (k < 1) k = 1;
			if (k > 4) { k = 4; f60_sched = now; } else f60_sched += (unsigned long long)k * P;
		} else {
			k = (long long)((now - f60_sched + P - 1) / P);
			if (k > 4) { k = 4; f60_sched = now; } else f60_sched += (unsigned long long)k * P;
		}
		pt_row(pre, xrc_qpc(), tgt_t, (int)k);
		M32(out) = (u32)(k * 5005);
		hf_monitor(out, fc);
	}
}

void XRC_CC xo_00ac33b0(XCtx *c) {
	static XFn orig;
	u32 ret = ret_addr(c), out = arg_u32(c, 1);
	if (!orig) orig = xrc_translation_of(0x00ac33b0u);
	orig(c);
	if (ret >= 0x00ab93f0u && ret < 0x00ab9a00u) fps60_tick(out);
	if (clamp_on < 0) clamp_on = getenv("XRC_CLAMP_LOG") != 0;
	if (clamp_on && ret >= 0x00ab93f0u && ret < 0x00ab9a00u) { /* from Framework::TickFrame */
		long long maxdt = 20020, dt = (int)M32(out);
		u32 cfg = M32(G_FRAME_CFG);
		if (cfg) {
			float mf = GFN(FrameCfg, cfg, max_fps);
			if (mf > 0.0f) maxdt = (long long)((double)(1.0f / mf) * 300000.0 + 0.5); /* (rounded: 20020, the game's clamp, = 4 ticks) */
		}
		cl_frames++; cl_total++;
		if (dt > maxdt) { cl_over++; cl_total_over++; cl_lost += dt - maxdt; }
		if (dt > cl_max) cl_max = dt;
		if (cl_frames >= 600) {
			fprintf(stderr, "xrc: clamp: frames %llu: %d of 600 had raw dt > %lld (max %lld = %.1f ms; clamp removed %.1f ms of game time); total over %llu\n",
			        cl_total, cl_over, maxdt, cl_max, (double)cl_max / 300.0, (double)cl_lost / 300.0, cl_total_over);
			cl_frames = cl_over = 0; cl_lost = cl_max = 0;
		}
	}
}

const XrcManual xrc_manual_frame[] = {
	{0x00a96590u, xo_00a96590, "FrameLimiter vblank wait: one precise wait (ovr_frame.c)"},
	{0x00ac33b0u, xo_00ac33b0, "FrameClock::GetQuantizedDelta: clamp counter (ovr_frame.c)"},
};
const unsigned xrc_manual_frame_n = sizeof xrc_manual_frame / sizeof xrc_manual_frame[0];
