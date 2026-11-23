/* hle_kernel32_io.c - KERNEL32 kernel objects and the file system for the portable runtime.
 *
 * Handles: one table of objects (handle = 0x1000 + 4 * index): files, file mappings, find searches, mutexes, events,
 * semaphores, threads. The std handles (0x10 / 0x14 / 0x18, GetStdHandle) are the host's stdin / stdout / stderr.
 *
 * Synchronization: guest threads are host threads under the runtime's GIL (xrc_thread.c); a wait polls its object
 * in xrc_block_wait, and whatever signals an object calls xrc_state_changed. A thread handle is signalled when the
 * thread has exited.
 *
 * Paths: the guest sees Windows paths. Drive C: is the host directory $XRC_CDRIVE (default ./cdrive), so the game
 * install is $XRC_CDRIVE/FINAL FANTASY XIII/... (the exe's own path, see GetModuleFileNameA). '\' and '/' both
 * separate, and each component is matched case-insensitively against the host directory (the game's file names are
 * not case-consistent; Linux file systems are case-sensitive). Relative paths resolve against the guest's current
 * directory (initially the exe's directory). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "xrc_rt.h"
#include "guest_types.h"
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_(p) mkdir((p), 0777)
#endif
#if !defined(_WIN32) || defined(__MINGW32__)
#include <dirent.h>
#define HAVE_DIRENT 1 /* (mingw has it too) */
#else
#define HAVE_DIRENT 0
#endif

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define S(a) ((char *)(xrc_mem + (u32)(a)))

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183
#define INVALID_HANDLE 0xffffffffu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 0x102u
#define INFINITE 0xffffffffu

static void set_err(u32 e) { GF(Teb, xrc_teb, last_error) = e; }

/* ============================================================================================ objects */
enum { O_FREE, O_FILE, O_MAPPING, O_FIND, O_MUTEX, O_EVENT, O_SEMAPHORE, O_THREAD };
/* a thread blocked on an object: queued on it; granted = the object was handed to it */
typedef struct WaitOn { struct Obj *o; u32 tid; int granted; struct WaitOn *next; } WaitOn;
typedef struct Obj {
	int kind;
	char name[64];      /* named sync objects (CreateMutexA "..."): opened again by name */
	FILE *f;            /* O_FILE */
	char path[1024];    /* O_FILE / O_MAPPING: host path; O_FIND: the directory */
	char pat[260];      /* O_FIND: the file-name pattern */
	void *dir;          /* O_FIND: DIR * */
	u32 size;           /* O_MAPPING: bytes (0: the file's size) */
	u32 file;           /* O_MAPPING: the file handle (or INVALID_HANDLE: page-file backed) */
	int count, max;     /* O_MUTEX: recursion count (0: free); O_EVENT: signalled; O_SEMAPHORE: count / max */
	int manual;         /* O_EVENT: manual reset */
	struct WaitOn *wq, *wq_tail; /* the threads blocked on it, first come first served (see grant) */
	u32 owner;          /* O_MUTEX: the owning thread id */
	u32 sig_tid, sig_ret; /* XRC_TRACE_WAITS: who signalled it last (thread, guest return address) */
	int thread;         /* O_THREAD: the runtime's thread index */
	int refs;
} Obj;
static void note_signal(void); /* XRC_TRACE_WAITS */
#define MAXOBJ 4096
static Obj objs[MAXOBJ];
#define H_OF(i) (0x1000u + 4u * (u32)(i))
static Obj *obj(u32 h, int kind) {
	u32 i = (h - 0x1000u) / 4u;
	if (h < 0x1000u || (h & 3) || i >= MAXOBJ || objs[i].kind == O_FREE) return 0;
	if (kind && objs[i].kind != kind) return 0;
	return &objs[i];
}
static u32 new_obj(int kind, Obj **out) {
	int i;
	for (i = 0; i < MAXOBJ; i++)
		if (objs[i].kind == O_FREE) {
			memset(&objs[i], 0, sizeof objs[i]);
			objs[i].kind = kind;
			objs[i].refs = 1;
			*out = &objs[i];
			return H_OF(i);
		}
	xrc_fatal(0, "out of kernel object handles");
	return 0;
}
/* a named sync object of `kind`: the existing one (another reference, ERROR_ALREADY_EXISTS) or 0 */
static u32 find_named(int kind, u32 name) {
	int i;
	if (!name || !*S(name)) return 0;
	for (i = 0; i < MAXOBJ; i++)
		if (objs[i].kind == kind && strcmp(objs[i].name, S(name)) == 0) {
			objs[i].refs++;
			set_err(ERROR_ALREADY_EXISTS);
			return H_OF(i);
		}
	return 0;
}

/* ============================================================================================ paths */
static const char *cdrive(void) {
	const char *e = getenv("XRC_CDRIVE");
	return e && *e ? e : "cdrive";
}
/* the guest's current directory (Windows form, no trailing '\') */
static char cwd[1024] = XRC_GUEST_EXE_DIR;

/* an absolute guest path with '\' separators, "." / ".." folded: out = "C:\a\b" */
static void full_guest_path(const char *p, char *out, size_t n) {
	char tmp[2048];
	char *parts[256];
	int np = 0, k;
	char *s, *save;
	if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':') {
		snprintf(tmp, sizeof tmp, "%s", p);
	} else if (p[0] == '\\' || p[0] == '/') {
		snprintf(tmp, sizeof tmp, "C:%s", p);
	} else {
		snprintf(tmp, sizeof tmp, "%s\\%s", cwd, p);
	}
	for (s = tmp; *s; s++) if (*s == '/') *s = '\\';
	s = strtok_r(tmp + 2, "\\", &save);
	while (s) {
		if (strcmp(s, "..") == 0) { if (np) np--; }
		else if (strcmp(s, ".") != 0 && *s) { if (np < 256) parts[np++] = s; }
		s = strtok_r(0, "\\", &save);
	}
	snprintf(out, n, "%c:", tmp[0] & ~32);
	for (k = 0; k < np; k++) { strncat(out, "\\", n - strlen(out) - 1); strncat(out, parts[k], n - strlen(out) - 1); }
	if (np == 0) strncat(out, "\\", n - strlen(out) - 1);
}

/* the host path of a guest path; components matched case-insensitively against what exists (a missing tail keeps
 * the guest's spelling, so files can be created). Returns 0 for a drive other than C:. */
static int host_path(const char *guest, char *out, size_t n) {
	char g[1024], *s, *save;
	full_guest_path(guest, g, sizeof g);
	if (g[0] != 'C') return 0;
	snprintf(out, n, "%s", cdrive());
	s = strtok_r(g + 2, "\\", &save);
	/* XRC_GAME_DIR: where C:\FINAL FANTASY XIII is (the install, wherever it is: the launcher's way, no link needed) */
	{
		const char *gd = getenv("XRC_GAME_DIR");
		if (gd && *gd && s && xrc_ieq(s, XRC_GAME_FOLDER)) {
			snprintf(out, n, "%s", gd);
			s = strtok_r(0, "\\", &save);
		}
	}
	while (s) {
		size_t l = strlen(out);
		int found = 0;
#if HAVE_DIRENT
		DIR *d = opendir(out);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d)) != 0)
				if (xrc_ieq(e->d_name, s)) { snprintf(out + l, n - l, "/%s", e->d_name); found = 1; break; }
			closedir(d);
		}
#endif
		if (!found) snprintf(out + l, n - l, "/%s", s);
		s = strtok_r(0, "\\", &save);
	}
	return 1;
}

static unsigned long long ft_of(time_t t) { return (unsigned long long)t * 10000000ull + 116444736000000000ull; }
static void put_ft(u32 p, unsigned long long ft) { if (p) { GF(FileTime, p, lo) = (u32)ft; GF(FileTime, p, hi) = (u32)(ft >> 32); } }

/* ============================================================================================ sync objects */
static void hle_CreateMutexA(XCtx *c) {
	Obj *o;
	u32 h = find_named(O_MUTEX, A(2));
	if (h) RET(h, 3);
	h = new_obj(O_MUTEX, &o);
	if (A(2)) snprintf(o->name, sizeof o->name, "%s", S(A(2)));
	o->count = A(1) ? 1 : 0;
	o->owner = A(1) ? xrc_thread_id() : 0;
	set_err(0);
	RET(h, 3);
}
/* Windows hands a signalled object to the threads already waiting for it, longest waiter first, before anyone else
 * can take it: a released mutex is owned by the first waiter at once (the releaser's next wait blocks), a semaphore's
 * count goes to waiters, an auto-reset event releases exactly one, a manual-reset event all of them (even if it is
 * reset before they run). Without it a thread that releases and re-waits in a loop (FF13's main thread on its frame
 * mutex) keeps the object for seconds. */
static void grant(Obj *o) {
	for (;;) {
		WaitOn *w = o->wq;
		if (!w) break;
		if (o->kind == O_MUTEX) {
			if (o->count) break;
			o->owner = w->tid; o->count = 1;
		} else if (o->kind == O_SEMAPHORE) {
			if (o->count <= 0) break;
			o->count--;
		} else if (o->kind == O_EVENT) {
			if (!o->count) break;
			if (!o->manual) o->count = 0;
		} else break;
		w->granted = 1;
		o->wq = w->next;
		if (!o->wq) o->wq_tail = 0;
	}
	xrc_wake((uintptr_t)o);
}
static void hle_ReleaseMutex(XCtx *c) {
	Obj *o = obj(A(0), O_MUTEX);
	if (!o || o->count == 0 || o->owner != xrc_thread_id()) { set_err(288 /* ERROR_NOT_OWNER */); RET(0, 1); }
	if (--o->count == 0) { o->owner = 0; o->sig_tid = xrc_thread_id(); o->sig_ret = xrc_stack_at(c, 1); note_signal(); grant(o); }
	RET(1, 1);
}
static void hle_CreateEventA(XCtx *c) {
	Obj *o;
	u32 h = find_named(O_EVENT, A(3));
	if (h) RET(h, 4);
	h = new_obj(O_EVENT, &o);
	if (A(3)) snprintf(o->name, sizeof o->name, "%s", S(A(3)));
	o->manual = A(1) != 0;
	o->count = A(2) != 0;
	set_err(0);
	RET(h, 4);
}
static void hle_SetEvent(XCtx *c) { Obj *o = obj(A(0), O_EVENT); if (o) { o->count = 1; o->sig_tid = xrc_thread_id(); o->sig_ret = xrc_stack_at(c, 1); note_signal(); grant(o); } RET(o != 0, 1); }
static void hle_ResetEvent(XCtx *c) { Obj *o = obj(A(0), O_EVENT); if (o) o->count = 0; RET(o != 0, 1); }
static void hle_CreateSemaphoreA(XCtx *c) {
	Obj *o;
	u32 h = find_named(O_SEMAPHORE, A(3));
	if (h) RET(h, 4);
	h = new_obj(O_SEMAPHORE, &o);
	if (A(3)) snprintf(o->name, sizeof o->name, "%s", S(A(3)));
	o->count = (int)A(1);
	o->max = (int)A(2);
	set_err(0);
	RET(h, 4);
}
static void hle_ReleaseSemaphore(XCtx *c) {
	Obj *o = obj(A(0), O_SEMAPHORE);
	if (!o || o->count + (int)A(1) > o->max) { set_err(298 /* ERROR_TOO_MANY_POSTS */); RET(0, 3); }
	if (A(2)) M32(A(2)) = (u32)o->count;
	o->count += (int)A(1);
	o->sig_tid = xrc_thread_id(); o->sig_ret = xrc_stack_at(c, 1); note_signal();
	grant(o);
	RET(1, 3);
}
/* takes the object if it is signalled (the wait's side effect: an auto-reset event resets, a semaphore counts down,
 * a mutex is owned) */
static int try_take(Obj *o) {
	switch (o->kind) {
	case O_MUTEX:
		if (o->count && o->owner != xrc_thread_id()) return 0;
		o->owner = xrc_thread_id();
		o->count++;
		return 1;
	case O_EVENT:
		if (!o->count) return 0;
		if (!o->manual) o->count = 0;
		return 1;
	case O_SEMAPHORE:
		if (o->count <= 0) return 0;
		o->count--;
		return 1;
	case O_THREAD: return xrc_thread_done(o->thread, 0);
	default: return 1; /* files ...: signalled */
	}
}
static int ready_take(void *p) {
	WaitOn *w = (WaitOn *)p;
	return w->granted || (w->o->kind == O_THREAD && try_take(w->o));
}
static void unqueue(WaitOn *w) {
	WaitOn **pp, *prev = 0;
	for (pp = &w->o->wq; *pp; prev = *pp, pp = &(*pp)->next)
		if (*pp == w) {
			*pp = w->next;
			if (w->o->wq_tail == w) w->o->wq_tail = prev;
			return;
		}
}
static int never(void *p) { (void)p; return 0; }
/* XRC_TRACE_WAITS: the blocking waits (thread, handle, timeout, guest caller) with their time and who ended them
 * (the signaller's thread and return address, or a timeout); xrc_wait_report lists the longest per frame */
typedef struct WaitAcct { u32 tid, h, ms, ret, ret2, kind, sig_tid, sig_ret; unsigned long long n, us, nto, us0, n0, gap, gap0; } WaitAcct;
#define NWACCT 512
static WaitAcct wacct[NWACCT];
static int nwacct, trace_waits = -1;
static unsigned long long w_now(void);
static unsigned long long last_sig_us[64]; /* XRC_TRACE_WAITS: each thread's last signal (us) */
static void note_signal(void) {
	int ti;
	if (trace_waits < 0) trace_waits = xrc_tool_env("XRC_TRACE_WAITS") != 0;
	if (!trace_waits) return;
	ti = xrc_thread_index();
	if (ti >= 0 && ti < 64) last_sig_us[ti] = w_now();
}
static unsigned long long w_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}
static void wait_acct(XCtx *c, u32 h, u32 ms, Obj *o, int r, unsigned long long us, unsigned long long t0) {
	u32 tid = xrc_thread_id(), ret = xrc_stack_at(c, 1), ret2 = xrc_stack_at(c, 2), st = r ? o->sig_tid : 0, sr = r ? o->sig_ret : 0;
	int k;
	for (k = 0; k < nwacct; k++) {
		WaitAcct *w = &wacct[k];
		if (w->tid == tid && w->h == h && w->ret == ret && w->ms == ms && w->sig_tid == st && w->sig_ret == sr) break;
	}
	if (k == nwacct) {
		if (nwacct == NWACCT) return;
		nwacct++;
		wacct[k].tid = tid; wacct[k].h = h; wacct[k].ret = ret; wacct[k].ret2 = ret2; wacct[k].ms = ms; wacct[k].kind = (u32)o->kind;
		wacct[k].sig_tid = st; wacct[k].sig_ret = sr;
	}
	wacct[k].n++; wacct[k].us += us;
	{ int ti = xrc_thread_index(); if (ti >= 0 && ti < 64 && last_sig_us[ti] && t0 > last_sig_us[ti]) wacct[k].gap += t0 - last_sig_us[ti]; }
	if (!r) wacct[k].nto++;
}
void xrc_wait_report(FILE *f, unsigned long long nframes) {
	int k, j, top[64], n = 0;
	if (!nframes) nframes = 1;
	for (k = 0; k < nwacct; k++) {
		unsigned long long d = wacct[k].us - wacct[k].us0;
		for (j = 0; j < n && d <= wacct[top[j]].us - wacct[top[j]].us0; j++) {}
		if (j < 64 && d) {
			if (n < 64) n++;
			memmove(top + j + 1, top + j, (size_t)(n - 1 - j) * sizeof *top);
			top[j] = k;
		}
	}
	if (f) {
		fprintf(f, "xrc:   blocking waits, ms/frame (thread handle kind timeout caller -> ended by; gap: average us from the waiter's own last signal to the wait):\n");
		for (j = 0; j < n; j++) {
			WaitAcct *w = &wacct[top[j]];
			fprintf(f, "xrc:     %7.2f ms %6.1f/frame  thread %x  h %x kind %u  %d ms  in %08x < %08x -> %s %x in %08x  gap %.0f us\n",
			        (double)(w->us - w->us0) / 1000.0 / (double)nframes, (double)(w->n - w->n0) / (double)nframes, w->tid, w->h,
			        w->kind, (int)w->ms, w->ret, w->ret2, w->sig_tid ? "signalled by thread" : (w->nto ? "timeout" : "free"), w->sig_tid, w->sig_ret,
			        w->n > w->n0 ? (double)(w->gap - w->gap0) / (double)(w->n - w->n0) : 0.0);
		}
	}
	for (k = 0; k < nwacct; k++) { wacct[k].us0 = wacct[k].us; wacct[k].n0 = wacct[k].n; wacct[k].gap0 = wacct[k].gap; }
}
static void hle_WaitForSingleObject(XCtx *c) {
	u32 h = A(0), ms = A(1);
	Obj *o = obj(h, 0);
	if (h == 0xfffffffeu || h == 0xffffffffu) { /* the current thread / process: never signalled while running */
		if (ms == INFINITE) xrc_fatal(c, "WaitForSingleObject on the current thread / process, INFINITE");
		XRC_WAIT_NOTE("WaitForSingleObject (itself: a sleep)", h);
		xrc_block_wait(never, 0, ms);
		RET(WAIT_TIMEOUT, 2);
	}
	if (!o) { set_err(ERROR_INVALID_HANDLE); RET(0xffffffffu, 2); }
	{
		uintptr_t key = (uintptr_t)o;
		int r;
		/* free and nobody before us, or a mutex this thread owns already (recursion) */
		if ((!o->wq || (o->kind == O_MUTEX && o->count && o->owner == xrc_thread_id())) && try_take(o)) r = 1;
		else if (ms == 0) r = 0;
		else {
			WaitOn w;
			w.o = o; w.tid = xrc_thread_id(); w.granted = 0; w.next = 0;
			if (o->kind != O_THREAD) {
				if (o->wq_tail) o->wq_tail->next = &w; else o->wq = &w;
				o->wq_tail = &w;
			}
			unsigned long long t0 = 0;
			if (trace_waits < 0) trace_waits = xrc_tool_env("XRC_TRACE_WAITS") != 0;
			if (trace_waits) t0 = w_now();
			XRC_WAIT_NOTE(o->kind == O_EVENT ? "WaitForSingleObject (event)" : o->kind == O_SEMAPHORE ? "WaitForSingleObject (semaphore)" : o->kind == O_MUTEX ? "WaitForSingleObject (mutex)" : o->kind == O_THREAD ? "WaitForSingleObject (thread)" : "WaitForSingleObject", h);
			r = xrc_block_wait_keys(&key, 1, ready_take, &w, ms);
			if (!w.granted && o->kind != O_THREAD) unqueue(&w);
			if (trace_waits) wait_acct(c, h, ms, o, r, w_now() - t0, t0);
		}
		RET(r ? WAIT_OBJECT_0 : WAIT_TIMEOUT, 2);
	}
}
/* CreateThread(attr, stack, start, param, flags, *tid) */
static void hle_CreateThread(XCtx *c) {
	Obj *o;
	u32 h, tid = 0;
	int t;
	if (A(4) & 4) xrc_fatal(c, "CreateThread(CREATE_SUSPENDED): no ResumeThread import, not expected");
	t = xrc_thread_create(A(2), A(3), A(1), &tid);
	if (!t) { set_err(8 /* ERROR_NOT_ENOUGH_MEMORY */); RET(0, 6); }
	h = new_obj(O_THREAD, &o);
	o->thread = t;
	if (A(5)) M32(A(5)) = tid;
	RET(h, 6);
}
/* OpenThread(access, inherit, tid) */
static void hle_OpenThread(XCtx *c) {
	Obj *o;
	u32 h;
	int t = xrc_thread_by_id(A(2));
	if (t < 0) { set_err(87); RET(0, 3); }
	h = new_obj(O_THREAD, &o);
	o->thread = t;
	RET(h, 3);
}
static void hle_ExitThread(XCtx *c) { xrc_thread_exit(A(0)); }

/* ============================================================================================ files */
#define H_STDIN 0x10u
#define H_STDOUT 0x14u
#define H_STDERR 0x18u
static FILE *file_of(u32 h) {
	Obj *o;
	if (h == H_STDIN) return stdin;
	if (h == H_STDOUT) return stdout;
	if (h == H_STDERR) return stderr;
	o = obj(h, O_FILE);
	return o ? o->f : 0;
}
static int is_dir(const char *hp) { struct stat st; return stat(hp, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR; }
static int exists(const char *hp) { struct stat st; return stat(hp, &st) == 0; }

/* XRC_IO_STATS=1 (tools builds, off by default): ReadFile / SetFilePointer / CreateFileA / MapViewOfFile cost, per file and per
 * thread: count, bytes, host time in fread (us), GIL re-acquire wait after a big read (us). Dumped to stderr every
 * XRC_IO_STATS_S seconds (default 5), cumulative, so a killed run still has it. */
#ifndef XRC_NO_TOOLS
typedef struct { char name[48]; unsigned long long rd, bytes, rd_us, gilw_us, small, seeks, opens, open_us, maps, map_bytes, map_us; } IoSt;
static IoSt io_f[256], io_t[64];
static int io_n, io_tn, io_on = -1;
static unsigned long long io_last, io_t0, io_period = 5000000;
static IoSt *io_slot(IoSt *tab, int *n, int cap, const char *nm) {
	int i;
	for (i = 0; i < *n; i++) if (!strcmp(tab[i].name, nm)) return &tab[i];
	if (*n >= cap) return &tab[cap - 1];
	snprintf(tab[*n].name, sizeof tab[*n].name, "%s", nm);
	return &tab[(*n)++];
}
static const char *io_base(const char *p) { const char *b = strrchr(p, '/'); const char *b2 = strrchr(p, '\\'); if (b2 > b) b = b2; return b ? b + 1 : p; }
static int io_enabled(void) { if (io_on < 0) { const char *e = getenv("XRC_IO_STATS"), *s = getenv("XRC_IO_STATS_S"); io_on = e && *e && *e != '0'; if (s && atoi(s) > 0) io_period = (unsigned long long)atoi(s) * 1000000ull; io_t0 = io_last = xrc_now_us(); } return io_on; }
static void io_dump(int final);
static void io_rec(const char *path, int op, unsigned long long bytes, unsigned long long us, unsigned long long gw) {
	char tn[16];
	IoSt *a, *b;
	unsigned long long now;
	const char *nm = io_base(path);
	snprintf(tn, sizeof tn, "%04x", xrc_thread_id());
	a = io_slot(io_f, &io_n, 256, nm); b = io_slot(io_t, &io_tn, 64, tn);
	if (op == 0) { a->rd++; b->rd++; a->bytes += bytes; b->bytes += bytes; a->rd_us += us; b->rd_us += us; a->gilw_us += gw; b->gilw_us += gw; if (bytes < 4096) { a->small++; b->small++; } }
	else if (op == 1) { a->seeks++; b->seeks++; }
	else if (op == 2) { a->opens++; b->opens++; a->open_us += us; b->open_us += us; }
	else { a->maps++; b->maps++; a->map_bytes += bytes; b->map_bytes += bytes; a->map_us += us; b->map_us += us; }
	now = xrc_now_us();
	if (now - io_last >= io_period) { io_last = now; io_dump(0); }
}
static unsigned long long io_cost(const IoSt *s) { return s->rd_us + s->open_us + s->map_us; }
static void io_print(const char *title, IoSt *tab, int n, int top) {
	int i, j, k, order[256];
	for (i = 0; i < n; i++) order[i] = i;
	for (i = 1; i < n; i++) for (j = i; j > 0 && io_cost(&tab[order[j]]) > io_cost(&tab[order[j - 1]]); j--) { k = order[j]; order[j] = order[j - 1]; order[j - 1] = k; }
	fprintf(stderr, "xrc io-stats: %s (reads/small<4K/MB/read_ms/gilwait_ms | seeks | opens/open_ms | maps/MB/map_ms)\n", title);
	for (i = 0; i < n && i < top; i++) {
		IoSt *s = &tab[order[i]];
		fprintf(stderr, "xrc io-stats:   %-40s %7llu/%6llu/%8.1f/%8.1f/%7.1f | %6llu | %4llu/%7.1f | %3llu/%7.1f/%7.1f\n", s->name, s->rd, s->small, s->bytes / 1048576.0,
		        s->rd_us / 1000.0, s->gilw_us / 1000.0, s->seeks, s->opens, s->open_us / 1000.0, s->maps, s->map_bytes / 1048576.0, s->map_us / 1000.0);
	}
}
static void io_dump(int final) {
	IoSt tot;
	int i;
	memset(&tot, 0, sizeof tot);
	for (i = 0; i < io_n; i++) { tot.rd += io_f[i].rd; tot.small += io_f[i].small; tot.bytes += io_f[i].bytes; tot.rd_us += io_f[i].rd_us; tot.gilw_us += io_f[i].gilw_us; tot.seeks += io_f[i].seeks; tot.opens += io_f[i].opens; tot.open_us += io_f[i].open_us; tot.maps += io_f[i].maps; tot.map_bytes += io_f[i].map_bytes; tot.map_us += io_f[i].map_us; }
	fprintf(stderr, "xrc io-stats: ---- t=%.1fs cumulative%s: reads %llu (small %llu) %.1f MB in %.1f ms, gil-wait %.1f ms, seeks %llu, opens %llu (%.1f ms), maps %llu (%.1f MB, %.1f ms)\n",
	        (xrc_now_us() - io_t0) / 1e6, final ? " FINAL" : "", tot.rd, tot.small, tot.bytes / 1048576.0, tot.rd_us / 1000.0, tot.gilw_us / 1000.0, tot.seeks, tot.opens, tot.open_us / 1000.0, tot.maps, tot.map_bytes / 1048576.0, tot.map_us / 1000.0);
	io_print("top files by host time", io_f, io_n, 12);
	io_print("threads", io_t, io_tn, 8);
}
#define IOS_ON() io_enabled()
#define IOS_T() xrc_now_us()
#else
#define IOS_ON() 0
#define IOS_T() 0ull
#define io_rec(...) ((void)0)
#endif
char xrc_last_open_path[1024];
int xrc_host_path(const char *guest, char *out, size_t n) { return host_path(guest, out, n); }
static void hle_CreateFileA(XCtx *c) {
	char hp[1024];
	u32 access = A(1), disp = A(4);
	int w = (access & 0x40000000u) != 0 || (access & 0x10000000u) != 0, r = (access & 0x80000000u) != 0 || !w;
	int ex;
	const char *mode;
	FILE *f;
	Obj *o;
	u32 h;
	unsigned long long ios_o0 = 0;
	if (!host_path(S(A(0)), hp, sizeof hp)) { set_err(ERROR_PATH_NOT_FOUND); RET(INVALID_HANDLE, 7); }
	if (xrc_tool_env("XRC_TRACE_FILES")) fprintf(stderr, "xrc: CreateFileA(\"%s\" -> %s, %s%s, disp %u)\n", S(A(0)), hp, r ? "r" : "", w ? "w" : "", disp);
	ios_o0 = IOS_ON() ? IOS_T() : 0;
	ex = exists(hp);
	if (ex && is_dir(hp)) { set_err(ERROR_ACCESS_DENIED); RET(INVALID_HANDLE, 7); }
	switch (disp) {
	case 1: /* CREATE_NEW */ if (ex) { set_err(ERROR_FILE_EXISTS); RET(INVALID_HANDLE, 7); } mode = r ? "w+b" : "wb"; break;
	case 2: /* CREATE_ALWAYS */ mode = r ? "w+b" : "wb"; break;
	case 3: /* OPEN_EXISTING */ if (!ex) { set_err(ERROR_FILE_NOT_FOUND); RET(INVALID_HANDLE, 7); } mode = w ? "r+b" : "rb"; break;
	case 4: /* OPEN_ALWAYS */ mode = ex ? (w ? "r+b" : "rb") : (r ? "w+b" : "wb"); break;
	case 5: /* TRUNCATE_EXISTING */ if (!ex) { set_err(ERROR_FILE_NOT_FOUND); RET(INVALID_HANDLE, 7); } mode = r ? "w+b" : "wb"; break;
	default: set_err(87); RET(INVALID_HANDLE, 7);
	}
	if (xrc_gil_held()) { xrc_gil_release(); f = fopen(hp, mode); xrc_gil_acquire(); } /* (an open an antivirus scans can take long) */
	else f = fopen(hp, mode);
	if (ios_o0) io_rec(hp, 2, 0, IOS_T() - ios_o0, 0);
	if (!f) { set_err(ex ? ERROR_ACCESS_DENIED : ERROR_PATH_NOT_FOUND); RET(INVALID_HANDLE, 7); }
	h = new_obj(O_FILE, &o);
	o->f = f;
	snprintf(o->path, sizeof o->path, "%s", hp);
	snprintf(xrc_last_open_path, sizeof xrc_last_open_path, "%s", hp);
	set_err((disp == 2 || disp == 4) && ex ? ERROR_ALREADY_EXISTS : 0);
	RET(h, 7);
}
/* XRC_TRACE_IO (tools builds): each ReadFile / SetFilePointer(Ex) on a file: thread, file, offset, size (asked / got)
 * and the guest callers */
#ifndef XRC_NO_TOOLS
static int trace_io = -1;
static void io_trace(XCtx *c, const char *op, u32 h, long long off, long long a, long long b) {
	Obj *o;
	const char *nm;
	if (trace_io < 0) trace_io = xrc_tool_env("XRC_TRACE_IO") != 0;
	if (!trace_io) return;
	o = obj(h, O_FILE);
	nm = o ? strrchr(o->path, '/') : 0;
	nm = nm ? nm + 1 : o ? o->path : "?";
	fprintf(stderr, "xrc io: thread %x %s %s @%lld %lld %lld  < %08x %08x %08x %08x\n", xrc_thread_id(), op, nm, off, a, b,
	        xrc_stack_at(c, 1), xrc_stack_at(c, 2), xrc_stack_at(c, 3), xrc_stack_at(c, 4));
}
#define IO_TRACE(...) io_trace(__VA_ARGS__)
#else
#define IO_TRACE(...) ((void)0)
#endif
static void hle_ReadFile(XCtx *c) {
	FILE *f = file_of(A(0));
	size_t n;
	unsigned long long ios_t0 = 0, ios_t1 = 0, ios_t2 = 0;
	int ios = IOS_ON();
	if (!f) { set_err(ERROR_INVALID_HANDLE); RET(0, 5); }
	if (A(4)) xrc_fatal(c, "ReadFile with OVERLAPPED is not implemented");
	if (ios) ios_t0 = IOS_T();
	/* (the GIL let go around a big read: a slow disk -- an antivirus scan, another program's I/O -- held every
	 * guest thread behind one read, timers and audio included: 2-4 fps at the logo and clicking sound) */
	if (A(2) >= 4096 && xrc_gil_held()) {
		xrc_gil_release();
		n = fread(xrc_mem + A(1), 1, A(2), f);
		if (ios) ios_t1 = IOS_T();
		xrc_gil_acquire();
		if (ios) ios_t2 = IOS_T();
	} else n = fread(xrc_mem + A(1), 1, A(2), f);
	if (ios) {
		Obj *o = obj(A(0), O_FILE);
		if (!ios_t1) ios_t1 = ios_t2 = IOS_T();
		io_rec(o ? o->path : "?", 0, n, ios_t1 - ios_t0, ios_t2 - ios_t1);
	}
	IO_TRACE(c, "read", A(0), (long long)ftello(f) - (long long)n, (long long)A(2), (long long)n);
	{ /* (diagnostic: a thread reading the same handle at EOF over and over -- the boot stall's read loop) */
		static u32 zh, zt; static unsigned zn;
		if (n == 0 && A(2)) {
			if (A(0) == zh && zt == xrc_thread_id()) zn++; else { zh = A(0); zt = xrc_thread_id(); zn = 1; }
			if (zn == 20000) {
				Obj *o = obj(A(0), O_FILE);
				long long pos = (long long)_ftelli64(f), len = (long long)_filelengthi64(_fileno(f));
				fprintf(stderr, "xrc: ReadFile: thread %x read 0 bytes 20000 times from handle %x (%s): asked %u at %lld, file %lld bytes, eof %d err %d\n",
				        zt, zh, o ? o->path : "?", A(2), pos, len, feof(f), ferror(f));
				xrc_print_stack();
			}
		} else if (A(0) == zh) zn = 0;
	}
	/* a read error (not the end of the file) fails as Windows' ReadFile does: the game's read loop (00747c50) ends
	 * only on a failure or a full count, and a TRUE with 0 bytes made it spin forever */
	if (n < A(2) && ferror(f)) {
		static int said;
		if (said++ < 8) {
			Obj *o = obj(A(0), O_FILE);
			fprintf(stderr, "xrc: ReadFile: read error on handle %x (%s): %u of %u bytes into %08x, errno %d\n",
			        A(0), o ? o->path : "?", (unsigned)n, A(2), A(1), errno);
		}
		clearerr(f);
		if (A(3)) M32(A(3)) = (u32)n;
		set_err(998 /* ERROR_NOACCESS */);
		RET(0, 5);
	}
	if (A(3)) M32(A(3)) = (u32)n;
	RET(1, 5);
}
static void hle_WriteFile(XCtx *c) {
	FILE *f = file_of(A(0));
	size_t n;
	if (!f) { set_err(ERROR_INVALID_HANDLE); RET(0, 5); }
	/* (a big write without the GIL, as ReadFile: a full disk cache / an antivirus scan can stall it; the guest's buffer is
	 * this thread's own while it writes, as on Windows) */
	if (A(2) >= 4096 && f != stdout && f != stderr && xrc_gil_held()) {
		xrc_gil_release();
		n = fwrite(xrc_mem + A(1), 1, A(2), f);
		xrc_gil_acquire();
	} else n = fwrite(xrc_mem + A(1), 1, A(2), f);
	if (A(3)) M32(A(3)) = (u32)n;
	RET(1, 5);
}
static long long seek(FILE *f, long long off, u32 method) {
	int wh = method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : SEEK_SET;
#ifdef _WIN32
	if (_fseeki64(f, off, wh) != 0) return -1;
	return _ftelli64(f);
#else
	if (fseeko(f, (off_t)off, wh) != 0) return -1;
	return (long long)ftello(f);
#endif
}
static void hle_SetFilePointer(XCtx *c) {
	FILE *f = file_of(A(0));
	long long off = (long long)(s32)A(1), r;
	if (A(2)) off = (long long)(((u64)M32(A(2)) << 32) | A(1));
	if (!f) { set_err(ERROR_INVALID_HANDLE); RET(0xffffffffu, 4); }
	r = seek(f, off, A(3));
	if (IOS_ON()) { Obj *o = obj(A(0), O_FILE); io_rec(o ? o->path : "?", 1, 0, 0, 0); }
	IO_TRACE(c, "seek", A(0), r, off, (long long)A(3));
	if (r < 0) { set_err(131 /* ERROR_NEGATIVE_SEEK */); RET(0xffffffffu, 4); }
	if (A(2)) M32(A(2)) = (u32)((u64)r >> 32);
	set_err(0);
	RET((u32)r, 4);
}
static void hle_SetFilePointerEx(XCtx *c) {
	FILE *f = file_of(A(0));
	long long r;
	if (!f) { set_err(ERROR_INVALID_HANDLE); RET(0, 5); }
	r = seek(f, (long long)(((u64)A(2) << 32) | A(1)), A(4));
	if (IOS_ON()) { Obj *o = obj(A(0), O_FILE); io_rec(o ? o->path : "?", 1, 0, 0, 0); }
	IO_TRACE(c, "seekx", A(0), r, (long long)(((u64)A(2) << 32) | A(1)), (long long)A(4));
	if (r < 0) { set_err(131); RET(0, 5); }
	if (A(3)) { GF(LargeInteger, A(3), lo) = (u32)r; GF(LargeInteger, A(3), hi) = (u32)((u64)r >> 32); }
	RET(1, 5);
}
static void hle_SetEndOfFile(XCtx *c) {
	FILE *f = file_of(A(0));
	long long pos;
	if (!f) { set_err(ERROR_INVALID_HANDLE); RET(0, 1); }
	fflush(f);
	pos = seek(f, 0, 1);
#ifdef _WIN32
	RET(_chsize_s(_fileno(f), pos) == 0, 1);
#else
	RET(ftruncate(fileno(f), (off_t)pos) == 0, 1);
#endif
}
static void hle_FlushFileBuffers(XCtx *c) { /* (no guest memory involved: without the GIL) */
	FILE *f = file_of(A(0));
	if (f) { int rel = xrc_gil_held(); if (rel) xrc_gil_release(); fflush(f); if (rel) xrc_gil_acquire(); }
	RET(f != 0, 1);
}
static void hle_GetFileType(XCtx *c) {
	u32 h = A(0);
	if (h == H_STDIN || h == H_STDOUT || h == H_STDERR) RET(2 /* FILE_TYPE_CHAR */, 1);
	RET(obj(h, O_FILE) ? 1 /* FILE_TYPE_DISK */ : 0, 1);
}
static void hle_GetFileTime(XCtx *c) {
	Obj *o = obj(A(0), O_FILE);
	struct stat st;
	if (!o || stat(o->path, &st) != 0) { set_err(ERROR_INVALID_HANDLE); RET(0, 4); }
	put_ft(A(1), ft_of(st.st_mtime));
	put_ft(A(2), ft_of(st.st_atime));
	put_ft(A(3), ft_of(st.st_mtime));
	RET(1, 4);
}
static void hle_CloseHandle(XCtx *c) {
	u32 h = A(0);
	Obj *o = obj(h, 0);
	if (h == H_STDIN || h == H_STDOUT || h == H_STDERR || h == 0xfffffffeu || h == 0xffffffffu) RET(1, 1);
	if (!o) { set_err(ERROR_INVALID_HANDLE); RET(0, 1); }
	if (--o->refs > 0) RET(1, 1);
	if (o->kind == O_FILE && o->f) fclose(o->f);
#if HAVE_DIRENT
	if (o->kind == O_FIND && o->dir) closedir((DIR *)o->dir);
#endif
	o->kind = O_FREE;
	RET(1, 1);
}
static void hle_DeleteFileA(XCtx *c) {
	char hp[1024];
	if (!host_path(S(A(0)), hp, sizeof hp) || remove(hp) != 0) { set_err(ERROR_FILE_NOT_FOUND); RET(0, 1); }
	RET(1, 1);
}
static void hle_CreateDirectoryA(XCtx *c) {
	char hp[1024];
	if (!host_path(S(A(0)), hp, sizeof hp)) { set_err(ERROR_PATH_NOT_FOUND); RET(0, 2); }
	if (exists(hp)) { set_err(ERROR_ALREADY_EXISTS); RET(0, 2); }
	if (mkdir_(hp) != 0) { set_err(ERROR_PATH_NOT_FOUND); RET(0, 2); }
	RET(1, 2);
}
static void hle_GetFileAttributesA(XCtx *c) {
	char hp[1024];
	struct stat st;
	if (!host_path(S(A(0)), hp, sizeof hp) || stat(hp, &st) != 0) { set_err(ERROR_FILE_NOT_FOUND); RET(0xffffffffu, 1); }
	RET((st.st_mode & S_IFMT) == S_IFDIR ? 0x10u /* DIRECTORY */ : 0x80u /* NORMAL */, 1);
}
static void hle_GetDriveTypeA(XCtx *c) { RET(A(0) && (*S(A(0)) | 32) != 'c' ? 1 /* NO_ROOT_DIR */ : 3 /* FIXED */, 1); }
static void hle_GetDiskFreeSpaceExA(XCtx *c) {
	const u64 g = 64ull << 30; /* report 64 GB free of 256 GB */
	if (A(1)) { GF(LargeInteger, A(1), lo) = (u32)g; GF(LargeInteger, A(1), hi) = (u32)(g >> 32); }
	if (A(2)) { GF(LargeInteger, A(2), lo) = 0; GF(LargeInteger, A(2), hi) = (u32)((g * 4) >> 32); }
	if (A(3)) { GF(LargeInteger, A(3), lo) = (u32)g; GF(LargeInteger, A(3), hi) = (u32)(g >> 32); }
	RET(1, 4);
}
static u32 put_str(u32 buf, u32 cap, const char *s) {
	u32 n = (u32)strlen(s);
	if (n + 1 > cap) return n + 1; /* the size needed, with the terminator */
	memcpy(xrc_mem + buf, s, n + 1);
	return n;
}
static void hle_GetCurrentDirectoryA(XCtx *c) { RET(put_str(A(1), A(0), cwd), 2); }
static void hle_SetCurrentDirectoryA(XCtx *c) {
	char g[1024], hp[1024];
	full_guest_path(S(A(0)), g, sizeof g);
	if (!host_path(g, hp, sizeof hp) || !is_dir(hp)) { set_err(ERROR_PATH_NOT_FOUND); RET(0, 1); }
	if (strlen(g) > 3 && g[strlen(g) - 1] == '\\') g[strlen(g) - 1] = 0;
	snprintf(cwd, sizeof cwd, "%s", g);
	RET(1, 1);
}
static void hle_GetFullPathNameA(XCtx *c) {
	char g[1024];
	u32 n;
	full_guest_path(S(A(0)), g, sizeof g);
	if (xrc_tool_env("XRC_TRACE_FILES")) fprintf(stderr, "xrc: GetFullPathNameA(%s) -> %s\n", S(A(0)), g);
	n = put_str(A(2), A(1), g);
	if (A(3) && n < A(1)) {
		char *last = strrchr(S(A(2)), '\\');
		M32(A(3)) = last && last[1] ? A(2) + (u32)(last + 1 - S(A(2))) : 0;
	}
	RET(n, 4);
}
static void hle_GetSystemDirectoryA(XCtx *c) { RET(put_str(A(0), A(1), "C:\\WINDOWS\\system32"), 2); }
static void hle_FileTimeToLocalFileTime(XCtx *c) { GF(FileTime, A(1), lo) = GF(FileTime, A(0), lo); GF(FileTime, A(1), hi) = GF(FileTime, A(0), hi); RET(1, 2); } /* UTC */
static void hle_FileTimeToSystemTime(XCtx *c) {
	u64 ft = ((u64)GF(FileTime, A(0), hi) << 32) | GF(FileTime, A(0), lo);
	time_t t = (time_t)((ft - 116444736000000000ull) / 10000000ull);
	struct tm tmv, *tm;
	u32 p = A(1);
#ifdef _WIN32
	tm = gmtime_s(&tmv, &t) == 0 ? &tmv : 0;
#else
	tm = gmtime_r(&t, &tmv);
#endif
	if (!tm) RET(0, 2);
	GF(SystemTime, p, year) = (u16)(tm->tm_year + 1900); GF(SystemTime, p, month) = (u16)(tm->tm_mon + 1); GF(SystemTime, p, day_of_week) = (u16)tm->tm_wday;
	GF(SystemTime, p, day) = (u16)tm->tm_mday; GF(SystemTime, p, hour) = (u16)tm->tm_hour; GF(SystemTime, p, minute) = (u16)tm->tm_min;
	GF(SystemTime, p, second) = (u16)tm->tm_sec; GF(SystemTime, p, milliseconds) = (u16)((ft / 10000ull) % 1000ull);
	RET(1, 2);
}

/* FindFirstFileA / FindNextFileA: one directory, a pattern with * and ? (case-insensitive) */
static int wild(const char *p, const char *s) {
	for (; *p; p++, s++) {
		if (*p == '*') {
			while (p[1] == '*') p++;
			if (!p[1]) return 1;
			for (; *s; s++) if (wild(p + 1, s)) return 1;
			return 0;
		}
		if (!*s) return 0;
		if (*p != '?' && (*p | 32) != (*s | 32)) return 0;
	}
	return *s == 0;
}
/* WIN32_FIND_DATAA: attrs, ftCreate, ftAccess, ftWrite, sizeHigh, sizeLow, res0, res1, cFileName[260], alt[14] */
static int find_next(Obj *o, u32 fd) {
#if !HAVE_DIRENT
	(void)o; (void)fd;
	return 0;
#else
	struct dirent *e;
	while (o->dir && (e = readdir((DIR *)o->dir)) != 0) {
		char hp[2048];
		struct stat st;
		if (!wild(o->pat, e->d_name)) continue;
		snprintf(hp, sizeof hp, "%s/%s", o->path, e->d_name);
		if (stat(hp, &st) != 0) continue;
		memset(xrc_mem + fd, 0, 320);
		GF(Win32FindData, fd, file_attributes) = (st.st_mode & S_IFMT) == S_IFDIR ? 0x10u : 0x80u;
		put_ft(fd + 4, ft_of(st.st_mtime));
		put_ft(fd + 12, ft_of(st.st_atime));
		put_ft(fd + 20, ft_of(st.st_mtime));
		GF(Win32FindData, fd, file_size_high) = (u32)((u64)st.st_size >> 32);
		GF(Win32FindData, fd, file_size_low) = (u32)st.st_size;
		snprintf(S(fd + 44), 260, "%s", e->d_name);
		return 1;
	}
	return 0;
#endif
}
static void hle_FindFirstFileA(XCtx *c) {
	char g[1024], hp[1024], *slash;
	Obj *o;
	u32 h;
	full_guest_path(S(A(0)), g, sizeof g);
	if (xrc_tool_env("XRC_TRACE_FILES")) fprintf(stderr, "xrc: FindFirstFileA(%s) -> %s\n", S(A(0)), g);
	{ const unsigned char *q = (const unsigned char *)S(A(0)); /* a garbage pattern: who passed it */
	  while (*q >= 0x20 && *q < 0x7f) q++;
	  if (*q) { fprintf(stderr, "xrc: FindFirstFileA: bad pattern at %08x (byte %02x)\n", A(0), *q); xrc_print_stack(); } }
	slash = strrchr(g, '\\');
	if (!slash) { set_err(ERROR_PATH_NOT_FOUND); RET(INVALID_HANDLE, 2); }
	*slash = 0;
	if (!host_path(*g && g[strlen(g) - 1] != ':' ? g : "C:\\", hp, sizeof hp) || !is_dir(hp)) { set_err(ERROR_PATH_NOT_FOUND); RET(INVALID_HANDLE, 2); }
	h = new_obj(O_FIND, &o);
	snprintf(o->path, sizeof o->path, "%s", hp);
	snprintf(o->pat, sizeof o->pat, "%s", strcmp(slash + 1, "*.*") == 0 ? "*" : slash + 1); /* *.* is everything */
#if HAVE_DIRENT
	o->dir = opendir(hp);
#endif
	if (!find_next(o, A(1))) {
		o->kind = O_FREE;
#if HAVE_DIRENT
		if (o->dir) closedir((DIR *)o->dir);
#endif
		set_err(ERROR_FILE_NOT_FOUND);
		RET(INVALID_HANDLE, 2);
	}
	RET(h, 2);
}
static void hle_FindNextFileA(XCtx *c) {
	Obj *o = obj(A(0), O_FIND);
	if (!o) { set_err(ERROR_INVALID_HANDLE); RET(0, 2); }
	if (!find_next(o, A(1))) { set_err(ERROR_NO_MORE_FILES); RET(0, 2); }
	RET(1, 2);
}
static void hle_FindClose(XCtx *c) {
	Obj *o = obj(A(0), O_FIND);
	if (!o) RET(0, 1);
#if HAVE_DIRENT
	if (o->dir) closedir((DIR *)o->dir);
#endif
	o->kind = O_FREE;
	RET(1, 1);
}

/* file mappings: a view is a guest heap copy of the file range (read-only use; written-to views are not written back) */
static void hle_CreateFileMappingA(XCtx *c) {
	Obj *o;
	u32 h = new_obj(O_MAPPING, &o);
	o->file = A(0);
	o->size = A(4); /* the low size (high ignored: < 4 GB) */
	if (A(5)) snprintf(o->name, sizeof o->name, "%s", S(A(5)));
	set_err(0);
	RET(h, 6);
}
static void hle_MapViewOfFile(XCtx *c) {
	Obj *m = obj(A(0), O_MAPPING);
	FILE *f;
	u32 off = A(3), n = A(4), p;
	unsigned long long ios_m0 = IOS_ON() ? IOS_T() : 0;
	if (!m) { set_err(ERROR_INVALID_HANDLE); RET(0, 5); }
	f = m->file == INVALID_HANDLE ? 0 : file_of(m->file);
	if (!n) {
		n = m->size;
		if (!n && f) { long long cur = seek(f, 0, 1), end = seek(f, 0, 2); seek(f, cur, 0); n = (u32)end - off; }
	}
	p = xrc_heap_alloc(n ? n : 1, 1);
	if (!p) { set_err(8); RET(0, 5); }
	if (f) {
		long long cur = seek(f, 0, 1);
		seek(f, off, 0);
		if (fread(xrc_mem + p, 1, n, f)) {}
		seek(f, cur, 0);
	}
	if (ios_m0) { Obj *fo = m->file == INVALID_HANDLE ? 0 : obj(m->file, O_FILE); io_rec(fo ? fo->path : "?", 3, n, IOS_T() - ios_m0, 0); }
	RET(p, 5);
}
static void hle_UnmapViewOfFile(XCtx *c) { RET(xrc_heap_free(A(0)), 1); }

/* ============================================================================================ the table */
#define E(n) {"KERNEL32.dll", #n, hle_##n}
const XrcImport xrc_hle_kernel32_io[] = {
	E(CreateMutexA), E(ReleaseMutex), E(CreateEventA), E(SetEvent), E(ResetEvent), E(CreateSemaphoreA),
	E(ReleaseSemaphore), E(WaitForSingleObject), E(CreateThread), E(OpenThread), E(ExitThread), E(CloseHandle),
	E(CreateFileA), E(ReadFile), E(WriteFile), E(SetFilePointer), E(SetFilePointerEx), E(SetEndOfFile),
	E(FlushFileBuffers), E(GetFileType), E(GetFileTime), E(DeleteFileA), E(CreateDirectoryA), E(GetFileAttributesA),
	E(GetDriveTypeA), E(GetDiskFreeSpaceExA), E(GetCurrentDirectoryA), E(SetCurrentDirectoryA), E(GetFullPathNameA),
	E(GetSystemDirectoryA), E(FileTimeToLocalFileTime), E(FileTimeToSystemTime), E(FindFirstFileA), E(FindNextFileA),
	E(FindClose), E(CreateFileMappingA), E(MapViewOfFile), E(UnmapViewOfFile),
};
const unsigned xrc_hle_kernel32_io_n = sizeof xrc_hle_kernel32_io / sizeof xrc_hle_kernel32_io[0];
