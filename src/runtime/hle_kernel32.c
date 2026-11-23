/* hle_kernel32.c - KERNEL32 for the portable runtime: what the game's statically linked CRT (VC9) needs to start, plus
 * the simple process / memory / string / locale queries. Single guest thread for now: critical sections are no-ops,
 * Interlocked* are plain read-modify-writes. Every function pops its stdcall arguments (the count is the real API's).
 *
 * Kernel objects and files are in hle_kernel32_io.c. Not here yet (they stop the run by name): SEH
 * (RaiseException/RtlUnwind), DLL loading beyond module-handle lookups. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "xrc_rt.h"
#include "guest_types.h"
#include <pthread.h>
#ifdef _WIN32 /* (the monitor's few: not windows.h, whose names this file defines for the guest) */
typedef struct { unsigned long cb, faults; size_t peak_ws, ws, qpp, qp, qpnp, qnp, pagefile, peak_pagefile, priv; } XPmc;
__declspec(dllimport) int __stdcall K32GetProcessMemoryInfo(void *proc, XPmc *pmc, unsigned long cb);
__declspec(dllimport) void *__stdcall GetCurrentProcess(void);
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
#else
#include <unistd.h>
#endif

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define S(a) ((char *)(xrc_mem + (u32)(a)))       /* a guest char string */
#define W16(a) ((u16 *)(xrc_mem + (u32)(a)))      /* a guest UTF-16 string */

/* Win32 error codes the stubs set */
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_INVALID_PARAMETER 87

/* GetLastError lives in the TEB (LastErrorValue, fs:[0x34]), where code may also read it directly */
static void set_err(u32 e) { GF(Teb, xrc_teb, last_error) = e; }

static unsigned long long now_100ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	/* FILETIME: 100 ns units since 1601-01-01 */
	return (unsigned long long)ts.tv_sec * 10000000ull + (unsigned long long)ts.tv_nsec / 100ull + 116444736000000000ull;
}

/* ============================================================================================ guest heap
 * Size-class allocator in [XRC_HEAP_LO, XRC_HEAP_HI). Each block has a 16-byte header {size, class, magic, 0}
 * before the payload; class k holds payloads of up to 16 << k bytes; freed blocks go on their class's list (the
 * link in the header's last word: a freed payload keeps its contents, as Windows' heap mostly does -- the game has
 * races where a thread still calls through an object just deleted by another). Blocks of class >= BIG_CLASS are page-rounded and reused only by size class
 * too, which is enough for the CRT's small-block heap on top. All heap handles share it. */
#define HEAP_MAGIC 0x50414548u /* "HEAP" */
#define NCLASS 28
static u32 heap_next;
static u32 free_head[NCLASS];

static int size_class(u32 n) {
	int k = 0;
	while (k < NCLASS - 1 && (16u << k) < n) k++;
	return k;
}

/* big blocks (>= 1 MB) come from the VirtualAlloc region, page-rounded to 64 KB instead of the next power of two (as
 * Windows serves large heap blocks with VirtualAlloc); a freed one is reused for a request it fits without wasting
 * more than a quarter. The header is the same, class BIG_MARK, and the block's index in the table below. */
#define BIG_MIN 0x100000u
#define BIG_MARK 0xffu
#define NBIG 8192
static u32 va_next; /* (the next address; xrc_va_down: the lowest one used) */
static void mem_report(void);
static void mem_monitor_start(void);
void xrc_heap_init(void) { heap_next = XRC_HEAP_LO; va_next = xrc_va_down ? XRC_VA_HI : XRC_VA_LO; mem_monitor_start(); }
/* a VirtualAlloc block of cap bytes (64 KB multiple): its address, or 0 when the region has no room */
static u32 va_take(u32 cap) {
	u32 p;
	if (xrc_va_down) {
		if (va_next - XRC_VA_LO < cap || va_next - cap < heap_next) return 0; /* (the heap below it) */
		va_next -= cap;
		p = va_next;
	} else {
		if (va_next + cap < va_next || va_next + cap > XRC_VA_HI) return 0;
		p = va_next;
		va_next += cap;
	}
	mem_report();
	return p;
}
static u32 va_room(void) { return xrc_va_down ? va_next - heap_next : XRC_VA_HI - va_next; }
static u32 heap_limit(void) { return xrc_va_down ? va_next : XRC_HEAP_HI; }
static u32 big_base[NBIG], big_cap[NBIG];
static u8 big_free[NBIG];
static unsigned nbig;
/* (longrun) A free table slot for a block: a dead one (left by a merge), else the next unused; NBIG if the table is full. */
static unsigned big_slot(void) {
	unsigned k;
	for (k = 0; k < nbig; k++) if (!big_cap[k] && !big_free[k]) return k;
	return nbig < NBIG ? nbig++ : NBIG;
}
static int big_cmp(const void *a, const void *b) {
	u32 x = big_base[*(const unsigned *)a], y = big_base[*(const unsigned *)b];
	return x < y ? -1 : x > y;
}
/* (longrun) Free blocks that touch (the region is carved in address order, so a freed run of them is one hole) are
 * merged into one, the later table entries left dead (cap 0, not free: reused by big_slot): without it a long session's
 * mix of sizes leaves many free fragments none of which fits the next request, and the region (or the 8192 table slots)
 * runs out. Run only when a request would otherwise fail. Returns whether anything merged. */
static int big_coalesce(void) {
	static unsigned idx[NBIG];
	unsigned k, n = 0, i, last = 0;
	int merged = 0;
	for (k = 0; k < nbig; k++) if (big_free[k] && big_cap[k]) idx[n++] = k;
	if (n < 2) return 0;
	qsort(idx, n, sizeof idx[0], big_cmp);
	last = idx[0];
	for (i = 1; i < n; i++) {
		k = idx[i];
		if (big_base[last] + big_cap[last] == big_base[k]) {
			big_cap[last] += big_cap[k];
			big_free[k] = 0; big_cap[k] = 0; big_base[k] = 0;
			merged = 1;
		} else last = k;
	}
	return merged;
}
/* (longrun) free block b is to serve a request of cap bytes: keep cap, the rest (>= 64 KB) goes back as a free block */
static void big_split(unsigned b, u32 cap) {
	u32 rest = big_cap[b] - cap;
	unsigned s;
	if (rest < 0x10000u || (s = big_slot()) == NBIG) return;
	big_base[s] = big_base[b] + cap; big_cap[s] = rest; big_free[s] = 1;
	big_cap[b] = cap;
}
/* (longrun) a free block of at least cap bytes taken for good (VirtualAlloc, when the region itself is full): its address, or 0 */
static u32 big_take_free(u32 cap) {
	unsigned k, best = NBIG;
	if (big_coalesce()) {}
	for (k = 0; k < nbig; k++)
		if (big_free[k] && big_cap[k] >= cap && (best == NBIG || big_cap[k] < big_cap[best])) best = k;
	if (best == NBIG) return 0;
	big_split(best, cap);
	big_free[best] = 0;
	return big_base[best];
}
static u32 big_alloc(u32 n) {
	u32 cap = (n + 16u + 0xffffu) & ~0xffffu, best, k, p;
	int retried = 0;
	if (cap < n) return 0;
retry:
	best = NBIG;
	for (k = 0; k < nbig; k++)
		if (big_free[k] && big_cap[k] >= cap && (best == NBIG || big_cap[k] < big_cap[best])) best = k;
	if (best != NBIG && big_cap[best] - cap > cap / 4) { /* (a free block that wastes over a quarter: a fresh one if the region has room, else (longrun) split it) */
		if (va_room() >= cap) best = NBIG; else big_split(best, cap);
	}
	if (best == NBIG) {
		u32 a;
		unsigned s = big_slot();
		if (s == NBIG || !(a = va_take(cap))) { if (!retried++ && big_coalesce()) goto retry; return 0; }
		best = s;
		big_base[best] = a;
		big_cap[best] = cap;
	}
	big_free[best] = 0;
	p = big_base[best] + 16u;
	M32(p - 16) = n; M32(p - 12) = BIG_MARK; M32(p - 8) = HEAP_MAGIC; M32(p - 4) = best;
	return p;
}

u32 xrc_heap_alloc(u32 n, int zero) {
	int k;
	u32 cap, b, p;
	if (n > 0x7fffffffu) return 0;
	if (n >= BIG_MIN) {
		p = big_alloc(n);
		if (p && zero) memset(xrc_mem + p, 0, n);
		return p;
	}
	k = size_class(n ? n : 1);
	cap = 16u << k;
	if (free_head[k]) {
		p = free_head[k];
		free_head[k] = M32(p - 4);
	} else {
		b = (heap_next + 15u) & ~15u;
		if (b + 16u + cap < b || b + 16u + cap > heap_limit()) return 0;
		heap_next = b + 16u + cap;
		p = b + 16u;
		mem_report();
	}
	M32(p - 16) = n; M32(p - 12) = (u32)k; M32(p - 8) = HEAP_MAGIC; M32(p - 4) = 0;
	if (zero) memset(xrc_mem + p, 0, n);
	return p;
}

static int heap_block(u32 p) {
	if (p >= XRC_HEAP_LO + 16u && p < heap_next) return M32(p - 8) == HEAP_MAGIC;
	if (xrc_va_down ? p >= va_next + 16u && p < XRC_VA_HI : p >= XRC_VA_LO + 16u && p < va_next) return M32(p - 8) == HEAP_MAGIC && M32(p - 12) == BIG_MARK && M32(p - 4) < nbig;
	return 0;
}
static u32 block_cap(u32 p) { return M32(p - 12) == BIG_MARK ? big_cap[M32(p - 4)] - 16u : 16u << M32(p - 12); }

int xrc_heap_free(u32 p) {
	u32 k;
	if (!p) return 1;
	if (!heap_block(p)) return 0;
	k = M32(p - 12);
	M32(p - 8) = 0; /* (a double free is then caught) */
	if (k == BIG_MARK) { big_free[M32(p - 4)] = 1; return 1; }
	M32(p - 4) = free_head[k];
	free_head[k] = p;
	return 1;
}

u32 xrc_heap_size(u32 p) { return heap_block(p) ? M32(p - 16) : 0xffffffffu; }

u32 xrc_heap_realloc(u32 p, u32 n, int zero) {
	u32 old, q;
	if (!p) return xrc_heap_alloc(n, zero);
	if (!heap_block(p)) return 0;
	old = M32(p - 16);
	if (n <= block_cap(p) && (n >= BIG_MIN) == (M32(p - 12) == BIG_MARK)) {
		if (zero && n > old) memset(xrc_mem + p + old, 0, n - old);
		M32(p - 16) = n;
		return p;
	}
	q = xrc_heap_alloc(n, 0);
	if (!q) return 0;
	memcpy(xrc_mem + q, xrc_mem + p, old < n ? old : n);
	if (zero && n > old) memset(xrc_mem + q + old, 0, n - old);
	xrc_heap_free(p);
	return q;
}

static void hle_GetProcessHeap(XCtx *c) { RET(XRC_PROCESS_HEAP, 0); }
static void hle_HeapCreate(XCtx *c) { static u32 n; RET(XRC_PROCESS_HEAP + 0x10000u * ++n, 3); }
static void hle_HeapAlloc(XCtx *c) {
	u32 p = xrc_heap_alloc(A(2), (A(1) & 8) != 0); /* HEAP_ZERO_MEMORY */
	if (!p) {
		fprintf(stderr, "xrc: HeapAlloc(%u bytes) failed: heap at %08x of %08x, VirtualAlloc / big blocks at %08x\n", A(2), heap_next, heap_limit(), va_next);
		set_err(ERROR_NOT_ENOUGH_MEMORY);
	}
	RET(p, 3);
}
static void hle_HeapFree(XCtx *c) { RET(xrc_heap_free(A(2)), 3); }
static void hle_HeapReAlloc(XCtx *c) {
	u32 p;
	if (A(1) & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */
		u32 q = A(2);
		p = heap_block(q) && A(3) <= (16u << M32(q - 12)) ? xrc_heap_realloc(q, A(3), (A(1) & 8) != 0) : 0;
	} else {
		p = xrc_heap_realloc(A(2), A(3), (A(1) & 8) != 0);
	}
	if (!p) set_err(ERROR_NOT_ENOUGH_MEMORY);
	RET(p, 4);
}
static void hle_HeapSize(XCtx *c) { RET(xrc_heap_size(A(2)), 3); }
static void hle_LocalAlloc(XCtx *c) { RET(xrc_heap_alloc(A(1), (A(0) & 0x40) != 0), 2); } /* LMEM_ZEROINIT */
static void hle_LocalFree(XCtx *c) { RET(xrc_heap_free(A(0)) ? 0 : A(0), 1); }
static void hle_GlobalLock(XCtx *c) { RET(A(0), 1); }
static void hle_GlobalUnlock(XCtx *c) { RET(1, 1); }

/* the guest memory in use (heap; VirtualAlloc with the heap's big blocks): printed each time it grows by another 64 MB
 * (what the game needs: the launcher's Game Memory: 2 GB leaves ~1 GB of heap and ~720 MB for the rest) */
static void mem_report(void) {
	static u32 last;
	u32 h = (heap_next - XRC_HEAP_LO) >> 20, v = (xrc_va_down ? XRC_VA_HI - va_next : va_next - XRC_VA_LO) >> 20;
	if ((h + v) / 64 > last) {
		last = (h + v) / 64;
		if (xrc_va_down)
			fprintf(stderr, "xrc: guest memory in use: heap %u MB, VirtualAlloc / big blocks %u MB, of %u MB\n", h, v, (XRC_VA_HI - XRC_VA_LO) >> 20);
		else
			fprintf(stderr, "xrc: guest memory in use: heap %u of %u MB, VirtualAlloc / big blocks %u of %u MB\n", h, (XRC_HEAP_HI - XRC_HEAP_LO) >> 20, v,
			        (XRC_VA_HI - XRC_VA_LO) >> 20);
	}
}
/* XRC_MEM_EVERY_MS=N: every N ms, a line of the memory in use (read racily: a monitor) -- the guest's: the heap's small
 * blocks (as far as they reached: freed ones are reused, not returned), its big blocks (all made / still in use) and
 * VirtualAlloc's own; the process's: its working set (the RAM it has) and its commit (on Windows: with the guest's whole
 * reservation, committed at the start) */
static void *mem_monitor(void *arg) {
	u32 ms = (u32)(uintptr_t)arg;
	for (;;) {
		unsigned long long ws = 0, priv = 0;
		u32 k, n = nbig, heap, va, big = 0, live = 0;
#ifdef _WIN32
		XPmc pm;
		Sleep(ms);
		memset(&pm, 0, sizeof pm);
		pm.cb = sizeof pm;
		if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm)) { ws = pm.ws; priv = pm.priv; }
#else
		struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
		FILE *f;
		unsigned long sz = 0, res = 0;
		nanosleep(&ts, 0);
		if ((f = fopen("/proc/self/statm", "r")) != 0) {
			if (fscanf(f, "%lu %lu", &sz, &res) == 2) { ws = (unsigned long long)res * (unsigned long long)sysconf(_SC_PAGESIZE); priv = (unsigned long long)sz * (unsigned long long)sysconf(_SC_PAGESIZE); }
			fclose(f);
		}
#endif
		for (k = 0; k < n && k < NBIG; k++) { big += big_cap[k] >> 10; if (!big_free[k]) live += big_cap[k] >> 10; }
		heap = (heap_next - XRC_HEAP_LO) >> 20;
		va = (xrc_va_down ? XRC_VA_HI - va_next : va_next - XRC_VA_LO) >> 20;
		fprintf(stderr, "xrc: mem: guest %u MB of %u (heap %u MB, big blocks %u MB made / %u MB in use, VirtualAlloc %u MB) | process: working set %llu MB, %s %llu MB\n",
		        heap + va, (u32)(XRC_GUEST_SIZE >> 20), heap, big >> 10, live >> 10, va - (big >> 10), ws >> 20,
#ifdef _WIN32
		        "commit",
#else
		        "virtual",
#endif
		        priv >> 20);
	}
	return 0;
}
static void mem_monitor_start(void) {
	const char *e = getenv("XRC_MEM_EVERY_MS");
	pthread_t th;
	u32 ms = e ? (u32)atoi(e) : 0;
	if (!ms) return;
	if (ms < 100) ms = 100;
	if (pthread_create(&th, 0, mem_monitor, (void *)(uintptr_t)ms) == 0) pthread_detach(th);
}

/* VirtualAlloc: page-granular bump allocation in [XRC_VA_LO, XRC_VA_HI) (va_take); committing inside an earlier
 * reservation returns that address (the memory is already there). VirtualFree releases nothing yet. */
static void hle_VirtualAlloc(XCtx *c) {
	u32 addr = A(0), size = A(1), p;
	if (addr) RET(addr & ~0xfffu, 4);
	size = (size + 0xffffu) & ~0xffffu; /* allocation granularity 64 KB */
	if (!size || !(!(p = va_take(size)) ? (p = big_take_free(size)) : p)) { /* (longrun: a free big block when the region is full) */
		fprintf(stderr, "xrc: VirtualAlloc(%u bytes) failed: %u MB free (VirtualFree releases nothing yet)\n", A(1), va_room() >> 20);
		set_err(ERROR_NOT_ENOUGH_MEMORY);
		RET(0, 4);
	}
	memset(xrc_mem + p, 0, size);
	RET(p, 4);
}
static void hle_VirtualFree(XCtx *c) { RET(1, 3); }

static void hle_GlobalMemoryStatus(XCtx *c) {
	u32 p = A(0);
	memset(xrc_mem + p, 0, 32);
	GF(MemoryStatus, p, length) = 32; GF(MemoryStatus, p, memory_load) = 25;           /* dwLength, dwMemoryLoad */
	GF(MemoryStatus, p, total_phys) = 0x7fff0000u; GF(MemoryStatus, p, avail_phys) = 0x60000000u; /* total / available physical */
	GF(MemoryStatus, p, total_page_file) = 0xffff0000u; GF(MemoryStatus, p, avail_page_file) = 0xc0000000u; /* page file */
	if (xrc_guest_2gb) { GF(MemoryStatus, p, total_virtual) = 0x7ffe0000u; GF(MemoryStatus, p, avail_virtual) = 0x60000000u; } /* virtual: 2 GB (as the original gets) */
	else { GF(MemoryStatus, p, total_virtual) = 0xfffe0000u; GF(MemoryStatus, p, avail_virtual) = 0xc0000000u; } /* virtual (a large-address-aware 32-bit process) */
	RET(0, 1);
}
static void hle_GlobalMemoryStatusEx(XCtx *c) {
	u32 p = A(0);
	memset(xrc_mem + p, 0, 64);
	GF(MemoryStatusEx, p, length) = 64; GF(MemoryStatusEx, p, memory_load) = 25;
	GF(MemoryStatusEx, p, total_phys.lo) = 0; GF(MemoryStatusEx, p, total_phys.hi) = 2;            /* ullTotalPhys 8 GB */
	GF(MemoryStatusEx, p, avail_phys.lo) = 0; GF(MemoryStatusEx, p, avail_phys.hi) = 1;           /* ullAvailPhys 4 GB */
	GF(MemoryStatusEx, p, total_page_file.lo) = 0; GF(MemoryStatusEx, p, total_page_file.hi) = 3;           /* ullTotalPageFile */
	GF(MemoryStatusEx, p, avail_page_file.lo) = 0; GF(MemoryStatusEx, p, avail_page_file.hi) = 2;
	GF(MemoryStatusEx, p, total_virtual.lo) = xrc_guest_2gb ? 0x7ffe0000u : 0xfffe0000u; GF(MemoryStatusEx, p, total_virtual.hi) = 0; /* ullTotalVirtual 2 / 4 GB */
	GF(MemoryStatusEx, p, avail_virtual.lo) = xrc_guest_2gb ? 0x60000000u : 0xc0000000u; GF(MemoryStatusEx, p, avail_virtual.hi) = 0;
	RET(1, 1);
}

/* ============================================================================================ TLS */
#define TLS_SLOTS 64
static u8 tls_used[TLS_SLOTS];
/* the slots are the TEB's TlsSlots (fs:[0xe10 + 4 * i]), where compiled code may read them directly */
static void hle_TlsAlloc(XCtx *c) {
	u32 i;
	for (i = 0; i < TLS_SLOTS; i++)
		if (!tls_used[i]) { tls_used[i] = 1; GFI(Teb, xrc_teb, tls_slots, i) = 0; RET(i, 0); }
	RET(0xffffffffu, 0); /* TLS_OUT_OF_INDEXES */
}
static void hle_TlsFree(XCtx *c) { if (A(0) < TLS_SLOTS) tls_used[A(0)] = 0; RET(1, 1); }
static void hle_TlsGetValue(XCtx *c) {
	if (A(0) >= TLS_SLOTS) { set_err(ERROR_INVALID_PARAMETER); RET(0, 1); }
	set_err(0);
	RET(GFI(Teb, xrc_teb, tls_slots, A(0)), 1);
}
static void hle_TlsSetValue(XCtx *c) {
	if (A(0) >= TLS_SLOTS) RET(0, 2);
	GFI(Teb, xrc_teb, tls_slots, A(0)) = A(1);
	RET(1, 2);
}

/* ============================================================================================ synchronization */
/* CRITICAL_SECTION: DebugInfo LockCount RecursionCount OwningThread LockSemaphore SpinCount; kept in the guest's
 * struct (RecursionCount, OwningThread = the thread id) */
static void cs_init(u32 cs, u32 spin) { memset(xrc_mem + cs, 0, 24); GF(CriticalSection, cs, lock_count) = 0xffffffffu; GF(CriticalSection, cs, spin_count) = spin; }
static void hle_InitializeCriticalSection(XCtx *c) { cs_init(A(0), 0); RET(0, 1); }
static void hle_InitializeCriticalSectionAndSpinCount(XCtx *c) { cs_init(A(0), A(1)); RET(1, 2); }
static int cs_try(u32 cs) {
	u32 me = xrc_thread_id();
	if (GF(CriticalSection, cs, owning_thread) && GF(CriticalSection, cs, owning_thread) != me) return 0;
	GF(CriticalSection, cs, owning_thread) = me;
	GF(CriticalSection, cs, recursion_count)++;
	return 1;
}
static int cs_ready(void *p) { return cs_try((u32)(uintptr_t)p); }
static void hle_EnterCriticalSection(XCtx *c) {
	if (!cs_try(A(0))) {
		uintptr_t key = A(0);
		XRC_WAIT_NOTE("EnterCriticalSection (owner thread in the handle)", M32(A(0) + 12));
		xrc_block_wait_keys(&key, 1, cs_ready, (void *)(uintptr_t)A(0), 0xffffffffu);
	}
	RET(0, 1);
}
static void hle_LeaveCriticalSection(XCtx *c) {
	u32 cs = A(0);
	if (GF(CriticalSection, cs, owning_thread) == xrc_thread_id() && GF(CriticalSection, cs, recursion_count) && --GF(CriticalSection, cs, recursion_count) == 0) {
		GF(CriticalSection, cs, owning_thread) = 0;
		xrc_wake(cs);
	}
	RET(0, 1);
}
static void hle_TryEnterCriticalSection(XCtx *c) { RET(cs_try(A(0)), 1); }
static void hle_DeleteCriticalSection(XCtx *c) { RET(0, 1); }
static void hle_InterlockedIncrement(XCtx *c) { RET(++M32(A(0)), 1); }
static void hle_InterlockedDecrement(XCtx *c) { RET(--M32(A(0)), 1); }
static void hle_InterlockedExchange(XCtx *c) { u32 o = M32(A(0)); M32(A(0)) = A(1); RET(o, 2); }
static void hle_InterlockedExchangeAdd(XCtx *c) { u32 o = M32(A(0)); M32(A(0)) = o + A(1); RET(o, 2); }
static void hle_InterlockedCompareExchange(XCtx *c) {
	u32 o = M32(A(0));
	if (o == A(2)) M32(A(0)) = A(1);
	RET(o, 3);
}

/* ============================================================================================ process / thread */
static void hle_GetCurrentProcessId(XCtx *c) { RET(0x1000, 0); }
static void hle_GetCurrentThreadId(XCtx *c) { RET(xrc_thread_id(), 0); }
static void hle_GetCurrentProcess(XCtx *c) { RET(0xffffffffu, 0); }
static void hle_GetCurrentThread(XCtx *c) { RET(0xfffffffeu, 0); }
static void hle_GetLastError(XCtx *c) { RET(GF(Teb, xrc_teb, last_error), 0); }
static void hle_SetLastError(XCtx *c) { set_err(A(0)); RET(0, 1); }
static void hle_ExitProcess(XCtx *c) {
	fflush(stdout);
	fprintf(stderr, "xrc: ExitProcess(%d)\n", (int)A(0));
	xrc_print_stack();
	xrc_exit_now((int)A(0));
}
static void hle_TerminateProcess(XCtx *c) {
	fflush(stdout);
	fprintf(stderr, "xrc: TerminateProcess(%08x, %d)\n", A(0), (int)A(1));
	xrc_exit_now((int)A(1));
}
static void hle_IsDebuggerPresent(XCtx *c) { RET(0, 0); }
/* RaiseException(code, flags, nargs, args). 0x406D1388 is the MSVC "set thread name" convention (args: THREADNAME_INFO
 * {0x1000, name, thread id, flags}), caught by the raiser's own __except when no debugger takes it: returning has the
 * same effect. Real exception dispatch (C++ throw 0xE06D7363 through the guest's SEH chain) is not modelled yet. */
static void hle_RaiseException(XCtx *c) {
	u32 code = A(0), a = A(3);
	if (code == 0x406d1388u && A(2) >= 3 && a && GF(ThreadNameInfo, a, type) == 0x1000) {
		if (xrc_tool_env("XRC_TRACE_THREADS"))
			fprintf(stderr, "xrc: thread %x is named \"%s\"\n", GF(ThreadNameInfo, a, thread_id) == 0xffffffffu ? 0 : GF(ThreadNameInfo, a, thread_id), GF(ThreadNameInfo, a, name) ? S(GF(ThreadNameInfo, a, name)) : "");
		RET(0, 4);
	}
	if (code == 0xe06d7363u && A(2) >= 3)
		xrc_fatal(c, "C++ exception thrown (object %08x, throw info %08x): guest exception dispatch is not implemented yet", GF(ThreadNameInfo, a, name), GF(ThreadNameInfo, a, thread_id));
	xrc_fatal(c, "RaiseException(%08x, flags %x, %u args): guest exception dispatch is not implemented yet", code, A(1), A(2));
}
static u32 unhandled_filter;
static void hle_SetUnhandledExceptionFilter(XCtx *c) { u32 o = unhandled_filter; unhandled_filter = A(0); RET(o, 1); }
static void hle_UnhandledExceptionFilter(XCtx *c) { RET(0, 1); } /* EXCEPTION_CONTINUE_SEARCH */
static void hle_GetProcessAffinityMask(XCtx *c) {
	if (A(1)) M32(A(1)) = 1;
	if (A(2)) M32(A(2)) = 1;
	RET(1, 3);
}
static void hle_SetThreadPriority(XCtx *c) { RET(1, 2); }
static void hle_GetThreadPriority(XCtx *c) { RET(0, 1); }
static void hle_SetThreadIdealProcessor(XCtx *c) { RET(0, 2); }
static void hle_IsBadReadPtr(XCtx *c) { RET(A(0) == 0 && A(1) != 0, 2); }
static void hle_OutputDebugStringA(XCtx *c) { fprintf(stderr, "[debug] %s", S(A(0))); RET(0, 1); }
/* The image loader's decode thread (white::io::FileControllerMethodImage::DecodeThread 0074ac50) polls its queue with Sleep(33)
 * when it is empty and moves one chunk per sleep: a load of ~165 MB took 38 s with the CPU idle. Its idle Sleep(33) is
 * shortened to XRC_DECODE_POLL_MS (default 2; 0 = leave it). No other thread or Sleep is touched. */
static void sleep_poll(XCtx *c, u32 ms) {
	if (ms == 33) {
		static int poll = -1;
		if (poll < 0) { const char *e = getenv("XRC_DECODE_POLL_MS"); poll = e ? atoi(e) : 2; }
		if (poll > 0 && (u32)poll < ms) {
			int k;
			for (k = 0; k < 3; k++) if (xrc_stack_at(c, k) == 0x74ac50u) { ms = (u32)poll; break; }
		}
	}
#ifndef XRC_NO_TOOLS
	{ /* XRC_SLEEP_STATS=1: count and requested ms of guest Sleeps per (ms, innermost guest function, its caller), dumped every 5 s */
		static int on = -1, n; static unsigned long long last;
		static struct { u32 ms, f0, f1; unsigned long long cnt; } t[64];
		if (on < 0) on = getenv("XRC_SLEEP_STATS") != 0;
		if (on) {
			int i; unsigned long long now = xrc_now_us();
			u32 f0 = xrc_stack_at(c, 0), f1 = xrc_stack_at(c, 1);
			for (i = 0; i < n && !(t[i].ms == ms && t[i].f0 == f0 && t[i].f1 == f1); i++) {}
			if (i == n && n < 64) { t[n].ms = ms; t[n].f0 = f0; t[n].f1 = f1; t[n].cnt = 0; n++; }
			if (i < 64) t[i].cnt++;
			if (now - last > 5000000ull) {
				last = now;
				for (i = 0; i < n; i++) if (t[i].cnt > 20) fprintf(stderr, "xrc sleep-stats: Sleep(%u) x%llu from %08x < %08x\n", t[i].ms, t[i].cnt, t[i].f0, t[i].f1);
				fprintf(stderr, "xrc sleep-stats: ----\n");
			}
		}
	}
#endif
	xrc_sleep(ms);
}
static void hle_Sleep(XCtx *c) { sleep_poll(c, A(0)); RET(0, 1); }
static void hle_SleepEx(XCtx *c) { sleep_poll(c, A(0)); RET(0, 2); }

/* GetStartupInfoA: an STARTUPINFOA of a console-less GUI start (no std handle inheritance) */
static void hle_GetStartupInfoA(XCtx *c) {
	u32 p = A(0);
	memset(xrc_mem + p, 0, 68);
	M32(p) = 68;
	RET(0, 1);
}

/* ============================================================================================ time */
static void hle_GetSystemTimeAsFileTime(XCtx *c) {
	unsigned long long t = now_100ns();
	GF(FileTime, A(0), lo) = (u32)t; GF(FileTime, A(0), hi) = (u32)(t >> 32);
	RET(0, 1);
}
static void hle_GetTickCount(XCtx *c) { RET(now_100ns() / 10000ull, 0); }
/* the performance counter: 100 ns ticks of a monotonic clock since "boot" (the runtime's start, minus a minute as
 * timeGetTime has it) -- code that has not seen the counter yet takes its first reading as the time since 0 */
static unsigned long long qpc_now(void) {
	static unsigned long long t0;
	struct timespec ts;
	unsigned long long t;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t = (unsigned long long)ts.tv_sec * 10000000ull + (unsigned long long)ts.tv_nsec / 100ull;
	static double scale = -1;
	if (scale < 0) scale = xrc_tool_env("XRC_TIME_SCALE") ? atof(xrc_tool_env("XRC_TIME_SCALE")) : 1.0;
	if (!t0) t0 = t - 600000000ull;
	/* XRC_TIME_SCALE (benchmarks): the counter runs that many times faster, so the game's frame limiter waits less */
	{
		/* XRC_TIME_SCALE_AT=secs: real time until then, scaled after (menus and XRC_INPUT taps at real speed first) */
		static double at = -1;
		unsigned long long r = t - t0, from;
		if (at < 0) at = xrc_tool_env("XRC_TIME_SCALE_AT") ? atof(xrc_tool_env("XRC_TIME_SCALE_AT")) : 0.0;
		from = 600000000ull + (unsigned long long)(at * 1e7);
		if (scale != 1.0 && scale > 0 && r > from) return from + (unsigned long long)((double)(r - from) * scale);
		return r;
	}
}
unsigned long long xrc_qpc(void) { return qpc_now(); }
static void hle_QueryPerformanceCounter(XCtx *c) {
	unsigned long long t = qpc_now();
	GF(FileTime, A(0), lo) = (u32)t; GF(FileTime, A(0), hi) = (u32)(t >> 32);
	RET(1, 1);
}
static void hle_QueryPerformanceFrequency(XCtx *c) { GF(LargeInteger, A(0), lo) = 10000000u; GF(LargeInteger, A(0), hi) = 0; RET(1, 1); }
/* TIME_ZONE_INFORMATION: UTC, no daylight saving */
static void hle_GetTimeZoneInformation(XCtx *c) { memset(xrc_mem + A(0), 0, 172); RET(0, 1); } /* TIME_ZONE_ID_UNKNOWN */

/* ============================================================================================ command line / env */
/* the exe's Windows path as the retail install has it; GetModuleFileName and argv[0] */
static const char exe_path[] = XRC_GUEST_EXE_DIR "\\" XRC_GUEST_EXE_NAME;

static u32 guest_strdup(const char *s) {
	u32 n = (u32)strlen(s) + 1, p = xrc_heap_alloc(n, 0);
	memcpy(xrc_mem + p, s, n);
	return p;
}
static u32 cmdline_a, env_a, env_w;
static void build_cmdline(void) {
	char buf[4096];
	int k;
	size_t n;
	if (cmdline_a) return;
	n = (size_t)snprintf(buf, sizeof buf, "\"%s\"", exe_path);
	for (k = 0; k < xrc_argc && n < sizeof buf - 2; k++) n += (size_t)snprintf(buf + n, sizeof buf - n, " %s", xrc_argv[k]);
	/* XRC_GAME_ARGS (the launcher's .cfg: e.g. "-Shadow=1024 -MSAA=4", as the retail launcher passes them): more of them */
	if (getenv("XRC_GAME_ARGS") && *getenv("XRC_GAME_ARGS") && n < sizeof buf - 2) snprintf(buf + n, sizeof buf - n, " %s", getenv("XRC_GAME_ARGS"));
	/* XRC_SHADOW_RES=<size> (the options menu's Shadow resolution): -Shadow=<size>, as the retail launcher passes it */
	if (getenv("XRC_SHADOW_RES") && atoi(getenv("XRC_SHADOW_RES")) > 0 && !(getenv("XRC_GAME_ARGS") && strstr(getenv("XRC_GAME_ARGS"), "-Shadow"))) {
		n = strlen(buf);
		if (n < sizeof buf - 24) snprintf(buf + n, sizeof buf - n, " -Shadow=%d", atoi(getenv("XRC_SHADOW_RES")));
	}
	cmdline_a = guest_strdup(buf);
}
static void hle_GetCommandLineA(XCtx *c) { build_cmdline(); RET(cmdline_a, 0); }

/* the environment block: NAME=value\0 ... \0 (a small fixed one: the game has no use for the host's) */
static const char *const env_vars[] = {"SystemRoot=C:\\Windows", "windir=C:\\Windows", "TEMP=C:\\Temp", "TMP=C:\\Temp", 0};
static void build_env(void) {
	u32 n = 1, k, p, q;
	if (env_a) return;
	for (k = 0; env_vars[k]; k++) n += (u32)strlen(env_vars[k]) + 1;
	env_a = xrc_heap_alloc(n, 1);
	env_w = xrc_heap_alloc(2 * n, 1);
	for (k = 0, p = 0; env_vars[k]; k++) {
		for (q = 0; env_vars[k][q]; q++, p++) {
			M8(env_a + p) = (u8)env_vars[k][q];
			W16(env_w)[p] = (u8)env_vars[k][q];
		}
		p++; /* the NULs are already there */
	}
}
static void hle_GetEnvironmentStrings(XCtx *c) { build_env(); RET(env_a, 0); }
static void hle_GetEnvironmentStringsW(XCtx *c) { build_env(); RET(env_w, 0); }
static void hle_FreeEnvironmentStringsA(XCtx *c) { RET(1, 1); }
static void hle_FreeEnvironmentStringsW(XCtx *c) { RET(1, 1); }
static void hle_SetEnvironmentVariableA(XCtx *c) { RET(1, 2); }

/* ============================================================================================ modules */
/* module handles: the exe's is its base; a known system DLL gets a fake handle (only GetProcAddress uses it) */
static const char *const known_dlls[] = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ole32.dll",
                                         "oleaut32.dll", "shell32.dll", "winmm.dll", "ws2_32.dll", "version.dll",
                                         "imm32.dll", "d3d9.dll", "d3dx9_43.dll", "dinput8.dll", "bink2w32.dll",
                                         "ntdll.dll", "mscoree.dll",
                                         "sgw.dll", /* the Steam wrapper the game insists on (hle_system.c) */
                                         0};
#define DLL_HANDLE(i) (0x70000000u + 0x00100000u * (u32)(i))

static int dll_index(const char *name) {
	char b[64];
	size_t n;
	int k;
	const char *s = strrchr(name, '\\');
	s = s ? s + 1 : name;
	snprintf(b, sizeof b, "%s", s);
	n = strlen(b);
	if (n < 4 || !xrc_ieq(b + n - 4, ".dll")) snprintf(b + n, sizeof b - n, ".dll");
	for (k = 0; known_dlls[k]; k++)
		if (xrc_ieq(b, known_dlls[k])) return k;
	return -1;
}
static void wide_to_ascii(u32 w, char *b, size_t cap) {
	size_t k;
	for (k = 0; k + 1 < cap && W16(w)[k]; k++) b[k] = W16(w)[k] < 0x80 ? (char)W16(w)[k] : '?';
	b[k] = 0;
}
static void hle_GetModuleHandleA(XCtx *c) {
	int k;
	if (!A(0)) RET(XRC_IMAGE_BASE, 1);
	k = dll_index(S(A(0)));
	if (k < 0) { set_err(ERROR_MOD_NOT_FOUND); RET(0, 1); }
	RET(DLL_HANDLE(k), 1);
}
static void hle_GetModuleHandleW(XCtx *c) {
	char b[260];
	int k;
	if (!A(0)) RET(XRC_IMAGE_BASE, 1);
	wide_to_ascii(A(0), b, sizeof b);
	k = dll_index(b);
	if (k < 0) { set_err(ERROR_MOD_NOT_FOUND); RET(0, 1); }
	RET(DLL_HANDLE(k), 1);
}
static void hle_LoadLibraryA(XCtx *c) {
	int k = dll_index(S(A(0)));
	if (k < 0) { fprintf(stderr, "xrc: LoadLibraryA(\"%s\") -> not found\n", S(A(0))); set_err(ERROR_MOD_NOT_FOUND); RET(0, 1); }
	RET(DLL_HANDLE(k), 1);
}
static void hle_LoadLibraryW(XCtx *c) {
	char b[260];
	int k;
	wide_to_ascii(A(0), b, sizeof b);
	k = dll_index(b);
	if (k < 0) { fprintf(stderr, "xrc: LoadLibraryW(\"%s\") -> not found\n", b); set_err(ERROR_MOD_NOT_FOUND); RET(0, 1); }
	RET(DLL_HANDLE(k), 1);
}
static void hle_FreeLibrary(XCtx *c) { RET(1, 1); }
static void hle_GetProcAddress(XCtx *c) {
	u32 h = A(0), n = A(1), a;
	const char *dll = 0;
	if (n < 0x10000u) { set_err(ERROR_PROC_NOT_FOUND); RET(0, 2); } /* by ordinal: none implemented */
	if (h >= DLL_HANDLE(0) && (h - DLL_HANDLE(0)) % 0x00100000u == 0) dll = known_dlls[(h - DLL_HANDLE(0)) / 0x00100000u];
	a = xrc_import_addr(dll, S(n));
	if (xrc_tool_env("XRC_TRACE_IMPORTS")) fprintf(stderr, "xrc: GetProcAddress(%s, %s) -> %08x\n", dll ? dll : "?", S(n), a);
	if (!a) set_err(ERROR_PROC_NOT_FOUND);
	RET(a, 2);
}
/* the path of a module: the exe's, a DLL that ships with the game next to it (its file is read as data -- the
 * Steam wrapper check hashes sgw.dll), or a system DLL in system32 */
static int module_path(u32 h, char *out, size_t n) {
	const char *dll;
	if (!h || h == XRC_IMAGE_BASE) { snprintf(out, n, "%s", exe_path); return 1; }
	if (h < DLL_HANDLE(0) || (h - DLL_HANDLE(0)) % 0x00100000u != 0) return 0;
	dll = known_dlls[(h - DLL_HANDLE(0)) / 0x00100000u];
	if (xrc_ieq(dll, "sgw.dll") || xrc_ieq(dll, "bink2w32.dll"))
		snprintf(out, n, XRC_GUEST_EXE_DIR "\\%s", dll);
	else
		snprintf(out, n, "C:\\WINDOWS\\system32\\%s", dll);
	return 1;
}
static void hle_GetModuleFileNameA(XCtx *c) {
	char p[300];
	u32 buf = A(1), cap = A(2), n;
	if (!module_path(A(0), p, sizeof p)) { set_err(ERROR_MOD_NOT_FOUND); RET(0, 3); }
	n = (u32)strlen(p);
	if (!cap) RET(0, 3);
	if (n >= cap) { memcpy(S(buf), p, cap - 1); M8(buf + cap - 1) = 0; set_err(ERROR_INSUFFICIENT_BUFFER); RET(cap, 3); }
	memcpy(S(buf), p, n + 1);
	RET(n, 3);
}
static void hle_GetModuleFileNameW(XCtx *c) {
	u32 buf = A(1), cap = A(2), n = (u32)strlen(exe_path), k;
	if (A(0) && A(0) != XRC_IMAGE_BASE) RET(0, 3);
	if (!cap) RET(0, 3);
	for (k = 0; k < n && k + 1 < cap; k++) W16(buf)[k] = (u8)exe_path[k];
	W16(buf)[k] = 0;
	if (n >= cap) { set_err(ERROR_INSUFFICIENT_BUFFER); RET(cap, 3); }
	RET(n, 3);
}

/* ============================================================================================ std handles */
#define H_STDIN 0x10u
#define H_STDOUT 0x14u
#define H_STDERR 0x18u
static void hle_GetStdHandle(XCtx *c) {
	switch (A(0)) {
	case 0xfffffff6u: RET(H_STDIN, 1);
	case 0xfffffff5u: RET(H_STDOUT, 1);
	case 0xfffffff4u: RET(H_STDERR, 1);
	}
	set_err(ERROR_INVALID_HANDLE);
	RET(0xffffffffu, 1);
}
static void hle_SetStdHandle(XCtx *c) { RET(1, 2); }
static void hle_SetHandleCount(XCtx *c) { RET(A(0), 1); }
static void hle_GetConsoleMode(XCtx *c) { RET(0, 2); } /* no console */
static void hle_GetConsoleCP(XCtx *c) { RET(437, 0); }
static void hle_GetConsoleOutputCP(XCtx *c) { RET(437, 0); }
static FILE *std_file(u32 h) { return h == H_STDOUT ? stdout : h == H_STDERR ? stderr : 0; }
static void hle_WriteConsoleA(XCtx *c) {
	FILE *f = std_file(A(0));
	if (f) fwrite(xrc_mem + A(1), 1, A(2), f);
	if (A(3)) M32(A(3)) = A(2);
	RET(f != 0, 5);
}
static void hle_WriteConsoleW(XCtx *c) {
	FILE *f = std_file(A(0));
	u32 k;
	if (f) for (k = 0; k < A(2); k++) fputc(W16(A(1))[k] < 0x80 ? W16(A(1))[k] : '?', f);
	if (A(3)) M32(A(3)) = A(2);
	RET(f != 0, 5);
}

/* ============================================================================================ code pages / strings
 * Code page 1252 (the game's ACP) is treated as Latin-1 and 65001 as UTF-8; the CRT only converts its own
 * environment, command line and locale names at startup. */
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001
static u32 real_cp(u32 cp) { return cp == CP_ACP ? 1252 : cp == CP_OEMCP ? 437 : cp; }
static void hle_GetACP(XCtx *c) { RET(1252, 0); }
static void hle_GetOEMCP(XCtx *c) { RET(437, 0); }
static void hle_IsValidCodePage(XCtx *c) { u32 cp = A(0); RET(cp == 1252 || cp == 437 || cp == 65001 || cp == 20127, 1); }
static void hle_GetCPInfo(XCtx *c) {
	u32 cp = real_cp(A(0)), p = A(1);
	memset(xrc_mem + p, 0, 20);
	GF(CpInfo, p, max_char_size) = cp == CP_UTF8 ? 4 : 1; /* MaxCharSize */
	GFI(CpInfo, p, default_char, 0) = '?';                /* DefaultChar; LeadByte[] all zero: no DBCS */
	RET(1, 2);
}

/* one code point from a multibyte string; returns the bytes used */
static u32 mb_decode(u32 cp, const u8 *s, u32 n, u32 *out) {
	if (cp != CP_UTF8 || s[0] < 0x80) { *out = s[0]; return 1; }
	if ((s[0] & 0xe0) == 0xc0 && n >= 2) { *out = (u32)(s[0] & 0x1f) << 6 | (s[1] & 0x3f); return 2; }
	if ((s[0] & 0xf0) == 0xe0 && n >= 3) { *out = (u32)(s[0] & 0x0f) << 12 | (u32)(s[1] & 0x3f) << 6 | (s[2] & 0x3f); return 3; }
	if ((s[0] & 0xf8) == 0xf0 && n >= 4) {
		*out = (u32)(s[0] & 0x07) << 18 | (u32)(s[1] & 0x3f) << 12 | (u32)(s[2] & 0x3f) << 6 | (s[3] & 0x3f);
		return 4;
	}
	*out = 0xfffd;
	return 1;
}
/* MultiByteToWideChar(CodePage, dwFlags, lpMultiByteStr, cbMultiByte, lpWideCharStr, cchWideChar) */
static void hle_MultiByteToWideChar(XCtx *c) {
	u32 cp = real_cp(A(0)), src = A(2), n = A(3), dst = A(4), cap = A(5), i = 0, o = 0;
	if (n == 0xffffffffu) n = (u32)strlen(S(src)) + 1;
	while (i < n) {
		u32 ch, k = mb_decode(cp, xrc_mem + src + i, n - i, &ch);
		i += k;
		if (ch >= 0x10000) { /* surrogate pair */
			if (cap) {
				if (o + 2 > cap) { set_err(ERROR_INSUFFICIENT_BUFFER); RET(0, 6); }
				W16(dst)[o] = (u16)(0xd800 + ((ch - 0x10000) >> 10));
				W16(dst)[o + 1] = (u16)(0xdc00 + ((ch - 0x10000) & 0x3ff));
			}
			o += 2;
		} else {
			if (cap) {
				if (o + 1 > cap) { set_err(ERROR_INSUFFICIENT_BUFFER); RET(0, 6); }
				W16(dst)[o] = (u16)ch;
			}
			o++;
		}
	}
	RET(o, 6);
}
/* WideCharToMultiByte(CodePage, dwFlags, lpWideCharStr, cchWideChar, lpMultiByteStr, cbMultiByte, lpDefaultChar,
 *                     lpUsedDefaultChar) */
static void hle_WideCharToMultiByte(XCtx *c) {
	u32 cp = real_cp(A(0)), src = A(2), n = A(3), dst = A(4), cap = A(5), i, o = 0, used = 0;
	u8 def = A(6) ? M8(A(6)) : '?';
	if (n == 0xffffffffu) { for (n = 0; W16(src)[n]; n++) {} n++; }
	for (i = 0; i < n; i++) {
		u32 ch = W16(src)[i];
		u8 b[4];
		u32 k = 0, j;
		if (ch >= 0xd800 && ch < 0xdc00 && i + 1 < n) { ch = 0x10000 + ((ch - 0xd800) << 10) + (W16(src)[i + 1] - 0xdc00); i++; }
		if (cp == CP_UTF8) {
			if (ch < 0x80) b[k++] = (u8)ch;
			else if (ch < 0x800) { b[k++] = (u8)(0xc0 | ch >> 6); b[k++] = (u8)(0x80 | (ch & 0x3f)); }
			else if (ch < 0x10000) { b[k++] = (u8)(0xe0 | ch >> 12); b[k++] = (u8)(0x80 | (ch >> 6 & 0x3f)); b[k++] = (u8)(0x80 | (ch & 0x3f)); }
			else { b[k++] = (u8)(0xf0 | ch >> 18); b[k++] = (u8)(0x80 | (ch >> 12 & 0x3f)); b[k++] = (u8)(0x80 | (ch >> 6 & 0x3f)); b[k++] = (u8)(0x80 | (ch & 0x3f)); }
		} else {
			b[k++] = ch < 0x100 ? (u8)ch : (used = 1, def);
		}
		if (cap) {
			if (o + k > cap) { set_err(ERROR_INSUFFICIENT_BUFFER); RET(0, 8); }
			for (j = 0; j < k; j++) M8(dst + o + j) = b[j];
		}
		o += k;
	}
	if (A(7)) M32(A(7)) = used;
	RET(o, 8);
}

/* C1 character type flags (CT_CTYPE1) of a code unit, ASCII / Latin-1 */
static u16 ctype1(u32 ch) {
	u16 f = 0;
	if (ch >= 'A' && ch <= 'Z') f |= 0x001 | 0x100;
	if (ch >= 'a' && ch <= 'z') f |= 0x002 | 0x100;
	if (ch >= 0xc0 && ch <= 0xde && ch != 0xd7) f |= 0x001 | 0x100;
	if (ch >= 0xdf && ch <= 0xff && ch != 0xf7) f |= 0x002 | 0x100;
	if (ch >= '0' && ch <= '9') f |= 0x004 | 0x080;
	if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) f |= 0x080;
	if (ch == ' ' || (ch >= 9 && ch <= 13)) f |= 0x008;
	if (ch == ' ' || ch == 9) f |= 0x040;
	if (ch < 0x20 || ch == 0x7f) f |= 0x020;
	if ((ch >= 0x21 && ch <= 0x2f) || (ch >= 0x3a && ch <= 0x40) || (ch >= 0x5b && ch <= 0x60) || (ch >= 0x7b && ch <= 0x7e) ||
	    (ch >= 0xa1 && ch <= 0xbf) || ch == 0xd7 || ch == 0xf7)
		f |= 0x010;
	if (ch == 0xa0) f |= 0x008 | 0x040;
	return f;
}
/* GetStringTypeW(dwInfoType, lpSrcStr, cchSrc, lpCharType) */
static void hle_GetStringTypeW(XCtx *c) {
	u32 src = A(1), n = A(2), out = A(3), k;
	if (n == 0xffffffffu) { for (n = 0; W16(src)[n]; n++) {} n++; }
	for (k = 0; k < n; k++) W16(out)[k] = A(0) == 1 ? ctype1(W16(src)[k]) : 0;
	RET(1, 4);
}
/* GetStringTypeA(Locale, dwInfoType, lpSrcStr, cchSrc, lpCharType) */
static void hle_GetStringTypeA(XCtx *c) {
	u32 src = A(2), n = A(3), out = A(4), k;
	if (n == 0xffffffffu) n = (u32)strlen(S(src)) + 1;
	for (k = 0; k < n; k++) W16(out)[k] = A(1) == 1 ? ctype1(M8(src + k)) : 0;
	RET(1, 5);
}
static u32 map_case(u32 ch, u32 flags) {
	if (flags & 0x100) { if ((ch >= 'A' && ch <= 'Z') || (ch >= 0xc0 && ch <= 0xde && ch != 0xd7)) ch += 32; } /* LCMAP_LOWERCASE */
	if (flags & 0x200) { if ((ch >= 'a' && ch <= 'z') || (ch >= 0xe0 && ch <= 0xfe && ch != 0xf7)) ch -= 32; } /* LCMAP_UPPERCASE */
	return ch;
}
/* LCMapStringW(Locale, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest): case mapping (sort keys unsupported) */
static void hle_LCMapStringW(XCtx *c) {
	u32 fl = A(1), src = A(2), n = A(3), dst = A(4), cap = A(5), k;
	if (fl & 0x400) xrc_fatal(c, "LCMapStringW: LCMAP_SORTKEY is not implemented");
	if (n == 0xffffffffu) { for (n = 0; W16(src)[n]; n++) {} n++; }
	if (!cap) RET(n, 6);
	if (n > cap) { set_err(ERROR_INSUFFICIENT_BUFFER); RET(0, 6); }
	for (k = 0; k < n; k++) W16(dst)[k] = (u16)map_case(W16(src)[k], fl);
	RET(n, 6);
}
static void hle_LCMapStringA(XCtx *c) {
	u32 fl = A(1), src = A(2), n = A(3), dst = A(4), cap = A(5), k;
	if (fl & 0x400) xrc_fatal(c, "LCMapStringA: LCMAP_SORTKEY is not implemented");
	if (n == 0xffffffffu) n = (u32)strlen(S(src)) + 1;
	if (!cap) RET(n, 6);
	if (n > cap) { set_err(ERROR_INSUFFICIENT_BUFFER); RET(0, 6); }
	for (k = 0; k < n; k++) M8(dst + k) = (u8)map_case(M8(src + k), fl);
	RET(n, 6);
}
/* CompareString(Locale, dwCmpFlags, s1, n1, s2, n2): code-unit order, NORM_IGNORECASE honoured */
static int cmp_units(u32 (*get)(u32, u32), u32 a, u32 na, u32 b, u32 nb, u32 fl) {
	u32 k;
	for (k = 0; k < na && k < nb; k++) {
		u32 x = get(a, k), y = get(b, k);
		if (fl & 1) { x = map_case(x, 0x100); y = map_case(y, 0x100); }
		if (x != y) return x < y ? 1 : 3;
	}
	return na < nb ? 1 : na > nb ? 3 : 2;
}
static u32 get_w(u32 s, u32 k) { return W16(s)[k]; }
static u32 get_a(u32 s, u32 k) { return M8(s + k); }
static void hle_CompareStringW(XCtx *c) {
	u32 n1 = A(3), n2 = A(5);
	if (n1 == 0xffffffffu) for (n1 = 0; W16(A(2))[n1]; n1++) {}
	if (n2 == 0xffffffffu) for (n2 = 0; W16(A(4))[n2]; n2++) {}
	RET(cmp_units(get_w, A(2), n1, A(4), n2, A(1)), 6);
}
static void hle_CompareStringA(XCtx *c) {
	u32 n1 = A(3), n2 = A(5);
	if (n1 == 0xffffffffu) n1 = (u32)strlen(S(A(2)));
	if (n2 == 0xffffffffu) n2 = (u32)strlen(S(A(4)));
	RET(cmp_units(get_a, A(2), n1, A(4), n2, A(1)), 6);
}
static void hle_lstrcpyA(XCtx *c) { strcpy(S(A(0)), S(A(1))); RET(A(0), 2); }
static void hle_lstrcatA(XCtx *c) { strcat(S(A(0)), S(A(1))); RET(A(0), 2); }

/* ============================================================================================ locale (en-US) */
static const char *locale_info(u32 type) {
	switch (type & 0xffff) { /* (LOCALE_NOUSEROVERRIDE etc. in the high bits) */
	case 0x0001: return "0409";          /* LOCALE_ILANGUAGE */
	case 0x0002: return "English (United States)";
	case 0x0003: return "ENU";           /* LOCALE_SABBREVLANGNAME */
	case 0x0007: return "ENU";
	case 0x0005: return "1";             /* LOCALE_ICOUNTRY */
	case 0x0006: return "United States";
	case 0x000b: return "437";           /* LOCALE_IDEFAULTCODEPAGE */
	case 0x000c: return ",";             /* LOCALE_SLIST */
	case 0x000e: return ".";             /* LOCALE_SDECIMAL */
	case 0x000f: return ",";             /* LOCALE_STHOUSAND */
	case 0x0010: return "3;0";           /* LOCALE_SGROUPING */
	case 0x0014: return "$";             /* LOCALE_SCURRENCY */
	case 0x0016: return ".";             /* LOCALE_SMONDECIMALSEP */
	case 0x0017: return ",";             /* LOCALE_SMONTHOUSANDSEP */
	case 0x0018: return "3;0";           /* LOCALE_SMONGROUPING */
	case 0x0019: return "2";             /* LOCALE_ICURRDIGITS */
	case 0x001b: return "0";             /* LOCALE_ICURRENCY */
	case 0x001c: return "0";             /* LOCALE_INEGCURR */
	case 0x001d: return "/";             /* LOCALE_SDATE */
	case 0x001e: return ":";             /* LOCALE_STIME */
	case 0x001f: return "M/d/yyyy";      /* LOCALE_SSHORTDATE */
	case 0x0020: return "dddd, MMMM d, yyyy";
	case 0x1003: return "h:mm:ss tt";    /* LOCALE_STIMEFORMAT */
	case 0x0028: return "AM";
	case 0x0029: return "PM";
	case 0x0050: return "";              /* LOCALE_SPOSITIVESIGN */
	case 0x0051: return "-";             /* LOCALE_SNEGATIVESIGN */
	case 0x0059: return "en";            /* LOCALE_SISO639LANGNAME */
	case 0x005a: return "US";            /* LOCALE_SISO3166CTRYNAME */
	case 0x1001: return "English";       /* LOCALE_SENGLANGUAGE */
	case 0x1002: return "United States"; /* LOCALE_SENGCOUNTRY */
	case 0x1004: return "1252";          /* LOCALE_IDEFAULTANSICODEPAGE */
	}
	return 0;
}
static void put_locale(XCtx *c, int wide) {
	u32 type = A(1), buf = A(2), cap = A(3), n, k;
	const char *v = locale_info(type);
	if (!v) {
		if (xrc_tool_env("XRC_TRACE_IMPORTS")) fprintf(stderr, "xrc: GetLocaleInfo type %08x not known\n", type);
		set_err(ERROR_INVALID_PARAMETER);
		RET(0, 4);
	}
	if (type & 0x20000000u) { /* LOCALE_RETURN_NUMBER: a DWORD */
		if (cap < (wide ? 2u : 4u)) { set_err(ERROR_INSUFFICIENT_BUFFER); RET(0, 4); }
		M32(buf) = (u32)strtoul(v, 0, 10);
		RET(wide ? 2 : 4, 4);
	}
	n = (u32)strlen(v) + 1;
	if (!cap) RET(n, 4);
	if (n > cap) { set_err(ERROR_INSUFFICIENT_BUFFER); RET(0, 4); }
	for (k = 0; k < n; k++) {
		if (wide) W16(buf)[k] = (u8)v[k]; else M8(buf + k) = (u8)v[k];
	}
	RET(n, 4);
}
static void hle_GetLocaleInfoA(XCtx *c) { put_locale(c, 0); }
static void hle_GetLocaleInfoW(XCtx *c) { put_locale(c, 1); }
static void hle_GetUserDefaultLCID(XCtx *c) { RET(0x0409, 0); }
static void hle_IsValidLocale(XCtx *c) { RET(A(0) == 0x0409 || A(0) == 0x0400 || A(0) == 0x0800, 2); }
/* EnumSystemLocalesA(lpLocaleEnumProc, dwFlags): would call back into guest code with "00000409"; report one
 * locale by calling the callback through the dispatcher */
static void hle_EnumSystemLocalesA(XCtx *c) {
	u32 cb = A(0), s = guest_strdup("00000409"), ret_esp;
	ret_esp = c->esp;
	c->esp -= 4; M32(c->esp) = s;            /* the callback's one argument (it is CALLBACK = stdcall) */
	c->esp -= 4; M32(c->esp) = XRC_RET_MAGIC;
	xrc_dispatch(c, cb);
	c->esp = ret_esp;
	xrc_heap_free(s);
	RET(1, 2);
}

/* ============================================================================================ the table */
#define E(n) {"KERNEL32.dll", #n, hle_##n}
const XrcImport xrc_hle_kernel32[] = {
	E(GetProcessHeap), E(HeapCreate), E(HeapAlloc), E(HeapFree), E(HeapReAlloc), E(HeapSize), E(LocalAlloc),
	E(LocalFree), E(GlobalLock), E(GlobalUnlock), E(VirtualAlloc), E(VirtualFree), E(GlobalMemoryStatus),
	E(GlobalMemoryStatusEx),
	E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue),
	E(InitializeCriticalSection), E(InitializeCriticalSectionAndSpinCount), E(EnterCriticalSection),
	E(LeaveCriticalSection), E(TryEnterCriticalSection), E(DeleteCriticalSection), E(InterlockedIncrement),
	E(InterlockedDecrement), E(InterlockedExchange), E(InterlockedExchangeAdd), E(InterlockedCompareExchange),
	E(GetCurrentProcessId), E(GetCurrentThreadId), E(GetCurrentProcess), E(GetCurrentThread), E(GetLastError),
	E(SetLastError), E(ExitProcess), E(TerminateProcess), E(IsDebuggerPresent), E(RaiseException), E(SetUnhandledExceptionFilter),
	E(UnhandledExceptionFilter), E(GetProcessAffinityMask), E(SetThreadPriority), E(GetThreadPriority),
	E(SetThreadIdealProcessor), E(IsBadReadPtr), E(OutputDebugStringA), E(Sleep), E(SleepEx), E(GetStartupInfoA),
	E(GetSystemTimeAsFileTime), E(GetTickCount), E(QueryPerformanceCounter), E(QueryPerformanceFrequency),
	E(GetTimeZoneInformation),
	E(GetCommandLineA), E(GetEnvironmentStrings), E(GetEnvironmentStringsW), E(FreeEnvironmentStringsA),
	E(FreeEnvironmentStringsW), E(SetEnvironmentVariableA),
	E(GetModuleHandleA), E(GetModuleHandleW), E(LoadLibraryA), E(LoadLibraryW), E(FreeLibrary), E(GetProcAddress),
	E(GetModuleFileNameA), E(GetModuleFileNameW),
	E(GetStdHandle), E(SetStdHandle), E(SetHandleCount), E(GetConsoleMode), E(GetConsoleCP),
	E(GetConsoleOutputCP), E(WriteConsoleA), E(WriteConsoleW),
	E(GetACP), E(GetOEMCP), E(IsValidCodePage), E(GetCPInfo), E(MultiByteToWideChar), E(WideCharToMultiByte),
	E(GetStringTypeW), E(GetStringTypeA), E(LCMapStringW), E(LCMapStringA), E(CompareStringW), E(CompareStringA),
	E(lstrcpyA), E(lstrcatA),
	E(GetLocaleInfoA), E(GetLocaleInfoW), E(GetUserDefaultLCID), E(IsValidLocale), E(EnumSystemLocalesA),
};
const unsigned xrc_hle_kernel32_n = sizeof xrc_hle_kernel32 / sizeof xrc_hle_kernel32[0];
