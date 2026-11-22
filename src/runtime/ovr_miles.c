/* Miles Sound System code the translator leaves out (it is in the coverage report as "override"):
 *   00c18810  a double buffer's read (stdcall: this, dst, n). Its spin wait uses pause (f3 90), which ff xrc reads as
 *             rep xchg and does not translate. Reached only through three function tables (Miles' stream drivers), so a
 *             manual override in the dispatcher covers every call.
 * (The Miles CPU probe 00de8000 is in hle_lr.c.) */
#include <string.h>
#include "xrc_rt.h"

/* this+0x18 == 1: a new half is ready, so swap: the shared pointer (this+8) goes from the half being read to the other
 * one, unless the writer holds it (-1: spin, letting the other guest threads run); then this+0x10 ^= 1, this+0x18 = 0.
 * Then memcpy(dst, base + (this+0x10 ^ 1) * half size, n), returning dst (eax) like the original. */
static void XRC_CC m_mss_dbuf_read(XCtx *c) {
	u32 s = xrc_arg(c, 0), dst = xrc_arg(c, 1), n = xrc_arg(c, 2);
	if (M32(s + 0x18) == 1) {
		for (;;) {
			u32 cur = M32(s + 0x10), sz = M32(s + 0x14), base = M32(s + 4);
			u32 cmp = cur * sz + base, xchg = (cur ^ 1) * sz + base, old = M32(s + 8);
			if (old == cmp) M32(s + 8) = xchg; /* (InterlockedCompareExchange: atomic under the GIL) */
			if (old != 0xffffffffu) break;
			xrc_yield();
		}
		M32(s + 0x10) ^= 1;
		M32(s + 0x18) = 0;
	}
	u32 src = (M32(s + 0x10) ^ 1) * M32(s + 0x14) + M32(s + 4);
	memmove(xrc_mem + dst, xrc_mem + src, n);
	c->eax = dst;
	xrc_stdcall_ret(c, 3);
}

const XrcManual xrc_manual_miles[] = {
	{0x00c18810u, m_mss_dbuf_read, "Miles double buffer read (pause spin: not translated)"},
};
const unsigned xrc_manual_miles_n = sizeof xrc_manual_miles / sizeof xrc_manual_miles[0];
