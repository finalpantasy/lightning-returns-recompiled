/* ovr_hooks.c - the dispatcher's entries for the `fast` / `wrapped` rows of overrides.tsv that have no table of their own,
 * and the shadow stability hold as a wrapped override of 00a6ab50 .
 * Translator: ps4star's XRC (public, Unlicense).
 *
 * Shadow stability (XRC_SHADOW_HOLD, menu row "Shadow stability"; vk_d3d9.c xrc_shadow_hold): the visible-object gather 00a6c5b0
 * tests each object's AABB against the camera frustum (AabbViewTest 00a6ab50, called at 00a6c5db with the object in ebx and at
 * 00a6c6ab with the object in edi). An object that tested visible within the last N frames is kept even when a later test culls it.
 * The wrapper reads the key (the object register) before the translation runs, runs it, and passes the result through the hold.
 * Registers are read from XCtx: at a call the pages keep XCtx current for every register (a trimmed spill only skips ones XCtx
 * already holds). Other callers of 00a6ab50 (none known) pass through unchanged, as the old patch only touched the two sites. */
#include "xrc_rt.h"

unsigned xrc_shadow_hold(unsigned site, unsigned key, unsigned res); /* vk_d3d9.c */

void XRC_CC xo_00a6ab50(XCtx *c) {
	static XFn orig;
	u32 ra = M32(c->esp), key = 0, site = 0;
	if (!orig) orig = xrc_translation_of(0x00a6ab50u);
	if (ra == 0x00a6c5e0u) { site = 1; key = c->ebx; }
	else if (ra == 0x00a6c6b0u) { site = 2; key = c->edi; }
	orig(c);
	if (site) c->eax = xrc_shadow_hold(site, key, c->eax);
}

void XRC_CC xo_00809600(XCtx *c); /* ovr_config_menu.c */
void XRC_CC xo_0080d0a0(XCtx *c);
void XRC_CC xo_007a4550(XCtx *c);
void XRC_CC xo_00744820(XCtx *c); /* ovr_input.c */
const XrcManual xrc_manual_hooks[] = {
	{0x00a6ab50u, xo_00a6ab50, "AabbViewTest: shadow stability hold at the gather's two call sites (ovr_hooks.c)"},
	{0x00809600u, xo_00809600, "the Settings menu's row list: + PC Graphics (ovr_config_menu.c)"},
	{0x0080d0a0u, xo_0080d0a0, "the Settings menu's confirm: PC Graphics opens the overlay (ovr_config_menu.c)"},
	{0x007a4550u, xo_007a4550, "the UI text lookup: the PC Graphics row's strings (ovr_config_menu.c)"},
	{0x00744820u, xo_00744820, "IsXInputDevice: true for the runtime's XInput pads (ovr_input.c)"},
};
const unsigned xrc_manual_hooks_n = sizeof xrc_manual_hooks / sizeof xrc_manual_hooks[0];
