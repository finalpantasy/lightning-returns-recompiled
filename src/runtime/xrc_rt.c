#define _GNU_SOURCE
/* xrc_rt.c - portable runtime of the ff xrc translations: guest memory, exe image, TEB/PEB/TLS, guest stack, dispatch,
 * import (HLE) registry, entry. See xrc_rt.h. Plain C99 plus one OS call to reserve the guest address space. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include "xrc_rt.h"
#include "host_movie_convert.h"
#include "host_audio.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <sys/mman.h>
#endif

u8 *xrc_mem;
int xrc_argc;
char **xrc_argv;
#ifdef _WIN32
unsigned xrc_tls_slot;
/* the slot, before main (TlsSlots[] in the TEB holds the first 64) */
__attribute__((constructor)) static void tls_init(void) {
	DWORD k = TlsAlloc();
	if (k == TLS_OUT_OF_INDEXES || k >= 64) { fprintf(stderr, "xrc: TlsAlloc gave %lu (need < 64)\n", (unsigned long)k); exit(2); }
	xrc_tls_slot = (unsigned)k;
}
XrcTls *xrc_tls_new(void) {
	XrcTls *p = (XrcTls *)TlsGetValue(xrc_tls_slot);
	if (p) return p;
	p = (XrcTls *)calloc(1, sizeof *p); /* (16-byte aligned) */
	if (!p) { fprintf(stderr, "xrc: no memory for a thread block\n"); exit(2); }
	p->teb = XRC_TEB;
	TlsSetValue(xrc_tls_slot, p);
	return p;
}
#else
XRC_THREAD XrcTls xrc_tls_v = {.teb = XRC_TEB};
#endif

/* ------------------------------------------------------------------------------------------------ guest memory */
int xrc_write_watch; /* Windows: guest memory tracks written pages (GetWriteWatch: the reference d3d9's uploads) */
#ifndef _WIN32
static int xrc_guest_hugetlb; /* Linux: guest memory on explicit huge pages (reserve_guest) */
#endif
u32 xrc_stack_lo = 0x30000000u, xrc_heap_lo = 0x40000000u, xrc_heap_hi = 0x7f000000u, xrc_va_lo = 0x80000000u, xrc_va_hi = 0xffe00000u;
int xrc_guest_2gb, xrc_va_down;
static u8 *reserve_guest(void) {
	size_t n;
	{
		const char *g = getenv("XRC_GUEST_2GB");
		xrc_guest_2gb = g && atoi(g) != 0;
		if (xrc_guest_2gb) {
			xrc_stack_lo = 0x02c00000u;
			xrc_heap_lo = xrc_va_lo = 0x03000000u;
			xrc_heap_hi = xrc_va_hi = 0x7f000000u;
			xrc_va_down = 1;
		}
	}
	n = (size_t)XRC_GUEST_SIZE;
#ifdef _WIN32
	/* committed lazily by the OS (pages are only backed once touched); with MEM_WRITE_WATCH unless XRC_NO_WRITE_WATCH */
	u8 *p = 0;
#ifdef XRC_FIXED_BASE /* the new xrc.h makes XM a constant host address (pages bake it into every access): reserve exactly there */
	void *want = (void *)(uintptr_t)XRC_FIXED_BASE;
#else
	void *want = 0;
#endif
	if (!getenv("XRC_NO_WRITE_WATCH")) p = (u8 *)VirtualAlloc(want, n, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
	if (p) { xrc_write_watch = 1; return p; }
	p = (u8 *)VirtualAlloc(want, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#ifdef XRC_FIXED_BASE
	if (!p) fprintf(stderr, "xrc: cannot reserve guest memory at fixed base %p (the exe must be linked above it: --image-base 0x200000000)\n", want);
#endif
	return p;
#else
	/* Linux: on huge pages (fewer TLB misses over the game's scattered 4 GB). 1 GB pages, else 2 MB ones, when the
	 * system has a pool of them for all of it (/sys/kernel/mm/hugepages: reserved at the mmap, so a short pool fails
	 * here, not as a SIGBUS later; they stay resident); else ordinary pages the kernel backs with transparent 2 MB ones
	 * (MADV_HUGEPAGE: THP "madvise" or "always"). Explicit huge pages cannot be write-protected 4 KB at a time, so
	 * protect_image then leaves the image's read-only sections writable. XRC_HUGEPAGES=0: none of it; =thp: no pool. */
	const char *e = getenv("XRC_HUGEPAGES");
	void *p;
	int k;
	static const struct { const char *pool; int shift; const char *name; } hp[] = {
		{"/sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages", 30, "1 GB"},
		{"/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages", 21, "2 MB"},
	};
	for (k = 0; (!e || (strcmp(e, "0") && strcmp(e, "thp"))) && k < 2; k++) {
		FILE *f = fopen(hp[k].pool, "r");
		unsigned long nfree = 0;
		if (!f) continue;
		if (fscanf(f, "%lu", &nfree) != 1) nfree = 0;
		fclose(f);
		if (nfree < (n >> hp[k].shift)) continue;
		p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (hp[k].shift << MAP_HUGE_SHIFT), -1, 0);
		if (p == MAP_FAILED) continue;
		xrc_guest_hugetlb = 1;
		if (xrc_tool_env("XRC_VERBOSE")) fprintf(stderr, "xrc: guest memory on %s pages\n", hp[k].name);
		return (u8 *)p;
	}
	p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED) return 0;
	if (!e || strcmp(e, "0")) madvise(p, n, MADV_HUGEPAGE);
	return (u8 *)p;
#endif
}

/* (from the guest heap: its own bump pointer here once started at the heap's start too, over the HLE heap's blocks) */
u32 xrc_alloc(u32 size, u32 align) {
	u32 a = xrc_heap_alloc(size + (align > 16 ? align : 0), 0);
	if (!a) xrc_fatal(0, "guest heap exhausted (%u bytes)", size);
	if (align > 16) a = (a + align - 1) & ~(align - 1);
	return a;
}

/* ------------------------------------------------------------------------------------------------ diagnostics */
#define XRC_MAXDEPTH 4096
/* each guest thread's call stack (diagnostics: XRC_WATCH, faults, the profiler). The dispatcher finds its thread's
 * without thread-local storage (mingw's is emulated: a call per access): XCtx.pad holds the index + 1, set on the
 * thread's first call (one XCtx per guest thread). */
#define XRC_MAXT 64
#ifdef XRC_DEEP_STACKS /* (the type is xrc.h's then: the translated code's direct calls write it too) */
typedef XrcTStack TStack;
TStack xrc_tstacks[XRC_MAXT];
#define tstacks xrc_tstacks
#else
typedef struct { u32 calls[XRC_MAXDEPTH]; volatile int depth; u32 tid; unsigned long long n; XCtx *ctx; } TStack; /* (n: calls, tools builds) */
static TStack tstacks[XRC_MAXT];
#endif
static int nall;
#define TS_DEPTH(ts) ((ts)->depth < XRC_MAXDEPTH ? (ts)->depth : XRC_MAXDEPTH) /* (the recorded frames: deep stacks only count past the array) */
#define my_ts (*(TStack **)&xrc_tls()->ts)
static unsigned long long ncalls;
static TStack *bind_stack(XCtx *c) {
	if (!my_ts) {
		if (nall >= XRC_MAXT) xrc_fatal(c, "more than %d guest threads", XRC_MAXT);
		my_ts = &tstacks[nall];
		my_ts->tid = xrc_thread_id();
#ifdef _WIN32
		{
			extern void *xrc_prof_handles[];
			DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &xrc_prof_handles[nall], 0, FALSE,
			                DUPLICATE_SAME_ACCESS);
		}
#endif
		__atomic_store_n(&nall, nall + 1, __ATOMIC_RELEASE);
	}
	c->pad = (u32)(my_ts - tstacks) + 1u;
	my_ts->ctx = c;
	return my_ts;
}
/* The guest stack, walked: translated calls push the original's return addresses (src/include/xrc.h, x_ra_), so the
 * guest's own frame-pointer chain gives the callers the dispatcher's record (tstacks: dispatched calls only, as most
 * calls are direct) does not have -- without recording every call (XRC_DEEP_STACKS does that, at a cost). From the
 * thread's XCtx: [esp] (the return address of the call the thread is in: exact for a thread blocked in an import, the
 * registers are spilled at every call), then [ebp + 4] up the chain of saved ebps. A running thread's XCtx is as of its
 * last call (the innermost frames may be missing or stale). out: the functions containing the return addresses
 * (innermost first); n of them. (Async-signal-safe: guest memory reads only.) */
static u32 image_size; /* (the image's: below) */
static u32 fn_containing(u32 a) {
	unsigned lo = 0, hi = xrc_nfns;
	if (a < XRC_IMAGE_BASE || a >= XRC_IMAGE_BASE + image_size || !xrc_nfns || a <= xrc_fns[0].addr) return 0;
	while (hi - lo > 1) { unsigned m = (lo + hi) / 2; if (xrc_fns[m].addr < a) lo = m; else hi = m; }
	return xrc_fns[lo].addr;
}
/* a guest dword that can be read without faulting: another thread's XCtx is read while it runs, so esp / ebp can be
 * stale or not set yet (a thread just made: esp 0) -- the page is checked first (2026-11-20: the stall report walked a new
 * thread's stack and the read of an unmapped page killed the game) */
#ifdef _WIN32
static int page_readable(u32 a) {
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery(XM + a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
	return !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
#endif
static int guest_readable(u32 a) {
	if (a < 0x10000u || a > 0xfffffffcu) return 0;
#ifdef _WIN32
	/* (the dword's first byte and, when it crosses into the next page, its last) */
	return page_readable(a) && ((a & 0xfffu) <= 0xffcu || page_readable(a + 3u));
#else
	return 1;
#endif
}
/* a dword of another thread's guest stack, which may be freed between the check and the read (a thread exiting while
 * the stall monitor walks it: lr_pad_err.txt, c0000005 here): ReadProcessMemory fails instead of faulting */
static int stack_read32(u32 a, u32 *v) {
	if (!guest_readable(a)) return 0;
#ifdef _WIN32
	{
		SIZE_T got = 0;
		return ReadProcessMemory(GetCurrentProcess(), XM + a, v, 4, &got) && got == 4;
	}
#else
	*v = M32(a);
	return 1;
#endif
}
/* (ras: 0, or each frame's return address, for the "+offset" of a named frame) */
static int walk_stack_ra(XCtx *c, u32 *out, u32 *ras, int max) {
	u32 esp, ebp, last = 0, top;
	int n = 0, k;
	if (!c) return 0;
#ifdef _WIN32
	{
		/* (another thread's XCtx can be gone too: a thread just made or exiting -- names2 run, 2026-11-20, c0000005
		 * reading c->esp of a depth-0 thread in the XRC_WATCH dump) */
		SIZE_T g1 = 0, g2 = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), &c->esp, &esp, 4, &g1) || g1 != 4 ||
		    !ReadProcessMemory(GetCurrentProcess(), &c->ebp, &ebp, 4, &g2) || g2 != 4) return 0;
	}
#else
	esp = c->esp; ebp = c->ebp;
#endif
	if (!stack_read32(esp, &top)) return 0;
	{
		u32 f = fn_containing(top);
		if (f && n < max) { if (ras) ras[n] = top; out[n++] = f; last = top; }
	}
	for (k = 0; k < 256 && n < max; k++) {
		u32 ra, nx;
		if (ebp < esp || ebp - esp > (16u << 20) || (ebp & 3u) || !stack_read32(ebp, &nx) || !stack_read32(ebp + 4u, &ra)) break;
		if (ra != last) {
			u32 f = fn_containing(ra);
			if (!f) break;
			if (ras) ras[n] = ra;
			out[n++] = f;
		}
		last = 0;
		if (nx <= ebp) break;
		ebp = nx;
	}
	return n;
}
int xrc_walk_stack(XCtx *c, u32 *out, int max) { return walk_stack_ra(c, out, 0, max); }
/* guest function names (xrc_names.c: names.tsv) */
void xrc_names_load(void);
int xrc_names_loaded(void);
const char *xrc_guest_fmt(char *buf, size_t n, u32 fn, u32 at);
const char *xrc_import_name(u32 a);
/* one stack entry: "DLL!name" for an import, else "00747c50 Class::method[+0x1a0]" (the address alone if unnamed) */
static const char *frame_str(char *buf, size_t n, u32 fn, u32 at) {
	const char *nm = xrc_import_name(fn);
	if (nm) return nm;
	return xrc_guest_fmt(buf, n, fn, at);
}
#ifndef XRC_DEEP_STACKS
static void print_walk(TStack *ts, int depth) {
	u32 w[64], ra[64];
	char b[192];
	int n = walk_stack_ra(ts->ctx, w, ra, depth < 64 ? depth : 64), k;
	if (!n) return;
	if (xrc_names_loaded()) {
		fprintf(stderr, "    frame chain (return addresses):\n");
		for (k = 0; k < n; k++) fprintf(stderr, "      %s\n", xrc_guest_fmt(b, sizeof b, w[k], ra[k]));
		return;
	}
	fprintf(stderr, "    (frame chain:");
	for (k = 0; k < n; k++) fprintf(stderr, " %08x", w[k]);
	fprintf(stderr, ")\n");
}
#else
#define print_walk(ts, depth) ((void)0)
#endif
/* XRC_WATCH=secs: every secs, each guest thread: its calls since the last snapshot (0: it made no progress), what it
 * is blocked in now (the wait, its handle, for how long: xrc_wait_describe), and its guest call stack, innermost
 * first, XRC_STACK_DEPTH deep (default 24; imports by name) -- read without stopping the threads */
const char *xrc_import_name(u32 a);
/* one snapshot of every guest thread (XRC_WATCH, xrc_stall_tick): calls since *last / tlast, what it waits in, its
 * CPU time (Windows), its call stack */
static void watch_print(unsigned long long *last, unsigned long long *tlast, int depth) {
	int t, k;
	fprintf(stderr, "xrc watch: %llu calls (+%llu)\n", ncalls, ncalls - *last);
	*last = ncalls;
	for (t = 0; t < nall; t++) {
		int d = TS_DEPTH(&tstacks[t]);
		unsigned long long n = tstacks[t].n, cpu_ms = 0;
#ifndef XRC_NO_TOOLS
		char wd[160];
		if (!xrc_wait_describe(tstacks[t].tid, wd, sizeof wd)) snprintf(wd, sizeof wd, "running");
#else
		const char *wd = "";
#endif
#ifdef _WIN32
		{
			extern void *xrc_prof_handles[];
			FILETIME c0, e0, kt, ut;
			if (xrc_prof_handles[t] && GetThreadTimes(xrc_prof_handles[t], &c0, &e0, &kt, &ut))
				cpu_ms = (((unsigned long long)kt.dwHighDateTime << 32 | kt.dwLowDateTime) +
				          ((unsigned long long)ut.dwHighDateTime << 32 | ut.dwLowDateTime)) / 10000ull;
		}
#endif
		fprintf(stderr, "  thread %x: +%llu calls, cpu %llu ms, %s; depth %d:", tstacks[t].tid, n - tlast[t], cpu_ms, wd, d);
		tlast[t] = n;
		if (xrc_names_loaded()) { /* (one frame a line, named) */
			char b[192];
			fprintf(stderr, "\n");
			for (k = d - 1; k >= 0 && k >= d - depth; k--)
				fprintf(stderr, "      %s\n", frame_str(b, sizeof b, tstacks[t].calls[k], 0));
			print_walk(&tstacks[t], depth);
			continue;
		}
		for (k = d - 1; k >= 0 && k >= d - depth; k--) {
			u32 a = tstacks[t].calls[k];
			const char *nm = xrc_import_name(a);
			if (nm) fprintf(stderr, " %s", nm); else fprintf(stderr, " %08x", a);
		}
		fprintf(stderr, "\n");
		print_walk(&tstacks[t], depth);
	}
}
static void *watchdog(void *arg) {
#if !defined(_WIN32) && !defined(XRC_NO_TOOLS)
	pthread_setname_np(pthread_self(), "xrc-watch");
#endif
	unsigned secs = (unsigned)(uintptr_t)arg;
	unsigned long long last = 0, tlast[XRC_MAXT] = {0};
	int depth = xrc_tool_env("XRC_STACK_DEPTH") && atoi(xrc_tool_env("XRC_STACK_DEPTH")) > 0 ? atoi(xrc_tool_env("XRC_STACK_DEPTH")) : 24;
	for (;;) {
		sleep(secs);
		watch_print(&last, tlast, depth);
	}
	return 0;
}
/* once a second (xrc_gil_hog_watch's thread): guest calls that have nearly stopped (under 200 a second) for 10 s in a
 * row -- the game stalled in a loop that calls nothing, or every thread waiting -- print every thread once, twice 5 s
 * apart (their CPU times and call counts then show which one spins); again only after the calls resume */
void xrc_stall_tick(void) {
	static unsigned long long prev, last, tlast[XRC_MAXT];
	static int quiet, told;
	unsigned long long n = ncalls;
	static int trace = -1;
	if (trace < 0) trace = getenv("XRC_STALL_TRACE") != 0;
	if (trace) fprintf(stderr, "xrc: stall tick: %llu calls (+%llu), quiet %d\n", n, n - prev, quiet);
	if (!prev) { prev = n; fprintf(stderr, "xrc: stall monitor running\n"); return; }
	if (n - prev < 200) quiet++; else { quiet = 0; told = 0; }
	prev = n;
	if (told >= 2 || (quiet != 10 && quiet != 15)) return;
	if (!told) {
		int t;
		fprintf(stderr, "xrc: guest calls have nearly stopped for 10 s: the threads (twice, 5 s apart)\n");
		last = n;
		for (t = 0; t < nall; t++) tlast[t] = tstacks[t].n;
	}
	told++;
	watch_print(&last, tlast, 24);
}

/* one snapshot of every guest thread on demand (the Vulkan backend's XRC_HITCH_STACK watchdog) */
void xrc_watch_snapshot(void) {
	static unsigned long long last, tlast[XRC_MAXT];
	watch_print(&last, tlast, 24);
}

void xrc_print_stack(void);
void xrc_fatal(XCtx *c, const char *fmt, ...) {
	va_list ap;
	int k;
	fflush(stdout);
	fprintf(stderr, "\nxrc: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	if (c)
		fprintf(stderr, "  eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x\n", c->eax, c->ecx,
		        c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
	(void)k;
	xrc_print_stack();
	exit(3);
}

/* another guest thread's call stack (by thread id; read without stopping it) */
void xrc_print_thread_stack(u32 tid) {
	int i, k;
	for (i = 0; i < nall; i++)
		if (tstacks[i].tid == tid) {
			int d = TS_DEPTH(&tstacks[i]);
			char b[192];
			fprintf(stderr, "  thread %x guest call stack (innermost first):\n", tid);
			for (k = d - 1; k >= 0 && k >= d - 24; k--) fprintf(stderr, "    %s\n", frame_str(b, sizeof b, tstacks[i].calls[k], 0));
			print_walk(&tstacks[i], 32);
			return;
		}
	fprintf(stderr, "  thread %x: no call stack\n", tid);
}
/* the k-th dispatched guest function of c's thread's call stack (0: the innermost, e.g. the running import; 0: none) */
u32 xrc_stack_at(XCtx *c, int k) {
	TStack *ts = c->pad ? &tstacks[c->pad - 1u] : 0;
	return ts && TS_DEPTH(ts) - 1 - k >= 0 ? ts->calls[TS_DEPTH(ts) - 1 - k] : 0;
}
/* the guest call stack (innermost first) and the call count, on stderr */
void xrc_print_stack(void) {
	int k;
	fprintf(stderr, "  %llu calls dispatched; guest call stack (innermost first):\n", ncalls);
	if (!my_ts) return;
	for (k = TS_DEPTH(my_ts) - 1; k >= 0 && k >= TS_DEPTH(my_ts) - 32; k--) {
		char b[192];
		fprintf(stderr, "    %s\n", frame_str(b, sizeof b, my_ts->calls[k], 0));
	}
	print_walk(my_ts, 32);
}

/* ------------------------------------------------------------------------------------------------ imports */
/* fn: the HLE function (0: none); wrap: the reference target's forwarder for this import (0: fn alone), which runs
 * fn itself (xrc_cur_body) and also calls the real DLL */
typedef struct ImpSlot { char dll[64], name[96]; XrcHle fn, wrap; } ImpSlot;
static ImpSlot *imps;
static unsigned nimps;

u32 xrc_arg(XCtx *c, int i) { return M32(c->esp + 4u + 4u * (u32)i); }
void xrc_stdcall_ret(XCtx *c, int nargs) { c->eip = M32(c->esp); c->esp += 4u + 4u * (u32)nargs; }
void xrc_cdecl_ret(XCtx *c) { c->eip = M32(c->esp); c->esp += 4u; }

int xrc_ieq(const char *a, const char *b) {
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return 0;
	}
	return *a == *b;
}

/* the HLE registry: one table per DLL (src/runtime/hle_*.c). An import not in any stops the run with its name. */
static const XrcImport *const hle_tables[] = {
#ifdef XRC_LR
	xrc_hle_msvcr100, xrc_hle_lr, xrc_hle_steam,
#endif
	xrc_hle_kernel32, xrc_hle_kernel32_io, xrc_hle_system, xrc_hle_com, xrc_hle_user32, xrc_hle_d3d9, xrc_hle_dinput, xrc_hle_xaudio2, xrc_hle_bink};
static const unsigned *hle_counts[] = {
#ifdef XRC_LR
	&xrc_hle_msvcr100_n, &xrc_hle_lr_n, &xrc_hle_steam_n,
#endif
	&xrc_hle_kernel32_n, &xrc_hle_kernel32_io_n, &xrc_hle_system_n, &xrc_hle_com_n, &xrc_hle_user32_n, &xrc_hle_d3d9_n, &xrc_hle_dinput_n, &xrc_hle_xaudio2_n, &xrc_hle_bink_n};

static XrcHle hle_find(const char *dll, const char *name) {
	unsigned t, k;
	for (t = 0; t < sizeof hle_tables / sizeof hle_tables[0]; t++)
		for (k = 0; k < *hle_counts[t]; k++) {
			const XrcImport *e = &hle_tables[t][k];
			if ((!dll || xrc_ieq(e->dll, dll)) && strcmp(e->name, name) == 0) return e->fn;
		}
	return 0;
}

static unsigned imp_cap;
unsigned xrc_nimports(void) { return nimps; }
static u32 add_import(const char *dll, const char *name, XrcHle fn) {
	ImpSlot *s;
	if (nimps == imp_cap) {
		imp_cap = imp_cap ? imp_cap * 2 : 512;
		imps = (ImpSlot *)realloc(imps, imp_cap * sizeof(ImpSlot));
	}
	s = &imps[nimps];
	snprintf(s->dll, sizeof s->dll, "%s", dll);
	snprintf(s->name, sizeof s->name, "%s", name);
	s->fn = fn;
#ifdef XRC_REFERENCE
	s->wrap = xrc_ref_wrap(dll, name);
#else
	s->wrap = 0;
#endif
	return XRC_IMP_BASE + 16u * nimps++;
}

/* GetProcAddress: a guest address for a function the HLE implements (0 if none: the caller's fallback runs) */
u32 xrc_import_addr(const char *dll, const char *name) {
	unsigned k;
	XrcHle fn = hle_find(dll, name);
	if (!fn) fn = hle_find(0, name);
	if (!fn) return 0;
	for (k = 0; k < nimps; k++)
		if (imps[k].fn == fn && strcmp(imps[k].name, name) == 0) return XRC_IMP_BASE + 16u * k;
	return add_import(dll ? dll : "?", name, fn);
}

/* an import slot for dll!name run by fn, whether or not an HLE table names it (synthetic C++ interfaces: a guest
 * vtable entry per method, each with its own name for the error a method without a handler stops with) */
u32 xrc_import_with(const char *dll, const char *name, XrcHle fn) {
	unsigned k;
	for (k = 0; k < nimps; k++)
		if (imps[k].fn == fn && strcmp(imps[k].name, name) == 0 && xrc_ieq(imps[k].dll, dll)) return XRC_IMP_BASE + 16u * k;
	return add_import(dll, name, fn);
}

/* an import slot for dll!name whether or not the HLE has it (COM vtables: a method without one stops the run with
 * its name when called) */
/* an import by name only (any DLL): the byte-matched code's calls of imports (ff xrcmatch) */
u32 xrc_import_named(const char *name) {
	unsigned k, found = ~0u;
	char nm[16];
	/* (the lifted code names an import by ordinal ordN: the runtime's #N) */
	if (strncmp(name, "ord", 3) == 0 && name[3] >= '0' && name[3] <= '9') { snprintf(nm, sizeof nm, "#%s", name + 3); name = nm; }
	for (k = 0; k < nimps; k++)
		if (strcmp(imps[k].name, name) == 0) {
			if (found != ~0u && !xrc_ieq(imps[found].dll, imps[k].dll))
				xrc_fatal(0, "byte-matched code calls %s, which both %s and %s export", name, imps[found].dll, imps[k].dll);
			found = k;
		}
	if (found == ~0u) xrc_fatal(0, "byte-matched code calls %s, which the exe does not import", name);
	return XRC_IMP_BASE + 16u * found;
}
/* the same, or the stdcall-decorated _name@<nbytes> (bink2w32.dll's exports) */
u32 xrc_import_named_n(const char *name, int nbytes) {
	unsigned k;
	char dn[128];
	for (k = 0; k < nimps; k++) if (strcmp(imps[k].name, name) == 0) return xrc_import_named(name);
	snprintf(dn, sizeof dn, "_%s@%d", name, nbytes);
	for (k = 0; k < nimps; k++) if (strcmp(imps[k].name, dn) == 0) return XRC_IMP_BASE + 16u * k;
	return xrc_import_named(name);
}
u32 xrc_import_slot(const char *dll, const char *name) {
	unsigned k;
	for (k = 0; k < nimps; k++)
		if (strcmp(imps[k].name, name) == 0 && xrc_ieq(imps[k].dll, dll)) return XRC_IMP_BASE + 16u * k;
	return add_import(dll, name, hle_find(dll, name));
}

/* a guest thread's context: the stack top, the Win32 start state (x87 CW 0x27f, MXCSR 0x1f80 = exceptions masked,
 * round to nearest; EFLAGS IF) */
void xrc_ctx_init(XCtx *c, u32 esp) {
	memset(c, 0, sizeof *c);
	c->esp = esp;
	c->fcw = 0x27f;
	c->mxcsr = 0x1f80;
	c->eflags_sys = 0x202;
}

u32 xrc_call_guest(XCtx *c, u32 fn, int nargs, const u32 *args) {
	u32 sp = c->esp;
	int k;
	for (k = nargs - 1; k >= 0; k--) { c->esp -= 4; M32(c->esp) = args[k]; }
	c->esp -= 4;
	M32(c->esp) = XRC_RET_MAGIC;
	xrc_dispatch(c, fn);
	c->esp = sp;
	return c->eax;
}

/* ------------------------------------------------------------------------------------------------ exe image */
static u32 entry_point, image_size;

/* the image's read-only sections, protected once the imports are bound (a write there is a bug -- on Windows too) */
static u32 ro_lo[16], ro_hi[16];
static int nro;
#ifndef _WIN32
#include <signal.h>
#include <sys/mman.h>
static void on_segv(int sig, siginfo_t *si, void *uc) {
	unsigned long long a = (unsigned long long)(uintptr_t)si->si_addr, b = (unsigned long long)(uintptr_t)xrc_mem;
	(void)uc;
	if (a >= b && a < b + 0x100000000ull)
		fprintf(stderr, "\nxrc: host fault (signal %d) at guest address %08x%s\n", sig, (u32)(a - b),
		        (u32)(a - b) >= XRC_IMAGE_BASE && (u32)(a - b) < XRC_IMAGE_BASE + image_size ? " (inside the image: a write to read-only data?)" : "");
	else
		fprintf(stderr, "\nxrc: host fault (signal %d) at host address %p\n", sig, si->si_addr);
	xrc_print_stack();
	fflush(stderr); _exit(3);
}
static void protect_image(void) {
	struct sigaction sa;
	int k;
	if (getenv("XRC_NO_PROTECT") || xrc_guest_hugetlb) return; /* (huge pages: no 4 KB protection) */
	{ /* (only whole host pages: on a host with 16 or 64 KB pages, e.g. ARM64, a read-only range's partial pages hold
	   * writable data too) */
		unsigned long long pg = (unsigned long long)sysconf(_SC_PAGESIZE);
		for (k = 0; k < nro; k++) {
			unsigned long long lo = ((unsigned long long)ro_lo[k] + pg - 1) & ~(pg - 1), hi = (unsigned long long)ro_hi[k] & ~(pg - 1);
			if (hi > lo) mprotect(xrc_mem + lo, hi - lo, PROT_READ);
		}
	}
	/* XRC_NO_TEXT: the first section (.text) unreadable: finds what still reads the original code bytes (the
	 * retail .text is encrypted; only the data sections come from the player's exe) */
	if (getenv("XRC_NO_TEXT") && nro) mprotect(xrc_mem + ro_lo[0], ro_hi[0] - ro_lo[0], PROT_NONE);
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, 0);
	sigaction(SIGBUS, &sa, 0);
}
#else
/* Windows: a fault in our own code (or a stack overflow anywhere) is reported here, with the guest call stack: left
 * alone, the exception search through frames without unwind data ends in a fail-fast (0xc0000409 in ntdll, exit code 9
 * from WSL) that says nothing. Faults in a DLL go on to its own handlers (drivers catch some on purpose); one nobody
 * handles reaches the unhandled filter. The host address is an offset from the image base (`x86_64-w64-mingw32-nm`). */
static void win_report(EXCEPTION_POINTERS *ep, const char *how) {
	EXCEPTION_RECORD *r = ep->ExceptionRecord;
	uintptr_t base = (uintptr_t)GetModuleHandleA(0), ip = (uintptr_t)r->ExceptionAddress;
	unsigned long long a = r->NumberParameters >= 2 ? (unsigned long long)r->ExceptionInformation[1] : 0, b = (unsigned long long)(uintptr_t)xrc_mem;
	fprintf(stderr, "\nxrc: host exception %08lx (%s) at ", (unsigned long)r->ExceptionCode, how);
	if (ip >= base && ip < base + 0x10000000u) fprintf(stderr, "image offset %llx", (unsigned long long)(ip - base));
	else fprintf(stderr, "%p (outside the image)", (void *)ip);
	if (r->ExceptionCode == 0xc0000005u) {
		if (a >= b && a < b + 0x100000000ull) fprintf(stderr, ", %s guest address %08x", r->ExceptionInformation[0] ? "writing" : "reading", (u32)(a - b));
		else fprintf(stderr, ", %s host address %llx", r->ExceptionInformation[0] ? "writing" : "reading", a);
	}
	fprintf(stderr, "; host thread %lu\n", (unsigned long)GetCurrentThreadId());
	xrc_print_stack();
	fflush(stderr);
	TerminateProcess(GetCurrentProcess(), 3);
}
static LONG WINAPI win_vectored(EXCEPTION_POINTERS *ep) {
	DWORD k = ep->ExceptionRecord->ExceptionCode;
	uintptr_t base = (uintptr_t)GetModuleHandleA(0), ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
	if (k == 0xc00000fdu) win_report(ep, "stack overflow");
	if (k != 0x406d1388u /* a thread's name */ && k != 0x40010006u && k != 0x4001000au /* debug output */) {
		/* a fault in a DLL: said (once per address) before its own handlers get it -- if none does, the search ends in
		 * a fail-fast without a word */
		static uintptr_t seen[32];
		static int nseen;
		int s_, dup = 0;
		for (s_ = 0; s_ < nseen; s_++) dup |= seen[s_] == ip;
		if (!dup && nseen < 32) {
			HMODULE m = 0;
			char mn[MAX_PATH] = "?";
			seen[nseen++] = ip;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &m))
				GetModuleFileNameA(m, mn, sizeof mn);
			fprintf(stderr, "xrc: first-chance exception %08lx at %s+%llx (host thread %lu)", (unsigned long)k, mn,
			        (unsigned long long)(ip - (uintptr_t)m), (unsigned long)GetCurrentThreadId());
			if (k == 0xc0000005u && ep->ExceptionRecord->NumberParameters >= 2)
				fprintf(stderr, ", %s %llx", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
				        (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
			fprintf(stderr, "\n");
			xrc_print_stack();
		}
	}
	if ((k == 0xc0000005u || k == 0xc000001du || k == 0xc0000094u || k == 0xc0000096u || k == 0xc0000095u) && ip >= base && ip < base + 0x10000000u)
		win_report(ep, "in our code");
	return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI win_unhandled(EXCEPTION_POINTERS *ep) { win_report(ep, "unhandled"); return EXCEPTION_EXECUTE_HANDLER; }
static void protect_image(void) {
	ULONG guarantee = 64u << 10;
	if (!getenv("XRC_NO_VEH")) AddVectoredExceptionHandler(1, win_vectored); /* XRC_NO_VEH: none (to compare) */
	SetUnhandledExceptionFilter(win_unhandled);
	SetThreadStackGuarantee(&guarantee); /* (room to report a stack overflow of the main thread) */
	/* XRC_NULL_GUARD=1: guest page 0 not accessible, as in the original process, so a null-pointer access or one of
	 * the game's asserts (a write to address 0) stops the run with its guest stack (win_report) instead of passing
	 * unseen. Needs fs: translated as XRC_SEG_FS + ea (-DXRC_SEG_FS=xrc_teb): the SEH chain head (fs:[0]) was at address 0. */
	if (getenv("XRC_NULL_GUARD") && atoi(getenv("XRC_NULL_GUARD"))) {
		DWORD old;
		if (VirtualProtect(xrc_mem, 0x1000, PAGE_NOACCESS, &old)) fprintf(stderr, "xrc: guest page 0 guarded (XRC_NULL_GUARD)\n");
	}
}
#endif
static int load_exe(const char *path) {
	FILE *f = fopen(path, "rb");
	u8 *d;
	long n;
	u32 pe, opt, nsec, sec, k, tlsdir;
	if (!f) { fprintf(stderr, "xrc: cannot open %s\n", path); return 0; }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	d = (u8 *)malloc((size_t)n);
	if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
	fclose(f);
#define R16(o) ((u32)d[o] | (u32)d[(o) + 1] << 8)
#define R32(o) (R16(o) | R16((o) + 2) << 16)
	pe = R32(0x3c);
	if (R32(pe) != 0x4550) { fprintf(stderr, "xrc: %s is not a PE file\n", path); return 0; }
	nsec = R16(pe + 6);
	opt = pe + 24;
	if (R32(opt + 28) != XRC_IMAGE_BASE) { fprintf(stderr, "xrc: unexpected image base %08x\n", R32(opt + 28)); return 0; }
	entry_point = XRC_IMAGE_BASE + R32(opt + 16);
	image_size = R32(opt + 56);
	if (xrc_guest_2gb && XRC_IMAGE_BASE + image_size > XRC_STACK_LO) { fprintf(stderr, "xrc: the image (%u MB) reaches the 2 GB layout's stack\n", image_size >> 20); return 0; }
	memcpy(xrc_mem + XRC_IMAGE_BASE, d, R32(opt + 60)); /* headers */
	sec = opt + R16(pe + 20);
	for (k = 0; k < nsec; k++, sec += 40) {
		u32 va = R32(sec + 12), rawsz = R32(sec + 16), raw = R32(sec + 20), vsz = R32(sec + 8);
		u32 sz = rawsz < vsz || vsz == 0 ? rawsz : vsz;
		if (raw + sz > (u32)n) sz = (u32)n > raw ? (u32)n - raw : 0;
		memcpy(xrc_mem + XRC_IMAGE_BASE + va, d + raw, sz); /* (the rest of the section stays zero) */
		if (!(R32(sec + 36) & 0x80000000u) && nro < 16) { /* not IMAGE_SCN_MEM_WRITE: read-only once loaded */
			ro_lo[nro] = XRC_IMAGE_BASE + va;
			ro_hi[nro] = XRC_IMAGE_BASE + va + (((vsz ? vsz : rawsz) + 0xfffu) & ~0xfffu);
			nro++;
		}
	}
	/* .text: none of it is used (it is all translated, its switch tables too; the retail copy's is encrypted anyway):
	 * zero, so a read of it is a plain 0 (XRC_NO_TEXT: a fault, to find one). The entry point from the dispatch table
	 * (the retail one is in SteamStub's .bind) */
	sec = opt + R16(pe + 20);
#ifndef XRC_LR /* LR: its .text is plain, and its switch tables are read from it at run time (translated without slots) */
	for (k = 0; k < nsec; k++, sec += 40)
		if (R32(sec + 36) & 0x20000000u /* CODE */) memset(xrc_mem + XRC_IMAGE_BASE + R32(sec + 12), 0, R32(sec + 8) ? R32(sec + 8) : R32(sec + 16));
#endif
	entry_point = xrc_entry;
	free(d);
#undef R16
#undef R32
	/* the same game: every other section as translated */
	{
		unsigned long long h = 0xcbf29ce484222325ull;
		u32 i;
		for (k = 0; k < xrc_nhash_ranges; k++)
			for (i = 0; i < xrc_hash_ranges[k].len; i++) h = (h ^ xrc_mem[xrc_hash_ranges[k].addr + i]) * 0x100000001b3ull;
		if (h != xrc_data_hash && !getenv("XRC_ANY_EXE")) {
			fprintf(stderr, "xrc: %s is not the version of the game this was translated from (data hash %016llx, expected %016llx)\n", path, h, xrc_data_hash);
			return 0;
		}
	}
	/* imports: every IAT slot gets a fake address that dispatches to the HLE registry */
	{
		u32 hdr = XRC_IMAGE_BASE + pe, o = hdr + 24;
		u32 irva = M32(o + 104), desc;
		unsigned count = 0;
		for (desc = XRC_IMAGE_BASE + irva; irva && M32(desc + 12); desc += 20)
			for (k = M32(desc) ? M32(desc) : M32(desc + 16); M32(XRC_IMAGE_BASE + k); k += 4) count++;
		(void)count;
		for (desc = XRC_IMAGE_BASE + irva; irva && M32(desc + 12); desc += 20) {
			const char *dll = (const char *)xrc_mem + XRC_IMAGE_BASE + M32(desc + 12);
			u32 look = XRC_IMAGE_BASE + (M32(desc) ? M32(desc) : M32(desc + 16));
			u32 iat = XRC_IMAGE_BASE + M32(desc + 16);
			for (; M32(look); look += 4, iat += 4) {
				u32 t = M32(look);
				char nm[96];
				if (t & 0x80000000u)
					snprintf(nm, sizeof nm, "#%u", t & 0xffffu);
				else
					snprintf(nm, sizeof nm, "%s", (const char *)xrc_mem + XRC_IMAGE_BASE + t + 2);
#ifdef XRC_LR
				{ /* data imports (msvcr100 _acmdln, __argc ...): the slot holds the variable's address */
					u32 dv = xrc_crt_data_import(dll, nm);
					if (dv) { M32(iat) = dv; continue; }
				}
#endif
				M32(iat) = add_import(dll, nm, hle_find(dll, nm));
			}
		}
		/* static TLS (__declspec(thread)): one block for the main thread, index 0 */
		tlsdir = M32(o + 96 + 9 * 8);
		if (tlsdir) {
			u32 t = XRC_IMAGE_BASE + tlsdir, lo = M32(t), hi = M32(t + 4), idx = M32(t + 8);
			if (hi > lo) memcpy(xrc_mem + XRC_TLS_BLOCK, xrc_mem + lo, hi - lo);
			xrc_tls_lo = lo; xrc_tls_hi = hi; xrc_tls_zfill = M32(t + 16);
			if (idx) M32(idx) = 0;
		}
	}
	return 1;
}

u32 xrc_tls_lo, xrc_tls_hi, xrc_tls_zfill;
static void setup_thread(void) {
	u32 t = XRC_TEB;
	M32(t + 0x00) = 0xffffffffu;                    /* SEH chain end */
	M32(t + 0x04) = XRC_STACK_LO + XRC_STACK_SIZE;  /* stack base */
	M32(t + 0x08) = XRC_STACK_LO;                   /* stack limit */
	M32(t + 0x18) = t;                              /* self */
	M32(t + 0x20) = 0x1000;                         /* process id */
	M32(t + 0x24) = 0x1004;                         /* thread id */
	M32(t + 0x2c) = t + 0x800;                      /* ThreadLocalStoragePointer -> [TLS block of module 0] */
	M32(t + 0x800) = XRC_TLS_BLOCK;
	M32(t + 0x30) = XRC_PEB;
	M32(XRC_PEB + 0x08) = XRC_IMAGE_BASE;           /* ImageBaseAddress */
	M32(XRC_PEB + 0x18) = XRC_PROCESS_HEAP;         /* ProcessHeap (a handle value; HeapAlloc is HLE) */
}

/* ------------------------------------------------------------------------------------------------ dispatch */

/* tables of the `fast` / `wrapped` rows of overrides.tsv (ovr_math.c, ovr_hooks.c) */
extern const XrcManual xrc_manual_math[], xrc_manual_hooks[];
extern const unsigned xrc_manual_math_n, xrc_manual_hooks_n;

/* the hand-written replacements; they take precedence over translations (build_htab enters them last) */
static const XrcManual **manual;
static unsigned nmanual, cmanual;
XFn xrc_translation_of(u32 addr) {
	unsigned k;
	for (k = 0; k < xrc_nfns; k++) if (xrc_fns[k].addr == addr) return xrc_fns[k].fn;
	xrc_fatal(0, "no translation of %08x", addr);
	return 0;
}
static void add_manual(const XrcManual *t, unsigned n) {
	unsigned i;
	for (i = 0; i < n; i++) {
		if (!t[i].fn) continue;
		if (nmanual == cmanual) {
			cmanual = cmanual ? 2 * cmanual : 256;
			manual = (const XrcManual **)realloc((void *)manual, cmanual * sizeof *manual);
		}
		manual[nmanual++] = &t[i];
	}
}
/* the byte-matched functions (ff xrcmatch: the old project's matched source as C), in place of their translations.
 * XRC_NO_MATCH=1: none of them; XRC_MATCH_LO / XRC_MATCH_HI (hex): only those in [lo, hi) (bisecting) */
static void add_matched(void) {
	static XrcManual *sel;
	u32 lo = getenv("XRC_MATCH_LO") ? (u32)strtoul(getenv("XRC_MATCH_LO"), 0, 16) : 0;
	u32 hi = getenv("XRC_MATCH_HI") ? (u32)strtoul(getenv("XRC_MATCH_HI"), 0, 16) : 0xffffffffu;
	unsigned k, n = 0;
	if (getenv("XRC_NO_MATCH")) return;
	sel = (XrcManual *)calloc(xrc_manual_match_n + 1, sizeof *sel);
	for (k = 0; k < xrc_manual_match_n; k++)
		if (xrc_manual_match[k].addr >= lo && xrc_manual_match[k].addr < hi) sel[n++] = xrc_manual_match[k];
	add_manual(sel, n);
	if (n) fprintf(stderr, "xrc: %u byte-matched functions in place of their translations\n", n);
}

/* every function by guest address, for xrc_dispatch: open addressing (linear probing, at most half full), the
 * hand-written versions entered last so they replace the translations */
typedef struct { u32 addr; XFn fn; } HEnt;
static HEnt *htab;
static u32 hmask;
static inline u32 hhash(u32 a) { return (a * 0x9e3779b1u) >> 7; }
static void hput(u32 a, XFn f) {
	u32 i = hhash(a) & hmask;
	while (htab[i].addr && htab[i].addr != a) i = (i + 1) & hmask;
	htab[i].addr = a;
	htab[i].fn = f;
}
static void build_htab(void) {
	unsigned n = 1024, k;
	while (n < 2u * (xrc_nfns + nmanual)) n <<= 1;
	htab = (HEnt *)calloc(n, sizeof *htab);
	hmask = n - 1;
	for (k = 0; k < xrc_nfns; k++) hput(xrc_fns[k].addr, xrc_fns[k].fn);
	for (k = 0; k < nmanual; k++) hput(manual[k]->addr, manual[k]->fn);
}
static inline XFn hfind(u32 a) {
	u32 i = hhash(a) & hmask;
	for (;;) {
		if (htab[i].addr == a) return htab[i].fn;
		if (!htab[i].addr) return 0;
		i = (i + 1) & hmask;
	}
}

const char *xrc_cur_import; /* the name of the import being run (generic COM method handlers) */
XrcHle xrc_cur_body;        /* its HLE function (for a reference forwarder) */
unsigned xrc_cur_slot;      /* its slot index */
extern volatile DWORD xrc_stall_main_tid; extern void xrc_stall_imp(const char *name, unsigned long long t0, unsigned long long dt);
static int trace_imports = -1;
/* XRC_IMPORT_TIME: wall time of each import (the outermost one of a thread: nested imports count in their caller),
 * split into time holding the GIL and time without it (blocked); per frame in xrc_import_report */
static int imp_time = -1;
#define imp_depth (xrc_tls()->imp_depth)
static unsigned long long *imp_n, *imp_held, *imp_off, *imp_pn, *imp_ph, *imp_po;
static unsigned it_cap;
static unsigned long long imp_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}
static void imp_acct(unsigned i, unsigned long long held, unsigned long long off) {
	if (i >= it_cap) { /* (under the GIL) */
		unsigned n = nimps + 64, k;
		unsigned long long **v[6] = {&imp_n, &imp_held, &imp_off, &imp_pn, &imp_ph, &imp_po};
		for (k = 0; k < 6; k++) {
			*v[k] = (unsigned long long *)realloc(*v[k], n * sizeof **v[k]);
			memset(*v[k] + it_cap, 0, (n - it_cap) * sizeof **v[k]);
		}
		it_cap = n;
	}
	imp_n[i]++; imp_held[i] += held; imp_off[i] += off;
}
void xrc_import_report(FILE *f, unsigned long long nframes) {
	unsigned i, k, n = 0, top[48];
	double th = 0, to = 0;
	if (!nframes) nframes = 1;
	for (i = 0; i < it_cap; i++) {
		unsigned long long dh = imp_held[i] - imp_ph[i];
		th += (double)dh; to += (double)(imp_off[i] - imp_po[i]);
		/* the 48 with the most held time */
		for (k = 0; k < n && dh <= imp_held[top[k]] - imp_ph[top[k]]; k++) {}
		if (k < 48 && dh) {
			if (n < 48) n++;
			memmove(top + k + 1, top + k, (n - 1 - k) * sizeof *top);
			top[k] = i;
		}
	}
	if (f) {
		fprintf(f, "xrc:   imports, ms/frame with the GIL / without: all %.2f / %.2f\n", th / 1000.0 / (double)nframes,
		        to / 1000.0 / (double)nframes);
		for (k = 0; k < n; k++) {
			i = top[k];
			fprintf(f, "xrc:     %8.3f / %8.3f  %7.1f calls  %s!%s\n", (double)(imp_held[i] - imp_ph[i]) / 1000.0 / (double)nframes,
			        (double)(imp_off[i] - imp_po[i]) / 1000.0 / (double)nframes, (double)(imp_n[i] - imp_pn[i]) / (double)nframes,
			        imps[i].dll, imps[i].name);
		}
	}
	for (i = 0; i < it_cap; i++) { imp_pn[i] = imp_n[i]; imp_ph[i] = imp_held[i]; imp_po[i] = imp_off[i]; }
}
static int trace_calls = -1; /* XRC_TRACE_CALLS: every dispatched call with the registers */
/* XRC_DISP_STATS=1: the dispatched calls by target and by (nearest dispatched caller, target), the most frequent
 * listed at frame XRC_DISP_STATS (hle_d3d9.c; to find what makes calls go through the dispatcher) */
static int disp_stats = -1;
#define DSN (1u << 16)
static struct { u32 t, from; unsigned long long n; } dst_t[DSN], dst_p[DSN];
static void dst_add(u32 t, u32 from) {
	u32 i = (t * 2654435761u) & (DSN - 1), j = ((t ^ (from * 0x85ebca6bu)) * 2654435761u) & (DSN - 1), g = 0;
	while (dst_t[i].n && dst_t[i].t != t && ++g < DSN) i = (i + 1) & (DSN - 1);
	dst_t[i].t = t; dst_t[i].n++;
	g = 0;
	while (dst_p[j].n && (dst_p[j].t != t || dst_p[j].from != from) && ++g < DSN) j = (j + 1) & (DSN - 1);
	dst_p[j].t = t; dst_p[j].from = from; dst_p[j].n++;
}
static int dst_cmp(const void *a, const void *b) {
	unsigned long long x = *(const unsigned long long *)((const u8 *)a + 8), y = *(const unsigned long long *)((const u8 *)b + 8);
	return x < y ? 1 : x > y ? -1 : 0;
}
void xrc_disp_dump(void) {
	unsigned k;
	qsort(dst_t, DSN, sizeof dst_t[0], dst_cmp);
	qsort(dst_p, DSN, sizeof dst_p[0], dst_cmp);
	fprintf(stderr, "xrc: dispatch stats: %llu calls; top targets:\n", ncalls);
	for (k = 0; k < 40 && dst_t[k].n; k++) fprintf(stderr, "xrc:   %12llu  %08x\n", dst_t[k].n, dst_t[k].t);
	fprintf(stderr, "xrc: top (dispatched caller -> target):\n");
	for (k = 0; k < 60 && dst_p[k].n; k++) fprintf(stderr, "xrc:   %12llu  %08x -> %08x\n", dst_p[k].n, dst_p[k].from, dst_p[k].t);
}
unsigned long long xrc_ncalls(void) { return ncalls; }
/* XRC_DATA_WATCH=ADDR or PTR+OFF (hex; PTR+OFF: the dword at [PTR] + OFF, looked up again each time -- 0 at PTR: not
 * yet): each change of that guest dword is reported, old -> new, with the thread and its guest call stack (16 deep),
 * at the first recorded call after the write (every call in XRC_DEEP_STACKS builds, else the dispatched ones) -- so the
 * innermost frames are the writer or its next call. XRC_DATA_WATCH_N: at most that many reports (default 500). */
volatile u32 xrc_dwatch_on;
#ifndef XRC_NO_TOOLS
static u32 dw_ptr, dw_off, dw_addr, dw_val, dw_left;
static int dw_have;
static void dwatch_init(void) {
	const char *e = xrc_tool_env("XRC_DATA_WATCH"), *n = xrc_tool_env("XRC_DATA_WATCH_N");
	char *end;
	if (!e) return;
	dw_left = n ? (u32)atoi(n) : 500;
	dw_addr = (u32)strtoul(e, &end, 16);
	if (*end == '+') { dw_ptr = dw_addr; dw_off = (u32)strtoul(end + 1, 0, 16); dw_addr = 0; }
	fprintf(stderr, "xrc: XRC_DATA_WATCH: %s%08x%s+%x\n", dw_ptr ? "[" : "", dw_ptr ? dw_ptr : dw_addr, dw_ptr ? "]" : "", dw_off);
	xrc_dwatch_on = 1;
}
void xrc_dwatch_check(XCtx *c) {
	u32 a = dw_ptr ? (M32(dw_ptr) ? M32(dw_ptr) + dw_off : 0) : dw_addr, v;
	TStack *ts;
	int k, d;
	if (!a) return;
	v = M32(a);
	if (dw_have && v == dw_val) return;
	if (!dw_have) { dw_have = 1; dw_val = v; fprintf(stderr, "xrc data watch: %08x = %08x (thread %x)\n", a, v, xrc_thread_id()); return; }
	fprintf(stderr, "xrc data watch: %08x %08x -> %08x  thread %x:", a, dw_val, v, xrc_thread_id());
	dw_val = v;
	ts = c->pad ? &tstacks[c->pad - 1u] : 0;
	d = ts ? TS_DEPTH(ts) : 0;
	for (k = d - 1; k >= 0 && k >= d - 16; k--) fprintf(stderr, " %08x", ts->calls[k]);
	fprintf(stderr, "\n");
	if (dw_left && !--dw_left) xrc_dwatch_on = 0;
}
#else
void xrc_dwatch_check(XCtx *c) { (void)c; }
static void dwatch_init(void) {}
#endif
/* XRC_TRACE_FROM=ADDR[,ADDR..] (hex, up to 8): each dispatched call made by those functions (the virtual and indirect
 * ones: the nearest recorded caller -- every function in XRC_DEEP_STACKS builds), with the target, ecx, the first stack
 * argument and the return value (eax) */
#ifndef XRC_NO_TOOLS
static int trace_from = -1;
static u32 tf_addr[8];
static void trace_from_init(void) {
	const char *e = xrc_tool_env("XRC_TRACE_FROM");
	int n = 0;
	while (e && *e && n < 8) { char *end; tf_addr[n++] = (u32)strtoul(e, &end, 16); e = *end == ',' ? end + 1 : 0; }
	trace_from = n;
}
static int trace_from_hit(u32 a) { int k; for (k = 0; k < trace_from; k++) if (tf_addr[k] == a) return 1; return 0; }
#endif
/* an import's "DLL!name" by its address in the import area (0: not an import) */
const char *xrc_import_name(u32 a) {
	static char buf[XRC_MAXT][176];
	static unsigned k;
	char *b;
	unsigned i;
	if (a < XRC_IMP_BASE) return 0;
	i = (a - XRC_IMP_BASE) / 16u;
	if (i >= nimps) return 0;
	b = buf[__atomic_fetch_add(&k, 1, __ATOMIC_RELAXED) % XRC_MAXT];
	snprintf(b, sizeof buf[0], "%s!%s", imps[i].dll, imps[i].name);
	return b;
}
#ifdef XRC_COUNT_CALLS
static void disp_count(u32 t);
#endif
void xrc_dispatch(XCtx *c, u32 t) {
	XFn f;
	TStack *ts;
	const u32 ra = M32(c->esp); /* the return address the caller pushed (the original's, or XRC_RET_MAGIC from the runtime) */
	ncalls++;
	if (trace_calls < 0) trace_calls = xrc_tool_env("XRC_TRACE_CALLS") != 0;
	ts = c->pad ? &tstacks[c->pad - 1u] : bind_stack(c);
#ifndef XRC_DEEP_STACKS
	if (ts->depth >= XRC_MAXDEPTH) xrc_fatal(c, "guest call depth over %d (runaway recursion?) at %08x", XRC_MAXDEPTH, t);
#else
	if (ts->depth >= 1000000) xrc_fatal(c, "guest call depth over a million (runaway recursion?) at %08x", t);
#endif
	if (disp_stats) {
		if (disp_stats < 0) { disp_stats = xrc_tool_env("XRC_DISP_STATS") != 0; }
		if (disp_stats) dst_add(t, TS_DEPTH(ts) ? ts->calls[TS_DEPTH(ts) - 1] : 0);
	}
#ifdef XRC_DEEP_STACKS
	if (ts->depth < XRC_MAXDEPTH) ts->calls[ts->depth] = t;
	ts->depth++;
#else
	ts->calls[ts->depth++] = t;
#endif
	if (xrc_dwatch_on) xrc_dwatch_check(c);
#ifndef XRC_NO_TOOLS
	ts->n++;
#endif
	if (xrc_gil_waiters) xrc_preempt();
	if (trace_calls) fprintf(stderr, "xrc: call %08x  eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x\n", t, c->eax,
	                         c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
	if (t >= XRC_IMP_BASE) {
		unsigned i = (t - XRC_IMP_BASE) / 16u;
		if (i >= nimps) xrc_fatal(c, "call to %08x in the import area (no such import)", t);
		if (!imps[i].fn && !imps[i].wrap) xrc_fatal(c, "unimplemented import %s!%s", imps[i].dll, imps[i].name);
		if (trace_imports < 0) trace_imports = xrc_tool_env("XRC_TRACE_IMPORTS") != 0;
		if (trace_imports) fprintf(stderr, "xrc: import %s!%s\n", imps[i].dll, imps[i].name);
		xrc_cur_import = imps[i].name;
		xrc_cur_body = imps[i].fn;
		xrc_cur_slot = i;
		if (imp_time < 0) imp_time = xrc_tool_env("XRC_IMPORT_TIME") ? atoi(xrc_tool_env("XRC_IMPORT_TIME")) : 0;
		if (imp_time && !imp_depth) {
			unsigned long long t0 = imp_now(), o0 = xrc_gil_off_us(), dt, off;
			imp_depth++;
			(imps[i].wrap ? imps[i].wrap : imps[i].fn)(c);
			imp_depth--;
			dt = imp_now() - t0;
			off = xrc_gil_off_us() - o0;
			if (off > dt) off = dt;
			imp_acct(i, dt - off, off);
		} else if (xrc_stall_main_tid && !imp_depth && GetCurrentThreadId() == xrc_stall_main_tid) { /* (integ5 XRC_STALL_DIAG: main-thread imports of 1 ms or more are logged, with the frame's CSV) */
			unsigned long long t0 = xrc_qpc(), dt;
			imp_depth++;
			(imps[i].wrap ? imps[i].wrap : imps[i].fn)(c);
			imp_depth--;
			dt = xrc_qpc() - t0;
			if (dt >= 10000ull) xrc_stall_imp(imps[i].name, t0, dt);
		} else (imps[i].wrap ? imps[i].wrap : imps[i].fn)(c);
	} else if ((f = hfind(t)) != 0) {
#ifdef XRC_COUNT_CALLS
		disp_count(t);
#endif
#ifndef XRC_NO_TOOLS
		if (trace_from) {
			u32 from = TS_DEPTH(ts) >= 2 ? ts->calls[TS_DEPTH(ts) - 2] : 0;
			if (trace_from < 0) trace_from_init();
			if (trace_from > 0 && from && trace_from_hit(from)) {
				u32 a0 = M32(c->esp + 4), ecx = c->ecx;
				unsigned long long t0 = xrc_now_us();
				f(c);
				fprintf(stderr, "xrc from: thread %x %08x -> %08x (ecx %08x, [esp+4] %08x) = %08x  %llu us\n", xrc_thread_id(), from, t, ecx, a0, c->eax,
				        xrc_now_us() - t0);
				goto traced;
			}
		}
#endif
		f(c);
	} else {
		unsigned k;
		for (k = 0; k < xrc_noverrides; k++)
			if (xrc_overrides[k].addr == t) xrc_fatal(c, "%08x needs a hand-written override (%s)", t, xrc_overrides[k].why);
		if (t >= XRC_IMAGE_BASE && t < XRC_IMAGE_BASE + image_size)
			xrc_fatal(c, "no translation for %08x (not a function start: a computed jump into a function?)", t);
		xrc_fatal(c, "call to %08x, outside the image (a bad function pointer)", t);
	}
#ifndef XRC_NO_TOOLS
traced:
#endif
	if (c->eip != ra) xrc_fatal(c, "%08x returned to %08x, not to its caller (%08x)", t, c->eip, ra);
	ts->depth--;
}

#ifndef XRC_NO_IC
/* an XCALL site's inline cache missed (src/include/xrc.h): dispatch, then cache the target -- not while a
 * diagnostic must see every call (XRC_TRACE_CALLS / XRC_TRACE_IMPORTS / XRC_IMPORT_TIME / counts),
 * and not a target that is neither an import nor in the table (it stopped in xrc_dispatch). XRC_NO_IC=1 (env): never
 * cache (to compare). XRC_DISP_STATS counts the misses only. */
static int ic_off = -1;
unsigned long long xrc_ic_misses;
void xrc_imp_enter(u32 i) {
	xrc_cur_import = imps[i].name;
	xrc_cur_body = imps[i].fn;
	xrc_cur_slot = i;
}
#ifdef XRC_PNONE
/* the import handlers keep the usual calling convention: a cached import call goes through this (XRC_CC) trampoline,
 * whose import XCALL's xrc_imp_enter has just named (xrc_cur_slot) */
static XRC_CC void imp_tramp(XCtx *c) { (imps[xrc_cur_slot].wrap ? imps[xrc_cur_slot].wrap : imps[xrc_cur_slot].fn)(c); }
#endif
/* indirect-call-site profile for the translator's devirtualization (XRC_SITE_FILE=FILE, XRC_SITE_DELAY=s, XRC_SITE_SECS=s):
 * every indirect call goes through here with the inline cache off; "calls return-address target" lines (decimal, hex, hex) are written once. Under the GIL: no locking. */
typedef struct { u32 ra, t, n; } SiteRow;
static SiteRow *site_tab; static int site_on = -1, site_done; static unsigned long long site_t0, site_t1;
#define SITE_CAP (1u << 21)
static void site_dump(void) {
	const char *fn = getenv("XRC_SITE_FILE"); FILE *f = fopen(fn, "w"); unsigned i, n = 0;
	if (!f) return;
	for (i = 0; i < SITE_CAP; i++) if (site_tab[i].n) { fprintf(f, "%u %08x %08x\n", site_tab[i].n, site_tab[i].ra, site_tab[i].t); n++; }
	fclose(f); fprintf(stderr, "xrc: site profile: %u (site,target) pairs -> %s\n", n, fn);
}
static void site_count(u32 ra, u32 t) {
	unsigned long long now = GetTickCount64();
	unsigned h;
	if (site_done || now < site_t0) return;
	if (now >= site_t1) { site_done = 1; site_dump(); return; }
	h = (ra * 2654435761u ^ t * 40503u) & (SITE_CAP - 1);
	for (;;) {
		SiteRow *r = &site_tab[h];
		if (!r->n) { r->ra = ra; r->t = t; r->n = 1; return; }
		if (r->ra == ra && r->t == t) { r->n++; return; }
		h = (h + 1) & (SITE_CAP - 1);
	}
}
void xrc_dispatch_ic(XCtx *c, u32 t, XIc *ic) {
	xrc_ic_misses++;
	if (site_on < 0) {
		site_on = getenv("XRC_SITE_FILE") != 0;
		if (site_on) {
			const char *d = getenv("XRC_SITE_DELAY"), *sc = getenv("XRC_SITE_SECS");
			site_tab = (SiteRow *)calloc(SITE_CAP, sizeof(SiteRow));
			site_t0 = GetTickCount64() + (unsigned long long)((d ? atof(d) : 0) * 1000); site_t1 = site_t0 + (unsigned long long)((sc ? atof(sc) : 60) * 1000);
			ic_off = 1; fprintf(stderr, "xrc: site profile on (XRC_SITE_FILE)\n");
		}
	}
	if (site_on && site_tab) site_count(M32(c->esp), t);
	xrc_dispatch(c, t);
	if (site_on) return;
	if (ic_off) {
		if (ic_off > 0) return;
		ic_off = getenv("XRC_NO_IC") || xrc_tool_env("XRC_TRACE_CALLS") || xrc_tool_env("XRC_TRACE_IMPORTS") || xrc_tool_env("XRC_IMPORT_TIME") ||
		         xrc_tool_env("XRC_TRACE_FROM");
#ifdef XRC_COUNT_CALLS
		ic_off = 1;
#endif
		if (ic_off) return;
	}
	if (t >= XRC_IMP_BASE) {
		unsigned i = (t - XRC_IMP_BASE) / 16u;
#ifdef XRC_PNONE
		ic->fn = imp_tramp;
#else
		ic->fn = imps[i].wrap ? imps[i].wrap : imps[i].fn;
#endif
		ic->imp = i + 1u;
		ic->addr = t;
	} else {
		XFn f = hfind(t);
		if (!f) return;
		ic->fn = f;
		ic->imp = 0;
		ic->addr = t;
	}
}
#endif

/* XRC_COUNT_CALLS builds: each translated function's call count (registered at its first call). XRC_COUNT_FILE=FILE
 * gets "count addr dispatched" lines, the most called first, of the calls after XRC_PROFILE_DELAY seconds (xrc_count_mark) */
#ifdef XRC_COUNT_CALLS
/* (and how many of each function's calls came through xrc_dispatch: an indirect call, a computed jump, a callback) */
#define DCN (1u << 17)
static struct { u32 a; unsigned long long n, base; } dcnt[DCN];
static void disp_count(u32 t) {
	u32 i = (t * 2654435761u) & (DCN - 1);
	while (dcnt[i].a && dcnt[i].a != t) i = (i + 1) & (DCN - 1);
	dcnt[i].a = t;
	dcnt[i].n++;
}
static unsigned long long disp_of(u32 t) {
	u32 i = (t * 2654435761u) & (DCN - 1);
	while (dcnt[i].a && dcnt[i].a != t) i = (i + 1) & (DCN - 1);
	return dcnt[i].a ? dcnt[i].n - dcnt[i].base : 0;
}
static XrcCount *count_list;
static unsigned count_n;
void xrc_count_reg(XrcCount *k) { k->reg = 1; k->next = count_list; count_list = k; count_n++; }
static int count_cmp(const void *a, const void *b) {
	unsigned long long x = (*(XrcCount *const *)a)->n - (*(XrcCount *const *)a)->base, y = (*(XrcCount *const *)b)->n - (*(XrcCount *const *)b)->base;
	return x < y ? 1 : x > y ? -1 : 0;
}
static unsigned long long count_disp0;
void xrc_count_mark(void) { XrcCount *k; u32 i; for (k = count_list; k; k = k->next) k->base = k->n; for (i = 0; i < DCN; i++) dcnt[i].base = dcnt[i].n; count_disp0 = ncalls; }
void xrc_count_dump(void) {
	const char *file = xrc_tool_env("XRC_COUNT_FILE");
	XrcCount **v, *k;
	unsigned i = 0;
	FILE *f;
	if (!file || !(f = fopen(file, "w"))) return;
	v = (XrcCount **)malloc((count_n + 1) * sizeof *v);
	for (k = count_list; k && i < count_n; k = k->next) v[i++] = k;
	qsort(v, i, sizeof *v, count_cmp);
	for (unsigned j = 0; j < i; j++)
		if (v[j]->n - v[j]->base) fprintf(f, "%llu %08x %llu\n", v[j]->n - v[j]->base, v[j]->addr, disp_of(v[j]->addr));
	fprintf(f, "# %llu calls through xrc_dispatch\n", ncalls - count_disp0);
	fclose(f);
	free(v);
	fprintf(stderr, "xrc: call counts of %u functions in %s\n", i, file);
}
#else
void xrc_count_mark(void) {}
void xrc_count_dump(void) {}
#endif

void xrc_ebp_bad(XCtx *c, u32 t, u32 was) {
	/* first time per callee, 64 callees at most: the SEH prolog / epilog helpers would fill any count of lines */
	static u32 seen[64]; static int n;
	for (int i = 0; i < n; i++) if (seen[i] == t) return;
	if (n >= 64) return;
	seen[n++] = t;
	fprintf(stderr, "xrc: ebp-check: %08x changed ebp %08x -> %08x (esp %08x, return %08x)\n", t, was, c->ebp, c->esp, c->eip);
}
/* a directly called translation (XCALLD) returned somewhere else than to its caller */
void xrc_bad_return(XCtx *c, u32 t) { xrc_fatal(c, "%08x returned to %08x, not to its caller", t, c->eip); }

/* ------------------------------------------------------------------------------------------------ profiler
 * XRC_PROFILE=FILE: SIGPROF samples (1 kHz of CPU time) of the running guest function (the top of the sampled thread's
 * call stack) and the host PC; at exit FILE gets one line per sample "guest host-offset thread guest-caller time-us
 * caller2 .. caller7" (the host PC relative to __executable_start, as nm prints the symbols of the PIE binary; time:
 * xrc_now_us; callers: the guest call stack below the top, 0 past its bottom), and "#frame time-us" per Present
 * (xrc_prof_frame) -- the Windows sampler's format, but of CPU time: a blocked thread has no samples.
 * XRC_PROFILE_SECS: written after that long (after XRC_PROFILE_DELAY) by a thread of its own while the game carries on --
 * the reliable way: a dump at SIGINT / SIGTERM sometimes stalls (not understood).
 * XRC_PROFILE_WALL=1: the main thread only, every millisecond of real time -- where its time goes, blocked included (a
 * PC in a library becomes its nearest caller in the executable). "#lib" lines map the libraries' code (the samples'
 * offsets outside the executable). */
#ifndef _WIN32
#include <sys/time.h>
#include <ucontext.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/syscall.h>
extern char __executable_start[], etext[];
static int prof_wall; /* XRC_PROFILE_WALL=1: the main thread only, every ms of real time (blocked or not) */
#define NPROF (1u << 22)
#define PROF_DEPTH 8
static u32 *prof_guest, *prof_stack, *prof_tid;
static unsigned long long *prof_host; /* (64 bits: a PC in a shared library is far from the executable) */
static unsigned long long *prof_tm, *prof_frames;
static volatile unsigned prof_n, prof_nframes;
#define NPFRAMES (1u << 20)
static const char *prof_file;
static double prof_from;
static void on_prof(int sig, siginfo_t *si, void *ucv) {
	ucontext_t *uc = (ucontext_t *)ucv;
	unsigned i;
	(void)sig; (void)si;
	if (prof_from) { /* XRC_PROFILE_DELAY: no samples before then */
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if ((double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 < prof_from) return;
	}
	i = __atomic_fetch_add(&prof_n, 1u, __ATOMIC_RELAXED);
	if (i >= NPROF) return;
	{
		TStack *t = my_ts;
		int d = t ? TS_DEPTH(t) : 0, k;
		prof_guest[i] = d ? t->calls[d - 1] : 0;
#ifdef XRC_DEEP_STACKS
		for (k = 0; k < PROF_DEPTH - 1; k++) prof_stack[(size_t)i * PROF_DEPTH + k] = d - 2 - k >= 0 ? t->calls[d - 2 - k] : 0;
#else
		{ /* the callers: the guest frame chain (every call, not only the dispatched ones) */
			u32 w[PROF_DEPTH - 1];
			int nw = t ? xrc_walk_stack(t->ctx, w, PROF_DEPTH - 1) : 0;
			for (k = 0; k < PROF_DEPTH - 1; k++) prof_stack[(size_t)i * PROF_DEPTH + k] = k < nw ? w[k] : 0;
		}
#endif
		prof_tid[i] = t ? t->tid : 0;
	}
	{
#if defined(__aarch64__)
		uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#else
		uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
		if (prof_wall && (pc < (uintptr_t)__executable_start || pc >= (uintptr_t)etext)) {
			/* (blocked or busy in a library: the nearest caller in the executable instead -- what waits) */
			void *bt[48];
			int nb = backtrace(bt, 48), k, at = -1;
			for (k = 0; k < nb; k++) if ((uintptr_t)bt[k] == pc) { at = k; break; }
			for (k = at < 0 ? 0 : at + 1; k < nb; k++)
				if ((uintptr_t)bt[k] >= (uintptr_t)__executable_start && (uintptr_t)bt[k] < (uintptr_t)etext && (void *)bt[k] != (void *)on_prof) {
					pc = (uintptr_t)bt[k] - 1;
					break;
				}
		}
		prof_host[i] = (unsigned long long)(pc - (uintptr_t)__executable_start);
	}
	prof_tm[i] = xrc_now_us();
}
/* the dump runs from a signal handler too (SIGINT / SIGTERM): no stdio, no allocation -- another thread may hold their
 * locks when it lands (a hang until killed, the file empty). The file is opened at the start; lines are formatted here. */
static int prof_fd = -1;
static char prof_buf[1 << 16];
static unsigned prof_len;
static void pb_flush(void) {
	unsigned o = 0;
	while (o < prof_len) { ssize_t w = write(prof_fd, prof_buf + o, prof_len - o); if (w <= 0) break; o += (unsigned)w; }
	prof_len = 0;
}
static void pb_str(const char *t) { while (*t) { if (prof_len == sizeof prof_buf) pb_flush(); prof_buf[prof_len++] = *t++; } }
static void pb_hex(unsigned long long v, int width) {
	char t[24];
	int n = 0;
	do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v || n < width);
	if (prof_len + 24 > sizeof prof_buf) pb_flush();
	while (n) prof_buf[prof_len++] = t[--n];
}
static void pb_dec(unsigned long long v) {
	char t[24];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	if (prof_len + 24 > sizeof prof_buf) pb_flush();
	while (n) prof_buf[prof_len++] = t[--n];
}
static unsigned long long hexnum(const char **q) {
	unsigned long long v = 0;
	for (;; (*q)++) {
		char ch = **q;
		if (ch >= '0' && ch <= '9') v = v * 16 + (unsigned)(ch - '0');
		else if (ch >= 'a' && ch <= 'f') v = v * 16 + (unsigned)(ch - 'a' + 10);
		else return v;
	}
}
static void prof_dump(void) {
	static volatile int once;
	unsigned k, j, n = prof_n < NPROF ? prof_n : NPROF;
	struct itimerval it;
	memset(&it, 0, sizeof it);
	setitimer(ITIMER_PROF, &it, 0);
	if (prof_wall) signal(SIGPROF, SIG_IGN);
/* a signal-safe write to stderr (none with -DXRC_NO_TOOLS: xrc_quiet.h) */
#ifdef XRC_NO_TOOLS
#define xrc_errwrite(buf, n) ((ssize_t)(n))
#else
#define xrc_errwrite(buf, n) write(2, buf, n)
#endif
#define TCK(m) if (xrc_tool_env("XRC_TRACE_TERM")) { ssize_t w_ = xrc_errwrite(m "\n", sizeof m); (void)w_; }
	TCK("xrc: dump: timer off")
	if (prof_fd < 0 || __atomic_exchange_n(&once, 1, __ATOMIC_SEQ_CST)) return;
	TCK("xrc: dump: imports")
	for (k = 0; k < nimps; k++) {
		pb_str("# "); pb_hex(XRC_IMP_BASE + 16u * k, 8); pb_str(" "); pb_str(imps[k].dll); pb_str("!"); pb_str(imps[k].name); pb_str("\n");
	}
	{ /* the shared libraries ("#lib start end file-offset path", relative to __executable_start, as the samples): a PC
	   * outside the executable is resolved with the library's own symbols */
		int mf = open("/proc/self/maps", O_RDONLY);
		static char mb[1 << 16];
		if (mf >= 0) {
			ssize_t got;
			unsigned ml = 0;
			while ((got = read(mf, mb + ml, sizeof mb - 1 - ml)) > 0) ml += (unsigned)got;
			close(mf);
			mb[ml] = 0;
			{
				char *l = mb, *e;
				for (; *l; l = e + 1) {
					unsigned long long a0, a1, off;
					const char *q, *perm;
					char *path;
					e = strchr(l, '\n');
					if (!e) break;
					*e = 0;
					path = strchr(l, '/');
					/* "start-end perms offset ..." (parsed by hand: no stdio here) */
					q = l; a0 = hexnum(&q); q++; a1 = hexnum(&q); q++; perm = q; q += 5; off = hexnum(&q);
					if (path && perm[2] == 'x') {
						pb_str("#lib "); pb_hex(a0 - (unsigned long long)(uintptr_t)__executable_start, 1); pb_str(" ");
						pb_hex(a1 - (unsigned long long)(uintptr_t)__executable_start, 1); pb_str(" "); pb_hex(off, 1); pb_str(" ");
						pb_str(path); pb_str("\n");
					}
				}
			}
		}
	}
	TCK("xrc: dump: samples")
	for (k = 0; k < n; k++) {
		const u32 *cs = prof_stack + (size_t)k * PROF_DEPTH;
		pb_hex(prof_guest[k], 8); pb_str(" "); pb_hex(prof_host[k], 1); pb_str(" "); pb_dec(prof_tid[k]); pb_str(" ");
		pb_hex(cs[0], 8); pb_str(" "); pb_dec(prof_tm[k]);
		for (j = 1; j < PROF_DEPTH - 1; j++) { pb_str(" "); pb_hex(cs[j], 8); }
		pb_str("\n");
	}
	for (k = 0; k < prof_nframes && k < NPFRAMES; k++) { pb_str("#frame "); pb_dec(prof_frames[k]); pb_str("\n"); }
	pb_flush();
	close(prof_fd);
	prof_fd = -1;
	pb_str("xrc: "); pb_dec(n); pb_str(" profile samples in "); pb_str(prof_file); pb_str("\n");
	{ ssize_t w = xrc_errwrite(prof_buf, prof_len); (void)w; }
	prof_len = 0;
	prof_file = 0;
}
void xrc_prof_frame(void) {
	unsigned i;
	struct timespec ts;
	if (!prof_frames || !prof_file) return;
	if (prof_from) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if ((double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 < prof_from) return;
	}
	i = __atomic_fetch_add(&prof_nframes, 1u, __ATOMIC_RELAXED);
	if (i < NPFRAMES) prof_frames[i] = xrc_now_us();
}
/* SIGINT / SIGTERM: the handler only wakes the dump thread (it may land on a thread inside anything) */
#include <semaphore.h>
#include <errno.h>
#include <pthread.h>
static sem_t term_sem;
static int term_thread_up;
static double prof_until; /* XRC_PROFILE_SECS: the dump at this time (CLOCK_REALTIME, for sem_timedwait) */
static void *term_thread(void *arg) {
#if !defined(_WIN32) && !defined(XRC_NO_TOOLS)
	pthread_setname_np(pthread_self(), "xrc-term");
#endif
	(void)arg;
	if (prof_until) {
		struct timespec ts;
		int r;
		ts.tv_sec = (time_t)prof_until;
		ts.tv_nsec = (long)((prof_until - (double)ts.tv_sec) * 1e9);
		while ((r = sem_timedwait(&term_sem, &ts)) != 0 && errno == EINTR) {}
		if (r != 0) { /* the time is up: the samples so far, and the game carries on */
			prof_dump();
			while (sem_wait(&term_sem) != 0) {}
			fflush(stderr); _exit(124);
		}
	} else
		while (sem_wait(&term_sem) != 0) {}
	if (xrc_tool_env("XRC_TRACE_TERM")) { ssize_t w = xrc_errwrite("xrc: dumping\n", 13); (void)w; }
	prof_dump();
	xrc_count_dump();
	fflush(stderr); _exit(124);
	return 0;
}
static void on_term(int sig) {
	(void)sig;
	{ ssize_t w = xrc_errwrite("xrc: a termination signal: stopping\n", 36); (void)w; }
	if (term_thread_up) sem_post(&term_sem);
	else _exit(124);
}
static void term_setup(void) {
	pthread_t t;
	if (term_thread_up) return;
	sem_init(&term_sem, 0, 0);
	if (pthread_create(&t, 0, term_thread, 0) == 0) term_thread_up = 1;
}
static void prof_start(void) {
	struct sigaction sa;
	struct itimerval it;
	prof_file = xrc_tool_env("XRC_PROFILE");
	if (xrc_tool_env("XRC_COUNT_FILE")) { term_setup(); signal(SIGTERM, on_term); signal(SIGINT, on_term); atexit(xrc_count_dump); }
	if (!prof_file || !*prof_file) return;
	if (xrc_tool_env("XRC_PROFILE_DELAY")) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		prof_from = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 + atof(xrc_tool_env("XRC_PROFILE_DELAY"));
	}
	if ((prof_fd = open(prof_file, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) { perror(prof_file); return; }
	prof_guest = (u32 *)calloc(NPROF, 4);
	prof_host = (unsigned long long *)calloc(NPROF, 8);
	prof_tid = (u32 *)calloc(NPROF, 4);
	prof_stack = (u32 *)calloc((size_t)NPROF * PROF_DEPTH, 4);
	prof_tm = (unsigned long long *)calloc(NPROF, 8);
	prof_frames = (unsigned long long *)calloc(NPFRAMES, 8);
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_prof;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &sa, 0);
	if (xrc_tool_env("XRC_PROFILE_SECS") && atof(xrc_tool_env("XRC_PROFILE_SECS")) > 0) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		prof_until = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 + atof(xrc_tool_env("XRC_PROFILE_SECS")) +
		             (xrc_tool_env("XRC_PROFILE_DELAY") ? atof(xrc_tool_env("XRC_PROFILE_DELAY")) : 0);
	}
	term_setup();
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	atexit(prof_dump);
	it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 1000;
	it.it_value = it.it_interval;
	if ((prof_wall = xrc_tool_env("XRC_PROFILE_WALL") != 0)) {
		/* the calling (main) thread's own real-time timer: its samples blocked too; outside the executable a sample
		 * takes the nearest caller in it (backtrace, primed here: its first call loads the unwinder) */
		struct sigevent sev;
		timer_t tm;
		struct itimerspec its;
		void *bt[4];
		backtrace(bt, 4);
		memset(&sev, 0, sizeof sev);
		sev.sigev_notify = SIGEV_THREAD_ID;
		sev.sigev_signo = SIGPROF;
		sev._sigev_un._tid = (int)syscall(SYS_gettid);
		if (timer_create(CLOCK_MONOTONIC, &sev, &tm) == 0) {
			its.it_interval.tv_sec = 0; its.it_interval.tv_nsec = 1000000;
			its.it_value = its.it_interval;
			timer_settime(tm, 0, &its, 0);
			fprintf(stderr, "xrc: XRC_PROFILE_WALL: the main thread, every ms of real time\n");
			return;
		}
	}
	setitimer(ITIMER_PROF, &it, 0);
}
#else
/* Windows: a sampler thread suspends each guest thread every millisecond (of real time) and reads its RIP; after
 * XRC_PROFILE_SECS (default 20; after XRC_PROFILE_DELAY seconds) it writes FILE: "guest host-offset thread guest-caller
 * time-us caller2 .. caller7" per sample (time: xrc_now_us; callers: the guest call stack below the top, 0 past its
 * bottom), the host offset relative to the image base (as `x86_64-w64-mingw32-nm` addresses minus the ImageBase), ~0
 * when the thread was outside the image (blocked in the system: then also "module+offset image-return", the system
 * address and the nearest 4 return addresses into the image, from a scan of its stack); and "#frame time-us" per Present in that time
 * (xrc_prof_frame): a slow frame's samples are the ones between its two Presents. Written by the sampler itself: a
 * process killed from WSL runs no exit handlers. */
void *xrc_prof_handles[XRC_MAXT];
#define NPROF (1u << 22)
#define PROF_DEPTH 8
static unsigned long long *prof_frames;
static volatile unsigned prof_nframes, prof_on;
static volatile long prof_stop, prof_written; /* (xrc_exit_now: sampling ends and the file is written before the exit) */
#define NPFRAMES (1u << 20)
void xrc_prof_frame(void) {
	unsigned i;
	if (!prof_on) return;
	i = __atomic_fetch_add(&prof_nframes, 1u, __ATOMIC_RELAXED);
	if (i < NPFRAMES) prof_frames[i] = xrc_now_us();
}
static DWORD WINAPI prof_thread(LPVOID arg) {
	const char *file = (const char *)arg;
	const char *e = xrc_tool_env("XRC_PROFILE_SECS");
	unsigned secs = e && atoi(e) > 0 ? (unsigned)atoi(e) : 20, n = 0, k, j, nf;
	u32 *h = (u32 *)calloc(NPROF, 4), *st = (u32 *)calloc((size_t)NPROF * PROF_DEPTH, 4);
	unsigned long long *tm = (unsigned long long *)calloc(NPROF, 8);
	u8 *th = (u8 *)calloc(NPROF, 1);
	unsigned long long *rip = (unsigned long long *)calloc(NPROF, 8);
	u32 *hc = (u32 *)calloc((size_t)NPROF * 4, 4); /* (up to 4 return addresses into the image) */
	uintptr_t base = (uintptr_t)GetModuleHandleA(0), lim;
	static DWORD64 pstk[2048]; /* (a copy of a sampled stack: 16 KB) */
	DWORD t0;
	FILE *f;
	{ /* the image's extent (its pages are all mapped: the call bytes before a return address can be read) */
		const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)base;
		const IMAGE_NT_HEADERS *nh = (const IMAGE_NT_HEADERS *)(base + (uintptr_t)dh->e_lfanew);
		lim = base + nh->OptionalHeader.SizeOfImage;
	}
	if (!h || !st || !tm || !th || !rip || !hc || !prof_frames) { fprintf(stderr, "xrc: profiler: out of memory\n"); return 0; }
	if ((e = xrc_tool_env("XRC_PROFILE_DELAY")) != 0 && atoi(e) > 0) Sleep((DWORD)atoi(e) * 1000u); /* (seconds before sampling) */
	prof_on = 1;
	t0 = GetTickCount();
	while (GetTickCount() - t0 < secs * 1000u && n + XRC_MAXT < NPROF && !prof_stop) {
		int i, na = __atomic_load_n(&nall, __ATOMIC_ACQUIRE);
		for (i = 0; i < na; i++) {
			CONTEXT ctx;
			HANDLE t = (HANDLE)xrc_prof_handles[i];
			if (!t || SuspendThread(t) == (DWORD)-1) continue;
			memset(&ctx, 0, sizeof ctx);
			ctx.ContextFlags = CONTEXT_CONTROL;
			if (GetThreadContext(t, &ctx)) {
				int d = TS_DEPTH(&tstacks[i]);
				rip[n] = ctx.Rip;
				hc[(size_t)n * 4] = hc[(size_t)n * 4 + 1] = hc[(size_t)n * 4 + 2] = hc[(size_t)n * 4 + 3] = 0xffffffffu;
				if (ctx.Rip < base || ctx.Rip >= lim) {
					/* outside the image: the nearest return addresses into it, found by scanning the stack (copied with
					 * ReadProcessMemory, which fails instead of faulting) for values just after a call instruction in
					 * the image. (Not RtlVirtualUnwind: a thread stopped anywhere, e.g. in a driver without unwind data,
					 * can make it raise STATUS_BAD_FUNCTION_TABLE.) */
					SIZE_T got_b = 0;
					int got = 0;
					unsigned w;
					{ /* (as much as can be read: a read past the stack's top fails as a whole) */
						SIZE_T want = sizeof pstk - (ctx.Rsp & 4095); /* (ending on a page boundary) */
						while (want >= 8 && (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)ctx.Rsp, pstk, want, &got_b) || !got_b)) {
							got_b = 0;
							want = want > 4096 ? want - 4096 : 0;
						}
					}
					for (w = 0; w < got_b / 8 && got < 4; w++) {
						DWORD64 q = pstk[w];
						const u8 *r = (const u8 *)(uintptr_t)q;
						if (q < base + 0x1000 + 8 || q >= lim) continue;
						if (r[-5] == 0xe8 || (r[-6] == 0xff && r[-5] == 0x15) || (r[-2] == 0xff && (r[-1] & 0xf8) == 0xd0)
						    || (r[-3] == 0xff && ((r[-2] >> 3) & 7) == 2 && (r[-2] >> 6) == 1))
							hc[(size_t)n * 4 + got++] = (u32)(q - base);
					}
				}
				for (j = 0; j < PROF_DEPTH; j++) st[(size_t)n * PROF_DEPTH + j] = d > (int)j ? tstacks[i].calls[d - 1 - (int)j] : 0;
				h[n] = ctx.Rip >= base && ctx.Rip < lim ? (u32)(ctx.Rip - base) : 0xffffffffu;
				th[n] = (u8)i;
				tm[n] = xrc_now_us();
				n++;
			}
			ResumeThread(t);
		}
		Sleep(1);
	}
	prof_on = 0;
	nf = prof_nframes < NPFRAMES ? prof_nframes : NPFRAMES;
	if ((f = fopen(file, "w")) != 0) {
		for (k = 0; k < nimps; k++) fprintf(f, "# %08x %s!%s\n", XRC_IMP_BASE + 16u * k, imps[k].dll, imps[k].name);
		for (k = 0; k < (unsigned)__atomic_load_n(&nall, __ATOMIC_ACQUIRE); k++) fprintf(f, "#thread %u %x\n", k, tstacks[k].tid);
		for (k = 0; k < nf; k++) fprintf(f, "#frame %llu\n", prof_frames[k]);
		for (k = 0; k < n; k++) {
			u32 *s = st + (size_t)k * PROF_DEPTH;
			fprintf(f, "%08x %x %u %08x %llu", s[0], h[k], th[k], s[1], tm[k]);
			for (j = 2; j < PROF_DEPTH; j++) fprintf(f, " %08x", s[j]);
			if (h[k] == 0xffffffffu) { /* outside: the module and offset, and the image's return address below it */
				HMODULE m = 0;
				char mn[MAX_PATH], *b;
				static HMODULE lm; static char lmn[MAX_PATH];
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)rip[k], &m) && m) {
					if (m != lm) { lm = m; if (!GetModuleFileNameA(m, lmn, sizeof lmn)) strcpy(lmn, "?"); }
					strcpy(mn, lmn);
					b = strrchr(mn, '\\');
					fprintf(f, " %s+%llx", b ? b + 1 : mn, (unsigned long long)(rip[k] - (uintptr_t)m));
				} else fprintf(f, " ?+%llx", rip[k]);
				for (j = 0; j < 4; j++) fprintf(f, " %x", hc[(size_t)k * 4 + j]);
			}
			fputc('\n', f);
		}
		fclose(f);
	}
	fprintf(stderr, "xrc: %u profile samples, %u frames in %s\n", n, nf, file);
	InterlockedExchange(&prof_written, 1);
	return 0;
}
/* (the exit: a sampling in progress ends there and its file is written -- up to 60 s) */
static void prof_finish(void) {
	int k;
	if (!prof_on || prof_written) return;
	InterlockedExchange(&prof_stop, 1);
	fprintf(stderr, "xrc: ending: writing the profile first\n");
	for (k = 0; k < 6000 && !prof_written; k++) Sleep(10);
}
static void prof_start(void) {
	const char *file = xrc_tool_env("XRC_PROFILE");
	if (!file || !*file) return;
	prof_frames = (unsigned long long *)calloc(NPFRAMES, 8);
	CloseHandle(CreateThread(0, 0, prof_thread, (LPVOID)file, 0, 0));
}
#endif

/* ------------------------------------------------------------------------------------------------ ending */
#ifndef XRC_CLOSE_WAIT_MS
#define XRC_CLOSE_WAIT_MS 3000
#endif
void xrc_audio_silence(void) {
	host_audio_silence_all();
#ifdef XRC_REFERENCE
	ref_xaudio2_silence();
#endif
}
#ifdef _WIN32
/* The game's own "Reboot" (its settings menu: File) saves the settings and exits for the launcher to start it again;
 * here the runtime starts itself again, with the same command line, environment, folder and standard handles.
 * XRC_NO_RELAUNCH=1: it only exits (tests). */
static void relaunch(void) {
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	char cmd[8192], n[16];
	const char *k = getenv("XRC_RESTARTS");
	int restarts = k ? atoi(k) : 0;
	if (restarts >= 20) { fprintf(stderr, "xrc: restarting: not again (20 restarts in a row)\n"); return; }
	snprintf(n, sizeof n, "%d", restarts + 1);
	SetEnvironmentVariableA("XRC_RESTARTS", n);
	SetEnvironmentVariableA("XRC_MENU_PICK", NULL); /* (a test's picks are not made again by the new copy) */
	SetEnvironmentVariableA("XRC_SETTINGS_PICK", NULL);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	snprintf(cmd, sizeof cmd, "%s", GetCommandLineA());
	if (CreateProcessA(0, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) {
		fprintf(stderr, "xrc: restarting: pid %lu\n", (unsigned long)pi.dwProcessId);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	} else fprintf(stderr, "xrc: restarting failed (%lu)\n", GetLastError());
	fflush(stderr);
}
void xrc_relaunch_at_exit(void) {
	static int once;
	if (once++) return;
	if (getenv("XRC_NO_RELAUNCH") && atoi(getenv("XRC_NO_RELAUNCH"))) { fprintf(stderr, "xrc: restart asked (XRC_NO_RELAUNCH: not done)\n"); return; }
	atexit(relaunch);
}
#endif
void xrc_exit_now(int code) {
	static volatile long ending;
	xrc_audio_silence();
#ifdef _WIN32
	if (InterlockedExchange(&ending, 1)) for (;;) Sleep(INFINITE); /* (the other caller ends it) */
#ifndef XRC_NO_TOOLS
	prof_finish();
#endif
	fflush(stdout);
	_cexit(); /* the atexit handlers and the streams' flush, without ExitProcess's DLL detach */
	TerminateProcess(GetCurrentProcess(), (UINT)code);
	for (;;) Sleep(INFINITE);
#else
	if (__sync_lock_test_and_set(&ending, 1)) for (;;) pause();
	exit(code);
#endif
}
#ifdef _WIN32
static DWORD WINAPI close_watch(LPVOID p) {
	(void)p;
	Sleep(XRC_CLOSE_WAIT_MS);
	fprintf(stderr, "xrc: the window was closed and the game did not end in %d ms: ending it\n", XRC_CLOSE_WAIT_MS);
	xrc_exit_now(0);
	return 0;
}
#else
static void *close_watch(void *p) {
	(void)p;
	usleep(XRC_CLOSE_WAIT_MS * 1000);
	fprintf(stderr, "xrc: the window was closed and the game did not end in %d ms: ending it\n", XRC_CLOSE_WAIT_MS);
	xrc_exit_now(0);
	return 0;
}
#endif
int xrc_view_w, xrc_view_h;
void *xrc_view_window;
int xrc_view_map(int cw, int ch, int *x0, int *y0, int *w, int *h) {
	double sx, sy, s;
	if (xrc_view_w <= 0 || xrc_view_h <= 0 || cw <= 0 || ch <= 0 || (cw == xrc_view_w && ch == xrc_view_h)) return 0;
	sx = (double)cw / xrc_view_w; sy = (double)ch / xrc_view_h; s = sx < sy ? sx : sy;
	*w = (int)(xrc_view_w * s); *h = (int)(xrc_view_h * s); /* (be_present's own arithmetic) */
	*x0 = (cw - *w) / 2; *y0 = (ch - *h) / 2;
	return *w > 0 && *h > 0;
}
void xrc_close_requested(void) {
	static volatile long started;
#ifdef _WIN32
	HANDLE h;
	if (InterlockedExchange(&started, 1)) return;
	if ((h = CreateThread(0, 0, close_watch, 0, 0, 0))) CloseHandle(h);
#else
	pthread_t t;
	if (__sync_lock_test_and_set(&started, 1)) return;
	if (!pthread_create(&t, 0, close_watch, 0)) pthread_detach(t);
#endif
}

/* ------------------------------------------------------------------------------------------------ entry */
#ifdef _WIN32
/* Ctrl-C / Ctrl-Break / the console closing (a timeout from WSL): end now. The default handler's ExitProcess can hang
 * in a DLL's detach (the driver's threads killed mid-call). */
static BOOL WINAPI on_console(DWORD ev) {
	(void)ev;
	xrc_audio_silence();
	host_movie_convert_stop(); /* (a background movie conversion: its processes and partial files) */
	fflush(stderr);
	fflush(stderr);
	TerminateProcess(GetCurrentProcess(), 124);
	return TRUE;
}
#endif
/* <this program without its extension>.cfg beside it (the launcher writes WhiteRecomp.cfg): "NAME=value" lines, each
 * set in the environment unless it is set already (XRC_GAME_DIR, XRC_CDRIVE, XRC_MOVIE_CONVERTER, ...); XRC_EXE is the
 * game's exe when the command line names none -- so the built game starts by itself, with a double click. */
#ifdef XRC_NAN_PROBE
unsigned long long xrc_nonfinite_n[2];
#endif
static void load_cfg(void) {
	char self[4096], line[4096];
	FILE *f;
	size_t n;
	char *dot, *sl;
#ifdef _WIN32
	n = GetModuleFileNameA(0, self, (DWORD)sizeof self - 8);
#else
	ssize_t r = readlink("/proc/self/exe", self, sizeof self - 8);
	n = r > 0 ? (size_t)r : 0;
#endif
	if (!n || n >= sizeof self - 8) return;
	self[n] = 0;
	dot = strrchr(self, '.');
	sl = strrchr(self, '/');
	if (!sl || (strrchr(self, '\\') && strrchr(self, '\\') > sl)) sl = strrchr(self, '\\');
	if (dot && (!sl || dot > sl)) strcpy(dot, ".cfg"); else strcat(self, ".cfg");
	if (!(f = fopen(self, "r"))) return;
	if (sl) sl[1] = 0; else self[0] = 0; /* (self: the program's folder, for the values that start with ./) */
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '='), *e, *v;
		char full[8192];
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		for (e = eq + 1 + strlen(eq + 1); e > eq + 1 && (e[-1] == '\n' || e[-1] == '\r'); ) *--e = 0;
		if (getenv(line)) continue;
		/* a value starting with ./ is a path from the program's folder (a cross-compiled build's .cfg: the folder
		 * is copied to the other machine, the game's install beside it) */
		v = eq + 1;
		if (v[0] == '.' && (v[1] == '/' || v[1] == '\\')) { snprintf(full, sizeof full, "%s%s", self, v + 2); v = full; }
#ifdef _WIN32
		_putenv_s(line, v);
#else
		setenv(line, v, 0);
#endif
	}
	fclose(f);
}
#ifdef XRC_LR
#define XRC_DEFAULT_EXE "LRFF13.exe"
#else
#define XRC_DEFAULT_EXE "ffxiiiimg.exe.unpacked.exe"
#endif
#ifdef _WIN32
/* spike7: an unbuffered stderr costs one WriteFile per formatted piece, ~0.3 ms each when stderr is a file or pipe (a 70-char
 * log line = 10-25 ms on the calling thread: the periodic 15-30 ms frames of the fps report). Now: a 64 KB buffer flushed by a
 * thread every 40 ms (and before every abnormal exit). XRC_STDERR_BUF=0: the old unbuffered stderr. */
static char s7_errbuf[65536];
static DWORD WINAPI s7_errflush(LPVOID a) { (void)a; for (;;) { Sleep(40); fflush(stderr); } return 0; }
static void spike7_stderr_init(void) {
	const char *e = getenv("XRC_STDERR_BUF");
	if (e && e[0] == '0') { setvbuf(stderr, 0, _IONBF, 0); return; }
	setvbuf(stderr, s7_errbuf, _IOFBF, sizeof s7_errbuf);
	CloseHandle(CreateThread(0, 0, s7_errflush, 0, 0, 0));
}
#endif
int main(int argc, char **argv) {
	static XCtx c;
#ifdef _WIN32
	spike7_stderr_init();
#else
	setvbuf(stderr, 0, _IONBF, 0); /* (a pipe would otherwise buffer it) */
#endif
#ifdef _WIN32
	SetConsoleCtrlHandler(on_console, TRUE);
#endif
	load_cfg();
#if defined(XRC_LR) && defined(_WIN32)
	if (argc > 2 && !strcmp(argv[1], "--convert-movies")) return host_movie_convert_all_lr(argv[2]); /* (the launcher's Prepare movies) */
#endif
	const char *exe = argc > 1 ? argv[1] : getenv("XRC_EXE") ? getenv("XRC_EXE") : XRC_DEFAULT_EXE;
	xrc_argc = argc > 2 ? argc - 2 : 0;
	xrc_argv = argv + 2; /* the game's own arguments follow the exe path */
#ifdef XRC_LR
	if (!getenv("XRC_GAME_DIR")) { /* the install: the exe's own folder */
		char gd[1024], *sl, *sl2;
		snprintf(gd, sizeof gd, "%s", exe);
		sl = strrchr(gd, '\\');
		sl2 = strrchr(gd, '/');
		if (!sl || (sl2 && sl2 > sl)) sl = sl2;
		if (sl) {
			*sl = 0;
#ifdef _WIN32
			_putenv_s("XRC_GAME_DIR", gd);
#else
			setenv("XRC_GAME_DIR", gd, 1);
#endif
		}
	}
#endif
	xrc_mem = reserve_guest();
	xrc_host_fp_mode();
	if (!xrc_mem) { fprintf(stderr, "xrc: cannot reserve %s of guest address space\n", xrc_guest_2gb ? "2 GB" : "4 GB"); return 2; }
	if (xrc_guest_2gb) fprintf(stderr, "xrc: guest memory: 2 GB (XRC_GUEST_2GB)\n");
	xrc_heap_init();
	add_matched();
#ifndef XRC_LR /* FFXIII's overrides sit at FFXIII addresses: none of them apply to Lightning Returns (full translation) */
	add_manual(xrc_manual_crt_math, xrc_manual_crt_math_n);
	add_manual(xrc_manual_crt_sort, xrc_manual_crt_sort_n);
	add_manual(xrc_manual_game, xrc_manual_game_n);
	add_manual(xrc_manual_simd, xrc_manual_simd_n);
#else
	add_manual(xrc_manual_lr_crt, xrc_manual_lr_crt_n); /* _ftol2 / _ftol2_sse linked into LRFF13.exe (hle_msvcr100.c) */
	add_manual(xrc_manual_miles, xrc_manual_miles_n);
	add_manual(xrc_manual_dbg, xrc_manual_dbg_n); /* ovr_debug.c: the debug camera (XRC_FREECAM) */
	add_manual(xrc_manual_frame, xrc_manual_frame_n); /* ovr_frame.c: frame limiter wait, clamp counter */
	add_manual(xrc_manual_math, xrc_manual_math_n); /* ovr_math.c, the 20 native math `fast` rows (were renamed xf_ symbols) */
	add_manual(xrc_manual_hooks, xrc_manual_hooks_n); /* ovr_hooks.c, the shadow hold wrapper and the other `fast` rows */
#endif
	build_htab();
	if (!load_exe(exe)) return 2;
	setup_thread();
	protect_image();
	prof_start();
#ifdef _WIN32
	timeBeginPeriod(1); /* 1 ms sleeps (the game asks winmm for it; its frame limiter sleeps) */
#endif
	xrc_gil_mark_main();
	xrc_gil_acquire();
	dwatch_init();
	xrc_names_load(); /* (names for the stack prints: xrc_names.c) */
	{
		extern void *xrc_gil_hog_watch(void *);
		const char *e = getenv("XRC_GIL_HOG_S");
		int s = e ? atoi(e) : 10;
		if (s > 0) {
			pthread_t w;
			if (!pthread_create(&w, 0, xrc_gil_hog_watch, (void *)(uintptr_t)s)) pthread_detach(w);
		}
	}
	if (xrc_tool_env("XRC_WATCH")) {
		pthread_t w;
		pthread_create(&w, 0, watchdog, (void *)(uintptr_t)(atoi(xrc_tool_env("XRC_WATCH")) > 0 ? atoi(xrc_tool_env("XRC_WATCH")) : 5));
		pthread_detach(w);
	}
	fprintf(stderr, "xrc: %s loaded, %u translated functions, %u overrides, %u imports; entry %08x\n", exe, xrc_nfns,
	        xrc_noverrides, nimps, entry_point);
	xrc_ctx_init(&c, XRC_STACK_LO + XRC_STACK_SIZE - 64);
	c.esp -= 4;
	M32(c.esp) = XRC_RET_MAGIC;
	{ void xrc_prof_start(XCtx *); xrc_prof_start(&c); } /* (XRC_PROF=1: xrc_prof.c) */
	xrc_dispatch(&c, entry_point);
	fprintf(stderr, "xrc: entry returned, eax %08x\n", c.eax);
	return (int)c.eax;
}

/* an invalid instruction reached (#UD). The guest's SEH would see STATUS_ILLEGAL_INSTRUCTION; not modelled yet. */
void xrc_ud(XCtx *c, u32 addr) {
	xrc_fatal(c, "invalid instruction (#UD) at %08x", addr);
}
