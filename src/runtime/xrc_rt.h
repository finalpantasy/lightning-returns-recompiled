/* xrc_rt.h - the portable runtime of the ff xrc translations (any 64-bit or 32-bit host, no x86 needed).
 *
 * Guest memory is one flat 4 GB block (xrc_mem); a guest address is an offset into it. The exe image is mapped at its
 * own addresses, so the translated code's absolute addresses (globals, vtables, string literals) are valid as is.
 * Calls between translated functions go through xrc_dispatch (guest address -> xf_ function, xrc_fns table from
 * `ff xrc --all --table`). Imports are fake guest addresses (XRC_IMP_BASE + 16 * i) that dispatch to host
 * implementations by name (the HLE registry in xrc_rt.c); an import without one stops the run with its name. */
#ifndef XRC_RT_H
#define XRC_RT_H
/* -DXRC_EXPERIMENTAL (the launcher's Experimental Optimizations): less-tested optimizations that may break things are
 * on -- code tests xrc_experimental (a constant: the off side compiles away) */
#ifdef XRC_EXPERIMENTAL
#define xrc_experimental 1
#else
#define xrc_experimental 0
#endif
#include "xrc_tool_env.h"
/* the reference target draws with the Vulkan backend (vk_d3d9.c) instead of the system's Direct3D 9: XRC_VK=1 (0: not),
 * else the build's default: Vulkan wherever it is built in (XRC_VULKAN), unless -DXRC_D3D9_DEFAULT (the launcher's
 * "Graphics Driver: D3D9"). (The system's Direct3D 9 path is broken: the cutscene after the first fight never plays.) */
static inline int xrc_use_vulkan(void) {
	const char *e = getenv("XRC_VK");
	if (e) return *e != 0 && *e != '0';
#if defined(XRC_VULKAN) && !defined(XRC_D3D9_DEFAULT)
	return 1;
#else
	return 0;
#endif
}
#include <stdint.h>
#include <stdio.h>
#define XRC_RUNTIME /* (xrc.h: the runtime names the guest thread state through an XCtx explicitly) */
#include "xrc.h"

typedef struct XrcFn { u32 addr; XFn fn; } XrcFn;
typedef struct XrcOverride { u32 addr; const char *why; } XrcOverride;
extern const XrcFn xrc_fns[];
extern const unsigned xrc_nfns;
extern const XrcOverride xrc_overrides[];
/* from the exe, written by ff xrc --table: for loading the player's own copy (retail: .text encrypted, entry in a stub) */
typedef struct XrcRange { u32 addr, len; } XrcRange;
extern const unsigned xrc_entry;
extern const XrcRange xrc_hash_ranges[];
extern const unsigned xrc_nhash_ranges;
extern const unsigned long long xrc_data_hash;
extern const unsigned xrc_noverrides;

/* guest layout */
#define XRC_IMAGE_BASE 0x00400000u
#define XRC_STACK_SIZE 0x00400000u /* 4 MB main-thread stack */
#define XRC_TEB 0x7ffde000u
#define XRC_PEB 0x7ffdf000u
#define XRC_TLS_BLOCK 0x7ff00000u   /* static TLS block of the main thread */
/* The main stack, the guest heap (HeapAlloc / LocalAlloc / the CRT's malloc on top of them: its small blocks) and
 * VirtualAlloc with the heap's big blocks (the game's ~1 GB: mostly big blocks):
 *   4 GB (default; as a large-address-aware process): stack 0x30000000, heap [0x40000000, 0x7f000000), VirtualAlloc
 *     [0x80000000, 0xffe00000) upward;
 *   2 GB (XRC_GUEST_2GB=1, the launcher's Game Memory: what the original exe -- not large-address-aware -- gets: guest
 *     memory is only the low 2 GB): stack 0x02c00000 (below it the image), then one region [0x03000000, 0x7f000000)
 *     the heap fills upward and VirtualAlloc downward (xrc_va_down), each as far as the other leaves. */
extern u32 xrc_stack_lo, xrc_heap_lo, xrc_heap_hi, xrc_va_lo, xrc_va_hi;
extern int xrc_guest_2gb, xrc_va_down;
#define XRC_STACK_LO xrc_stack_lo
#define XRC_HEAP_LO xrc_heap_lo
#define XRC_HEAP_HI xrc_heap_hi
#define XRC_VA_LO xrc_va_lo
#define XRC_VA_HI xrc_va_hi
#define XRC_GUEST_SIZE (xrc_guest_2gb ? 0x80000000ull : 0x100000000ull)
void xrc_heap_init(void); /* (the allocators' start: after the layout is chosen) */
#define XRC_PROCESS_HEAP 0x00a00000u /* heap handles are just values (one allocator serves all heaps) */
#define XRC_IMP_BASE 0xfff00000u    /* fake import addresses */
#define XRC_RET_MAGIC 0xfeedf00du   /* the return address the runtime pushes when it calls guest code (translated calls push the original's) */

/* an HLE import: runs with the guest registers in c; [c->esp] is the return address, args above it. It sets eax
 * (and edx / st0) and pops: xrc_stdcall_ret(c, nargs) for WINAPI functions, xrc_cdecl_ret(c) for cdecl. */
typedef void (*XrcHle)(XCtx *c);
typedef struct XrcImport { const char *dll, *name; XrcHle fn; } XrcImport;

u32 xrc_arg(XCtx *c, int i); /* i-th stack argument (0-based) */
u32 xrc_import_addr(const char *dll, const char *name); /* GetProcAddress (dll may be 0: any) */
extern const char *xrc_cur_import; /* name of the HLE import running now ("Interface::Method" for COM) */
extern XrcHle xrc_cur_body;        /* its HLE function (0: none), for a reference forwarder to run */
extern unsigned xrc_cur_slot;      /* its import slot */
/* the reference targets (XRC_REFERENCE): the forwarder for dll!name that runs the HLE function and also calls the
 * real DLL, 0 for the HLE alone (ref_rt.c) */
XrcHle xrc_ref_wrap(const char *dll, const char *name);
unsigned xrc_nimports(void); /* the next import slot's index */
/* hle_user32.c <-> ref_user32.c */
int xrc_u32_post(u32 h, u32 msg, u32 wp, u32 lp);
/* ends the game now (ExitProcess, the window's close): the exit handlers run (a background movie conversion is stopped,
 * the tools' dumps written), then -- Windows -- the process ends without unloading its DLLs, which can hang there */
__attribute__((noreturn)) void xrc_exit_now(int code);
/* the window's close button: the game gets its WM_CLOSE; if it has not ended in XRC_CLOSE_WAIT_MS (3000), it is ended */
void xrc_close_requested(void);
/* the picture the game draws (its back buffer: xrc_view_w x xrc_view_h, 0 before the first Present) is shown scaled
 * to fit the window, aspect kept, centred (vk_d3d9.c be_present); the input side maps the window's coordinates back
 * to the game's with xrc_view_map: window client (cw x ch) -> the picture's offset (*x0, *y0) and size (*w x *h);
 * 0 when there is no scaling (no picture yet, or the window is the picture's size) */
extern int xrc_view_w, xrc_view_h;
extern void *xrc_view_window; /* (Windows: the real window the picture is shown in) */
int xrc_view_map(int cw, int ch, int *x0, int *y0, int *w, int *h);
/* the game is ending: nothing more is heard (every host output and, Windows, the real XAudio2 engine stopped) */
void xrc_audio_silence(void);
#ifdef XRC_REFERENCE
void ref_xaudio2_silence(void); /* (ref_xaudio2.c) */
#endif
u32 xrc_u32_send(XCtx *c, u32 h, u32 msg, u32 wp, u32 lp);
int xrc_u32_get(u32 h, int *x, int *y, int *cx, int *cy, u32 *style, u32 *exstyle, const char **text, int *visible);
void xrc_u32_moved(u32 h, int x, int y, int cx, int cy);
#ifdef XRC_REFERENCE
void ref_u32_created(XCtx *c, u32 h);
void ref_u32_destroyed(u32 h);
void ref_u32_show(XCtx *c, u32 h, u32 cmd);
void ref_u32_pos(XCtx *c, u32 h);
void ref_u32_pump(XCtx *c);
int ref_u32_defproc(u32 h, u32 msg, u32 wp, u32 lp, u32 *result);
void *xrc_ref_hwnd(u32 h); /* the real window of a guest one (0: none) */
#else
#define ref_u32_created(c, h) ((void)0)
#define ref_u32_destroyed(h) ((void)0)
#define ref_u32_show(c, h, cmd) ((void)0)
#define ref_u32_pos(c, h) ((void)0)
#define ref_u32_pump(c) ((void)0)
#define ref_u32_defproc(h, m, w, l, r) 0
#endif
u32 xrc_import_slot(const char *dll, const char *name); /* a slot even without an HLE function (COM vtables) */
int xrc_ieq(const char *a, const char *b);             /* ASCII case-insensitive equality */
extern int xrc_argc;                                    /* the game's command-line arguments (host side) */
extern char **xrc_argv;
/* per-DLL HLE tables (src/runtime/hle_<dll>.c) */
extern const XrcImport xrc_hle_kernel32[];
extern const unsigned xrc_hle_kernel32_n;
extern const XrcImport xrc_hle_kernel32_io[]; /* kernel objects, files (hle_kernel32_io.c) */
extern const unsigned xrc_hle_kernel32_io_n;
extern const XrcImport xrc_hle_system[]; /* WINMM ADVAPI32 SHELL32 IMM32 VERSION ole32 OLEAUT32 (hle_system.c) */
extern const unsigned xrc_hle_system_n;
extern const XrcImport xrc_hle_user32[]; /* USER32 GDI32: windows, messages, display (hle_user32.c) */
extern const unsigned xrc_hle_user32_n;
extern const XrcImport xrc_hle_d3d9[]; /* Direct3D 9, headless (hle_d3d9.c) */
extern const unsigned xrc_hle_d3d9_n;
extern const XrcImport xrc_hle_dinput[]; /* DirectInput 8, idle devices (hle_dinput.c) */
extern const unsigned xrc_hle_dinput_n;
extern const XrcImport xrc_hle_xaudio2[]; /* XAudio2 2.7, silent (hle_xaudio2.c) */
extern const unsigned xrc_hle_xaudio2_n;
extern const XrcImport xrc_hle_bink[]; /* bink2w32: movies from the MP4s tools/bink2mp4 makes (hle_bink.c) */
extern const unsigned xrc_hle_bink_n;
extern char xrc_last_open_path[1024]; /* the host path CreateFileA opened last (hle_bink.c: which movie) */
u32 xrc_xaudio2_create(const char *clsid, u32 out); /* CoCreateInstance of its classes ("{...}" strings) */
extern const XrcImport xrc_hle_com[]; /* COM objects: DxDiag ... (hle_com.c) */
#ifdef XRC_LR
extern const XrcImport xrc_hle_msvcr100[]; /* MSVCR100 / MSVCP100: the VC10 C and C++ runtime LR imports (hle_msvcr100.c) */
extern const unsigned xrc_hle_msvcr100_n;
u32 xrc_crt_data_import(const char *dll, const char *name); /* a data import's guest address (0: not one) */
u32 xrc_k32(XCtx *c, const char *name, int n, const u32 *args); /* call the runtime's KERNEL32!name (hle_msvcr100.c) */
extern const XrcImport xrc_hle_lr[]; /* LR's other small imports (hle_lr.c) */
extern const XrcImport xrc_hle_steam[]; /* steam_api.dll, forwarded to the 64-bit steam_api64.dll (hle_steam.c) */
extern const unsigned xrc_hle_steam_n;
u32 xrc_import_with(const char *dll, const char *name, XrcHle fn);
/* where the game sits on the guest's drive C: (the retail install layout), mapped to XRC_GAME_DIR on the host */
#ifdef XRC_LR
#define XRC_GAME_FOLDER "LIGHTNING RETURNS FINAL FANTASY XIII"
#define XRC_GUEST_EXE_DIR "C:\\LIGHTNING RETURNS FINAL FANTASY XIII"
#define XRC_GUEST_EXE_NAME "LRFF13.exe"
#else
#define XRC_GAME_FOLDER "FINAL FANTASY XIII"
#define XRC_GUEST_EXE_DIR "C:\\FINAL FANTASY XIII\\white_data\\prog\\win\\bin"
#define XRC_GUEST_EXE_NAME "ffxiiiimg.exe"
#endif
extern const unsigned xrc_hle_lr_n;
#endif

extern const unsigned xrc_hle_com_n;
u32 xrc_com_create(u32 clsid, u32 iid, u32 out); /* CoCreateInstance: 0 = created into *out */
u32 xrc_com_class_object(u32 clsid, u32 out);     /* CoGetClassObject: 0 = an IClassFactory into *out */
u32 xrc_bstr_from(const char *s);                /* a new BSTR of an ASCII string */
void xrc_stdcall_ret(XCtx *c, int nargs);
/* calls guest code from the HLE (window procedures, enumeration callbacks): pushes args (args[0] last, so it is the
 * first argument), runs fn and returns its eax; esp is restored either way (stdcall or cdecl callee). The callee keeps
 * ebx/esi/edi/ebp; eax/ecx/edx are the HLE's to set afterwards. */
u32 xrc_call_guest(XCtx *c, u32 fn, int nargs, const u32 *args);
int xrc_host_path(const char *guest, char *out, size_t n); /* a guest path's host path (hle_kernel32_io.c) */
void xrc_cdecl_ret(XCtx *c);
u32 xrc_alloc(u32 size, u32 align); /* runtime-internal bump allocation (never freed) */
/* the guest heap (hle_kernel32.c): HeapAlloc & co. and anything the HLE allocates for the guest */
u32 xrc_heap_alloc(u32 n, int zero);
int xrc_heap_free(u32 p);
u32 xrc_heap_realloc(u32 p, u32 n, int zero);
u32 xrc_heap_size(u32 p);
void xrc_fatal(XCtx *c, const char *fmt, ...);
void xrc_print_stack(void); /* the guest call stack, on stderr */
void xrc_print_thread_stack(u32 tid); /* another thread's */

/* guest threads (xrc_thread.c): one runs at a time under the GIL; see there */
void xrc_gil_acquire(void);
void xrc_gil_mark_main(void); /* the calling thread is the main one (GIL priority) */
void xrc_gil_release(void);
int xrc_gil_held(void);         /* this thread holds the GIL */
void xrc_thread_adopt(XCtx *c); /* the calling host thread becomes a guest thread (GIL held) */
#define xrc_import_ctx (xrc_tls()->import_ctx) /* reference targets: the context of this thread's running import */
void xrc_yield(void);
void xrc_preempt(void);                                        /* from xrc_dispatch: hand over now and then */
extern volatile int xrc_gil_waiters;
/* blocking waits: 1 ready, 0 timed out (ms 0xffffffff: forever). A wait with keys is woken by xrc_wake of one of them
 * (or xrc_state_changed); one without keys only by xrc_state_changed. */
int xrc_block_wait(int (*ready)(void *), void *arg, u32 ms);
int xrc_block_wait_keys(const uintptr_t *keys, int nkeys, int (*ready)(void *), void *arg, u32 ms);
void xrc_wake(uintptr_t key);  /* the object key changed (a lock released, an event set ...) */
void xrc_state_changed(void); /* something changed: every waiter rechecks */
void xrc_sleep(u32 ms);
void xrc_sleep_c(u32 ms, int cls); /* xrc_sleep with a caller class for XRC_SLEEP_CAP_MASK */
int xrc_thread_create(u32 start, u32 param, u32 stack, u32 *tid);
void xrc_thread_exit(u32 code);
u32 xrc_thread_id(void);
u32 xrc_thread_id_of(int i);
int xrc_thread_index(void);
int xrc_thread_by_id(u32 tid);
#ifndef XRC_NO_TOOLS
void xrc_wait_note(const char *what, u32 h);            /* XRC_WATCH: the wait about to block (tools builds) */
int xrc_wait_describe(u32 tid, char *buf, size_t n);  /* that thread's current wait (0: not blocked) */
#define XRC_WAIT_NOTE(what, h) xrc_wait_note(what, h)
#else
#define XRC_WAIT_NOTE(what, h) ((void)0)
#endif
int xrc_thread_done(int i, u32 *code);
unsigned long long xrc_gil_off_us(void); /* the calling thread's total time without the GIL (us) */
void xrc_prof_frame(void);
/* host input for the DirectInput HLE (hle_dinput.c): SDL scancodes, mouse deltas, SDL mouse buttons (1..) */
void xrc_input_key(int scancode, int down);
void xrc_input_mouse(int dx, int dy, int dz);
void xrc_input_button(int b, int down);
void xrc_input_release_all(void);
int xrc_input_dik_down(int dik); /* a key of the window's keyboard held (DIK code) */
/* the window's mouse as the game's window messages and cursor (hle_user32.c) */
void xrc_user32_mouse_move(int dx, int dy, int cx, int cy);
void xrc_user32_mouse_button(int b, int down);
void xrc_user32_mouse_wheel(int dz);
void xrc_user32_mouse_release_all(void);
int xrc_user32_mouse_look(void); /* the game's camera reading the cursor: the window takes the pointer */ /* XRC_PROFILE (Windows): a Present's time (xrc_rt.c) */
unsigned long long xrc_now_us(void); /* the monotonic clock (xrc_thread.c) */
void xrc_host_fp_mode(void); /* this host thread's SSE mode for guest code: XRC_DENORMALS=0 flushes them (xrc_thread.c) */
void xrc_import_report(FILE *f, unsigned long long nframes); /* XRC_IMPORT_TIME: per-import time since the last call */
void xrc_thread_wake_report(FILE *f);
void xrc_thread_report(FILE *f, unsigned long long nframes); /* per-thread CPU / GIL ms per frame since the last call */
extern u32 xrc_tls_lo, xrc_tls_hi, xrc_tls_zfill; /* the static TLS template (the exe's TLS directory) */
void xrc_ctx_init(XCtx *c, u32 esp); /* a fresh guest thread context (stack top, start control state) */

/* hand-written replacements of guest functions (src/runtime/ovr_*.c): run in place of the function at addr (an
 * override the translator could not do, or a library routine better done natively) */
typedef struct XrcManual { u32 addr; XFn fn; const char *name; } XrcManual;
extern const XrcManual xrc_manual_match[]; /* ff xrcmatch (build/xrc_gen/match_table.c) */
extern const unsigned xrc_manual_match_n;
u32 xrc_import_named(const char *name); /* an import's slot by its name only */
u32 xrc_import_named_n(const char *name, int nbytes); /* ... or _name@nbytes */
extern const XrcManual xrc_manual_crt_math[];
extern const XrcManual xrc_manual_crt_sort[];
extern const XrcManual xrc_manual_game[];
extern const XrcManual xrc_manual_simd[]; /* ovr_simd.c: the fast overrides */
extern const unsigned xrc_manual_simd_n;
extern const XrcManual xrc_manual_lr_crt[]; /* hle_msvcr100.c */
extern const unsigned xrc_manual_lr_crt_n;
extern const XrcManual xrc_manual_miles[]; /* ovr_miles.c: Miles code ff xrc leaves out */
extern const unsigned xrc_manual_miles_n;
extern const XrcManual xrc_manual_dbg[]; /* ovr_debug.c: the debug features (free camera) */
extern const unsigned xrc_manual_dbg_n;
extern const XrcManual xrc_manual_frame[]; /* ovr_frame.c: frame limiter wait, clamp counter */
extern const unsigned xrc_manual_frame_n;
extern const unsigned xrc_manual_game_n;
extern const unsigned xrc_manual_crt_sort_n;
extern int xrc_write_watch; /* (Windows) guest memory is MEM_WRITE_WATCH */
u32 xrc_stack_at(XCtx *c, int k); /* the k-th dispatched function of the thread's guest call stack, innermost 0 */
unsigned long long xrc_qpc(void); /* the guest's QueryPerformanceCounter (100 ns; XRC_TIME_SCALE applies) */
XFn xrc_translation_of(u32 addr); /* the translation of a guest function (also one a manual override replaces) */
extern const unsigned xrc_manual_crt_math_n;

#include "xrc_quiet.h" /* (-DXRC_NO_TOOLS: nothing printed) */
#endif
