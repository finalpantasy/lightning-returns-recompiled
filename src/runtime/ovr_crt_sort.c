/* ovr_crt_sort.c - the CRT's qsort (VC9 libcmt, 01011d90) for the comparators known to be plain key orders.
 *
 * The game sorts a 32768-entry pointer table every frame (00ec4560) with 00ec4440, an unsigned compare of the two
 * u32 elements: some 130k comparator calls a frame, each through the dispatcher. Its order is total and equal
 * elements are identical, so any correct sort gives the very array the CRT's would: that one is radix-sorted natively.
 * 00f049f0 orders u16 indices by a 64-bit key from a table (render items, every frame): equal keys can belong to
 * different elements, so its sort is the CRT's own algorithm (crt_qsort below, as VC9's qsort.c) with the comparator
 * native -- the same array, no dispatcher call per comparison.
 * Any other comparator runs the CRT's qsort as translated (it stays in the dispatch table: overrides.tsv lists it as
 * "wrapped", so nothing calls it directly). XRC_QSORT_CHECK=1 also runs the translation on a copy and compares. */
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"

/* ascending u32s: an LSD radix sort (4 passes of 8 bits; a pass whose byte is the same everywhere is skipped) -- no
 * comparator calls, linear in n */
static void sort_u32(u32 *v, u32 n) {
	static u32 *tmp;
	static u32 cap;
	u32 cnt[256], *src = v, *dst, sh, i;
	if (n < 2) return;
	if (n > cap) { free(tmp); tmp = (u32 *)malloc((size_t)n * 4); cap = n; }
	dst = tmp;
	for (sh = 0; sh < 32; sh += 8) {
		u32 sum = 0, *t;
		memset(cnt, 0, sizeof cnt);
		for (i = 0; i < n; i++) cnt[(src[i] >> sh) & 0xff]++;
		if (cnt[(src[0] >> sh) & 0xff] == n) continue; /* (all the same byte: already in order for this pass) */
		for (i = 0; i < 256; i++) { u32 k = cnt[i]; cnt[i] = sum; sum += k; }
		for (i = 0; i < n; i++) dst[cnt[(src[i] >> sh) & 0xff]++] = src[i];
		t = src; src = dst; dst = t;
	}
	if (src != v) memcpy(v, src, (size_t)n * 4);
}
/* VC9 libcmt's qsort (qsort.c) with a native comparator: quicksort, median of three, the partition that follows the
 * pivot as it moves, selection sort ("shortsort") at 8 elements or fewer -- element for element the CRT's result */
typedef int (*NCmp)(const u8 *a, const u8 *b);
static void swap_w(u8 *a, u8 *b, u32 w) {
	if (a != b) while (w--) { u8 t = *a; *a++ = *b; *b++ = t; }
}
static void crt_shortsort(u8 *lo, u8 *hi, u32 w, NCmp cmp) {
	while (hi > lo) {
		u8 *max = lo, *p;
		for (p = lo + w; p <= hi; p += w) if (cmp(p, max) > 0) max = p;
		swap_w(max, hi, w);
		hi -= w;
	}
}
static void crt_qsort(u8 *base, u32 num, u32 w, NCmp cmp) {
	u8 *lo, *hi, *mid, *loguy, *higuy, *lostk[64], *histk[64];
	int sp = 0;
	if (num < 2 || !w) return;
	lo = base;
	hi = base + (size_t)w * (num - 1);
	for (;;) {
		size_t size = (size_t)(hi - lo) / w + 1;
		if (size <= 8) crt_shortsort(lo, hi, w, cmp);
		else {
			mid = lo + (size / 2) * w;
			if (cmp(lo, mid) > 0) swap_w(lo, mid, w);
			if (cmp(lo, hi) > 0) swap_w(lo, hi, w);
			if (cmp(mid, hi) > 0) swap_w(mid, hi, w);
			loguy = lo;
			higuy = hi;
			for (;;) {
				if (mid > loguy) do loguy += w; while (loguy < mid && cmp(loguy, mid) <= 0);
				if (mid <= loguy) do loguy += w; while (loguy <= hi && cmp(loguy, mid) <= 0);
				do higuy -= w; while (higuy > mid && cmp(higuy, mid) > 0);
				if (higuy < loguy) break;
				swap_w(loguy, higuy, w);
				if (mid == higuy) mid = loguy;
			}
			higuy += w;
			if (mid < higuy) do higuy -= w; while (higuy > mid && cmp(higuy, mid) == 0);
			if (mid >= higuy) do higuy -= w; while (higuy > lo && cmp(higuy, mid) == 0);
			if (higuy - lo >= hi - loguy) {
				if (lo < higuy) { lostk[sp] = lo; histk[sp] = higuy; sp++; }
				if (loguy < hi) { lo = loguy; continue; }
			} else {
				if (loguy < hi) { lostk[sp] = loguy; histk[sp] = hi; sp++; }
				if (lo < higuy) { hi = higuy; continue; }
			}
		}
		if (--sp < 0) return;
		lo = lostk[sp];
		hi = histk[sp];
	}
}
/* 00f049f0: elements' u16 index i; key ((u64)T[i].+4 << 32) | T[i].+8, T = *(*(0x028670a0) + 0x108) (12-byte
 * records), compared unsigned (the original's __allshl of the sign-extended +4 by 32 keeps its low word only) */
static const u8 *cmp_f049f0_t;
static u64 key_f049f0(const u8 *e) {
	const u8 *r = cmp_f049f0_t + 12u * (u32)(e[0] | e[1] << 8);
	u32 hi, lo;
	memcpy(&hi, r + 4, 4); memcpy(&lo, r + 8, 4);
	return (u64)hi << 32 | lo;
}
static int cmp_f049f0(const u8 *a, const u8 *b) {
	u64 x = key_f049f0(a), y = key_f049f0(b);
	return x > y ? 1 : x < y ? -1 : 0;
}
static void XRC_CC m_qsort(XCtx *c) {
	static XFn orig;
	static int check = -1;
	u32 base = xrc_arg(c, 0), num = xrc_arg(c, 1), width = xrc_arg(c, 2), cmp = xrc_arg(c, 3);
	if (!orig) orig = xrc_translation_of(0x01011d90u);
	if (check < 0) check = xrc_tool_env("XRC_QSORT_CHECK") != 0;
	if (cmp == 0x00ec4440u && width == 4 && num < 0x10000000u) {
		u32 *v = (u32 *)(xrc_mem + base);
		u32 *want = 0;
		if (check && num) { /* the translation's result, for comparison */
			u32 *save = (u32 *)malloc((size_t)num * 4);
			memcpy(save, v, (size_t)num * 4);
			orig(c);
			want = (u32 *)malloc((size_t)num * 4);
			memcpy(want, v, (size_t)num * 4);
			memcpy(v, save, (size_t)num * 4);
			free(save);
		}
		sort_u32(v, num);
		if (want) {
			if (memcmp(want, v, (size_t)num * 4)) xrc_fatal(c, "ovr qsort: the native sort differs from the CRT's (%u elements)", num);
			free(want);
			return; /* (orig already returned for this call) */
		}
		xrc_cdecl_ret(c);
		return;
	}
	static int native_f049f0 = -1; /* (XRC_QSORT_NATIVE=0: the translated CRT's sort, to compare) */
	if (native_f049f0 < 0) native_f049f0 = !(xrc_tool_env("XRC_QSORT_NATIVE") && atoi(xrc_tool_env("XRC_QSORT_NATIVE")) == 0);
	if (cmp == 0x00f049f0u && native_f049f0 && width >= 2 && num < 0x10000000u && (unsigned long long)num * width < 0x40000000ull) {
		u8 *v = xrc_mem + base, *want = 0;
		size_t bytes = (size_t)num * width;
		cmp_f049f0_t = xrc_mem + M32(M32(0x028670a0u) + 0x108u);
		if (check && num) {
			u8 *save = (u8 *)malloc(bytes);
			memcpy(save, v, bytes);
			orig(c);
			want = (u8 *)malloc(bytes);
			memcpy(want, v, bytes);
			memcpy(v, save, bytes);
			free(save);
		}
		crt_qsort(v, num, width, cmp_f049f0);
		if (want) {
			static unsigned nchk;
			if (memcmp(want, v, bytes)) xrc_fatal(c, "ovr qsort: the native f049f0 sort differs from the CRT's (%u x %u bytes)", num, width);
			free(want);
			if ((++nchk, nchk & (nchk - 1)) == 0) fprintf(stderr, "xrc: ovr qsort: %u f049f0 sorts checked (this one %u x %u bytes)\n", nchk, num, width);
			return;
		}
		xrc_cdecl_ret(c);
		return;
	}
	orig(c);
}

const XrcManual xrc_manual_crt_sort[] = {
	{0x01011d90u, m_qsort, "qsort"},
};
const unsigned xrc_manual_crt_sort_n = sizeof xrc_manual_crt_sort / sizeof xrc_manual_crt_sort[0];
