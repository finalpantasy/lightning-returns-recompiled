/* Guest function names for the runtime's stack prints (xrc_print_stack, the XRC_WATCH / stall dumps): the table
 * analysis/names.tsv (tools/names/build_names.py: RTTI, imports, strings, hand-curated) read once at startup.
 * File: XRC_NAMES=path (XRC_NAMES=off: none), else names.tsv beside the runtime's exe, else build\win\names.tsv.
 * Lines: "address<TAB>name[<TAB>source<TAB>confidence]", '#' comments; an address is a function start. Read-only after
 * xrc_names_load (called from main before the guest's threads exist), so the lookups need no lock. */
#include "xrc_rt.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct NameEnt { u32 addr, off; } NameEnt;
static NameEnt *ents;
static unsigned nents;
static char *pool;

static int ent_cmp(const void *a, const void *b) {
	u32 x = ((const NameEnt *)a)->addr, y = ((const NameEnt *)b)->addr;
	return x < y ? -1 : x > y;
}

static FILE *names_open(char *path, size_t n) {
	const char *e = getenv("XRC_NAMES");
	FILE *f;
	if (e && *e) {
		if (!strcmp(e, "off") || !strcmp(e, "0")) return 0;
		snprintf(path, n, "%s", e);
		return fopen(path, "rb");
	}
#ifdef _WIN32
	{
		char exe[MAX_PATH];
		DWORD k = GetModuleFileNameA(0, exe, sizeof exe);
		if (k && k < sizeof exe) {
			char *s = strrchr(exe, '\\');
			if (s) {
				s[1] = 0;
				snprintf(path, n, "%snames.tsv", exe);
				if ((f = fopen(path, "rb"))) return f;
			}
		}
	}
#endif
	snprintf(path, n, "build/win/names.tsv");
	return fopen(path, "rb");
}

void xrc_names_load(void) {
	char path[1024];
	FILE *f;
	long sz;
	char *p, *end;
	unsigned cap;
	if (pool) return;
	if (!(f = names_open(path, sizeof path))) return;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	pool = (char *)malloc((size_t)sz + 1);
	if (!pool || fread(pool, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(pool); pool = 0; return; }
	fclose(f);
	pool[sz] = 0;
	cap = 1024;
	ents = (NameEnt *)malloc(cap * sizeof *ents);
	for (p = pool, end = pool + sz; p < end;) {
		char *nl = memchr(p, '\n', (size_t)(end - p)), *tab, *q;
		if (!nl) nl = end;
		*nl = 0;
		if (nl > p && nl[-1] == '\r') nl[-1] = 0;
		if (*p != '#' && (tab = strchr(p, '\t'))) {
			u32 a = (u32)strtoul(p, &q, 16);
			if (q == tab && a) {
				char *nm = tab + 1, *t2 = strchr(nm, '\t');
				if (t2) *t2 = 0;
				if (*nm) {
					if (nents == cap) { cap *= 2; ents = (NameEnt *)realloc(ents, cap * sizeof *ents); }
					ents[nents].addr = a;
					ents[nents].off = (u32)(nm - pool);
					nents++;
				}
			}
		}
		p = nl + 1;
	}
	qsort(ents, nents, sizeof *ents, ent_cmp);
	fprintf(stderr, "xrc: %u guest function names from %s\n", nents, path);
}

/* the name of the guest function starting at fn (0: none) */
const char *xrc_guest_name(u32 fn) {
	unsigned lo = 0, hi = nents;
	while (lo < hi) {
		unsigned m = (lo + hi) / 2;
		if (ents[m].addr < fn) lo = m + 1; else hi = m;
	}
	return lo < nents && ents[lo].addr == fn ? pool + ents[lo].off : 0;
}

/* "00747c50 FileStream::ReadAll+0x1a0": fn the function's start, at an address inside it (at == fn or 0: no offset) */
const char *xrc_guest_fmt(char *buf, size_t n, u32 fn, u32 at) {
	const char *nm = xrc_guest_name(fn);
	if (!nm) {
		if (at > fn) snprintf(buf, n, "%08x+0x%x", fn, at - fn); else snprintf(buf, n, "%08x", fn);
	} else if (at > fn) snprintf(buf, n, "%08x %s+0x%x", fn, nm, at - fn);
	else snprintf(buf, n, "%08x %s", fn, nm);
	return buf;
}

int xrc_names_loaded(void) { return nents != 0; }
