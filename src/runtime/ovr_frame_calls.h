/* ovr_frame_calls.h - compiled into pages 00ab and 00ac (clang -include): its direct calls of 00a96590 / 00ac33b0 go to ovr_frame.c's
 * xo_ functions (a translation's direct call skips the dispatcher's manual overrides). */
#ifndef OVR_FRAME_CALLS_H
#define OVR_FRAME_CALLS_H
#include "xrc.h"
void XRC_CC xo_00a96590(XCtx *c);
void XRC_CC xo_00ac33b0(XCtx *c);
#define XRC_FRAME_FN_(tgt, fn) ((tgt) == 0xa96590u ? xo_00a96590 : (tgt) == 0xac33b0u ? xo_00ac33b0 : (fn))
#undef XCALLD
#define XCALLD(tgt, fn) { r_esp -= 4; M32(r_esp) = x_ra_; XRC_SPILL XRC_PREEMPT() XRC_DS_PUSH(tgt) XRC_FRAME_FN_(tgt, fn)(c); XRC_DS_POP() \
	if (c->eip != x_ra_) xrc_bad_return(c, (tgt)); XRC_EBPCK(tgt) XRC_FILL }
#endif
