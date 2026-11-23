/* xrc_thread.c -- guest threads.
 *
 * Each guest thread is a host thread with its own XCtx, guest stack, TEB and static TLS block, but only one runs
 * guest code at a time: the global lock (the GIL, a fair ticket lock) is held while guest code or the HLE runs. That
 * keeps the HLE's tables and the translated code's locked instructions (lock xadd / cmpxchg become plain C) correct
 * without making either thread-safe. The GIL changes hands
 *   - in blocking waits (xrc_block_wait: WaitForSingleObject, EnterCriticalSection ...) and Sleep,
 *   - every XRC_SLICE dispatched calls when another thread is waiting for it (xrc_preempt from xrc_dispatch: the
 *     calling translated function has its registers in its XCtx at that point).
 * A guest spin loop with no call in it would not yield; none has been seen so far.
 *
 * Waits poll their condition: a thread that changes something a waiter may be waiting for calls xrc_state_changed,
 * which wakes the waiters to check again (they also recheck every 10 ms). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* (pthread_setname_np) */
#endif
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include "xrc_rt.h"
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

/* the lock and condition variables of the GIL and the waits: Windows' own (SRW locks, condition variables: futex-like,
 * much cheaper to hand over than winpthreads'), pthreads elsewhere. Timed waits take an absolute CLOCK_MONOTONIC
 * time in ms; they return 0 on a timeout. */
#if defined(_WIN32) && !defined(XRC_PTHREAD_LOCKS)
#include <windows.h>
typedef SRWLOCK xmtx;
typedef CONDITION_VARIABLE xcv;
#define XM_INIT SRWLOCK_INIT
static inline void xm_lock(xmtx *m) { AcquireSRWLockExclusive(m); }
static inline void xm_unlock(xmtx *m) { ReleaseSRWLockExclusive(m); }
static inline void xc_init(xcv *cv) { InitializeConditionVariable(cv); }
static inline void xc_wait(xcv *cv, xmtx *m) { SleepConditionVariableSRW(cv, m, INFINITE, 0); }
static inline void xc_signal(xcv *cv) { WakeConditionVariable(cv); }
static unsigned long long now_ms(void);
static inline int xc_wait_until(xcv *cv, xmtx *m, unsigned long long until) {
	unsigned long long t = now_ms();
	if (t >= until) return 0;
	return SleepConditionVariableSRW(cv, m, (DWORD)(until - t), 0) || GetLastError() != ERROR_TIMEOUT;
}
#else
typedef pthread_mutex_t xmtx;
typedef pthread_cond_t xcv;
#define XM_INIT PTHREAD_MUTEX_INITIALIZER
static inline void xm_lock(xmtx *m) { pthread_mutex_lock(m); }
static inline void xm_unlock(xmtx *m) { pthread_mutex_unlock(m); }
static inline void xc_init(xcv *cv) {
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(cv, &ca);
	pthread_condattr_destroy(&ca);
}
static inline void xc_wait(xcv *cv, xmtx *m) { pthread_cond_wait(cv, m); }
static inline void xc_signal(xcv *cv) { pthread_cond_signal(cv); }
static inline int xc_wait_until(xcv *cv, xmtx *m, unsigned long long until) {
	struct timespec ts;
	ts.tv_sec = (time_t)(until / 1000);
	ts.tv_nsec = (long)(until % 1000) * 1000000L;
	return pthread_cond_timedwait(cv, m, &ts) != ETIMEDOUT;
}
#endif

#define XRC_SLICE 512 /* calls between looks at the clock */

/* ============================================================================================ the GIL
 * A FIFO of waiting threads, each on its own condition variable: a release hands the lock to exactly the first one
 * (no thundering herd), and a yield goes to the back of the line. */
typedef struct GilW { xcv cv; volatile int go; int init, sleeping; struct GilW *next; } GilW;
typedef struct Waiter {
	xcv cv;
	int init, woken, nkeys;
	unsigned long long wake_us; /* (when a waker signalled it: the wake-latency accounting) */
	const uintptr_t *keys;
	struct Waiter *next, *prev;
} Waiter;
/* this file's per-thread state, in the thread's XrcTls block (xrc.h: by hand on Windows, MinGW's emulated TLS cost a
 * call per access) */
typedef struct ThrTls {
	GilW gself;
	Waiter wself;
	int gil_mine, cur, is_main;
	unsigned slice;
	unsigned long long gil_t0, gil_rel_t, gil_off;
	void *sleep_timer;
} ThrTls;
_Static_assert(sizeof(ThrTls) <= sizeof(((XrcTls *)0)->thr), "XrcTls.thr: too small for ThrTls");
#define TT ((ThrTls *)xrc_tls()->thr)
#define gself (TT->gself)
#define wself (TT->wself)
#define gil_mine (TT->gil_mine)
#define cur (TT->cur)
#define is_main (TT->is_main)
#define slice (TT->slice)
#define gil_t0 (TT->gil_t0)
#define gil_rel_t (TT->gil_rel_t)
#define gil_off (TT->gil_off)
#define sleep_timer (TT->sleep_timer)
static xmtx gm = XM_INIT;
static GilW *gq_head, *gq_tail;
static int gil_held;
/* main-thread priority (XRC_GIL_FIFO=1: off, a plain FIFO): the main thread (index 0: it runs the frame) goes to the
 * front of the line, and while it waits, the thread holding the GIL hands it over after a short slice (XRC_SLICE_BG_US,
 * default 200 us) instead of a full one. The others still get the GIL whenever the main thread blocks (several times a
 * frame) or at the end of its slice. */
static volatile int main_waiting;
static int gil_fifo = -1;
/* (xrc_rt.c, on the main thread before its first acquire). Windows: the main thread a priority step or two above the
 * process's others (XRC_MAIN_PRIO = -2..2, default 2: THREAD_PRIORITY_HIGHEST; 0: as the process). It holds the GIL
 * for most of each frame, and a busy PC (a background encode, a compile on every core) preempted it while it held it:
 * every guest thread then stood still -- frames of 300-900 ms with 30 ms of CPU in them (2026-11-20 soaks: the main
 * thread held the GIL ~300 ms a frame and ran 30). The other threads keep the process's priority. */
void xrc_gil_mark_main(void) {
	is_main = 1;
#ifdef _WIN32
	const char *e = getenv("XRC_MAIN_PRIO");
	int p = e && *e ? atoi(e) : 2;
	if (p < -2) p = -2;
	if (p > 2) p = 2;
	if (p) SetThreadPriority(GetCurrentThread(), p);
#endif
}
/* the monotonic clock in microseconds. Windows: QueryPerformanceCounter with the frequency read once (winpthreads'
 * clock_gettime reads the frequency and divides on every call; this is read every few hundred guest calls) */
unsigned long long xrc_now_us(void) {
#ifdef _WIN32
	static unsigned long long freq;
	LARGE_INTEGER c;
	if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = (unsigned long long)f.QuadPart; }
	QueryPerformanceCounter(&c);
	return (unsigned long long)c.QuadPart / freq * 1000000ull + (unsigned long long)c.QuadPart % freq * 1000000ull / freq;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
#endif
}
#define now_us xrc_now_us
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#endif
}
volatile int xrc_gil_waiters;
static volatile int gil_owner_idx = -1; /* (the GIL holder, as of its acquire: xrc_gil_hog_watch) */
static volatile unsigned long long gil_owner_t0;
int xrc_gil_held(void) { return gil_mine; }
static void acct_gil(int hold, unsigned long long us);
/* time this thread spent without the GIL (from a release to the next acquire's return): xrc_gil_off_us */
/* the accounting is on with XRC_THREAD_CPU or XRC_IMPORT_TIME only (a clock read per hand-over otherwise) */
static int acct_on = -1;
static int acct(void) {
	if (acct_on < 0) acct_on = getenv("XRC_THREAD_CPU") || xrc_tool_env("XRC_IMPORT_TIME") || xrc_tool_env("XRC_HITCH_MS");
	return acct_on;
}
unsigned long long xrc_gil_off_us(void) { return gil_off; }

void xrc_gil_acquire(void) {
	GilW *w = &gself;
	if (!w->init) { xc_init(&w->cv); w->init = 1; }
	if (gil_fifo < 0) gil_fifo = getenv("XRC_GIL_FIFO") != 0;
	xm_lock(&gm);
	if (!gil_held && !gq_head) {
		gil_held = 1;
		xm_unlock(&gm);
		gil_mine = 1;
		gil_t0 = now_us();
		gil_owner_idx = cur; gil_owner_t0 = gil_t0;
		if (acct() && gil_rel_t) gil_off += gil_t0 - gil_rel_t;
		return;
	}
	unsigned long long tw = acct() ? now_us() : 0;
	w->go = 0;
	w->sleeping = 0;
	w->next = 0;
	if (is_main && !gil_fifo) { /* the main thread: to the front */
		w->next = gq_head;
		gq_head = w;
		if (!gq_tail) gq_tail = w;
		main_waiting = 1;
	} else {
		if (gq_tail) gq_tail->next = w; else gq_head = w;
		gq_tail = w;
	}
	xrc_gil_waiters++;
	/* the first in line spins a little before sleeping: most hand-overs then need no wake-up (which costs from tens of
	 * microseconds to a millisecond on Windows, with the GIL idle meanwhile) */
	if (gq_head == w) {
		int k;
		xm_unlock(&gm);
		/* (XRC_SPIN: the main thread's spin count; XRC_SPIN_BG the others': a long spin there only burns a core) */
		static int spin = -1, spin_bg = -1;
		if (spin < 0) spin = getenv("XRC_SPIN") ? atoi(getenv("XRC_SPIN")) : 20000;
		if (spin_bg < 0) spin_bg = getenv("XRC_SPIN_BG") ? atoi(getenv("XRC_SPIN_BG")) : gil_fifo ? spin : 2000;
		int n = is_main ? spin : spin_bg;
		for (k = 0; k < n && !__atomic_load_n(&w->go, __ATOMIC_ACQUIRE); k++) cpu_relax();
		xm_lock(&gm);
	}
	while (!w->go) { w->sleeping = 1; xc_wait(&w->cv, &gm); }
	w->sleeping = 0;
	xrc_gil_waiters--;
	if (is_main) main_waiting = 0;
	xm_unlock(&gm);
	gil_mine = 1;
	gil_t0 = now_us();
	gil_owner_idx = cur; gil_owner_t0 = gil_t0;
	if (tw) acct_gil(0, gil_t0 - tw);
	if (acct() && gil_rel_t) gil_off += gil_t0 - gil_rel_t;
}
void xrc_gil_release(void) {
	if (acct()) {
		gil_rel_t = now_us();
		acct_gil(1, gil_rel_t - gil_t0);
	}
	gil_mine = 0;
	xm_lock(&gm);
	if (gq_head) { /* handed over: the lock stays held */
		GilW *w = gq_head;
		gq_head = w->next;
		if (!gq_head) gq_tail = 0;
		__atomic_store_n(&w->go, 1, __ATOMIC_RELEASE);
		if (w->sleeping) xc_signal(&w->cv);
	} else gil_held = 0;
	xm_unlock(&gm);
}
void xrc_yield(void) {
	xrc_gil_release();
	xrc_gil_acquire();
}
/* a guest spin loop (XRC_SPIN_YIELD): what it waits for needs another thread to run, so the GIL goes now */
void xrc_spin_yield(void) { xrc_yield(); }
/* a thread keeps the GIL for at least a time slice (XRC_SLICE_US, default 4000 us; 2000 with XRC_GIL_FIFO; a background
 * thread XRC_SLICE_BG_US = 200 us while the main thread waits) before handing it to a waiting one:
 * every hand-over may cost a wake-up */
static unsigned long long slice_us, slice_bg_us;
/* XRC_PREEMPT's slow path: XRC_PREEMPT_N calls counted at once */
void xrc_preempt_n(XCtx *c) {
	c->pre_n = 0;
	slice += XRC_PREEMPT_N - 1u;
	xrc_preempt();
}
void xrc_preempt(void) {
	unsigned long long lim;
	/* (a background thread looks at the clock more often while the main thread waits) */
	if (++slice < (main_waiting && !is_main ? XRC_SLICE / 8 : XRC_SLICE)) return;
	slice = 0;
	if (!slice_us) {
		const char *e = getenv("XRC_SLICE_US"), *b = getenv("XRC_SLICE_BG_US");
		if (gil_fifo < 0) gil_fifo = getenv("XRC_GIL_FIFO") != 0;
		slice_us = e && atoi(e) > 0 ? (unsigned long long)atoi(e) : gil_fifo ? 2000 : 4000;
		slice_bg_us = b && atoi(b) > 0 ? (unsigned long long)atoi(b) : 200;
	}
	lim = is_main || !main_waiting || gil_fifo ? slice_us : slice_bg_us;
	if (xrc_gil_waiters > 0 && now_us() - gil_t0 >= lim) xrc_yield();
}

/* ============================================================================================ waits
 * A blocked thread sleeps on its own condition variable, registered with the keys (guest objects) it waits for;
 * whoever changes one calls xrc_wake(key), which wakes exactly those waiters (xrc_state_changed wakes all). A waiter
 * checks its condition holding the GIL and registers before releasing it, and wakers run holding the GIL, so no
 * wake is lost; the 1 s recheck is only a safety net. */
static xmtx sm = XM_INIT;
static Waiter *wlist;

/* XRC_WATCH (tools builds): what each thread is blocked in right now -- the kind of wait and its handle (noted by the
 * HLE import just before it blocks: xrc_wait_note), its timeout and since when; xrc_wait_describe prints it */
static unsigned long long now_ms(void);
#ifndef XRC_NO_TOOLS
typedef struct { const char *what; u32 h, ms; unsigned long long since; } WaitNow;
static WaitNow wnow[64];
void xrc_wait_note(const char *what, u32 h) { int t = xrc_thread_index(); if (t >= 0 && t < 64) { wnow[t].what = what; wnow[t].h = h; } }
static void wait_begin(u32 ms) { int t = xrc_thread_index(); if (t >= 0 && t < 64) { wnow[t].ms = ms; __atomic_store_n(&wnow[t].since, now_ms(), __ATOMIC_RELEASE); } }
static void wait_end(void) { int t = xrc_thread_index(); if (t >= 0 && t < 64) { __atomic_store_n(&wnow[t].since, 0, __ATOMIC_RELEASE); wnow[t].what = 0; wnow[t].h = 0; } }
int xrc_wait_describe(u32 tid, char *buf, size_t n) {
	int t = xrc_thread_by_id(tid);
	unsigned long long s;
	if (t < 0 || t >= 64 || !(s = __atomic_load_n(&wnow[t].since, __ATOMIC_ACQUIRE))) return 0;
	if (wnow[t].ms == 0xffffffffu) snprintf(buf, n, "blocked %.1f s in %s (handle %x, no timeout)", (double)(now_ms() - s) / 1000.0, wnow[t].what ? wnow[t].what : "a wait", wnow[t].h);
	else snprintf(buf, n, "blocked %.1f s in %s (handle %x, timeout %u ms)", (double)(now_ms() - s) / 1000.0, wnow[t].what ? wnow[t].what : "a wait", wnow[t].h, wnow[t].ms);
	return 1;
}
#else
#define wait_begin(ms) ((void)0)
#define wait_end() ((void)0)
#endif

static unsigned long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
static void wake_matching(uintptr_t key, int all) {
	Waiter *w;
	xm_lock(&sm);
	for (w = wlist; w; w = w->next) {
		int k, hit = all;
		for (k = 0; !hit && k < w->nkeys; k++) hit = w->keys[k] == key;
		if (hit && !w->woken) { w->woken = 1; if (acct()) w->wake_us = now_us(); xc_signal(&w->cv); }
	}
	xm_unlock(&sm);
}
/* the waiters per key (hashed; updated under sm by the waiters, which hold the GIL as they register): a wake whose key
 * no one waits for -- most of them (a ReleaseMutex / SetEvent nobody is blocked on: ~1000 a frame) -- returns without
 * taking the lock or walking the list */
#define WKEYS 1024u
static int wkey_n[WKEYS];
static inline u32 wkey_h(uintptr_t k) { return (u32)((k >> 3) ^ (k >> 13) ^ (k >> 23)) & (WKEYS - 1u); }
void xrc_wake(uintptr_t key) {
	if (!__atomic_load_n(&wlist, __ATOMIC_ACQUIRE) || !__atomic_load_n(&wkey_n[wkey_h(key)], __ATOMIC_ACQUIRE)) return;
	wake_matching(key, 0);
}
void xrc_state_changed(void) { if (wlist) wake_matching(0, 1); }

static void wl_note(unsigned long long os, unsigned long long gil);
static int nthreads_alive = 1, nblocked_forever;
int xrc_block_wait_keys(const uintptr_t *keys, int nkeys, int (*ready)(void *), void *arg, u32 ms) {
	unsigned long long deadline = ms == 0xffffffffu ? 0 : now_ms() + ms, warn = now_ms() + 20000;
	int forever = ms == 0xffffffffu, warned = 0;
	Waiter *w = &wself;
	if (!w->init) { xc_init(&w->cv); w->init = 1; }
	wait_begin(ms);
	for (;;) {
		unsigned long long t, until;
		if (ready(arg)) { wait_end(); return 1; }
		t = now_ms();
		if (!forever && t >= deadline) { wait_end(); return 0; }
		if (forever && !warned && t >= warn) {
			fprintf(stderr, "xrc: thread %x has been blocked for 20 s (%d of %d threads blocked without a timeout)\n",
			        xrc_thread_id(), nblocked_forever + 1, nthreads_alive);
			xrc_print_stack();
			warned = 1;
		}
		until = forever ? t + 1000 : (deadline - t > 1000 ? t + 1000 : deadline);
		xm_lock(&sm);
		w->keys = keys; w->nkeys = nkeys; w->woken = 0;
		{ int k; for (k = 0; k < nkeys; k++) __atomic_add_fetch(&wkey_n[wkey_h(keys[k])], 1, __ATOMIC_RELEASE); }
		w->prev = 0; w->next = wlist;
		if (wlist) wlist->prev = w;
		wlist = w;
		if (forever) nblocked_forever++;
		xrc_gil_release();
		while (!w->woken)
			if (!xc_wait_until(&w->cv, &sm, until)) break;
		if (w->prev) w->prev->next = w->next; else wlist = w->next;
		if (w->next) w->next->prev = w->prev;
		{ int k; for (k = 0; k < nkeys; k++) __atomic_sub_fetch(&wkey_n[wkey_h(keys[k])], 1, __ATOMIC_RELEASE); }
		if (forever) nblocked_forever--;
		xm_unlock(&sm);
		if (w->wake_us && acct()) {
			unsigned long long t1 = now_us(), t2, wk = w->wake_us;
			w->wake_us = 0;
			xrc_gil_acquire();
			t2 = now_us();
			wl_note(t1 - wk, t2 - t1);
			continue;
		}
		xrc_gil_acquire();
	}
}
int xrc_block_wait(int (*ready)(void *), void *arg, u32 ms) { return xrc_block_wait_keys(0, 0, ready, arg, ms); }
#ifdef _WIN32
/* Windows: a high-resolution waitable timer (~0.5 ms), as Sleep is 15.6 ms unless the timer resolution is raised --
 * and Windows 11 ignores timeBeginPeriod for a process whose window is hidden or occluded (a background test run):
 * the game's 1 ms timeSetEvent thread, its frame pacer's clock, then ticked at 64 Hz */
static void host_sleep_ms(u32 ms) {
	if (!sleep_timer) sleep_timer = CreateWaitableTimerExW(0, 0, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, 0x1f0003 /* TIMER_ALL_ACCESS */);
	if (sleep_timer) {
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)ms * 10000;
		if (SetWaitableTimer(sleep_timer, &due, 0, 0, 0, FALSE)) { WaitForSingleObject(sleep_timer, INFINITE); return; }
	}
	Sleep(ms);
}
#endif
void xrc_sleep(u32 ms) { xrc_sleep_c(ms, 1); }
/* class 1: Sleep / SleepEx, 2: the timeSetEvent threads, 4: the XAudio2 engine thread (XRC_SLEEP_CAP_MASK picks the classes the cap applies to) */
void xrc_sleep_c(u32 ms, int cls) {
	{ /* XRC_SLEEP_CAP=<ms> (diagnostic, default off): no guest Sleep on a worker thread lasts longer than this */
		static int cap = -2;
		if (cap == -2) { const char *e = getenv("XRC_SLEEP_CAP"); cap = e ? atoi(e) : -1; }
		static int mask = -1;
		if (mask < 0) { const char *m = getenv("XRC_SLEEP_CAP_MASK"); mask = m ? atoi(m) : 7; }
		if (cap >= 0 && !is_main && (mask & cls) && ms > (u32)cap) ms = (u32)cap;
	}
	if (ms == 0) { if (xrc_gil_waiters) xrc_yield(); return; }
#ifndef XRC_NO_TOOLS
	xrc_wait_note("Sleep", 0);
	wait_begin(ms);
#endif
	xrc_gil_release();
#ifdef _WIN32
	host_sleep_ms(ms);
#else
	{
		struct timespec ts;
		ts.tv_sec = ms / 1000;
		ts.tv_nsec = (long)(ms % 1000) * 1000000L;
		while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
	}
#endif
	xrc_gil_acquire();
	wait_end();
}

/* ============================================================================================ threads */
typedef struct {
	u32 tid, start, param, teb, stack_lo, stack_hi;
	volatile int done;
	u32 code;
	jmp_buf exit_jb;
	pthread_t host;
	/* accounting for xrc_thread_report: the host thread's CPU clock, and time (us) holding / waiting for the GIL */
	int clk_ok;
#ifdef _WIN32
	void *h;
#else
	clockid_t clk;
#endif
	volatile unsigned long long gil_hold, gil_wait;
	volatile unsigned long long wl_os, wl_gil, wl_n; /* per frame: the worst wake-up (signal to running again) and the worst GIL wait after it, us; wakes */
} Thread;
#define MAXTHREADS 64
static Thread threads[MAXTHREADS];
static int nthreads = 1; /* 0: the main thread */

u32 xrc_thread_id(void) { return threads[cur].tid ? threads[cur].tid : 0x1004u; }
int xrc_thread_index(void) { return cur; }
int xrc_thread_done(int i, u32 *code) {
	if (i <= 0 || i >= nthreads) return 0;
	if (threads[i].done && code) *code = threads[i].code;
	return threads[i].done;
}
int xrc_thread_by_id(u32 tid) {
	int i;
	for (i = 0; i < nthreads; i++)
		if (xrc_thread_id_of(i) == tid) return i;
	return -1;
}
u32 xrc_thread_id_of(int i) { return i == 0 ? 0x1004u : threads[i].tid; }

/* a host thread (xrc_rt.c starts it; XRC_GIL_HOG_S=secs, default 10, 0: off): a guest thread that has held the GIL
 * that long while others wait for it (a translated loop that makes no call and so never ends its slice: the game
 * stalls) is named once, with its guest call stack (as of its last call) */
void xrc_print_thread_stack(u32 tid);
void xrc_stall_tick(void);
void *xrc_gil_hog_watch(void *arg) {
	unsigned long long lim = (unsigned long long)(uintptr_t)arg * 1000000ull, told = 0;
	for (;;) {
		unsigned long long t0;
		int i;
#ifdef _WIN32
		Sleep(1000);
#else
		sleep(1);
#endif
		xrc_stall_tick();
		t0 = gil_owner_t0;
		i = gil_owner_idx;
		if (!__atomic_load_n(&gil_held, __ATOMIC_RELAXED) || !xrc_gil_waiters || i < 0 || !t0 || t0 == told || t0 != gil_owner_t0)
			continue;
		if (now_us() - t0 < lim) continue;
		told = t0;
		fprintf(stderr, "xrc: thread %x has held the GIL for %llu s with %d waiting (a loop without calls?)\n",
		        xrc_thread_id_of(i), (now_us() - t0) / 1000000ull, xrc_gil_waiters);
		xrc_print_thread_stack(xrc_thread_id_of(i));
	}
	return 0;
}

/* ---- per-thread accounting (xrc_thread_report) */
static void self_clock(Thread *t) {
#ifdef _WIN32
	HANDLE h = 0;
	if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
		t->h = h;
		t->clk_ok = 1;
	}
#else
	if (pthread_getcpuclockid(pthread_self(), &t->clk) == 0) t->clk_ok = 1;
#endif
}
/* the main thread (index 0) is the one that runs the constructors */
__attribute__((constructor)) static void main_clock(void) { self_clock(&threads[0]); }
unsigned long long xrc_main_gil_wait_us(void) { return threads[0].gil_wait; } /* (vk_d3d9.c's XRC_HITCH_MS) */
static void acct_gil(int hold, unsigned long long us) {
	Thread *t = &threads[cur];
	if (hold) t->gil_hold += us; else t->gil_wait += us;
}
static unsigned long long thread_cpu_us(Thread *t) {
	if (!t->clk_ok) return 0;
#ifdef _WIN32
	{
		FILETIME cr, ex, k, u;
		if (!GetThreadTimes((HANDLE)t->h, &cr, &ex, &k, &u)) return 0;
		return ((((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) +
		        (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10ull;
	}
#else
	{
		struct timespec ts;
		if (clock_gettime(t->clk, &ts) != 0) return 0;
		return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
	}
#endif
}
/* one line per busy thread since the last call: CPU, GIL held and GIL waited (spinning or asleep), in ms per frame
 * over nframes frames; the busiest first (f 0: only sets the baseline). Threads with under 0.1 ms/frame of any of them are left out. */
void xrc_thread_report(FILE *f, unsigned long long nframes) {
	static unsigned long long pc[MAXTHREADS], ph[MAXTHREADS], pw[MAXTHREADS];
	double cpu[MAXTHREADS], hold[MAXTHREADS], wait[MAXTHREADS], tc = 0, th = 0, tw = 0;
	int order[MAXTHREADS], n = nthreads, i, j, k = 0;
	if (!nframes) nframes = 1;
	for (i = 0; i < n; i++) {
		unsigned long long c = thread_cpu_us(&threads[i]), h = threads[i].gil_hold, w = threads[i].gil_wait;
		cpu[i] = (double)(c - pc[i]) / 1000.0 / (double)nframes;
		hold[i] = (double)(h - ph[i]) / 1000.0 / (double)nframes;
		wait[i] = (double)(w - pw[i]) / 1000.0 / (double)nframes;
		pc[i] = c; ph[i] = h; pw[i] = w;
		tc += cpu[i]; th += hold[i]; tw += wait[i];
		if (!f) continue;
		if (cpu[i] >= 0.1 || hold[i] >= 0.1 || wait[i] >= 0.1) order[k++] = i;
	}
	for (i = 1; i < k; i++) /* by CPU, descending */
		for (j = i; j > 0 && cpu[order[j]] > cpu[order[j - 1]]; j--) { int x = order[j]; order[j] = order[j - 1]; order[j - 1] = x; }
	if (!f) return;
	fprintf(f, "xrc:   threads, ms/frame cpu / GIL held / GIL waited: all %.2f / %.2f / %.2f\n", tc, th, tw);
	for (i = 0; i < k; i++) {
		Thread *t = &threads[order[i]];
		char nm[16];
		if (order[i] == 0) snprintf(nm, sizeof nm, "main");
		else snprintf(nm, sizeof nm, "%08x", t->start);
		fprintf(f, "xrc:     %04x %-8s %6.2f / %6.2f / %6.2f%s\n", xrc_thread_id_of(order[i]), nm, cpu[order[i]],
		        hold[order[i]], wait[order[i]], t->done ? " (exited)" : "");
	}
}

/* XRC_HITCH_THREADS: per thread, since the last call: the worst wake-up latency (a waker's signal to the thread running again) and
 * the worst wait for the GIL after it, in us, and the wakes (f 0: only reset) */
static void wl_note(unsigned long long os, unsigned long long gil) {
	Thread *th = &threads[cur];
	if (os > th->wl_os) th->wl_os = os;
	if (gil > th->wl_gil) th->wl_gil = gil;
	th->wl_n++;
}
void xrc_thread_wake_report(FILE *f) {
	int i;
	for (i = 0; i < nthreads; i++) {
		Thread *t = &threads[i];
		if (f && t->wl_n) fprintf(f, "xrc:     %04x wakes %llu: worst wake-up %llu us, then GIL wait %llu us\n", xrc_thread_id_of(i), (unsigned long long)t->wl_n, (unsigned long long)t->wl_os, (unsigned long long)t->wl_gil);
		t->wl_os = t->wl_gil = t->wl_n = 0;
	}
}

/* the TEB of a thread (as setup_thread in xrc_rt.c does for the main one) */
static u32 make_teb(u32 tid, u32 lo, u32 hi) {
	u32 raw = xrc_heap_alloc(0x2000, 1), t = (raw + 0xfffu) & ~0xfffu, tls, n;
	M32(t + 0x00) = 0xffffffffu;
	M32(t + 0x04) = hi;
	M32(t + 0x08) = lo;
	M32(t + 0x18) = t;
	M32(t + 0x20) = 0x1000;
	M32(t + 0x24) = tid;
	M32(t + 0x2c) = t + 0x800;
	M32(t + 0x30) = XRC_PEB;
	/* static TLS: a fresh copy of the template */
	n = xrc_tls_hi > xrc_tls_lo ? xrc_tls_hi - xrc_tls_lo : 0;
	tls = xrc_heap_alloc(n + xrc_tls_zfill + 16, 1);
	if (n) memcpy(xrc_mem + tls, xrc_mem + xrc_tls_lo, n);
	M32(t + 0x800) = tls;
	return t;
}

/* XRC_DENORMALS=0: the host's SSE flushes denormal results to zero and reads denormal inputs as zero (MXCSR FTZ |
 * DAZ) on every thread that runs guest code -- no slow microcode paths for tiny values, but results unlike the
 * original's (its MXCSR was the default 0x1f80: denormals on). Set on the main thread and each guest thread. */
void xrc_host_fp_mode(void) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
	static int off = -1;
	if (off < 0) { const char *e = getenv("XRC_DENORMALS"); off = e && atoi(e) == 0 && *e == '0'; }
	if (off) _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif
}
static void *thread_main(void *p) {
	Thread *t = (Thread *)p;
	XCtx c;
	xrc_host_fp_mode();
	cur = (int)(t - threads);
	self_clock(t);
#ifdef _WIN32
	{ /* (XRC_GUEST_PRIO = -2..2, default 1: the guest threads a step above the process's others -- the GIL lets one run
	   * at a time, so this costs the rest of the PC about one core at most, and a worker preempted while it holds the
	   * GIL stalls the main thread too; the main thread one more: xrc_gil_mark_main) */
		const char *e = getenv("XRC_GUEST_PRIO");
		const char *mp = getenv("XRC_MAIN_PRIO");
		int pr = e && *e ? atoi(e) : (mp && *mp && atoi(mp) == 0 ? 0 : 1); /* (the menu's Normal: XRC_MAIN_PRIO=0 also means the guest threads) */
		if (pr < -2) pr = -2;
		if (pr > 2) pr = 2;
		if (pr) SetThreadPriority(GetCurrentThread(), pr);
	}
#endif
#if !defined(_WIN32) && !defined(XRC_NO_TOOLS)
	{ /* (the guest thread id as the host thread's name: top -H, /proc/PID/task/T/comm) */
		char nm[16];
		snprintf(nm, sizeof nm, "g%x", t->tid);
		pthread_setname_np(pthread_self(), nm);
	}
#endif
	xrc_gil_acquire();
	xrc_teb = t->teb;
	if (xrc_tool_env("XRC_TRACE_THREADS") && t->param > 0x10000u) {
		int q_;
		fprintf(stderr, "xrc:   param at start:");
		for (q_ = 0; q_ < 9; q_++) fprintf(stderr, " %08x", M32(t->param + 4u * (u32)q_));
		fprintf(stderr, "\n");
	}
	xrc_ctx_init(&c, t->stack_hi - 64);
	if (!setjmp(t->exit_jb)) {
		u32 a = t->param;
		xrc_call_guest(&c, t->start, 1, &a);
		t->code = c.eax;
	}
	t->done = 1;
	nthreads_alive--;
	if (xrc_tool_env("XRC_TRACE_THREADS")) fprintf(stderr, "xrc: thread %x exited (%u)\n", t->tid, t->code);
	xrc_state_changed();
	xrc_gil_release();
	return 0;
}

/* a new guest thread running start(param); returns its index (0 on failure), *tid its id */
int xrc_thread_create(u32 start, u32 param, u32 stack, u32 *tid) {
	Thread *t;
	pthread_attr_t at;
	u32 raw;
	if (nthreads == MAXTHREADS) return 0;
	t = &threads[nthreads++];
	memset(t, 0, sizeof *t);
	if (stack < 0x40000u) stack = 0x100000u;
	stack = (stack + 0xfffu) & ~0xfffu;
	raw = xrc_heap_alloc(stack + 0x1000u, 0);
	if (!raw) { nthreads--; return 0; }
	t->stack_lo = (raw + 0xfffu) & ~0xfffu;
	t->stack_hi = t->stack_lo + stack;
	t->tid = 0x1004u + 4u * (u32)(t - threads);
	t->start = start;
	t->param = param;
	t->teb = make_teb(t->tid, t->stack_lo, t->stack_hi);
	nthreads_alive++;
	/* host stack: the translated code recurses as deep as the guest does, with bigger frames */
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 64u << 20);
	if (pthread_create(&t->host, &at, thread_main, t) != 0) xrc_fatal(0, "pthread_create failed");
	pthread_attr_destroy(&at);
	pthread_detach(t->host);
	if (xrc_tool_env("XRC_TRACE_THREADS"))
		fprintf(stderr, "xrc: thread %x created: start %08x(%08x), stack %08x..%08x\n", t->tid, start, param, t->stack_lo, t->stack_hi);
	if (xrc_tool_env("XRC_TRACE_THREADS") && param > 0x10000u) {
		int q_;
		fprintf(stderr, "xrc:   param at create:");
		for (q_ = 0; q_ < 9; q_++) fprintf(stderr, " %08x", M32(param + 4u * (u32)q_));
		fprintf(stderr, "\n");
	}
	if (tid) *tid = t->tid;
	return (int)(t - threads);
}
/* makes the calling host thread (one the runtime did not start: a DLL's callback thread) a guest thread with its own
 * stack and TEB, and *c its context. With the GIL held. */
void xrc_thread_adopt(XCtx *c) {
	Thread *t;
	u32 raw, stack = 0x40000u;
	if (nthreads == MAXTHREADS) xrc_fatal(0, "too many threads (adopting a host thread)");
	t = &threads[nthreads++];
	memset(t, 0, sizeof *t);
	raw = xrc_heap_alloc(stack + 0x1000u, 0);
	t->stack_lo = (raw + 0xfffu) & ~0xfffu;
	t->stack_hi = t->stack_lo + stack;
	t->tid = 0x1004u + 4u * (u32)(t - threads);
	t->teb = make_teb(t->tid, t->stack_lo, t->stack_hi);
	cur = (int)(t - threads);
	self_clock(t);
	xrc_teb = t->teb;
	nthreads_alive++;
	xrc_ctx_init(c, t->stack_hi - 64);
	if (xrc_tool_env("XRC_TRACE_THREADS")) fprintf(stderr, "xrc: host thread adopted as %x\n", t->tid);
}
void xrc_thread_exit(u32 code) {
	if (cur == 0) {
		fprintf(stderr, "xrc: ExitThread(%u) on the main thread\n", code);
		exit((int)code);
	}
	threads[cur].code = code;
	longjmp(threads[cur].exit_jb, 1);
}
