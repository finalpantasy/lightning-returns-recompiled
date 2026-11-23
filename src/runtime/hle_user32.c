/* hle_user32.c -- USER32 and GDI32: windows, the message queue, the display, the cursor; headless for now.
 *
 * Windows are runtime objects (HWND = WND_BASE + 4 * index) with their class's window procedure, styles, rectangles,
 * window longs and text. Messages the game sends or the runtime generates (WM_NCCREATE, WM_CREATE, WM_SIZE,
 * WM_ACTIVATE ...) call the guest window procedure through xrc_call_guest; posted messages wait in one queue
 * (single thread) for PeekMessageA. There is no host window yet: the display is a fixed mode (XRC_WIDTH x XRC_HEIGHT,
 * default 1280 x 720) and input is idle; a later SDL backend feeds the same queue. GDI drawing is accepted and
 * dropped. The reference targets give each top-level window a real one (ref_user32.c, through the ref_u32_ hooks). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"
#include "guest_types.h"

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define S(a) ((char *)(xrc_mem + (u32)(a)))

static void set_err(u32 e) { GF(Teb, xrc_teb, last_error) = e; }
#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_CLASS_DOES_NOT_EXIST 1411u

/* messages */
#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_PAINT 0x000fu
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ERASEBKGND 0x0014u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001cu
#define WM_SETCURSOR 0x0020u
#define WM_GETMINMAXINFO 0x0024u
#define WM_WINDOWPOSCHANGING 0x0046u
#define WM_WINDOWPOSCHANGED 0x0047u
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_NCCALCSIZE 0x0083u
#define WM_NCACTIVATE 0x0086u
#define WM_SYSCOMMAND 0x0112u

/* window styles */
#define WS_CAPTION 0x00c00000u
#define WS_BORDER 0x00800000u
#define WS_THICKFRAME 0x00040000u
#define WS_VISIBLE 0x10000000u
#define WS_CHILD 0x40000000u

/* hires2: the display is asked of xrc_rt.c (the monitor / window the game is on, physical pixels) */
void xrc_disp_size(int *w, int *h);
int xrc_mode_list(int idx, int *w, int *h);
static int disp_w(void) { int w = 0, h = 0; xrc_disp_size(&w, &h); return w; }
static int disp_h(void) { int w = 0, h = 0; xrc_disp_size(&w, &h); return h; }
static int trace_msgs = -1;
static int tracing(void) {
	if (trace_msgs < 0) trace_msgs = xrc_tool_env("XRC_TRACE_MSGS") != 0;
	return trace_msgs;
}

/* ============================================================================================ classes */
typedef struct { char name[64]; u32 atom, wndproc, style, extra, icon, cursor, brush; } WClass;
#define NCLS 64
static WClass classes[NCLS];
static int ncls;
static void builtin_classes(void) {
	static const char *const sys[] = {"STATIC", "BUTTON", "EDIT", "LISTBOX", "COMBOBOX", "SCROLLBAR"};
	unsigned k;
	if (ncls) return;
	for (k = 0; k < sizeof sys / sizeof sys[0]; k++) { /* no window procedure: DefWindowProc behaviour */
		WClass *w = &classes[ncls++];
		memset(w, 0, sizeof *w);
		snprintf(w->name, sizeof w->name, "%s", sys[k]);
		w->atom = 0x8000u + k;
	}
}
static WClass *find_class(u32 name) {
	int k;
	builtin_classes();
	for (k = 0; k < ncls; k++) {
		if (name < 0x10000u ? classes[k].atom == name : xrc_ieq(classes[k].name, S(name))) return &classes[k];
	}
	return 0;
}
/* RegisterClassExA(WNDCLASSEXA *): cbSize style lpfnWndProc cbClsExtra cbWndExtra hInstance hIcon hCursor
 * hbrBackground lpszMenuName lpszClassName hIconSm */
static void hle_RegisterClassExA(XCtx *c) {
	u32 w = A(0), nm = GF(WndClassEx, w, class_name);
	WClass *k;
	builtin_classes();
	if (ncls == NCLS) RET(0, 1);
	k = &classes[ncls++];
	memset(k, 0, sizeof *k);
	if (nm < 0x10000u) snprintf(k->name, sizeof k->name, "#%u", nm);
	else snprintf(k->name, sizeof k->name, "%s", S(nm));
	k->atom = 0xc000u + (u32)ncls;
	k->style = GF(WndClassEx, w, style);
	k->wndproc = GF(WndClassEx, w, wndproc);
	k->extra = GF(WndClassEx, w, wnd_extra);
	k->icon = GF(WndClassEx, w, icon);
	k->cursor = GF(WndClassEx, w, cursor);
	k->brush = GF(WndClassEx, w, background);
	if (tracing()) fprintf(stderr, "xrc: RegisterClassExA(%s, wndproc %08x)\n", k->name, k->wndproc);
	RET(k->atom, 1);
}
static void hle_SetClassLongA(XCtx *c) {
	RET(0, 3); /* GCL_HICON / GCL_HCURSOR ...: nothing to show them on */
}

/* ============================================================================================ windows */
typedef struct {
	int used;
	WClass *cls;
	u32 wndproc, style, exstyle, parent, menu, inst, userdata, id;
	int x, y, w, h; /* the window rectangle (screen) */
	int visible, iconic;
	char text[128];
	u32 extra[16];
} Wnd;
#define NWND 64
#define WND_BASE 0x00010000u
static Wnd wnds[NWND];
static u32 active_wnd, focus_wnd, capture_wnd;

static Wnd *wnd(u32 h) {
	u32 i;
	if (h < WND_BASE || (h - WND_BASE) % 4u) return 0;
	i = (h - WND_BASE) / 4u;
	return i < NWND && wnds[i].used ? &wnds[i] : 0;
}
static u32 hwnd_of(Wnd *w) { return WND_BASE + 4u * (u32)(w - wnds); }

/* the frame the styles add around the client area */
static void frame(u32 style, u32 exstyle, int *l, int *t, int *r, int *b) {
	int f = 0, cap = 0;
	(void)exstyle;
	if (style & WS_THICKFRAME) f = 8;
	else if (style & (WS_BORDER | WS_CAPTION)) f = 3;
	if ((style & WS_CAPTION) == WS_CAPTION) cap = 23;
	*l = f; *r = f; *t = f + cap; *b = f;
}
static void client_size(Wnd *w, int *cw, int *ch) {
	int l, t, r, b;
	frame(w->style, w->exstyle, &l, &t, &r, &b);
	*cw = w->w - l - r; *ch = w->h - t - b;
	if (*cw < 0) *cw = 0;
	if (*ch < 0) *ch = 0;
}

static u32 send(XCtx *c, u32 h, u32 msg, u32 wp, u32 lp) {
	Wnd *w = wnd(h);
	u32 args[4], r;
	if (!w) return 0;
	if (!w->wndproc) return msg == WM_NCCREATE; /* a system class: DefWindowProc */
	args[0] = h; args[1] = msg; args[2] = wp; args[3] = lp;
	if (tracing()) fprintf(stderr, "xrc: -> wndproc %08x(%08x, msg %04x, %08x, %08x)\n", w->wndproc, h, msg, wp, lp);
	r = xrc_call_guest(c, w->wndproc, 4, args);
	if (tracing()) fprintf(stderr, "xrc: <- msg %04x = %08x\n", msg, r);
	return r;
}
static void send_size(XCtx *c, Wnd *w) {
	int cw, ch;
	client_size(w, &cw, &ch);
	send(c, hwnd_of(w), WM_SIZE, w->iconic ? 1u : 0u, (u32)(cw & 0xffff) | (u32)(ch & 0xffff) << 16);
}

/* CreateWindowExA(exstyle, class, name, style, x, y, w, h, parent, menu, inst, param) */
#define CW_USEDEFAULT 0x80000000u
static void hle_CreateWindowExA(XCtx *c) {
	WClass *k = find_class(A(1));
	Wnd *w = 0;
	u32 cs, h, r;
	int i;
	if (!k) { set_err(ERROR_CLASS_DOES_NOT_EXIST); RET(0, 12); }
	for (i = 0; i < NWND; i++)
		if (!wnds[i].used) { w = &wnds[i]; break; }
	if (!w) RET(0, 12);
	memset(w, 0, sizeof *w);
	w->used = 1;
	w->cls = k;
	w->wndproc = k->wndproc;
	w->exstyle = A(0);
	w->style = A(3);
	w->x = A(4) == CW_USEDEFAULT ? 0 : (int)A(4);
	w->y = A(5) == CW_USEDEFAULT ? 0 : (int)A(5);
	w->w = A(6) == CW_USEDEFAULT ? disp_w() : (int)A(6);
	w->h = A(7) == CW_USEDEFAULT ? disp_h() : (int)A(7);
	w->parent = A(8);
	w->menu = A(9);
	w->inst = A(10);
	if (A(2)) snprintf(w->text, sizeof w->text, "%s", S(A(2)));
	h = hwnd_of(w);
	fprintf(stderr, "xrc: CreateWindowExA(%s, \"%s\", style %08x, %d,%d %dx%d) -> %08x\n", k->name, w->text, w->style, w->x,
	        w->y, w->w, w->h, h);
	/* CREATESTRUCTA: lpCreateParams hInstance hMenu hwndParent cy cx y x style lpszName lpszClass dwExStyle */
	cs = xrc_heap_alloc(48, 1);
	GF(CreateStruct, cs, create_params) = A(11); GF(CreateStruct, cs, instance) = A(10); GF(CreateStruct, cs, menu) = A(9); GF(CreateStruct, cs, parent) = A(8);
	GF(CreateStruct, cs, cy) = (u32)w->h; GF(CreateStruct, cs, cx) = (u32)w->w; GF(CreateStruct, cs, y) = (u32)w->y; GF(CreateStruct, cs, x) = (u32)w->x;
	GF(CreateStruct, cs, style) = w->style; GF(CreateStruct, cs, name) = A(2); GF(CreateStruct, cs, class_name) = A(1); GF(CreateStruct, cs, ex_style) = w->exstyle;
	r = send(c, h, WM_NCCREATE, 0, cs);
	if (r) r = send(c, h, WM_CREATE, 0, cs) != 0xffffffffu;
	xrc_heap_free(cs);
	if (!r || !wnd(h)) { w->used = 0; RET(0, 12); }
	send_size(c, w);
	send(c, h, WM_MOVE, 0, (u32)(w->x & 0xffff) | (u32)(w->y & 0xffff) << 16);
	if (w->style & WS_VISIBLE) w->visible = 1;
	if (!(w->style & WS_CHILD)) ref_u32_created(c, h);
	RET(h, 12);
}
static void hle_DestroyWindow(XCtx *c) {
	Wnd *w = wnd(A(0));
	u32 h = A(0);
	if (!w) { set_err(ERROR_INVALID_WINDOW_HANDLE); RET(0, 1); }
	send(c, h, WM_DESTROY, 0, 0);
	send(c, h, WM_NCDESTROY, 0, 0);
	if ((w = wnd(h))) w->used = 0;
	ref_u32_destroyed(h);
	if (active_wnd == h) active_wnd = 0;
	if (focus_wnd == h) focus_wnd = 0;
	RET(1, 1);
}
static void activate(XCtx *c, Wnd *w) {
	u32 h = hwnd_of(w);
	if (active_wnd == h) return;
	active_wnd = h;
	focus_wnd = h;
	send(c, h, WM_ACTIVATEAPP, 1, 0);
	send(c, h, WM_NCACTIVATE, 1, 0);
	send(c, h, WM_ACTIVATE, 1 /* WA_ACTIVE */, 0);
	send(c, h, WM_SETFOCUS, 0, 0);
}
/* ShowWindow(h, cmd): SW_HIDE 0, SW_MINIMIZE 6 / SW_SHOWMINIMIZED 2 ..., otherwise shown */
static void hle_ShowWindow(XCtx *c) {
	Wnd *w = wnd(A(0));
	u32 cmd = A(1);
	int was;
	if (!w) RET(0, 2);
	was = w->visible;
	if (cmd == 0) {
		ref_u32_show(c, A(0), cmd);
		w->visible = 0;
		if (was) send(c, A(0), WM_SHOWWINDOW, 0, 0);
		RET(was, 2);
	}
	ref_u32_show(c, A(0), cmd);
	w->visible = 1;
	w->iconic = cmd == 2 || cmd == 6 || cmd == 7 || cmd == 11;
	if (!was) send(c, A(0), WM_SHOWWINDOW, 1, 0);
	if (!w->iconic && !(w->style & WS_CHILD)) activate(c, w);
	RET(was, 2);
}
static void hle_UpdateWindow(XCtx *c) { RET(wnd(A(0)) != 0, 1); }
static void hle_InvalidateRect(XCtx *c) { RET(1, 3); }
static void hle_IsWindow(XCtx *c) { RET(wnd(A(0)) != 0, 1); }
static void hle_IsWindowVisible(XCtx *c) { Wnd *w = wnd(A(0)); RET(w && w->visible, 1); }
static void hle_IsIconic(XCtx *c) { Wnd *w = wnd(A(0)); RET(w && w->iconic, 1); }
static void hle_GetParent(XCtx *c) { Wnd *w = wnd(A(0)); RET(w ? w->parent : 0, 1); }
static void hle_SetWindowTextA(XCtx *c) {
	Wnd *w = wnd(A(0));
	if (!w) RET(0, 2);
	snprintf(w->text, sizeof w->text, "%s", A(1) ? S(A(1)) : "");
	ref_u32_pos(c, A(0));
	RET(1, 2);
}
static void put_rect(u32 r, int l, int t, int rr, int b) {
	GF(GRect, r, left) = (u32)l; GF(GRect, r, top) = (u32)t; GF(GRect, r, right) = (u32)rr; GF(GRect, r, bottom) = (u32)b;
}
static void hle_GetClientRect(XCtx *c) {
	Wnd *w = wnd(A(0));
	int cw, ch;
	if (!w) { set_err(ERROR_INVALID_WINDOW_HANDLE); RET(0, 2); }
	client_size(w, &cw, &ch);
	put_rect(A(1), 0, 0, cw, ch);
	RET(1, 2);
}
static void hle_GetWindowRect(XCtx *c) {
	Wnd *w = wnd(A(0));
	if (!w) { set_err(ERROR_INVALID_WINDOW_HANDLE); RET(0, 2); }
	put_rect(A(1), w->x, w->y, w->x + w->w, w->y + w->h);
	RET(1, 2);
}
/* AdjustWindowRectEx(rect, style, menu, exstyle) */
static void hle_AdjustWindowRectEx(XCtx *c) {
	u32 r = A(0);
	int l, t, rr, b;
	frame(A(1), A(3), &l, &t, &rr, &b);
	if (A(2)) t += 20;
	put_rect(r, (int)GF(GRect, r, left) - l, (int)GF(GRect, r, top) - t, (int)GF(GRect, r, right) + rr, (int)GF(GRect, r, bottom) + b);
	RET(1, 4);
}
static void hle_SetRect(XCtx *c) { put_rect(A(0), (int)A(1), (int)A(2), (int)A(3), (int)A(4)); RET(1, 5); }
/* SetWindowPos(h, after, x, y, cx, cy, flags): SWP_NOSIZE 1, SWP_NOMOVE 2, SWP_SHOWWINDOW 0x40, SWP_HIDEWINDOW 0x80 */
static void hle_SetWindowPos(XCtx *c) {
	Wnd *w = wnd(A(0));
	u32 f = A(6);
	int moved = 0, sized = 0;
	if (!w) { set_err(ERROR_INVALID_WINDOW_HANDLE); RET(0, 7); }
	if (!(f & 2) && (w->x != (int)A(2) || w->y != (int)A(3))) { w->x = (int)A(2); w->y = (int)A(3); moved = 1; }
	if (!(f & 1) && (w->w != (int)A(4) || w->h != (int)A(5))) { w->w = (int)A(4); w->h = (int)A(5); sized = 1; }
	if (f & 0x40) w->visible = 1;
	if (f & 0x80) w->visible = 0;
	if (sized) send_size(c, w);
	if (moved) send(c, A(0), WM_MOVE, 0, (u32)(w->x & 0xffff) | (u32)(w->y & 0xffff) << 16);
	ref_u32_pos(c, A(0));
	RET(1, 7);
}
/* WINDOWPLACEMENT: length flags showCmd ptMinPosition ptMaxPosition rcNormalPosition */
static void hle_GetWindowPlacement(XCtx *c) {
	Wnd *w = wnd(A(0));
	u32 p = A(1);
	if (!w) RET(0, 2);
	GF(WindowPlacement, p, flags) = 0;
	GF(WindowPlacement, p, show_cmd) = w->iconic ? 2 : w->visible ? 1 : 0;
	GF(WindowPlacement, p, min_position_x) = GF(WindowPlacement, p, min_position_y) = 0xffffffffu;
	GF(WindowPlacement, p, max_position_x) = GF(WindowPlacement, p, max_position_y) = 0xffffffffu;
	put_rect(p + 28, w->x, w->y, w->x + w->w, w->y + w->h);
	RET(1, 2);
}
static void hle_SetWindowPlacement(XCtx *c) {
	Wnd *w = wnd(A(0));
	u32 p = A(1);
	if (!w) RET(0, 2);
	w->x = (int)GF(WindowPlacement, p, normal_left); w->y = (int)GF(WindowPlacement, p, normal_top);
	w->w = (int)GF(WindowPlacement, p, normal_right) - w->x; w->h = (int)GF(WindowPlacement, p, normal_bottom) - w->y;
	w->iconic = GF(WindowPlacement, p, show_cmd) == 2 || GF(WindowPlacement, p, show_cmd) == 6 || GF(WindowPlacement, p, show_cmd) == 7;
	if (GF(WindowPlacement, p, show_cmd)) w->visible = 1;
	send_size(c, w);
	ref_u32_pos(c, A(0));
	RET(1, 2);
}
/* GWL_WNDPROC -4, GWL_HINSTANCE -6, GWL_ID -12, GWL_STYLE -16, GWL_EXSTYLE -20, GWL_USERDATA -21; >= 0: extra bytes */
static u32 *wlong(Wnd *w, int i) {
	switch (i) {
	case -4: return &w->wndproc;
	case -6: return &w->inst;
	case -12: return &w->id;
	case -16: return &w->style;
	case -20: return &w->exstyle;
	case -21: return &w->userdata;
	}
	if (i >= 0 && i / 4 < 16 && i % 4 == 0) return &w->extra[i / 4];
	return 0;
}
static void hle_GetWindowLongA(XCtx *c) {
	Wnd *w = wnd(A(0));
	u32 *p = w ? wlong(w, (int)A(1)) : 0;
	if (!p) { set_err(ERROR_INVALID_WINDOW_HANDLE); RET(0, 2); }
	RET(*p, 2);
}
static void hle_SetWindowLongA(XCtx *c) {
	Wnd *w = wnd(A(0));
	u32 *p = w ? wlong(w, (int)A(1)) : 0, old;
	if (!p) { set_err(ERROR_INVALID_WINDOW_HANDLE); RET(0, 2); }
	old = *p;
	*p = A(2);
	if ((int)A(1) == -16 || (int)A(1) == -20) ref_u32_pos(c, A(0));
	RET(old, 3);
}
/* EnumWindows(proc, lparam): the top-level windows */
static void hle_EnumWindows(XCtx *c) {
	u32 proc = A(0), lp = A(1), args[2];
	int i;
	for (i = 0; i < NWND; i++) {
		if (!wnds[i].used || (wnds[i].style & WS_CHILD)) continue;
		args[0] = hwnd_of(&wnds[i]); args[1] = lp;
		if (!xrc_call_guest(c, proc, 2, args)) break;
	}
	RET(1, 2);
}

/* ============================================================================================ messages */
static int cursor_count; /* (ShowCursor's count: below 0, hidden) */
static int cur_x, cur_y; /* the cursor, in screen coordinates */
static unsigned long long last_getcursor_us; /* (GetCursorPos's last call: mouse look on) */
typedef struct { u32 hwnd, msg, wp, lp; } Msg;
#define NQ 1024
static Msg queue[NQ];
static int q_head, q_n;
static int quit_posted;
static u32 quit_code;

static int post(u32 h, u32 msg, u32 wp, u32 lp) {
	Msg *m;
	if (q_n == NQ) return 0;
	m = &queue[(q_head + q_n++) % NQ];
	m->hwnd = h; m->msg = msg; m->wp = wp; m->lp = lp;
	return 1;
}
static void hle_PostMessageA(XCtx *c) { RET(post(A(0), A(1), A(2), A(3)), 4); }
static void hle_PostQuitMessage(XCtx *c) { quit_posted = 1; quit_code = A(0); RET(0, 1); }
static void hle_SendMessageA(XCtx *c) {
	u32 r = send(c, A(0), A(1), A(2), A(3));
	RET(r, 4);
}
static unsigned long long npeek;
/* MSG: hwnd message wParam lParam time pt.x pt.y */
static void fill_msg(u32 p, const Msg *m) {
	GF(WinMsg, p, hwnd) = m->hwnd; GF(WinMsg, p, message) = m->msg; GF(WinMsg, p, wparam) = m->wp; GF(WinMsg, p, lparam) = m->lp;
	GF(WinMsg, p, time) = (u32)(npeek * 16); GF(WinMsg, p, pt_x) = (u32)cur_x; GF(WinMsg, p, pt_y) = (u32)cur_y; /* (pt: the cursor) */
}
static int msg_match(const Msg *m, u32 h, u32 lo, u32 hi) {
	if (h && h != 0xffffffffu && m->hwnd != h) return 0;
	if ((lo || hi) && (m->msg < lo || m->msg > hi)) return 0;
	return 1;
}
#ifdef XRC_LR
static int lr_thread_peek(XCtx *c, u32 p, u32 h, u32 lo, u32 hi, int rm); /* thread messages, timers (hle_user32_lr.inc) */
static void menu_poll(void); /* the game's menu, picked in the real window (hle_user32_lr.inc) */
void xrc_relaunch_at_exit(void); /* (xrc_rt.c) */
static u32 ui_tid;
#endif
/* PeekMessageA(msg, hwnd, min, max, remove): PM_REMOVE 1 */
static int peek(XCtx *c, u32 p, u32 h, u32 lo, u32 hi, int rm) {
	int k;
	npeek++;
	ref_u32_pump(c);
#ifdef XRC_LR
	if (lr_thread_peek(c, p, h, lo, hi, rm)) return 1;
	if (!ui_tid) ui_tid = xrc_thread_id();
	if (xrc_thread_id() != ui_tid) return 0; /* window messages go to the window's thread */
	menu_poll();
#endif
	for (k = 0; k < q_n; k++) {
		int at = (q_head + k) % NQ;
		Msg m = queue[at];
		if (!msg_match(&m, h, lo, hi)) continue;
		fill_msg(p, &m);
		if (rm) {
			/* close the gap */
			int j;
			for (j = k; j > 0; j--) queue[(q_head + j) % NQ] = queue[(q_head + j - 1) % NQ];
			q_head = (q_head + 1) % NQ;
			q_n--;
		}
		return 1;
	}
	if (quit_posted && (!(lo || hi) || (WM_QUIT >= lo && WM_QUIT <= hi))) {
		Msg m = {0, WM_QUIT, quit_code, 0};
		fill_msg(p, &m);
		if (rm) quit_posted = 0;
		return 1;
	}
	return 0;
}
static void hle_PeekMessageA(XCtx *c) { RET(peek(c, A(0), A(1), A(2), A(3), (int)(A(4) & 1)), 5); }
static void hle_WaitMessage(XCtx *c) { ref_u32_pump(c); RET(1, 0); }
static void hle_TranslateMessage(XCtx *c) { RET(0, 1); }
static void hle_DispatchMessageA(XCtx *c) {
	u32 p = A(0);
#ifdef XRC_LR
	if (GF(WinMsg, p, message) == 0x0113u && GF(WinMsg, p, lparam)) { /* WM_TIMER with a TimerProc: TimerProc(hwnd, WM_TIMER, id, time) */
		u32 a[4] = {GF(WinMsg, p, hwnd), 0x0113u, GF(WinMsg, p, wparam), GF(WinMsg, p, time)};
		xrc_call_guest(c, GF(WinMsg, p, lparam), 4, a);
		RET(0, 1);
	}
#endif
	if (!GF(WinMsg, p, hwnd)) RET(0, 1);
	RET(send(c, GF(WinMsg, p, hwnd), GF(WinMsg, p, message), GF(WinMsg, p, wparam), GF(WinMsg, p, lparam)), 1);
}
/* DefWindowProcA(h, msg, wp, lp) */
#define SC_CLOSE 0xf060u
static void hle_DefWindowProcA(XCtx *c) {
	u32 h = A(0), msg = A(1), wp = A(2), r;
	if (ref_u32_defproc(h, msg, wp, A(3), &r)) RET(r, 4); /* the real window's default */
	switch (msg) {
	case WM_NCCREATE: RET(1, 4);
	case WM_CLOSE: {
		u32 args[1];
		(void)args;
		if (wnd(h)) {
			send(c, h, WM_DESTROY, 0, 0);
			send(c, h, WM_NCDESTROY, 0, 0);
			if (wnd(h)) wnd(h)->used = 0;
			ref_u32_destroyed(h);
		}
		RET(0, 4);
	}
	case WM_SYSCOMMAND:
		if ((wp & 0xfff0u) == SC_CLOSE) post(h, WM_CLOSE, 0, 0);
		RET(0, 4);
	case WM_ERASEBKGND: RET(1, 4);
	case WM_SETCURSOR: RET(0, 4);
	case WM_NCACTIVATE: RET(1, 4);
	}
	RET(0, 4);
}

#include "hle_user32_lr.inc"

/* ============================================================================================ for ref_user32.c */
int xrc_u32_post(u32 h, u32 msg, u32 wp, u32 lp) { return post(h, msg, wp, lp); }
u32 xrc_u32_send(XCtx *c, u32 h, u32 msg, u32 wp, u32 lp) { return send(c, h, msg, wp, lp); }
int xrc_u32_get(u32 h, int *x, int *y, int *cx, int *cy, u32 *style, u32 *exstyle, const char **text, int *visible) {
	Wnd *w = wnd(h);
	if (!w) return 0;
	*x = w->x; *y = w->y; *cx = w->w; *cy = w->h; *style = w->style; *exstyle = w->exstyle; *text = w->text;
	*visible = w->visible;
	return 1;
}
void xrc_u32_moved(u32 h, int x, int y, int cx, int cy) {
	Wnd *w = wnd(h);
	if (w) { w->x = x; w->y = y; w->w = cx; w->h = cy; }
}

/* ============================================================================================ display, monitors */
/* DEVMODEA: dmDeviceName[32] dmSpecVersion dmDriverVersion dmSize(36) dmDriverExtra dmFields(40) ...
 * dmBitsPerPel(104) dmPelsWidth(108) dmPelsHeight(112) dmDisplayFlags(116) dmDisplayFrequency(120) */
static const int modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1600, 900}, {1920, 1080}};
#define NMODES (int)(sizeof modes / sizeof modes[0])
static void fill_devmode(u32 d, int w, int h) {
	u16 sz = GF(DevMode, d, size) ? GF(DevMode, d, size) : 156;
	memset(xrc_mem + d, 0, sz);
	memcpy(xrc_mem + d, "\\\\.\\DISPLAY1", 13);
	GF(DevMode, d, size) = sz;
	GF(DevMode, d, fields) = 0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u; /* BITSPERPEL PELSWIDTH PELSHEIGHT DISPLAYFREQUENCY */
	GF(DevMode, d, bits_per_pel) = 32; GF(DevMode, d, pels_width) = (u32)w; GF(DevMode, d, pels_height) = (u32)h; GF(DevMode, d, display_frequency) = 60;
}
/* EnumDisplaySettingsA(device, mode, devmode): ENUM_CURRENT_SETTINGS -1, ENUM_REGISTRY_SETTINGS -2 */
static void hle_EnumDisplaySettingsA(XCtx *c) {
	u32 i = A(1);
	if (i == 0xffffffffu || i == 0xfffffffeu) { fill_devmode(A(2), disp_w(), disp_h()); RET(1, 3); }
	{ int mw = 0, mh = 0; /* (hires2: the modes that fit the monitor, its own, and the game's display) */
	  if (!xrc_mode_list((int)i, &mw, &mh)) RET(0, 3);
	  fill_devmode(A(2), mw, mh); }
	RET(1, 3);
}
static void hle_ChangeDisplaySettingsExA(XCtx *c) { RET(0 /* DISP_CHANGE_SUCCESSFUL */, 5); }
/* EnumDisplayDevicesA(device, i, DISPLAY_DEVICEA *, flags): cb DeviceName[32] DeviceString[128] StateFlags
 * DeviceID[128] DeviceKey[128] */
static void hle_EnumDisplayDevicesA(XCtx *c) {
	u32 d = A(2);
	if (A(1) != 0 || !d) RET(0, 4);
	memset(xrc_mem + d + 4, 0, 420);
	if (A(0)) { /* the monitor of the adapter */
		snprintf(S(d + 4), 32, "%s\\Monitor0", S(A(0)));
		snprintf(S(d + 36), 128, "Generic PnP Monitor");
		GF(DisplayDevice, d, state_flags) = 3;
	} else {
		snprintf(S(d + 4), 32, "\\\\.\\DISPLAY1");
		snprintf(S(d + 36), 128, "NVIDIA GeForce GTX 560");
		GF(DisplayDevice, d, state_flags) = 1 | 4; /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
		snprintf(S(d + 168), 128, "PCI\\VEN_10DE&DEV_1201");
	}
	RET(1, 4);
}
#define HMONITOR1 0x00012340u
static void hle_MonitorFromWindow(XCtx *c) { RET(HMONITOR1, 2); }
/* MONITORINFO(EX)A: cbSize rcMonitor rcWork dwFlags [szDevice[32]] */
static void hle_GetMonitorInfoA(XCtx *c) {
	u32 m = A(1);
	if (A(0) != HMONITOR1 || !m) RET(0, 2);
	put_rect(m + 4, 0, 0, disp_w(), disp_h());
	put_rect(m + 20, 0, 0, disp_w(), disp_h() - 40);
	GF(MonitorInfoEx, m, flags) = 1; /* MONITORINFOF_PRIMARY */
	if (GF(MonitorInfoEx, m, size) >= 72) snprintf(S(m + 40), 32, "\\\\.\\DISPLAY1");
	RET(1, 2);
}

/* ============================================================================================ cursor, input */
static void hle_LoadIconA(XCtx *c) { RET(0x00013000u + (A(1) & 0xfff), 2); }
static void hle_LoadCursorA(XCtx *c) { RET(0x00014000u + (A(1) & 0xfff), 2); }
static void hle_ShowCursor(XCtx *c) { cursor_count += A(0) ? 1 : -1; RET((u32)cursor_count, 1); }
static void hle_SetCapture(XCtx *c) { u32 o = capture_wnd; capture_wnd = A(0); RET(o, 1); }
static void hle_ClipCursor(XCtx *c) { RET(1, 1); }
static void hle_GetCursorPos(XCtx *c) { last_getcursor_us = xrc_now_us(); GF(GPoint, A(0), x) = (u32)cur_x; GF(GPoint, A(0), y) = (u32)cur_y; RET(1, 1); }
static void hle_SetCursorPos(XCtx *c) {
	static int n;
	if (xrc_tool_env("XRC_TRACE_INPUT") && n++ < 5) fprintf(stderr, "xrc: input: SetCursorPos(%d, %d)\n", (int)A(0), (int)A(1));
	cur_x = (int)A(0); cur_y = (int)A(1); RET(1, 2);
}
static void hle_ScreenToClient(XCtx *c) {
	Wnd *w = wnd(A(0));
	int l, t, r, b;
	if (!w) RET(0, 2);
	frame(w->style, w->exstyle, &l, &t, &r, &b);
	GF(GPoint, A(1), x) -= (u32)(w->x + l);
	GF(GPoint, A(1), y) -= (u32)(w->y + t);
	RET(1, 2);
}
/* the host window's mouse (vk_d3d9.c feeds its SDL events here): what Windows would send the game's window. The game
 * puts the cursor at its window's centre every frame (SetCursorPos) and takes each WM_MOUSEMOVE's offset from there as
 * camera movement (mouse look), so the cursor here is where SetCursorPos left it plus the motion since; with the
 * cursor shown (ShowCursor count >= 0) it follows the window's pointer instead. Buttons and the wheel are their
 * messages at the cursor (MK_* flags in wParam). */
static u32 mouse_mk; /* MK_LBUTTON 1, MK_RBUTTON 2, MK_MBUTTON 0x10, MK_XBUTTON1 0x20, MK_XBUTTON2 0x40 */
static u32 mouse_wnd(int *cx, int *cy) {
	u32 h = focus_wnd ? focus_wnd : active_wnd;
	Wnd *w = wnd(h);
	int l, t, r, b;
	if (!w) return 0;
	frame(w->style, w->exstyle, &l, &t, &r, &b);
	*cx = cur_x - (w->x + l); *cy = cur_y - (w->y + t);
	return h;
}
static u32 xy(int x, int y) { return (u32)(x & 0xffff) | (u32)(y & 0xffff) << 16; }
static u32 mk_keys(void) { return mouse_mk | (xrc_input_dik_down(0x2a) || xrc_input_dik_down(0x36) ? 4u : 0u) | (xrc_input_dik_down(0x1d) || xrc_input_dik_down(0x9d) ? 8u : 0u); }
/* mouse look on: the cursor hidden and the game reading it (GetCursorPos: the camera, in 3D play) in the last half
 * second -- then the window takes the pointer (vk_d3d9.c); in menus, the title, cutscenes it is left free */
int xrc_user32_mouse_look(void) { return cursor_count < 0 && last_getcursor_us && xrc_now_us() - last_getcursor_us < 500000ull; }
/* motion: dx dy (relative, in the game window's pixels); cx cy: the pointer in its client area (-1: none) */
void xrc_user32_mouse_move(int dx, int dy, int cx, int cy) {
	int x, y;
	u32 h;
	if (cursor_count >= 0 && cx >= 0 && (h = mouse_wnd(&x, &y))) { cur_x += cx - x; cur_y += cy - y; }
	else { cur_x += dx; cur_y += dy; }
	if ((h = mouse_wnd(&x, &y))) post(h, 0x200 /* WM_MOUSEMOVE */, mk_keys(), xy(x, y));
}
void xrc_user32_mouse_button(int b, int down) { /* SDL: 1 left 2 middle 3 right 4 X1 5 X2 */
	static const u32 bit[6] = {0, 1, 0x10, 2, 0x20, 0x40}, msg[6] = {0, 0x201, 0x207, 0x204, 0x20b, 0x20b};
	int x, y;
	u32 h;
	if (b < 1 || b > 5) return;
	if (down) mouse_mk |= bit[b]; else mouse_mk &= ~bit[b];
	if ((h = mouse_wnd(&x, &y))) post(h, msg[b] + (down ? 0u : 1u), mk_keys() | (b >= 4 ? (u32)(b - 3) << 16 : 0u), xy(x, y));
	if (xrc_tool_env("XRC_TRACE_INPUT")) fprintf(stderr, "xrc: input: mouse button %d %s -> message %#x to window %08x at %d,%d\n", b, down ? "down" : "up", msg[b] + (down ? 0u : 1u), h, x, y);
}
void xrc_user32_mouse_wheel(int dz) { /* WM_MOUSEWHEEL: the delta in wParam's high word, the cursor in screen coordinates */
	int x, y;
	u32 h;
	if ((h = mouse_wnd(&x, &y))) post(h, 0x20a, (u32)(dz & 0xffff) << 16 | mk_keys(), xy(cur_x, cur_y));
}
void xrc_user32_mouse_release_all(void) { mouse_mk = 0; }
/* GetAsyncKeyState(vk): the window's input (the game asks for VK_LCONTROL); 0x8000 while down */
static void hle_GetAsyncKeyState(XCtx *c) {
	u32 vk = A(0) & 0xff;
	int d = 0;
	switch (vk) {
	case 0x01: d = mouse_mk & 1; break;
	case 0x02: d = mouse_mk & 2; break;
	case 0x04: d = mouse_mk & 0x10; break;
	case 0x05: d = mouse_mk & 0x20; break;
	case 0x06: d = mouse_mk & 0x40; break;
	case 0x10: d = xrc_input_dik_down(0x2a) || xrc_input_dik_down(0x36); break;
	case 0x11: d = xrc_input_dik_down(0x1d) || xrc_input_dik_down(0x9d); break;
	case 0x12: d = xrc_input_dik_down(0x38) || xrc_input_dik_down(0xb8); break;
	case 0xa0: d = xrc_input_dik_down(0x2a); break;
	case 0xa1: d = xrc_input_dik_down(0x36); break;
	case 0xa2: d = xrc_input_dik_down(0x1d); break;
	case 0xa3: d = xrc_input_dik_down(0x9d); break;
	case 0xa4: d = xrc_input_dik_down(0x38); break;
	case 0xa5: d = xrc_input_dik_down(0xb8); break;
	}
	RET(d ? 0x8000u : 0u, 1);
}
/* GetGUIThreadInfo(thread, GUITHREADINFO *): cbSize flags hwndActive hwndFocus hwndCapture ... */
static void hle_GetGUIThreadInfo(XCtx *c) {
	u32 g = A(1);
	if (!g || GF(GuiThreadInfo, g, size) < 48) RET(0, 2);
	memset(xrc_mem + g + 4, 0, 44);
	GF(GuiThreadInfo, g, hwnd_active) = active_wnd; GF(GuiThreadInfo, g, hwnd_focus) = focus_wnd; GF(GuiThreadInfo, g, hwnd_capture) = capture_wnd;
	RET(1, 2);
}

/* ============================================================================================ clipboard */
static void hle_IsClipboardFormatAvailable(XCtx *c) { RET(0, 1); }
static void hle_OpenClipboard(XCtx *c) { RET(0, 1); }
static void hle_GetClipboardData(XCtx *c) { RET(0, 1); }
static void hle_CloseClipboard(XCtx *c) { RET(1, 0); }

/* ============================================================================================ wsprintfA
 * cdecl wsprintfA(buf, fmt, ...): %[-][0][width][.prec][l|h]{d i u x X c s %} (no floats in wsprintf); the output is
 * at most 1024 characters. */
static void hle_wsprintfA(XCtx *c) {
	u32 out = A(0), f = A(1);
	int ai = 2;
	char buf[1100], spec[32], tmp[300];
	size_t n = 0;
	const char *p = S(f);
	while (*p && n < 1024) {
		size_t sl = 0;
		if (*p != '%') { buf[n++] = *p++; continue; }
		spec[sl++] = *p++;
		while (*p && strchr("-+ #0123456789.", *p) && sl < 20) spec[sl++] = *p++;
		while (*p == 'l' || *p == 'h') p++;
		if (!*p) break;
		if (*p == '%') { buf[n++] = '%'; p++; continue; }
		spec[sl++] = *p == 'i' ? 'd' : *p;
		spec[sl] = 0;
		switch (*p++) {
		case 'd': case 'i': snprintf(tmp, sizeof tmp, spec, (int)A(ai++)); break;
		case 'u': case 'x': case 'X': snprintf(tmp, sizeof tmp, spec, A(ai++)); break;
		case 'c': snprintf(tmp, sizeof tmp, spec, (int)(A(ai++) & 0xff)); break;
		case 's': { u32 s = A(ai++); snprintf(tmp, sizeof tmp, spec, s ? S(s) : "(null)"); break; }
		default: tmp[0] = 0; break;
		}
		for (sl = 0; tmp[sl] && n < 1024; sl++) buf[n++] = tmp[sl];
	}
	buf[n] = 0;
	memcpy(S(out), buf, n + 1);
	c->eax = (u32)n;
	xrc_cdecl_ret(c);
}

/* ============================================================================================ GDI32
 * Objects are handles with nothing behind them; text is measured as 8 x 16 cells. */
static u32 gdi_next = 0x00015000u;
static void hle_GetStockObject(XCtx *c) { RET(0x00016000u + (A(0) & 0xff), 1); }
static void hle_CreatePen(XCtx *c) { RET(gdi_next += 4, 3); }
static void hle_CreateFontIndirectA(XCtx *c) { RET(gdi_next += 4, 1); }
static void hle_SelectObject(XCtx *c) { RET(0x00016000u, 2); }
static void hle_DeleteObject(XCtx *c) { RET(1, 1); }
static void hle_MoveToEx(XCtx *c) { if (A(3)) { GF(GPoint, A(3), x) = 0; GF(GPoint, A(3), y) = 0; } RET(1, 4); }
static void hle_LineTo(XCtx *c) { RET(1, 3); }
static void hle_Ellipse(XCtx *c) { RET(1, 5); }
static void hle_TextOutA(XCtx *c) { RET(1, 5); }
static void hle_GetTextExtentPoint32A(XCtx *c) { GF(GSize, A(3), cx) = 8u * A(2); GF(GSize, A(3), cy) = 16; RET(1, 4); }

/* ============================================================================================ the table */
#define U(n) {"USER32.dll", #n, hle_##n}
#define G(n) {"GDI32.dll", #n, hle_##n}
const XrcImport xrc_hle_user32[] = {
	U(RegisterClassExA), U(SetClassLongA), U(CreateWindowExA), U(DestroyWindow), U(ShowWindow), U(UpdateWindow),
	U(InvalidateRect), U(IsWindow), U(IsWindowVisible), U(IsIconic), U(GetParent), U(SetWindowTextA),
	U(GetClientRect), U(GetWindowRect), U(AdjustWindowRectEx), U(SetRect), U(SetWindowPos), U(GetWindowPlacement),
	U(SetWindowPlacement), U(GetWindowLongA), U(SetWindowLongA), U(EnumWindows), U(PostMessageA),
	U(PostQuitMessage), U(SendMessageA), U(PeekMessageA), U(WaitMessage), U(TranslateMessage), U(DispatchMessageA),
	U(DefWindowProcA), U(EnumDisplaySettingsA), U(ChangeDisplaySettingsExA), U(EnumDisplayDevicesA),
	U(MonitorFromWindow), U(GetMonitorInfoA), U(LoadIconA), U(LoadCursorA), U(ShowCursor), U(SetCapture),
	U(ClipCursor), U(GetCursorPos), U(SetCursorPos), U(ScreenToClient), U(GetAsyncKeyState), U(GetGUIThreadInfo),
	U(IsClipboardFormatAvailable), U(OpenClipboard), U(GetClipboardData), U(CloseClipboard), U(wsprintfA),
	G(GetStockObject), G(CreatePen), G(CreateFontIndirectA), G(SelectObject), G(DeleteObject), G(MoveToEx),
	G(LineTo), G(Ellipse), G(TextOutA), G(GetTextExtentPoint32A),
#ifdef XRC_LR
	U(CreateMenu), U(DestroyMenu), U(DeleteMenu), U(DrawMenuBar), U(SetMenu), U(GetMenuItemCount), U(GetMenuItemInfoA),
	U(SetMenuItemInfoA), U(InsertMenuItemW), U(CallWindowProcA), U(FindWindowA), U(GetFocus), U(GetForegroundWindow),
	U(SetActiveWindow), U(SetForegroundWindow), U(GetKeyboardLayout), U(GetKeyboardLayoutNameA), U(GetMessageA),
	U(PostThreadMessageA), U(GetPropA), U(SetPropA), U(RemovePropA), U(GetSystemMetrics), U(SetTimer), U(KillTimer),
#endif
};
const unsigned xrc_hle_user32_n = sizeof xrc_hle_user32 / sizeof xrc_hle_user32[0];
