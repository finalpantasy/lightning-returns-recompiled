/* xrc_quiet.h: with -DXRC_NO_TOOLS (the launcher's "Debug & Profile Tools: OFF"), nothing is printed -- every runtime
 * source gets this header through the runtime's own headers (at their end: xrc_rt.h, hle_d3d9.h, ref.h, host_*.h,
 * vk_sm3.h), after the system headers its file included first. Writes to stdout and
 * stderr are not made at all and their strings never formatted (the arguments are not even evaluated): each output
 * call checks its stream first (they are statements: no return value, except fwrite's count, which the runtime uses
 * for real files). Writes to real files go on as before.
 * (The Windows build is then also linked as a GUI program: no console window. FFmpeg's own log is turned off in
 * host_movie.c.) Without XRC_NO_TOOLS this header does nothing. */
#ifndef XRC_QUIET_H
#define XRC_QUIET_H
#ifdef XRC_NO_TOOLS
#include <stdarg.h>
#include <stdio.h>
/* the macros below are defined after these headers declared the functions, so their declarations stay intact; the
 * name inside a macro's own expansion is the real function (C does not expand a macro within itself) */
#define XRC_CONSOLE(f) ((f) == stderr || (f) == stdout)
#define printf(...) ((void)0)
#define puts(s) ((void)0)
#define perror(s) ((void)0)
#define fprintf(f, ...) (XRC_CONSOLE(f) ? (void)0 : (void)fprintf(f, __VA_ARGS__))
#define vfprintf(f, fmt, ap) (XRC_CONSOLE(f) ? (void)0 : (void)vfprintf(f, fmt, ap))
#define fputs(s, f) (XRC_CONSOLE(f) ? (void)0 : (void)fputs(s, f))
#define fputc(ch, f) (XRC_CONSOLE(f) ? (void)0 : (void)fputc(ch, f))
#define fwrite(p, sz, n, f) (XRC_CONSOLE(f) ? (size_t)(n) : fwrite(p, sz, n, f))
#define fflush(f) ((f) && XRC_CONSOLE(f) ? (void)0 : (void)fflush(f))
/* (write(2, ...): xrc_rt.c's signal-safe messages, xrc_errwrite there) */
#endif
#endif
