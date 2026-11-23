/* hires2: the display the game is told about (xrc_rt.c) -- used by vk_d3d9.c, hle_d3d9.c, hle_user32.c */
#ifndef XRC_DISP_H
#define XRC_DISP_H
typedef struct { int x, y, w, h, wx, wy, ww, wh, dpi, hz; const char *src; } XrcMon;
int xrc_mon_get(void *hwnd, XrcMon *m);                     /* the monitor of hwnd (0: of the cursor), physical pixels, always filled */
void xrc_default_client(const XrcMon *m, int *w, int *h);   /* the windowed client size to start with */
void xrc_disp_size(int *w, int *h);                         /* what the game is told */
void xrc_size_note(const char *what);                       /* the one-line size log */
void xrc_size_frame(int sw, int sh, int dw, int dh);        /* the final copy's source and destination sizes, each Present */
extern void *xrc_disp_hwnd;
extern volatile int xrc_disp_fs;
extern volatile long xrc_disp_gen;
extern int xrc_g_bb_w, xrc_g_bb_h, xrc_g_out_w, xrc_g_out_h;
#endif
