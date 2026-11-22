/* ovr_fast_calls.h - until the next ff xrc regeneration: compiled into a page (clang -include) it makes that page's
 * direct calls of the newest "fast" overrides (overrides.tsv) go to xo_<addr>, as the regenerated page will. Only the
 * pages that call them need it (build/win/pages_cfgrow.sh: page_0079, 007b, 0080, 009e); every other call is as
 * before (the target is a constant, so the choice folds away). Remove after the regeneration. */
#ifndef OVR_FAST_CALLS_H
#define OVR_FAST_CALLS_H
#include "xrc.h"
void XRC_CC xo_00809600(XCtx *c);
void XRC_CC xo_0080d0a0(XCtx *c);
void XRC_CC xo_007a4550(XCtx *c);
#define XRC_FAST_FN_(tgt, fn) ((tgt) == 0x809600u ? xo_00809600 : (tgt) == 0x80d0a0u ? xo_0080d0a0 : \
	(tgt) == 0x7a4550u ? xo_007a4550 : (fn))
#undef XCALLD
#define XCALLD(tgt, fn) { r_esp -= 4; M32(r_esp) = x_ra_; XRC_SPILL XRC_PREEMPT() XRC_DS_PUSH(tgt) XRC_FAST_FN_(tgt, fn)(c); XRC_DS_POP() \
	if (c->eip != x_ra_) xrc_bad_return(c, (tgt)); XRC_EBPCK(tgt) XRC_FILL }
#endif
