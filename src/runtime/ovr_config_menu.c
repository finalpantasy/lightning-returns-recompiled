/* ovr_config_menu.c - a "PC Graphics..." row in the game's own Settings menu (title menu and pause menu), which opens
 * the runtime's settings overlay. "fast" overrides (overrides.tsv): the translations call xo_<addr> directly, the
 * dispatcher maps the address to it too (xrc_manual_simd in ovr_simd.c).
 *
 * The game's menu (GuiMainConfigMenu, code 00806000-00810000) is a list of rows, one per option id (0..28). The list
 * object (GuiMainConfigMenu+0x8c, +0xc4) holds the ids at +0, the current choice of each row at +0x74 (29 slots each)
 * and the row count at +0xe8. A row's label is the text key $f_con_<n> (n = the label table 020c9668[id]), its help
 * text $f_con_<n>h, the value shown $f_con_<n+1+choice>. Confirm on a row (0080d0a0) opens that option's panel.
 *
 * The new row uses id 22: it has a label number (34) but the game never lists it, and its get (007bfec0) answers 1
 * and its set (007c0110) writes nothing, so the menu's commit loop (0080d4a0) finds it unchanged and the saves stay
 * untouched. Three overrides:
 *   00809600  the list builder: the translation, then one row (id 22, choice = get(22)) appended if there is room
 *   0080d0a0  confirm on a row: on the id-22 row it plays the confirm sound and calls xrc_settings_open() instead of
 *             opening a panel; every other row runs the translation
 *   007a4550  the text lookup by key (this = [050f19f4], cdecl-like thiscall, ret 4: char * of a "$..." key): our
 *             strings for $f_con_34 (label), $f_con_34h (help), $f_con_35 / $f_con_36 (value); others the translation
 * XRC_NO_CFG_ROW=1: no row (the overrides run the translations only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"
#include "guest_types.h"

#define CFG_ID       22u
#define FN_ROWS      0x00809600u
#define FN_CONFIRM   0x0080d0a0u
#define FN_TEXT      0x007a4550u
#define FN_GET       0x007bfec0u /* cdecl int get(id) */
#define FN_SOUND     0x007a6d50u /* thiscall ([050f19f4]) play_se(n) */
#define FN_GUI_ALLOC 0x007b6b70u /* cdecl void *gui_alloc(size) (freed by GuiObject::Free 007b6a70) */
#define G_UI         G_GUIMGR
#define SE_CONFIRM   4u

/* the runtime's settings overlay (xrc_menu.c); this weak stub until it exists */
__attribute__((weak)) void xrc_settings_open(void) {
	fprintf(stderr, "xrc: config menu: xrc_settings_open is a stub (no overlay linked)\n");
}

static int off(void) {
	static int v = -1;
	if (v < 0) v = getenv("XRC_NO_CFG_ROW") != 0;
	return v;
}

void XRC_CC xo_00809600(XCtx *c) {
	static XFn orig;
	u32 list = c->ecx, n, k;
	if (!orig) orig = xrc_translation_of(FN_ROWS);
	orig(c);
	if (off()) return;
	n = GF(ConfigRowList, list, count);
	if (n >= CFG_ROWS_MAX) return;
	for (k = 0; k < n; k++) if (GFI(ConfigRowList, list, ids, k) == CFG_ID) return;
	{
		u32 a[1] = {CFG_ID}, eax = c->eax, ecx = c->ecx, edx = c->edx, eip = c->eip, v;
		v = xrc_call_guest(c, FN_GET, 1, a);
		c->eax = eax; c->ecx = ecx; c->edx = edx; c->eip = eip; /* (the call leaves eip at its return magic) */
		GFI(ConfigRowList, list, ids, n) = CFG_ID;
		GFI(ConfigRowList, list, choice, n) = v;
		GF(ConfigRowList, list, count) = n + 1;
	}
}

void XRC_CC xo_0080d0a0(XCtx *c) {
	static XFn orig;
	u32 t = c->ecx, l, sel;
	if (!orig) orig = xrc_translation_of(FN_CONFIRM);
	if (!off() && !GF(ConfigMenu, t, unk_a8) && !GF(ConfigMenu, t, unk_a0) && !GF(ConfigMenu, t, unk_b4) && (l = GF(ConfigMenu, t, list)) != 0 && !GF(ConfigListObj, l, unk_1c8)) {
		sel = GF(ConfigListObj, l, selected_row);
		if (sel < GF(ConfigListObj, l, rows.count) && GFI(ConfigListObj, l, rows.ids, sel) == CFG_ID) {
			u32 a[1] = {SE_CONFIRM};
			fprintf(stderr, "xrc: config menu: PC Graphics row confirmed\n");
			c->ecx = M32(G_UI);
			if (c->ecx) xrc_call_guest(c, FN_SOUND, 1, a);
			xrc_settings_open();
			xrc_cdecl_ret(c);
			return;
		}
	}
	orig(c);
}

/* a copy of s in the GUI's heap: the lookup's result belongs to its caller, which frees it (007bb4f0: GuiString
 * keeps it with its owned flag; GuiObject::Free 007b6a70) */
static u32 gstr(XCtx *c, const char *s) {
	u32 n = (u32)strlen(s) + 1, a[1] = {n}, eip = c->eip, ecx = c->ecx, edx = c->edx, p;
	p = xrc_call_guest(c, FN_GUI_ALLOC, 1, a);
	c->eip = eip; c->ecx = ecx; c->edx = edx;
	if (p) memcpy(xrc_mem + p, s, n);
	return p;
}

void XRC_CC xo_007a4550(XCtx *c) {
	static XFn orig;
	u32 key = arg_u32(c, 1);
	if (!orig) orig = xrc_translation_of(FN_TEXT);
	if (key && !off() && M8(key) == '$' && M8(key + 1) == 'f' && M8(key + 6) == '_' && M8(key + 7) == '3') {
		const char *k = (const char *)(xrc_mem + key);
		u32 r = 0;
		if (!strcmp(k, "$f_con_34")) r = gstr(c, "PC Graphics...");
		else if (!strcmp(k, "$f_con_34h")) r = gstr(c, "Display, graphics and enhancement options. Opens the PC settings panel.");
		else if (!strcmp(k, "$f_con_35") || !strcmp(k, "$f_con_36")) r = gstr(c, "Open");
		if (r) {
			c->eax = r;
			xrc_stdcall_ret(c, 1);
			return;
		}
	}
	orig(c);
}
