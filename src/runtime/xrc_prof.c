/* XRC_PROF=1: a cheap sampling profiler of the main guest thread (Windows hosts).
 *
 * A host thread wakes every millisecond, suspends the main thread (the one that runs the guest's entry point: the game
 * loop), reads its host registers and unwinds its host stack with the executable's own unwind tables (.pdata, parsed
 * here, so nothing that can take a lock runs while the thread is stopped). Every host frame whose function is a
 * translation (xf_<addr>, found by its start in the xrc_fns table) is a guest frame; other frames in the executable are
 * the runtime's own functions (HLE, dispatch, Vulkan backend), named from the executable's COFF symbols at the dump.
 * A sample whose PC is outside the executable (a wait in a system DLL, the Vulkan driver) counts as "[outside the\n * exe]", with the guest callers from the guest stack walk (xrc_walk_stack: exact for a thread blocked in an import).
 *
 * Report (written every XRC_PROF_EVERY seconds, default 30, and at exit; runs killed with taskkill keep the last one):
 *   XRC_PROF_FILE (default %TEMP%\xrc_prof.txt): samples, then
 *   - flat top 50 by self samples (the innermost frame, host or guest),
 *   - flat top 50 of guest functions by "guest self" (the innermost guest frame: its own code plus the runtime / HLE
 *     helpers it calls directly),
 *   - inclusive top 50 (each function counted once per sample in which it is anywhere on the stack).
 * XRC_PROF_DELAY=secs: no samples before then (skip the boot). XRC_PROF_HZ: samples a second (default 1000). */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"

const char *xrc_guest_name(u32 fn);
int xrc_walk_stack(XCtx *c, u32 *out, int max);

#define PF_DEPTH 64
#define PF_HOST 0x80000000u   /* a key with this bit: a host function (its RVA); else a guest function address */
#define PF_OUTSIDE 0xffffffffu /* the PC outside the executable */
#define PF_HBITS 18
typedef struct { u32 key; u32 self, gself, incl, stamp, wself, wincl; } PfEnt; /* (w*: samples blocked outside the exe) */
static PfEnt *pf_tab;
static volatile LONG pf_lock; /* (the dump and the sampler share pf_tab: a spin flag) */
static unsigned long long pf_samples, pf_outside, pf_lost;
static HANDLE pf_thread;
static XCtx *pf_ctx;
static unsigned char *pf_base;
static RUNTIME_FUNCTION *pf_rf;
static unsigned pf_nrf;
static struct HG { u32 rva, guest; } *pf_hg; /* the translations' host starts -> guest address, sorted by rva */
static unsigned pf_nhg;
static const char *pf_file;
static double pf_every = 30, pf_secs;
static struct { HMODULE m; unsigned long long n; } pf_mods[32]; /* the outside samples by module */
static void count_module(u64 pc) {
	HMODULE m = 0;
	int k;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)pc, &m);
	for (k = 0; k < 32; k++)
		if (pf_mods[k].m == m || !pf_mods[k].n) { pf_mods[k].m = m; pf_mods[k].n++; return; }
}

static int hg_cmp(const void *a, const void *b) {
	u32 x = ((const struct HG *)a)->rva, y = ((const struct HG *)b)->rva;
	return x < y ? -1 : x > y;
}
static u32 guest_of_rva(u32 rva) {
	unsigned lo = 0, hi = pf_nhg;
	while (lo < hi) {
		unsigned m = (lo + hi) / 2;
		if (pf_hg[m].rva < rva) lo = m + 1; else hi = m;
	}
	return lo < pf_nhg && pf_hg[lo].rva == rva ? pf_hg[lo].guest : 0;
}
/* the unwind entry of an RVA in the executable (none: a leaf function, its return address at [rsp]) */
static RUNTIME_FUNCTION *rf_of(u32 rva) {
	unsigned lo = 0, hi = pf_nrf;
	while (lo < hi) {
		unsigned m = (lo + hi) / 2;
		if (pf_rf[m].EndAddress <= rva) lo = m + 1; else hi = m;
	}
	return lo < pf_nrf && pf_rf[lo].BeginAddress <= rva && rva < pf_rf[lo].EndAddress ? &pf_rf[lo] : 0;
}
static PfEnt *ent(u32 key) {
	u32 i = (key * 0x9e3779b1u) >> (32 - PF_HBITS), n = 0;
	while (pf_tab[i].key && pf_tab[i].key != key) {
		i = (i + 1) & ((1u << PF_HBITS) - 1);
		if (++n > (1u << PF_HBITS) / 2) return 0;
	}
	pf_tab[i].key = key;
	return &pf_tab[i];
}

typedef struct { u32 rva; const char *name; } HSym;
static HSym *pf_sym;
static unsigned pf_nsym;
/* the start of the symbol at or before an RVA (0: none): a leaf function's start (symbols loaded before sampling) */
static u32 sym_start(u32 rva) {
	unsigned lo = 0, hi = pf_nsym;
	while (lo < hi) {
		unsigned m = (lo + hi) / 2;
		if (pf_sym[m].rva <= rva) lo = m + 1; else hi = m;
	}
	return lo ? pf_sym[lo - 1].rva : 0;
}
/* one sample of the stopped thread: keys, innermost first */
static int unwind(CONTEXT *cx, u32 *keys) {
	int n = 0, guard = 0;
	size_t size = 0;
	{
		IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(pf_base + ((IMAGE_DOS_HEADER *)pf_base)->e_lfanew);
		size = nt->OptionalHeader.SizeOfImage;
	}
	while (n < PF_DEPTH && guard++ < 256) {
		u64 pc = cx->Rip;
		u32 rva;
		RUNTIME_FUNCTION *rf;
		if (pc < (u64)(uintptr_t)pf_base || pc >= (u64)(uintptr_t)pf_base + size) {
			if (n == 0) keys[n++] = PF_OUTSIDE;
			break;
		}
		rva = (u32)(pc - (u64)(uintptr_t)pf_base);
		rf = rf_of(n ? rva - 1 : rva); /* (a return address: the call before it) */
		if (!rf && n) break;
		if (rf) {
			u32 g = guest_of_rva(rf->BeginAddress);
			void *hd = 0;
			DWORD64 est = 0;
			u32 key = g ? g : (PF_HOST | rf->BeginAddress);
			if (!n || keys[n - 1] != key) keys[n++] = key;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, (DWORD64)(uintptr_t)pf_base, pc, rf, cx, &hd, &est, 0);
		} else {
			/* a leaf: no frame of its own; its start from the symbols (a translation's start when it is one) */
			u32 st = sym_start(rva), g = st ? guest_of_rva(st) : 0;
			u32 key = g ? g : PF_HOST | (st ? st : rva);
			if (!n || keys[n - 1] != key) keys[n++] = key;
			cx->Rip = *(DWORD64 *)(uintptr_t)cx->Rsp;
			cx->Rsp += 8;
		}
		if (!cx->Rip || cx->Rip == pc) break;
	}
	return n;
}

static void add_sample(u32 *keys, int n, int outside) {
	static u32 stamp;
	int k, gs = -1;
	PfEnt *e;
	stamp++;
	for (k = 0; k < n; k++) if (!(keys[k] & PF_HOST) && keys[k] != PF_OUTSIDE) { gs = k; break; }
	while (InterlockedCompareExchange(&pf_lock, 1, 0)) Sleep(0);
	pf_samples++;
	if (outside) {
		pf_outside++;
		if (gs >= 0 && (e = ent(keys[gs]))) e->wself++;
		for (k = 0; k < n; k++)
			if ((e = ent(keys[k])) && e->stamp != stamp) { e->stamp = stamp; e->wincl++; }
	} else {
		if (n > 0 && (e = ent(keys[0]))) e->self++;
		if (gs >= 0 && (e = ent(keys[gs]))) e->gself++;
		for (k = 0; k < n; k++)
			if ((e = ent(keys[k])) && e->stamp != stamp) { e->stamp = stamp; e->incl++; }
	}
	InterlockedExchange(&pf_lock, 0);
}

/* ------------------------------------------------------------------------------------------------ host names */
static int sym_cmp(const void *a, const void *b) {
	u32 x = ((const HSym *)a)->rva, y = ((const HSym *)b)->rva;
	return x < y ? -1 : x > y;
}
/* the executable's COFF symbol table (llvm-mingw keeps it): function symbols of .text */
static void load_syms(void) {
	char path[MAX_PATH];
	FILE *f;
	long sz;
	unsigned char *b;
	u32 pe, symoff, nsym, i, textva = 0, nsec, opt;
	if (pf_sym) return;
	if (!GetModuleFileNameA(0, path, sizeof path) || !(f = fopen(path, "rb"))) return;
	fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
	b = (unsigned char *)malloc((size_t)sz);
	if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return; }
	fclose(f);
	pe = *(u32 *)(b + 0x3c);
	symoff = *(u32 *)(b + pe + 12); nsym = *(u32 *)(b + pe + 16);
	nsec = *(unsigned short *)(b + pe + 6); opt = *(unsigned short *)(b + pe + 20);
	if (!symoff || symoff >= (u32)sz) { free(b); return; }
	{
		unsigned char *sec = b + pe + 24 + opt;
		unsigned short s;
		u32 *va = (u32 *)calloc(nsec + 1, 4);
		const char *strtab = (const char *)(b + symoff + nsym * 18);
		for (s = 0; s < nsec; s++) va[s + 1] = *(u32 *)(sec + 40 * s + 12);
		textva = va[1];
		(void)textva;
		pf_sym = (HSym *)calloc(nsym + 1, sizeof *pf_sym);
		for (i = 0; i < nsym; i++) {
			unsigned char *r = b + symoff + i * 18;
			short secno = *(short *)(r + 12);
			unsigned char cls = r[16], naux = r[17];
			if (secno > 0 && secno <= (short)nsec && (cls == 2 || cls == 3) && (*(unsigned short *)(r + 14) & 0x20)) {
				char nm[9];
				const char *name;
				if (*(u32 *)r == 0) name = strtab + *(u32 *)(r + 4);
				else { memcpy(nm, r, 8); nm[8] = 0; name = _strdup(nm); }
				pf_sym[pf_nsym].rva = va[secno] + *(u32 *)(r + 8);
				pf_sym[pf_nsym++].name = name;
			}
			i += naux;
		}
		free(va);
		/* (b stays: the long names point into its string table) */
	}
	qsort(pf_sym, pf_nsym, sizeof *pf_sym, sym_cmp);
}
static const char *host_name(u32 rva, char *buf, size_t n) {
	unsigned lo = 0, hi = pf_nsym;
	if (rva & 1u) { /* (a leaf's page) */
		rva &= ~1u;
	}
	while (lo < hi) {
		unsigned m = (lo + hi) / 2;
		if (pf_sym[m].rva <= rva) lo = m + 1; else hi = m;
	}
	if (lo && pf_nsym) snprintf(buf, n, "host %s%s", pf_sym[lo - 1].name, rva != pf_sym[lo - 1].rva ? "+" : "");
	else snprintf(buf, n, "host +%x", rva);
	return buf;
}
static const char *key_name(u32 key, char *buf, size_t n) {
	if (key == PF_OUTSIDE) return "[outside the exe: waits, system DLLs, driver]";
	if (key & PF_HOST) return host_name(key & ~PF_HOST, buf, n);
	{
		const char *g = xrc_guest_name(key);
		snprintf(buf, n, "%08x %s", key, g ? g : "");
	}
	return buf;
}

static int cmp_self(const void *a, const void *b) { return (int)((const PfEnt *)b)->self - (int)((const PfEnt *)a)->self; }
static int cmp_gself(const void *a, const void *b) { return (int)((const PfEnt *)b)->gself - (int)((const PfEnt *)a)->gself; }
static int cmp_incl(const void *a, const void *b) { return (int)((const PfEnt *)b)->incl - (int)((const PfEnt *)a)->incl; }
static int cmp_wself(const void *a, const void *b) { return (int)((const PfEnt *)b)->wself - (int)((const PfEnt *)a)->wself; }
static void dump(void) {
	static PfEnt *cp;
	unsigned i, n = 0;
	unsigned long long tot;
	FILE *f;
	char buf[512];
	if (!pf_tab) return;
	if (!cp) cp = (PfEnt *)malloc(sizeof *cp << PF_HBITS);
	if (!cp) return;
	load_syms();
	while (InterlockedCompareExchange(&pf_lock, 1, 0)) Sleep(0);
	for (i = 0; i < (1u << PF_HBITS); i++) if (pf_tab[i].key) cp[n++] = pf_tab[i];
	tot = pf_samples;
	InterlockedExchange(&pf_lock, 0);
	if (!tot || !(f = fopen(pf_file, "w"))) return;
	{
		unsigned long long cpu = tot - pf_outside;
		double ms = pf_secs > 0 ? 1000.0 * pf_secs / (double)tot : 0;
		fprintf(f, "xrc_prof: %llu samples of the main thread over %.0f s (%.2f ms apart): %llu running in the exe (CPU), %llu "
		        "outside it (waits, system DLLs, driver), %llu lost\n", tot, pf_secs, ms, cpu, pf_outside, pf_lost);
		fprintf(f, "percentages below: of the CPU samples (%llu)\n", cpu);
		if (!cpu) cpu = 1;
		fprintf(f, "\n== flat: self (innermost frame, host or guest), top 50\n   samples      %%  function\n");
		qsort(cp, n, sizeof *cp, cmp_self);
		for (i = 0; i < n && i < 50 && cp[i].self; i++)
			fprintf(f, "%9u %6.2f  %s\n", cp[i].self, 100.0 * cp[i].self / cpu, key_name(cp[i].key, buf, sizeof buf));
		fprintf(f, "\n== flat: guest self (innermost guest frame, with the runtime / HLE helpers it calls), top 50\n");
		qsort(cp, n, sizeof *cp, cmp_gself);
		for (i = 0; i < n && i < 50 && cp[i].gself; i++)
			fprintf(f, "%9u %6.2f  %s\n", cp[i].gself, 100.0 * cp[i].gself / cpu, key_name(cp[i].key, buf, sizeof buf));
		fprintf(f, "\n== inclusive (on the stack), top 50\n");
		qsort(cp, n, sizeof *cp, cmp_incl);
		for (i = 0; i < n && i < 50 && cp[i].incl; i++)
			fprintf(f, "%9u %6.2f  %s\n", cp[i].incl, 100.0 * cp[i].incl / cpu, key_name(cp[i].key, buf, sizeof buf));
		fprintf(f, "\n== inclusive, guest functions only, top 100\n");
		{
			unsigned m = 0;
			for (i = 0; i < n && m < 100 && cp[i].incl; i++)
				if (!(cp[i].key & PF_HOST) && cp[i].key != PF_OUTSIDE) {
					fprintf(f, "%9u %6.2f  %s\n", cp[i].incl, 100.0 * cp[i].incl / cpu, key_name(cp[i].key, buf, sizeof buf));
					m++;
				}
		}
		fprintf(f, "\n== outside samples by module (percent of all samples)\n");
		for (i = 0; i < 32 && pf_mods[i].n; i++) {
			char mp[MAX_PATH] = "?";
			if (pf_mods[i].m) GetModuleFileNameA(pf_mods[i].m, mp, sizeof mp);
			fprintf(f, "%9llu %6.2f  %s\n", pf_mods[i].n, 100.0 * pf_mods[i].n / tot, mp);
		}
		fprintf(f, "\n== blocked: the guest function a wait was called from (percent of all samples), top 20\n");
		qsort(cp, n, sizeof *cp, cmp_wself);
		for (i = 0; i < n && i < 20 && cp[i].wself; i++)
			fprintf(f, "%9u %6.2f  %s\n", cp[i].wself, 100.0 * cp[i].wself / tot, key_name(cp[i].key, buf, sizeof buf));
	}
	fclose(f);
}


/* spike7: XRC_SPIKE_LOG=path: every sample (qpc stamp, outside?, module of RIP, guest/host keys) appended to a text file every 15 s */
extern unsigned long long xrc_qpc(void);
typedef struct { unsigned long long t; u32 out, mod, n; u32 k[7]; } SpRec;
#define SP_MAX 400000
static SpRec *sp_rec; static unsigned sp_n, sp_w;
static const char *sp_path;
static HMODULE sp_mods[64]; static unsigned sp_nm;
static u32 *sp_seen; static unsigned sp_nseen;
static void sp_add(int outside, u64 rip, u32 *keys, int n) {
	SpRec *r; unsigned m; HMODULE hm = 0; int k;
	if (!sp_rec || sp_n >= SP_MAX) return;
	r = &sp_rec[sp_n++]; r->t = xrc_qpc(); r->out = outside; r->n = n > 7 ? 7 : n;
	{ int j = 0; r->n = 0; for (k = 0; k < n && j < 7; k++) if (k < 3 || !(keys[k] & PF_HOST)) { r->k[j++] = keys[k]; } r->n = j; }
	r->mod = 0;
	if (outside) {
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)rip, &hm);
		for (m = 0; m < sp_nm; m++) if (sp_mods[m] == hm) break;
		if (m == sp_nm && sp_nm < 64) sp_mods[sp_nm++] = hm;
		r->mod = m + 1;
	}
}
static int sp_has(u32 k) { unsigned i; for (i = 0; i < sp_nseen; i++) if (sp_seen[i] == k) return 1; return 0; }
static void sp_flush(void) {
	FILE *f; unsigned i; int k; char buf[256];
	if (!sp_rec || sp_w >= sp_n) return;
	f = fopen(sp_path, sp_w ? "a" : "w"); if (!f) return;
	for (; sp_w < sp_n; sp_w++) {
		SpRec *r = &sp_rec[sp_w];
		for (k = 0; k < (int)r->n; k++) if (!sp_has(r->k[k]) && sp_nseen < 20000) { sp_seen[sp_nseen++] = r->k[k]; fprintf(f, "K %x %s\n", r->k[k], key_name(r->k[k], buf, sizeof buf)); }
		fprintf(f, "S %llu %u %u", r->t, r->out, r->mod);
		for (k = 0; k < (int)r->n; k++) fprintf(f, " %x", r->k[k]);
		fprintf(f, "\n");
	}
	for (i = 0; i < sp_nm; i++) { char mn[MAX_PATH] = "?"; GetModuleFileNameA(sp_mods[i], mn, sizeof mn); fprintf(f, "M %u %s\n", i + 1, mn); }
	fclose(f);
}

static DWORD WINAPI sampler(LPVOID arg) {
	CONTEXT cx;
	u32 keys[PF_DEPTH + 16];
	double delay = getenv("XRC_PROF_DELAY") ? atof(getenv("XRC_PROF_DELAY")) : 0;
	int hz = getenv("XRC_PROF_HZ") ? atoi(getenv("XRC_PROF_HZ")) : 1000;
	DWORD period = hz > 0 && hz <= 1000 ? 1000u / (DWORD)hz : 1;
	ULONGLONG t0 = GetTickCount64(), last = t0, ts0;
	/* (a high-resolution timer: Windows 11 ignores timeBeginPeriod for a process without a visible window, and Sleep(1)
	 * then sleeps 15.6 ms) */
	HANDLE tm = CreateWaitableTimerExW(0, 0, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
	(void)arg; (void)t0;
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	if (delay > 0) Sleep((DWORD)(delay * 1000));
	ts0 = GetTickCount64();
	for (;;) {
		int n, outside;
		if (tm) {
			LARGE_INTEGER due;
			due.QuadPart = -(LONGLONG)period * 10000;
			SetWaitableTimer(tm, &due, 0, 0, 0, FALSE);
			WaitForSingleObject(tm, INFINITE);
		} else
			Sleep(period);
		if (SuspendThread(pf_thread) == (DWORD)-1) { pf_lost++; continue; }
		memset(&cx, 0, sizeof cx);
		cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		if (!GetThreadContext(pf_thread, &cx)) { ResumeThread(pf_thread); pf_lost++; continue; }
		n = unwind(&cx, keys);
		ResumeThread(pf_thread);
		outside = n == 1 && keys[0] == PF_OUTSIDE;
		if (outside) {
			count_module(cx.Rip);
			/* the guest callers from the guest stack (the thread's XCtx as of its last call) */
			u32 w[PF_DEPTH];
			int k, m = xrc_walk_stack(pf_ctx, w, PF_DEPTH - 1);
			for (k = 0; k < m; k++) if (w[k] && (!n || keys[n - 1] != w[k])) keys[n++] = w[k];
		}
		if (n) { add_sample(keys, n, outside); sp_add(outside, cx.Rip, keys, n); }
		{ static ULONGLONG spl; if (sp_rec && GetTickCount64() - spl > 15000) { spl = GetTickCount64(); sp_flush(); } }
		pf_secs = (double)(GetTickCount64() - ts0) / 1000.0;
		if (GetTickCount64() - last >= (ULONGLONG)(pf_every * 1000)) { last = GetTickCount64(); dump(); }
	}
	return 0;
}

/* one sample of the main thread now, printed (XRC_HITCH_STACK's watchdog; needs XRC_PROF=1): the innermost frames, host functions and
 * guest ones named */
void xrc_prof_snapshot(void) {
	CONTEXT cx;
	u32 keys[PF_DEPTH + 16];
	char buf[256];
	int n, k;
	if (!pf_tab || !pf_thread) return;
	if (SuspendThread(pf_thread) == (DWORD)-1) return;
	memset(&cx, 0, sizeof cx);
	cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (!GetThreadContext(pf_thread, &cx)) { ResumeThread(pf_thread); return; }
	n = unwind(&cx, keys);
	ResumeThread(pf_thread);
	if (n == 1 && keys[0] == PF_OUTSIDE) { /* (in a system DLL or the driver: which, and the guest callers) */
		HMODULE hm = 0;
		char mn[MAX_PATH] = "?";
		u32 w[PF_DEPTH];
		int m = xrc_walk_stack(pf_ctx, w, 12);
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)cx.Rip, &hm)) GetModuleFileNameA(hm, mn, sizeof mn);
		fprintf(stderr, "xrc: main thread sample: outside the exe in %s (rip %llx), guest callers:", mn, (unsigned long long)cx.Rip);
		for (k = 0; k < m; k++) fprintf(stderr, " | %s", key_name(w[k], buf, sizeof buf));
		fprintf(stderr, "\n");
		return;
	}
	fprintf(stderr, "xrc: main thread sample:");
	for (k = 0; k < n && k < 14; k++) fprintf(stderr, " | %s", key_name(keys[k], buf, sizeof buf));
	fprintf(stderr, "\n");
}

static void at_exit(void) { dump(); }

/* on the main thread, before the guest's entry point; c: its XCtx */
void xrc_prof_start(XCtx *c) {
	static char path[MAX_PATH];
	const char *e = getenv("XRC_PROF");
	IMAGE_NT_HEADERS *nt;
	IMAGE_DATA_DIRECTORY *dd;
	unsigned k;
	if (!e || !*e || *e == '0') return;
	pf_ctx = c;
	pf_base = (unsigned char *)GetModuleHandleA(0);
	nt = (IMAGE_NT_HEADERS *)(pf_base + ((IMAGE_DOS_HEADER *)pf_base)->e_lfanew);
	dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
	pf_rf = (RUNTIME_FUNCTION *)(pf_base + dd->VirtualAddress);
	pf_nrf = dd->Size / sizeof *pf_rf;
	pf_hg = (struct HG *)malloc((xrc_nfns + 1) * sizeof *pf_hg);
	for (k = 0; k < xrc_nfns; k++) {
		pf_hg[pf_nhg].rva = (u32)((unsigned char *)(uintptr_t)xrc_fns[k].fn - pf_base);
		pf_hg[pf_nhg++].guest = xrc_fns[k].addr;
	}
	qsort(pf_hg, pf_nhg, sizeof *pf_hg, hg_cmp);
	pf_tab = (PfEnt *)calloc(1u << PF_HBITS, sizeof *pf_tab);
	load_syms();
	if (getenv("XRC_PROF_FILE")) pf_file = getenv("XRC_PROF_FILE");
	else { GetTempPathA(sizeof path - 16, path); strcat(path, "xrc_prof.txt"); pf_file = path; }
	if (getenv("XRC_PROF_EVERY")) pf_every = atof(getenv("XRC_PROF_EVERY"));
	if (pf_every < 1) pf_every = 1;
	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &pf_thread,
	                     THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) return;
	atexit(at_exit);
	{
		/* Windows 11 throttles the timers of a process with no visible window (power throttling: Sleep(1) and even
		 * high-resolution timers then fire every 15.6 ms or more): opt this process out while profiling */
		struct { ULONG Version, ControlMask, StateMask; } pt = {1, 0x4 /* IGNORE_TIMER_RESOLUTION */, 0};
		typedef BOOL(WINAPI * SPI)(HANDLE, int, LPVOID, DWORD);
		SPI spi = (SPI)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
		if (spi) spi(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &pt, sizeof pt);
		timeBeginPeriod(1);
	}
	if (getenv("XRC_SPIKE_LOG")) { sp_path = getenv("XRC_SPIKE_LOG"); sp_rec = (SpRec *)calloc(SP_MAX, sizeof(SpRec)); sp_seen = (u32 *)calloc(20000, 4); }
	CloseHandle(CreateThread(0, 0, sampler, 0, 0, 0));
	fprintf(stderr, "xrc: prof: sampling the main thread (%u unwind entries, %u translations) -> %s\n", pf_nrf, pf_nhg, pf_file);
}
#else
#include "xrc_rt.h"
void xrc_prof_start(XCtx *c) { (void)c; }
#endif
