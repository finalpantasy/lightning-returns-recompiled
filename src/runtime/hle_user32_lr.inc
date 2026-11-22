/* ============================================================================================ LR (XRC_LR)
 * USER32 functions Lightning Returns adds: menus (kept, not drawn), window properties, timers, thread messages and
 * GetMessageA (a worker thread's message loop), focus / foreground, the keyboard layout, system metrics. */
#ifdef XRC_LR
/* menus: items with an id, type, state, submenu, data and text (nothing is drawn). MENUITEMINFO (A and W alike up to
 * the text): cbSize 0, fMask 4, fType 8, fState 12, wID 16, hSubMenu 20, hbmpChecked 24, hbmpUnchecked 28,
 * dwItemData 32, dwTypeData 36, cch 40, hbmpItem 44 */
typedef struct { u32 id, type, state, sub, data, bmp; char text[64]; } MItem;
#define NMITEM 48
typedef struct { int used, n; MItem it[NMITEM]; } Menu;
#define NMENU 64
#define MENU_BASE 0x00020000u
#define MIIM_STATE 0x1u
#define MIIM_ID 0x2u
#define MIIM_SUBMENU 0x4u
#define MIIM_TYPE 0x10u
#define MIIM_DATA 0x20u
#define MIIM_STRING 0x40u
#define MIIM_BITMAP 0x80u
#define MIIM_FTYPE 0x100u
#define MF_BYPOSITION 0x400u
static Menu menus[NMENU];
static Menu *menu(u32 h) {
	u32 i;
	if (h < MENU_BASE || (h - MENU_BASE) % 4u) return 0;
	i = (h - MENU_BASE) / 4u;
	return i < NMENU && menus[i].used ? &menus[i] : 0;
}
/* the item `item` (a position, or a command id) of menu h; its menu in *mo (an id is searched in submenus too) */
static int menu_item(u32 h, u32 item, int by_pos, Menu **mo) {
	Menu *m = menu(h);
	int k;
	if (!m) return -1;
	if (by_pos) { *mo = m; return item < (u32)m->n ? (int)item : -1; }
	for (k = 0; k < m->n; k++)
		if (!m->it[k].sub && m->it[k].id == item) { *mo = m; return k; }
	for (k = 0; k < m->n; k++)
		if (m->it[k].sub) { int r = menu_item(m->it[k].sub, item, 0, mo); if (r >= 0) return r; }
	return -1;
}
static void menu_free(u32 h) {
	Menu *m = menu(h);
	int k;
	if (!m) return;
	m->used = 0;
	for (k = 0; k < m->n; k++) if (m->it[k].sub) menu_free(m->it[k].sub);
}
static void hle_CreateMenu(XCtx *c) {
	int i;
	for (i = 0; i < NMENU; i++)
		if (!menus[i].used) { memset(&menus[i], 0, sizeof menus[i]); menus[i].used = 1; RET(MENU_BASE + 4u * (u32)i, 0); }
	RET(0, 0);
}
static void hle_DestroyMenu(XCtx *c) { int ok = menu(A(0)) != 0; menu_free(A(0)); RET(ok, 1); }
/* DeleteMenu(menu, item, flags): the item goes, and its submenu is destroyed */
static void hle_DeleteMenu(XCtx *c) {
	Menu *m = 0;
	int k = menu_item(A(0), A(1), (A(2) & MF_BYPOSITION) != 0, &m);
	if (k < 0) { set_err(1456 /* ERROR_MENU_ITEM_NOT_FOUND */); RET(0, 3); }
	if (m->it[k].sub) menu_free(m->it[k].sub);
	memmove(&m->it[k], &m->it[k + 1], (size_t)(m->n - k - 1) * sizeof m->it[0]);
	m->n--;
	RET(1, 3);
}
static void hle_DrawMenuBar(XCtx *c) { RET(wnd(A(0)) != 0, 1); }
static void hle_SetMenu(XCtx *c) { Wnd *w = wnd(A(0)); if (w) w->menu = A(1); RET(w != 0, 2); }
static void hle_GetMenuItemCount(XCtx *c) { Menu *m = menu(A(0)); if (!m) set_err(1401 /* ERROR_INVALID_MENU_HANDLE */); RET(m ? (u32)m->n : 0xffffffffu, 1); }
static void mii_set(MItem *it, u32 mii, int wide) {
	u32 mask = M32(mii + 4);
	if (mask & MIIM_STATE) it->state = M32(mii + 12);
	if (mask & MIIM_ID) it->id = M32(mii + 16);
	if (mask & MIIM_SUBMENU) it->sub = M32(mii + 20);
	if (mask & MIIM_DATA) it->data = M32(mii + 32);
	if (mask & MIIM_BITMAP) it->bmp = M32(mii + 44);
	if (mask & (MIIM_TYPE | MIIM_FTYPE)) it->type = M32(mii + 8);
	if (getenv("XRC_MENU_DUMP")) {
		u32 s = M32(mii + 36);
		fprintf(stderr, "xrc: mii %s mask %x type %x id %u text %08x cch %u: %04x %04x %04x %04x\n", wide ? "W" : "A", mask, M32(mii + 8),
		        M32(mii + 16), s, M32(mii + 40), s ? M16(s) : 0, s ? M16(s + 2) : 0, s ? M16(s + 4) : 0, s ? M16(s + 6) : 0);
	}
	if ((mask & MIIM_STRING) || ((mask & MIIM_TYPE) && !(it->type & 0x804u /* MFT_BITMAP | MFT_SEPARATOR */))) {
		u32 s = M32(mii + 36);
		size_t k = 0;
		if (s) for (; k + 1 < sizeof it->text && (wide ? M16(s + 2u * (u32)k) : M8(s + (u32)k)); k++)
			it->text[k] = (char)(wide ? M16(s + 2u * (u32)k) : M8(s + (u32)k));
		it->text[k] = 0;
	}
}
/* GetMenuItemInfoA(menu, item, by_position, MENUITEMINFOA *) */
static void hle_GetMenuItemInfoA(XCtx *c) {
	Menu *m = 0;
	u32 mii = A(3), mask = M32(mii + 4);
	int k = menu_item(A(0), A(1), A(2) != 0, &m);
	MItem *it;
	if (k < 0) { set_err(1456); RET(0, 4); }
	it = &m->it[k];
	if (mask & MIIM_STATE) M32(mii + 12) = it->state;
	if (mask & MIIM_ID) M32(mii + 16) = it->id;
	if (mask & MIIM_SUBMENU) M32(mii + 20) = it->sub;
	if (mask & MIIM_DATA) M32(mii + 32) = it->data;
	if (mask & MIIM_BITMAP) M32(mii + 44) = it->bmp;
	if (mask & (MIIM_TYPE | MIIM_FTYPE)) M32(mii + 8) = it->type;
	if (mask & (MIIM_STRING | MIIM_TYPE)) {
		u32 s = M32(mii + 36), cap = M32(mii + 40), n = (u32)strlen(it->text);
		if (s && cap) { u32 w = n < cap - 1 ? n : cap - 1; memcpy(xrc_mem + s, it->text, w); M8(s + w) = 0; }
		M32(mii + 40) = n;
	}
	RET(1, 4);
}
static void hle_SetMenuItemInfoA(XCtx *c) {
	Menu *m = 0;
	int k = menu_item(A(0), A(1), A(2) != 0, &m);
	if (k < 0) { set_err(1456); RET(0, 4); }
	mii_set(&m->it[k], A(3), 0);
	RET(1, 4);
}
/* InsertMenuItemW(menu, item, by_position, MENUITEMINFOW *): before that item, or at the end */
static void hle_InsertMenuItemW(XCtx *c) {
	Menu *m = menu(A(0)), *mm = 0;
	int k;
	if (!m) { set_err(1401); RET(0, 4); }
	if (m->n == NMITEM) xrc_fatal(c, "InsertMenuItemW: more than %d items in one menu", NMITEM);
	k = menu_item(A(0), A(1), A(2) != 0, &mm);
	if (k < 0 || mm != m) k = m->n;
	memmove(&m->it[k + 1], &m->it[k], (size_t)(m->n - k) * sizeof m->it[0]);
	memset(&m->it[k], 0, sizeof m->it[0]);
	mii_set(&m->it[k], A(3), 1);
	m->n++;
	RET(1, 4);
}

/* The game's menu bar (Graphics: presentation, resolution, quality, frame rate; Control; File: Quit, Reboot), shown
 * for real: the window thread (vk_d3d9.c, F10) asks for a host popup menu made from it, and the item picked goes to
 * the game as WM_COMMAND, from the window's thread on its next PeekMessage. The menu is read from that thread as is
 * (the game only changes it when a setting changes, from the same message handler). */
#define MFT_SEPARATOR_ 0x800u
static volatile int menu_cmd;
static u32 menu_owner(void) {
	int i;
	for (i = 0; i < NWND; i++) if (wnds[i].used && menu(wnds[i].menu)) return hwnd_of(&wnds[i]);
	return 0;
}
/* the menu tree for the window's thread: cb(ctx, kind, id, state, text) with kind 0 an item, 1 a submenu (its items
 * follow, then kind 2), 3 a separator; state: the MFS_ bits (8 checked, 3 grayed). 0: the game has no menu (yet) */
typedef void (*XrcMenuCb)(void *ctx, int kind, unsigned id, unsigned state, const char *text);
static void menu_walk_(u32 h, int depth, XrcMenuCb cb, void *ctx) {
	Menu *m = menu(h);
	int k;
	if (!m || depth > 4) return;
	for (k = 0; k < m->n; k++) {
		MItem it = m->it[k];
		const char *t = it.text[0] ? it.text : "?";
		if (it.type & MFT_SEPARATOR_) cb(ctx, 3, 0, 0, "");
		else if (it.sub) { cb(ctx, 1, 0, it.state, t); menu_walk_(it.sub, depth + 1, cb, ctx); cb(ctx, 2, 0, 0, ""); }
		else cb(ctx, 0, it.id, it.state, t);
	}
}
int xrc_menu_walk(XrcMenuCb cb, void *ctx) {
	u32 o = menu_owner();
	if (!o) return 0;
	menu_walk_(wnd(o)->menu, 0, cb, ctx);
	return 1;
}
void xrc_menu_pick(unsigned id) { __atomic_store_n(&menu_cmd, (int)id, __ATOMIC_SEQ_CST); }
static void menu_dump(u32 h, int depth) {
	Menu *m = menu(h);
	int k;
	if (!m || depth > 4) return;
	for (k = 0; k < m->n; k++)
		fprintf(stderr, "xrc: menu %*s%s id %u type %x state %x%s\n", depth * 2, "", m->it[k].text, m->it[k].id, m->it[k].type,
		        m->it[k].state, m->it[k].sub ? " >" : ""), menu_dump(m->it[k].sub, depth + 1);
}
/* (from the window's thread) a picked item as WM_COMMAND; XRC_MENU_DUMP=1 logs the menu once it is there,
 * XRC_MENU_PICK=secs:id picks an item at that time (a test without a window) */
static void menu_poll(void) {
	static int dumped, pick_id = -1;
	static double pick_t;
	int id;
	u32 o;
	if (pick_id < 0) {
		const char *e = getenv("XRC_MENU_PICK");
		pick_id = 0;
		if (e && strchr(e, ':')) { pick_t = atof(e) + xrc_now_us() / 1e6; pick_id = atoi(strchr(e, ':') + 1); }
	}
	if (!(o = menu_owner())) return;
	if (!dumped && getenv("XRC_MENU_DUMP")) { dumped = 1; menu_dump(wnd(o)->menu, 0); }
	if (pick_id > 0 && xrc_now_us() / 1e6 >= pick_t) {
		fprintf(stderr, "xrc: menu: test pick of item %d\n", pick_id);
		xrc_menu_pick((unsigned)pick_id);
		pick_id = 0;
	}
	if ((id = __atomic_exchange_n(&menu_cmd, 0, __ATOMIC_SEQ_CST)) != 0) {
		fprintf(stderr, "xrc: menu: item %d picked\n", id);
		if (id == 1) xrc_relaunch_at_exit(); /* File > Reboot: the game saves its settings and exits */
		post(o, 0x0111 /* WM_COMMAND */, (u32)id & 0xffffu, 0);
	}
}

/* CallWindowProcA(prev, hwnd, msg, wp, lp) */
static void hle_CallWindowProcA(XCtx *c) {
	u32 a[4] = {A(1), A(2), A(3), A(4)};
	RET(A(0) ? xrc_call_guest(c, A(0), 4, a) : 0, 5);
}
static void hle_FindWindowA(XCtx *c) { RET(0, 2); } /* no other copy of the game is running */
static void hle_GetFocus(XCtx *c) { RET(focus_wnd, 0); }
static void hle_GetForegroundWindow(XCtx *c) { RET(active_wnd, 0); }
static void hle_SetActiveWindow(XCtx *c) { u32 prev = active_wnd; if (wnd(A(0))) active_wnd = A(0); RET(prev, 1); }
static void hle_SetForegroundWindow(XCtx *c) { if (wnd(A(0))) active_wnd = A(0); RET(wnd(A(0)) != 0, 1); }
static void hle_GetKeyboardLayout(XCtx *c) { RET(0x04090409u, 1); } /* US English */
static void hle_GetKeyboardLayoutNameA(XCtx *c) { strcpy(S(A(0)), "00000409"); RET(1, 1); }

/* GetSystemMetrics: the display the game draws to (XRC_WIDTH x XRC_HEIGHT), one monitor, standard frame sizes */
static void hle_GetSystemMetrics(XCtx *c) {
	u32 v = 0;
	switch (A(0)) {
	case 0: case 16: case 78: v = (u32)disp_w(); break; /* SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN */
	case 1: case 79: v = (u32)disp_h(); break;          /* SM_CYSCREEN, SM_CYVIRTUALSCREEN */
	case 17: v = (u32)disp_h() - 23; break;              /* SM_CYFULLSCREEN */
	case 4: v = 23; break;                               /* SM_CYCAPTION */
	case 5: case 6: v = 1; break;                        /* SM_CXBORDER, SM_CYBORDER */
	case 7: case 8: v = 3; break;                        /* SM_CXFIXEDFRAME, SM_CYFIXEDFRAME */
	case 32: case 33: v = 8; break;                      /* SM_CXSIZEFRAME, SM_CYSIZEFRAME */
	case 15: v = 20; break;                              /* SM_CYMENU */
	case 43: v = 3; break;                               /* SM_CMOUSEBUTTONS */
	case 19: v = 1; break;                               /* SM_MOUSEPRESENT */
	case 80: v = 1; break;                               /* SM_CMONITORS */
	case 81: v = 1; break;                               /* SM_SAMEDISPLAYFORMAT */
	}
	RET(v, 1);
}

/* window properties (SetPropA's name: a string, or an atom in the low word) */
typedef struct { int used; u32 hwnd, atom, data; char name[48]; } Prop;
#define NPROP 64
static Prop props[NPROP];
static Prop *prop(u32 h, u32 name, int make) {
	int k;
	for (k = 0; k < NPROP; k++)
		if (props[k].used && props[k].hwnd == h &&
		    (name < 0x10000u ? props[k].atom == name : (!props[k].atom && xrc_ieq(props[k].name, S(name)))))
			return &props[k];
	if (!make) return 0;
	for (k = 0; k < NPROP; k++)
		if (!props[k].used) {
			Prop *p = &props[k];
			memset(p, 0, sizeof *p);
			p->used = 1; p->hwnd = h;
			if (name < 0x10000u) p->atom = name; else snprintf(p->name, sizeof p->name, "%s", S(name));
			return p;
		}
	return 0;
}
static void hle_SetPropA(XCtx *c) { Prop *p = prop(A(0), A(1), 1); if (p) p->data = A(2); RET(p != 0, 3); }
static void hle_GetPropA(XCtx *c) { Prop *p = prop(A(0), A(1), 0); RET(p ? p->data : 0, 2); }
static void hle_RemovePropA(XCtx *c) { Prop *p = prop(A(0), A(1), 0); u32 d = p ? p->data : 0; if (p) p->used = 0; RET(d, 2); }

/* thread messages (PostThreadMessageA: hwnd 0, for one thread's GetMessage / PeekMessage) and timers */
typedef struct { u32 tid; Msg m; } TMsg;
#define NTQ 256
static TMsg tq[NTQ];
static int tq_n;
typedef struct { int used; u32 hwnd, id, ms, proc, tid; unsigned long long due; } Timer;
#define NTIMER 32
static Timer timers[NTIMER];
#define WM_TIMER 0x0113u

static void hle_PostThreadMessageA(XCtx *c) {
	if (tq_n == NTQ) RET(0, 4);
	tq[tq_n].tid = A(0);
	tq[tq_n].m.hwnd = 0; tq[tq_n].m.msg = A(1); tq[tq_n].m.wp = A(2); tq[tq_n].m.lp = A(3);
	tq_n++;
	RET(1, 4);
}
static int lr_thread_peek(XCtx *c, u32 p, u32 h, u32 lo, u32 hi, int rm) {
	u32 tid = xrc_thread_id();
	unsigned long long now;
	int k;
	(void)c;
	if (!h || h == 0xffffffffu)
		for (k = 0; k < tq_n; k++) {
			if (tq[k].tid != tid || !msg_match(&tq[k].m, 0, lo, hi)) continue;
			fill_msg(p, &tq[k].m);
			if (rm) { memmove(&tq[k], &tq[k + 1], (size_t)(tq_n - k - 1) * sizeof tq[0]); tq_n--; }
			return 1;
		}
	if ((lo || hi) && (WM_TIMER < lo || WM_TIMER > hi)) return 0;
	now = xrc_now_us();
	for (k = 0; k < NTIMER; k++) {
		Timer *t = &timers[k];
		Msg m;
		if (!t->used || t->tid != tid || t->due > now) continue;
		if (h && h != 0xffffffffu && h != t->hwnd) continue;
		m.hwnd = t->hwnd; m.msg = WM_TIMER; m.wp = t->id; m.lp = t->proc;
		fill_msg(p, &m);
		if (rm) t->due = now + 1000ull * t->ms;
		return 1;
	}
	return 0;
}
/* SetTimer(hwnd, id, ms, proc): with no window the id is the system's */
static void hle_SetTimer(XCtx *c) {
	u32 h = A(0), id = A(1), ms = A(2) < 10 ? 10 : A(2);
	int k, free_k = -1;
	for (k = 0; k < NTIMER; k++) {
		if (!timers[k].used) { if (free_k < 0) free_k = k; continue; }
		if (h && timers[k].hwnd == h && timers[k].id == id) { free_k = k; break; }
	}
	if (free_k < 0) RET(0, 4);
	timers[free_k].used = 1;
	timers[free_k].hwnd = h;
	timers[free_k].id = h ? id : 0x7fff0000u + (u32)free_k;
	timers[free_k].ms = ms;
	timers[free_k].proc = A(3);
	timers[free_k].tid = xrc_thread_id();
	timers[free_k].due = xrc_now_us() + 1000ull * ms;
	RET(h ? (id ? id : 1) : timers[free_k].id, 4);
}
static void hle_KillTimer(XCtx *c) {
	int k;
	for (k = 0; k < NTIMER; k++)
		if (timers[k].used && timers[k].hwnd == A(0) && timers[k].id == A(1)) { timers[k].used = 0; RET(1, 2); }
	RET(0, 2);
}
/* GetMessageA(msg, hwnd, min, max): waits; 0 for WM_QUIT */
static void hle_GetMessageA(XCtx *c) {
	u32 p = A(0), h = A(1), lo = A(2), hi = A(3);
	for (;;) {
		if (peek(c, p, h, lo, hi, 1)) RET(M32(p + 4) != WM_QUIT, 4);
		xrc_sleep(1);
	}
}
#endif
