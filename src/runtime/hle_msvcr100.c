/* hle_msvcr100.c - the Visual C++ 2010 runtime (MSVCR100.dll, MSVCP100.dll) that Lightning Returns imports.
 *
 * FFXIII linked its C runtime statically, so the translator saw (and translated) it; LRFF13.exe imports it instead.
 * This file implements the imported names the game uses, over guest memory:
 *   - strings and memory: plain C, "C" locale, single-byte code page;
 *   - printf / scanf families: a formatter that reads the guest's varargs from the guest stack (or a va_list);
 *   - FILE streams: on top of the runtime's KERNEL32 HLE (CreateFileA ...), so paths map the same way;
 *   - CRT startup (__getmainargs, _initterm ...), atexit, threads (_beginthreadex = CreateThread);
 *   - x87 math helpers (_CIsin ...): arguments in c->fa[0..1] (st0, st1), result in c->st0, as ovr_crt_math.c;
 *   - the few MSVCP100 members the game calls (iterator-debugging containers, std::exception).
 * C++ exceptions (_CxxThrowException, __CxxFrameHandler3) stop the run: the runtime has no guest exception
 * dispatch, as for FFXIII.
 * Also the LR-address overrides for the CRT helpers linked into the exe (_ftol2, _ftol2_sse). */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "xrc_rt.h"

#define A(i) xrc_arg(c, (i))
#define S(a) ((char *)(xrc_mem + (u32)(a)))
#define G(p) ((u32)((const u8 *)(p) - xrc_mem)) /* a host pointer into guest memory -> its guest address */
#define CRET(v) do { c->eax = (u32)(v); xrc_cdecl_ret(c); return; } while (0)
#define CRET64(v) do { u64 v_ = (u64)(v); c->eax = (u32)v_; c->edx = (u32)(v_ >> 32); xrc_cdecl_ret(c); return; } while (0)
#define CRETF(v) do { c->st0 = (double)(v); xrc_cdecl_ret(c); return; } while (0)
#define TRET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0) /* thiscall: callee pops */
#define D(i) f64_of((u64)A(i) | (u64)A((i) + 1) << 32) /* a double argument at stack slots i, i+1 */

enum { EINVAL_ = 22, ERANGE_ = 34, ENOENT_ = 2, EBADF_ = 9 };
static u32 errno_var; /* guest address of errno (_errno()); 0 until first needed */
static void set_errno(int e) {
	if (!errno_var) errno_var = xrc_heap_alloc(4, 1);
	if (errno_var) M32(errno_var) = (u32)e;
}

/* ---------------------------------------------------------------------------------------- calling KERNEL32 HLE */
/* Calls the runtime's KERNEL32 import `name` with guest arguments. Out-parameters live in a scratch area carved
 * from the guest stack below the current esp (k32_scratch), released when the HLE function returns. */
u32 xrc_k32(XCtx *c, const char *name, int n, const u32 *args) {
	u32 fn = xrc_import_addr("KERNEL32.dll", name);
	if (!fn) xrc_fatal(c, "msvcr100: the runtime has no KERNEL32!%s", name);
	return xrc_call_guest(c, fn, n, args);
}
#define k32 xrc_k32
#define SCRATCH_BEGIN(c_, bytes_) u32 sp_save_ = (c_)->esp, scr_ = ((c_)->esp - (u32)(bytes_)) & ~15u; (c_)->esp = scr_
#define SCRATCH_END(c_) (c_)->esp = sp_save_

/* ------------------------------------------------------------------------------------------- guest printf */
typedef struct Out { char *p; size_t n, cap; } Out;
static void out_put(Out *o, const char *s, size_t n) {
	if (o->n + n + 1 > o->cap) {
		o->cap = (o->n + n + 1) * 2 + 64;
		o->p = (char *)realloc(o->p, o->cap);
	}
	memcpy(o->p + o->n, s, n);
	o->n += n;
	o->p[o->n] = 0;
}
/* Formats guest format string f with the arguments starting at guest address ap (4-byte slots). The result goes to
 * o (host memory, NUL-terminated). Follows MSVC's printf: I64 / I32 / I sizes, %p as 8 hex digits, %S = the other
 * width's string, %n stores the count so far. */
static void gfmt(const char *f, u32 ap, Out *o) {
	char spec[64], tmp[512];
	while (*f) {
		const char *pct = strchr(f, '%');
		if (!pct) { out_put(o, f, strlen(f)); break; }
		out_put(o, f, (size_t)(pct - f));
		f = pct + 1;
		if (*f == '%') { out_put(o, "%", 1); f++; continue; }
		/* flags */
		size_t k = 0;
		spec[k++] = '%';
		while (*f && strchr("-+ #0", *f)) { if (k < 16) spec[k++] = *f; f++; }
		/* width */
		int width = -1, prec = -1;
		if (*f == '*') { width = (int)M32(ap); ap += 4; f++; if (width < 0) { spec[k++] = '-'; width = -width; } }
		else if (isdigit((unsigned char)*f)) { width = 0; while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0'); }
		if (*f == '.') {
			f++;
			prec = 0;
			if (*f == '*') { prec = (int)M32(ap); ap += 4; f++; }
			else while (isdigit((unsigned char)*f)) prec = prec * 10 + (*f++ - '0');
		}
		if (width >= 0) k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", width);
		if (prec >= 0) k += (size_t)snprintf(spec + k, sizeof spec - k, ".%d", prec);
		/* size */
		int sz = 0; /* -2 hh, -1 h, 0 int, 1 l, 2 64-bit, 3 long double */
		for (;;) {
			if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { sz = 2; f += 3; }
			else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') { sz = 0; f += 3; }
			else if (f[0] == 'I') { sz = 0; f++; }
			else if (f[0] == 'l' && f[1] == 'l') { sz = 2; f += 2; }
			else if (f[0] == 'h' && f[1] == 'h') { sz = -2; f += 2; }
			else if (f[0] == 'h') { sz = -1; f++; }
			else if (f[0] == 'l') { sz = 1; f++; }
			else if (f[0] == 'L') { sz = 3; f++; }
			else if (f[0] == 'w') { sz = 1; f++; }
			else break;
		}
		char conv = *f;
		if (!conv) break;
		f++;
		int n = 0;
		switch (conv) {
		case 'd': case 'i': {
			long long v;
			if (sz == 2) { v = (long long)M64(ap); ap += 8; }
			else { v = (s32)M32(ap); ap += 4; if (sz == -1) v = (short)v; if (sz == -2) v = (signed char)v; }
			spec[k] = 'l'; spec[k + 1] = 'l'; spec[k + 2] = 'd'; spec[k + 3] = 0;
			n = snprintf(tmp, sizeof tmp, spec, v);
			break;
		}
		case 'u': case 'o': case 'x': case 'X': {
			unsigned long long v;
			if (sz == 2) { v = M64(ap); ap += 8; }
			else { v = M32(ap); ap += 4; if (sz == -1) v = (u16)v; if (sz == -2) v = (u8)v; }
			spec[k] = 'l'; spec[k + 1] = 'l'; spec[k + 2] = conv; spec[k + 3] = 0;
			n = snprintf(tmp, sizeof tmp, spec, v);
			break;
		}
		case 'p': {
			u32 v = M32(ap);
			ap += 4;
			n = snprintf(tmp, sizeof tmp, "%08X", v);
			break;
		}
		case 'c': case 'C': {
			u32 v = M32(ap);
			ap += 4;
			char ch = (char)((conv == 'C' || sz == 1) && v > 0xff ? '?' : v);
			spec[k] = 'c'; spec[k + 1] = 0;
			n = snprintf(tmp, sizeof tmp, spec, ch);
			break;
		}
		case 's': case 'S': case 'Z': {
			u32 v = M32(ap);
			ap += 4;
			int wide = conv == 'S' ? sz != -1 : sz == 1;
			char *s;
			if (!v) s = strdup(wide ? "(null)" : "(null)");
			else if (wide) { /* UTF-16 guest string -> bytes (non-Latin-1 as '?') */
				size_t len = 0, i;
				while (M16(v + 2 * len)) len++;
				if (prec >= 0 && (size_t)prec < len) len = (size_t)prec;
				s = (char *)malloc(len + 1);
				for (i = 0; i < len; i++) { u16 w = M16(v + 2 * i); s[i] = (char)(w > 0xff ? '?' : w); }
				s[len] = 0;
			} else {
				size_t len = 0;
				while (S(v)[len] && (prec < 0 || len < (size_t)prec)) len++;
				s = (char *)malloc(len + 1);
				memcpy(s, S(v), len);
				s[len] = 0;
			}
			size_t len = strlen(s);
			if (width > 0 && (size_t)width > len) {
				size_t pad = (size_t)width - len, i;
				int left = strchr(spec, '-') != 0;
				if (left) out_put(o, s, len);
				for (i = 0; i < pad; i++) out_put(o, " ", 1);
				if (!left) out_put(o, s, len);
			} else out_put(o, s, len);
			free(s);
			continue;
		}
		case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
			double v = f64_of(M64(ap));
			ap += 8;
			spec[k] = conv; spec[k + 1] = 0;
			if (((u64_of(v) >> 52) & 0x7ff) == 0x7ff) { /* (bits: -ffast-math assumes no NaN / infinity) */ /* MSVC 2010 spells them 1.#QNAN0 / 1.#INF00 */
				const char *t = xrc_isnan(v) ? ((u64_of(v) >> 63) ? "-1.#IND00" : "1.#QNAN0") : (v < 0 ? "-1.#INF00" : "1.#INF00");
				n = snprintf(tmp, sizeof tmp, "%*s", width > 0 ? (strchr(spec, '-') ? -width : width) : 0, t);
			} else n = snprintf(tmp, sizeof tmp, spec, v);
			break;
		}
		case 'n': {
			u32 v = M32(ap);
			ap += 4;
			if (v) M32(v) = (u32)o->n;
			continue;
		}
		default: /* unknown: print it as is (as the CRT's invalid-format handling does not, but harmless) */
			tmp[0] = '%'; tmp[1] = conv; n = 2;
			break;
		}
		if (n < 0) n = 0;
		if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
		out_put(o, tmp, (size_t)n);
	}
	if (!o->p) out_put(o, "", 0);
}

/* ---------------------------------------------------------------------------------------------- guest scanf */
/* Reads in (narrow, or UTF-16 when win) with guest format fmt (same width as in), storing through the pointers at
 * guest address ap. secure: the _s form (%s %c %[ take a buffer size after the pointer). Returns the count of
 * assigned fields, or -1 when the input ends before the first conversion. */
static int gscan(XCtx *c, u32 in, u32 fmt, u32 ap, int win, int secure) {
#define IN(i) (win ? (u32)M16(in + 2u * (i)) : (u32)(u8)M8(in + (i)))
#define FM(i) (win ? (u32)M16(fmt + 2u * (i)) : (u32)(u8)M8(fmt + (i)))
	u32 ip = 0, fp = 0;
	int count = 0, any = 0;
	(void)c;
	while (FM(fp)) {
		u32 fc = FM(fp);
		if (fc < 128 && isspace((int)fc)) {
			while (FM(fp) && FM(fp) < 128 && isspace((int)FM(fp))) fp++;
			while (IN(ip) && IN(ip) < 128 && isspace((int)IN(ip))) ip++;
			continue;
		}
		if (fc != '%' || FM(fp + 1) == '%') {
			if (fc == '%') fp++;
			if (IN(ip) != fc) break;
			ip++; fp++;
			continue;
		}
		fp++;
		int suppress = 0, width = 0, sz = 0;
		if (FM(fp) == '*') { suppress = 1; fp++; }
		while (FM(fp) >= '0' && FM(fp) <= '9') width = width * 10 + (int)(FM(fp++) - '0');
		for (;;) {
			if (FM(fp) == 'I' && FM(fp + 1) == '6' && FM(fp + 2) == '4') { sz = 2; fp += 3; }
			else if (FM(fp) == 'l' && FM(fp + 1) == 'l') { sz = 2; fp += 2; }
			else if (FM(fp) == 'h' && FM(fp + 1) == 'h') { sz = -2; fp += 2; }
			else if (FM(fp) == 'h') { sz = -1; fp++; }
			else if (FM(fp) == 'l' || FM(fp) == 'L') { sz = 1; fp++; }
			else break;
		}
		u32 conv = FM(fp++);
		if (!conv) break;
		if (conv == 'n') {
			if (!suppress) { M32(M32(ap)) = ip; ap += 4; }
			continue;
		}
		if (conv != 'c' && conv != '[')
			while (IN(ip) && IN(ip) < 128 && isspace((int)IN(ip))) ip++;
		if (!IN(ip)) { if (!any) return -1; break; }
		any = 1;
		u32 dst = suppress ? 0 : M32(ap);
		u32 cap = ~0u;
		if (!suppress) { ap += 4; if (secure && (conv == 's' || conv == 'c' || conv == '[')) { cap = M32(ap); ap += 4; } }
		if (conv == 'c' || conv == 's' || conv == '[' || conv == 'C' || conv == 'S') {
			int wout = (conv == 'C' || conv == 'S') ? !win : (win ? sz != -1 : sz == 1);
			char set[256];
			int neg = 0;
			if (conv == '[') { /* scanset */
				memset(set, 0, sizeof set);
				if (FM(fp) == '^') { neg = 1; fp++; }
				if (FM(fp) == ']') { set[']'] = 1; fp++; }
				while (FM(fp) && FM(fp) != ']') {
					u32 a = FM(fp);
					if (FM(fp + 1) == '-' && FM(fp + 2) && FM(fp + 2) != ']') { u32 b = FM(fp + 2), x; for (x = a; x <= b && x < 256; x++) set[x] = 1; fp += 3; }
					else { if (a < 256) set[a] = 1; fp++; }
				}
				if (FM(fp) == ']') fp++;
			}
			u32 lim = (u32)(width ? width : (conv == 'c' || conv == 'C') ? 1 : 0x7fffffff), n = 0;
			while (IN(ip) && n < lim) {
				u32 ch = IN(ip);
				if ((conv == 's' || conv == 'S') && ch < 128 && isspace((int)ch)) break;
				if (conv == '[' && ((ch < 256 && set[ch]) == neg)) break;
				if (dst) {
					if (n + ((conv == 'c' || conv == 'C') ? 0u : 1u) >= cap) { /* buffer too small: the field fails */
						if (wout) M16(dst) = 0; else M8(dst) = 0;
						return count;
					}
					if (wout) M16(dst + 2 * n) = (u16)ch; else M8(dst + n) = (u8)(ch > 0xff ? '?' : ch);
				}
				n++; ip++;
			}
			if (!n) break;
			if (dst && conv != 'c' && conv != 'C') { if (wout) M16(dst + 2 * n) = 0; else M8(dst + n) = 0; }
			if (dst) count++;
			continue;
		}
		/* numbers: copy the field (up to width) to a host buffer and parse it with strtoll / strtod */
		char buf[128];
		u32 n = 0, lim = (u32)(width ? width : 127);
		while (IN(ip) && n < lim && n < 127) {
			u32 ch = IN(ip);
			if (ch > 127 || isspace((int)ch)) break;
			buf[n++] = (char)ch;
			ip++;
		}
		buf[n] = 0;
		char *end = buf;
		if (conv == 'e' || conv == 'f' || conv == 'g' || conv == 'E' || conv == 'G' || conv == 'a') {
			double v = strtod(buf, &end);
			if (end == buf) break;
			if (dst) { if (sz == 1) M64(dst) = (u64)u64_of(v); else { float fv = (float)v; memcpy(xrc_mem + dst, &fv, 4); } count++; }
		} else {
			int base = conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
			long long v = (conv == 'd' || conv == 'i') ? strtoll(buf, &end, base) : (long long)strtoull(buf, &end, base);
			if (end == buf) break;
			if (dst) {
				if (sz == 2) M64(dst) = (u64)v;
				else if (sz == -1) M16(dst) = (u16)v;
				else if (sz == -2) M8(dst) = (u8)v;
				else M32(dst) = (u32)v;
				count++;
			}
		}
		ip -= n - (u32)(end - buf); /* give back what the parser did not take */
	}
	return count;
#undef IN
#undef FM
}

/* ------------------------------------------------------------------------------------------------- FILEs */
/* A guest FILE is a 32-byte struct (VC10 layout); the stream behind it is a host-side record, its index in the
 * _tmpfname field (+28). The first three (__iob_func) are stdin / stdout / stderr. */
#define NFILE 256
typedef struct Stream {
	u32 guest, h;         /* the guest FILE, the KERNEL32 handle (0: a host std stream) */
	FILE *std;            /* stdin / stdout / stderr */
	int text, append, eof, err, used;
	u8 buf[4096];         /* read-ahead */
	u32 pos, len;
} Stream;
static Stream streams[NFILE];
static u32 iob; /* guest address of the 3 standard FILEs */
static void iob_init(void) {
	int i;
	if (iob) return;
	iob = xrc_heap_alloc(3 * 32, 1);
	for (i = 0; i < 3; i++) {
		streams[i].guest = iob + 32u * (u32)i;
		streams[i].std = i == 0 ? stdin : i == 1 ? stdout : stderr;
		streams[i].used = 1;
		streams[i].text = 1;
		M32(streams[i].guest + 16) = (u32)i;  /* _file */
		M32(streams[i].guest + 28) = (u32)i;  /* stream index */
	}
}
static Stream *stream_of(XCtx *c, u32 f) {
	u32 i;
	iob_init();
	if (!f) xrc_fatal(c, "msvcr100: NULL FILE*");
	i = M32(f + 28);
	if (i >= NFILE || !streams[i].used || streams[i].guest != f) xrc_fatal(c, "msvcr100: %08x is not an open FILE", f);
	return &streams[i];
}
static u32 set_ptr(XCtx *c, u32 h, s32 off, u32 method) {
	u32 a[4] = {h, (u32)off, 0, method};
	return k32(c, "SetFilePointer", 4, a);
}
/* the read-ahead: give back what was buffered and not consumed, so the handle's position is the stream's */
static void unbuffer(XCtx *c, Stream *s) {
	if (s->h && s->pos < s->len) set_ptr(c, s->h, -(s32)(s->len - s->pos), 1);
	s->pos = s->len = 0;
}
static int fill(XCtx *c, Stream *s) {
	if (s->std) {
		size_t n = fread(s->buf, 1, sizeof s->buf, s->std);
		s->pos = 0; s->len = (u32)n;
		if (!n) s->eof = 1;
		return n > 0;
	}
	SCRATCH_BEGIN(c, 64);
	u32 bufg = xrc_heap_alloc(sizeof s->buf, 0), a[5] = {s->h, bufg, (u32)sizeof s->buf, scr_, 0}, ok, got;
	M32(scr_) = 0;
	ok = k32(c, "ReadFile", 5, a);
	got = M32(scr_);
	SCRATCH_END(c);
	if (got) memcpy(s->buf, xrc_mem + bufg, got);
	xrc_heap_free(bufg);
	s->pos = 0; s->len = ok ? got : 0;
	if (!ok) s->err = 1;
	if (!s->len) s->eof = 1;
	return s->len > 0;
}
/* the next byte (-1 at the end); text mode drops the CR of CR LF and stops at Ctrl-Z */
static int getb(XCtx *c, Stream *s) {
	int b;
	for (;;) {
		if (s->pos == s->len && !fill(c, s)) return -1;
		b = s->buf[s->pos++];
		if (!s->text) return b;
		if (b == 0x1a) { s->eof = 1; s->pos--; return -1; }
		if (b == '\r') {
			if (s->pos == s->len && !fill(c, s)) return '\r';
			if (s->buf[s->pos] == '\n') continue;
		}
		return b;
	}
}
static u32 writeb(XCtx *c, Stream *s, const u8 *p, u32 n) {
	if (s->std) return (u32)fwrite(p, 1, n, s->std);
	unbuffer(c, s);
	if (s->append) set_ptr(c, s->h, 0, 2);
	u8 *t = (u8 *)p;
	u32 m = n, k, j = 0;
	if (s->text) { /* LF -> CR LF */
		for (k = 0; k < n; k++) j += p[k] == '\n';
		t = (u8 *)malloc(n + j + 1);
		for (k = 0, m = 0; k < n; k++) { if (p[k] == '\n') t[m++] = '\r'; t[m++] = p[k]; }
	}
	SCRATCH_BEGIN(c, 64);
	u32 bufg = xrc_heap_alloc(m ? m : 1, 0), a[5] = {s->h, bufg, m, scr_, 0}, ok, put;
	memcpy(xrc_mem + bufg, t, m);
	M32(scr_) = 0;
	ok = k32(c, "WriteFile", 5, a);
	put = M32(scr_);
	SCRATCH_END(c);
	xrc_heap_free(bufg);
	if (t != p) free(t);
	if (!ok || put != m) { s->err = 1; return 0; }
	return n;
}
/* fopen's mode -> a stream on a new handle (0: failed, errno set) */
static u32 open_stream(XCtx *c, u32 name, const char *mode) {
	u32 access = 0, disp = 0, h, i, gf;
	int plus = strchr(mode, '+') != 0, text = !strchr(mode, 'b');
	iob_init();
	switch (mode[0]) {
	case 'r': access = 0x80000000u; disp = 3; break;              /* GENERIC_READ, OPEN_EXISTING */
	case 'w': access = 0x40000000u; disp = 2; break;              /* GENERIC_WRITE, CREATE_ALWAYS */
	case 'a': access = 0x40000000u; disp = 4; break;              /* OPEN_ALWAYS */
	default: set_errno(EINVAL_); return 0;
	}
	if (plus) access = 0xc0000000u;
	for (i = 3; i < NFILE && streams[i].used; i++) {}
	if (i == NFILE) { set_errno(24 /* EMFILE */); return 0; }
	{
		u32 a[7] = {name, access, 3 /* share read | write */, 0, disp, 0x80 /* NORMAL */, 0};
		h = k32(c, "CreateFileA", 7, a);
	}
	if (h == 0xffffffffu) { set_errno(ENOENT_); return 0; }
	gf = xrc_heap_alloc(32, 1);
	memset(&streams[i], 0, sizeof streams[i]);
	streams[i].guest = gf; streams[i].h = h; streams[i].used = 1;
	streams[i].text = text; streams[i].append = mode[0] == 'a';
	M32(gf + 16) = i; M32(gf + 28) = i;
	return gf;
}
static void hle_fopen(XCtx *c) { CRET(open_stream(c, A(0), S(A(1)))); }
static void hle_fopen_s(XCtx *c) {
	u32 pf = A(0), f;
	if (!pf || !A(1) || !A(2)) CRET(EINVAL_);
	f = open_stream(c, A(1), S(A(2)));
	M32(pf) = f;
	CRET(f ? 0 : (errno_var ? M32(errno_var) : ENOENT_));
}
static void hle_fclose(XCtx *c) {
	Stream *s = stream_of(c, A(0));
	if (s->std) { fflush(s->std); CRET(0); }
	{ u32 a[1] = {s->h}; k32(c, "CloseHandle", 1, a); }
	xrc_heap_free(s->guest);
	memset(s, 0, sizeof *s);
	CRET(0);
}
static void hle_fread(XCtx *c) {
	u32 dst = A(0), size = A(1), count = A(2), total = size * count, n = 0;
	Stream *s = stream_of(c, A(3));
	if (!size || !count) CRET(0);
	if (!s->text) { /* binary: buffered bytes, then straight from the file */
		while (n < total) {
			if (s->pos == s->len && !fill(c, s)) break;
			u32 k = s->len - s->pos;
			if (k > total - n) k = total - n;
			memcpy(xrc_mem + dst + n, s->buf + s->pos, k);
			s->pos += k; n += k;
		}
	} else {
		int b;
		while (n < total && (b = getb(c, s)) >= 0) M8(dst + n++) = (u8)b;
	}
	CRET(n / size);
}
static void hle_fwrite(XCtx *c) {
	u32 size = A(1), count = A(2);
	Stream *s = stream_of(c, A(3));
	if (!size || !count) CRET(0);
	CRET(writeb(c, s, xrc_mem + A(0), size * count) / size);
}
static void hle_fputs(XCtx *c) {
	Stream *s = stream_of(c, A(1));
	u32 n = (u32)strlen(S(A(0)));
	CRET(writeb(c, s, xrc_mem + A(0), n) == n ? 0 : 0xffffffffu);
}
static void hle_fgets(XCtx *c) {
	u32 buf = A(0), n = A(1), k = 0;
	Stream *s = stream_of(c, A(2));
	int b = 0;
	if ((s32)n <= 0) CRET(0);
	while (k + 1 < n && (b = getb(c, s)) >= 0) { M8(buf + k++) = (u8)b; if (b == '\n') break; }
	M8(buf + k) = 0;
	CRET(k ? buf : 0);
}
static void hle_ftell(XCtx *c) {
	Stream *s = stream_of(c, A(0));
	if (s->std) CRET(0xffffffffu);
	CRET(set_ptr(c, s->h, 0, 1) - (s->len - s->pos));
}
static void hle_fseek(XCtx *c) {
	Stream *s = stream_of(c, A(0));
	if (s->std || A(2) > 2) CRET(0xffffffffu);
	if (A(2) == 1) unbuffer(c, s); else s->pos = s->len = 0;
	s->eof = 0;
	CRET(set_ptr(c, s->h, (s32)A(1), A(2)) == 0xffffffffu ? 0xffffffffu : 0);
}
static Stream *stream_of_fd(XCtx *c, u32 fd) {
	iob_init();
	if (fd >= NFILE || !streams[fd].used) { set_errno(EBADF_); return 0; }
	(void)c;
	return &streams[fd];
}
static void hle__fileno(XCtx *c) { CRET(M32(stream_of(c, A(0))->guest + 16)); }
static void hle__filelength(XCtx *c) {
	Stream *s = stream_of_fd(c, A(0));
	u32 cur, end;
	if (!s || s->std) CRET(0xffffffffu);
	cur = set_ptr(c, s->h, 0, 1);
	end = set_ptr(c, s->h, 0, 2);
	set_ptr(c, s->h, (s32)cur, 0);
	CRET(end);
}
static void hle___iob_func(XCtx *c) { iob_init(); CRET(iob); }

/* write a formatted host string to a stream */
static void put_out(XCtx *c, Stream *s, Out *o) { writeb(c, s, (const u8 *)o->p, (u32)o->n); }
static void hle_printf(XCtx *c) {
	Out o = {0};
	gfmt(S(A(0)), c->esp + 8, &o);
	fwrite(o.p, 1, o.n, stdout);
	free(o.p);
	CRET(o.n);
}
static void hle_fprintf(XCtx *c) {
	Out o = {0};
	Stream *s = stream_of(c, A(0));
	gfmt(S(A(1)), c->esp + 12, &o);
	put_out(c, s, &o);
	free(o.p);
	CRET(o.n);
}

/* --------------------------------------------------------------------------------------- sprintf family */
/* the result of a bounded print into guest buffer buf of cap bytes (cap counts the NUL) */
static void store(u32 buf, const Out *o, u32 n) { memcpy(xrc_mem + buf, o->p, n); }
static void hle_sprintf(XCtx *c) {
	Out o = {0};
	gfmt(S(A(1)), c->esp + 12, &o);
	store(A(0), &o, (u32)o.n + 1);
	free(o.p);
	CRET(o.n);
}
static void hle_vsprintf(XCtx *c) {
	Out o = {0};
	gfmt(S(A(1)), A(2), &o);
	store(A(0), &o, (u32)o.n + 1);
	free(o.p);
	CRET(o.n);
}
/* sprintf_s(buf, size, fmt, ...): a result that does not fit is an invalid-parameter error (the CRT terminates) */
static void print_s(XCtx *c, u32 buf, u32 size, u32 fmt, u32 ap) {
	Out o = {0};
	if (!buf || !size || !fmt) xrc_fatal(c, "msvcr100: sprintf_s: invalid parameter");
	gfmt(S(fmt), ap, &o);
	if (o.n + 1 > size) xrc_fatal(c, "msvcr100: sprintf_s: buffer of %u bytes too small for \"%.80s\"", size, o.p);
	store(buf, &o, (u32)o.n + 1);
	free(o.p);
	CRET(o.n);
}
static void hle_sprintf_s(XCtx *c) { print_s(c, A(0), A(1), A(2), c->esp + 16); }
static void hle_vsprintf_s(XCtx *c) { print_s(c, A(0), A(1), A(2), A(3)); }
/* _snprintf(buf, count, fmt, ...): count bytes at most; NUL only when it fits; -1 when the result is longer */
static void nprint(XCtx *c, u32 buf, u32 count, u32 fmt, u32 ap) {
	Out o = {0};
	gfmt(S(fmt), ap, &o);
	if (o.n < count) store(buf, &o, (u32)o.n + 1);
	else store(buf, &o, count);
	free(o.p);
	CRET(o.n <= count ? (u32)o.n : 0xffffffffu);
}
static void hle__snprintf(XCtx *c) { nprint(c, A(0), A(1), A(2), c->esp + 16); }
static void hle__vsnprintf(XCtx *c) { nprint(c, A(0), A(1), A(2), A(3)); }
/* _snprintf_s(buf, size, count, fmt, ...): count == _TRUNCATE fits what it can and returns -1 when cut */
static void nprint_s(XCtx *c, u32 buf, u32 size, u32 count, u32 fmt, u32 ap) {
	Out o = {0};
	u32 lim;
	if (!buf || !size || !fmt) xrc_fatal(c, "msvcr100: _snprintf_s: invalid parameter");
	gfmt(S(fmt), ap, &o);
	lim = count == 0xffffffffu ? size - 1 : count < size ? count : size - 1;
	if (o.n <= lim) { store(buf, &o, (u32)o.n + 1); free(o.p); CRET(o.n); }
	if (count == 0xffffffffu || count < size) { store(buf, &o, lim); M8(buf + lim) = 0; free(o.p); CRET(0xffffffffu); }
	xrc_fatal(c, "msvcr100: _snprintf_s: buffer of %u bytes too small for \"%.80s\"", size, o.p);
}
static void hle__snprintf_s(XCtx *c) { nprint_s(c, A(0), A(1), A(2), A(3), c->esp + 20); }
static void hle__vsnprintf_s(XCtx *c) { nprint_s(c, A(0), A(1), A(2), A(3), A(4)); }
static void hle_sscanf_s(XCtx *c) { CRET((u32)gscan(c, A(0), A(1), c->esp + 12, 0, 1)); }
static void hle_swscanf_s(XCtx *c) { CRET((u32)gscan(c, A(0), A(1), c->esp + 12, 1, 1)); }

/* ---------------------------------------------------------------------------------------- strings, memory */
static int lc(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
static void hle_memcpy(XCtx *c) { memmove(xrc_mem + A(0), xrc_mem + A(1), A(2)); CRET(A(0)); }
static void hle_memmove(XCtx *c) { memmove(xrc_mem + A(0), xrc_mem + A(1), A(2)); CRET(A(0)); }
static void hle_memset(XCtx *c) { memset(xrc_mem + A(0), (int)A(1), A(2)); CRET(A(0)); }
static void hle_memcmp(XCtx *c) { int r = memcmp(xrc_mem + A(0), xrc_mem + A(1), A(2)); CRET(r < 0 ? -1 : r > 0); }
static void hle_memchr(XCtx *c) { const u8 *p = (const u8 *)memchr(xrc_mem + A(0), (int)(u8)A(1), A(2)); CRET(p ? G(p) : 0); }
static void hle_memcpy_s(XCtx *c) {
	if (!A(3)) CRET(0);
	if (!A(0) || !A(2) || A(1) < A(3)) { if (A(0) && A(1)) memset(xrc_mem + A(0), 0, A(1)); xrc_fatal(c, "msvcr100: memcpy_s: invalid parameter (dst %u bytes, count %u)", A(1), A(3)); }
	memmove(xrc_mem + A(0), xrc_mem + A(2), A(3));
	CRET(0);
}
static void hle_memmove_s(XCtx *c) {
	if (!A(3)) CRET(0);
	if (!A(0) || !A(2) || A(1) < A(3)) xrc_fatal(c, "msvcr100: memmove_s: invalid parameter (dst %u bytes, count %u)", A(1), A(3));
	memmove(xrc_mem + A(0), xrc_mem + A(2), A(3));
	CRET(0);
}
static void hle_strcpy(XCtx *c) { memmove(S(A(0)), S(A(1)), strlen(S(A(1))) + 1); CRET(A(0)); }
static void hle_strncpy(XCtx *c) {
	u32 d = A(0), s = A(1), n = A(2), k = 0;
	for (; k < n && M8(s + k); k++) M8(d + k) = M8(s + k);
	for (; k < n; k++) M8(d + k) = 0;
	CRET(d);
}
static void hle_strncat(XCtx *c) {
	u32 d = A(0), s = A(1), n = A(2), e = d + (u32)strlen(S(d)), k = 0;
	for (; k < n && M8(s + k); k++) M8(e + k) = M8(s + k);
	M8(e + k) = 0;
	CRET(d);
}
static void hle_strcpy_s(XCtx *c) {
	u32 n = (u32)strlen(S(A(2)));
	if (!A(0) || !A(1) || !A(2)) xrc_fatal(c, "msvcr100: strcpy_s: invalid parameter");
	if (n + 1 > A(1)) xrc_fatal(c, "msvcr100: strcpy_s: \"%.80s\" does not fit %u bytes", S(A(2)), A(1));
	memmove(S(A(0)), S(A(2)), n + 1);
	CRET(0);
}
static void hle_strcat_s(XCtx *c) {
	u32 have, n;
	if (!A(0) || !A(1) || !A(2)) xrc_fatal(c, "msvcr100: strcat_s: invalid parameter");
	have = (u32)strnlen(S(A(0)), A(1));
	n = (u32)strlen(S(A(2)));
	if (have == A(1) || have + n + 1 > A(1)) xrc_fatal(c, "msvcr100: strcat_s: \"%.40s\" + \"%.40s\" does not fit %u bytes", S(A(0)), S(A(2)), A(1));
	memmove(S(A(0)) + have, S(A(2)), n + 1);
	CRET(0);
}
static void hle_strncmp(XCtx *c) { int r = strncmp(S(A(0)), S(A(1)), A(2)); CRET(r < 0 ? -1 : r > 0); }
static void hle__stricmp(XCtx *c) {
	const u8 *a = (const u8 *)S(A(0)), *b = (const u8 *)S(A(1));
	int x, y;
	do { x = lc(*a++); y = lc(*b++); } while (x && x == y);
	CRET((u32)(x - y));
}
static void hle__strnicmp(XCtx *c) {
	const u8 *a = (const u8 *)S(A(0)), *b = (const u8 *)S(A(1));
	u32 n = A(2);
	int x = 0, y = 0;
	while (n--) { x = lc(*a++); y = lc(*b++); if (!x || x != y) break; }
	CRET((u32)(x - y));
}
static void hle_strchr(XCtx *c) { const char *p = strchr(S(A(0)), (int)(char)A(1)); CRET(p ? G(p) : 0); }
static void hle_strrchr(XCtx *c) { const char *p = strrchr(S(A(0)), (int)(char)A(1)); CRET(p ? G(p) : 0); }
static void hle_strstr(XCtx *c) { const char *p = strstr(S(A(0)), S(A(1))); CRET(p ? G(p) : 0); }
static void hle__mbsrchr(XCtx *c) { const char *p = strrchr(S(A(0)), (int)A(1)); CRET(p ? G(p) : 0); }
static void hle__mbsinc(XCtx *c) { CRET(A(0) + 1); }
static void hle__ismbblead(XCtx *c) { CRET(0); }
static void hle_wcsstr(XCtx *c) {
	u32 s = A(0), t = A(1), k;
	if (!M16(t)) CRET(s);
	for (; M16(s); s += 2) {
		for (k = 0; M16(t + 2 * k) && M16(s + 2 * k) == M16(t + 2 * k); k++) {}
		if (!M16(t + 2 * k)) CRET(s);
	}
	CRET(0);
}
/* "C" locale ctype; the CRT returns the class bit (_UPPER 1, _DIGIT 4) */
static void hle_isupper(XCtx *c) { CRET(A(0) >= 'A' && A(0) <= 'Z' ? 1 : 0); }
static void hle_isdigit(XCtx *c) { CRET(A(0) >= '0' && A(0) <= '9' ? 4 : 0); }
static void hle_toupper(XCtx *c) { CRET(A(0) >= 'a' && A(0) <= 'z' ? A(0) - 32 : A(0)); }
static void hle_abs(XCtx *c) { CRET((s32)A(0) < 0 ? 0u - A(0) : A(0)); }
static void hle_atoi(XCtx *c) { CRET((u32)strtol(S(A(0)), 0, 10)); }
static void hle__atoi64(XCtx *c) { CRET64(strtoll(S(A(0)), 0, 10)); }
/* atof: decimal only (the 2010 CRT reads no "inf", "nan" or hex) */
static void hle_atof(XCtx *c) {
	const char *s = S(A(0)), *p = s;
	while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
	if (*p == '+' || *p == '-') p++;
	if (!(isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1])))) CRETF(0.0);
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) CRETF(0.0);
	CRETF(strtod(s, 0));
}

/* ------------------------------------------------------------------------------------------------ heap */
static void hle_malloc(XCtx *c) { u32 p = xrc_heap_alloc(A(0) ? A(0) : 1, 0); if (!p) set_errno(12); CRET(p); }
static void hle_calloc(XCtx *c) {
	u64 n = (u64)A(0) * A(1);
	if (n > 0x7fffffffu) { set_errno(12); CRET(0); }
	CRET(xrc_heap_alloc(n ? (u32)n : 1, 1));
}
static void hle_free(XCtx *c) { if (A(0) && !xrc_heap_free(A(0))) xrc_fatal(c, "msvcr100: free(%08x): not a heap block", A(0)); CRET(0); }
/* _aligned_malloc: the block's start is kept in the word below the aligned pointer */
static void hle__aligned_malloc(XCtx *c) {
	u32 size = A(0), align = A(1) < 4 ? 4 : A(1), raw, p;
	if (align & (align - 1)) { set_errno(EINVAL_); CRET(0); }
	raw = xrc_heap_alloc(size + align + 4, 0);
	if (!raw) CRET(0);
	p = (raw + 4 + align - 1) & ~(align - 1);
	M32(p - 4) = raw;
	CRET(p);
}
static void hle__aligned_free(XCtx *c) { if (A(0)) xrc_heap_free(M32(A(0) - 4)); CRET(0); }

/* ------------------------------------------------------------------------------------------- sort, search */
static XCtx *cmp_ctx;
static u32 cmp_fn;
static int gcmp(u32 a, u32 b) { u32 args[2] = {a, b}; return (int)xrc_call_guest(cmp_ctx, cmp_fn, 2, args); }
static void swap_w(u32 a, u32 b, u32 w) {
	u8 t;
	if (a == b) return;
	while (w--) { t = M8(a); M8(a++) = M8(b); M8(b++) = t; }
}
static void shortsort(u32 lo, u32 hi, u32 w) {
	u32 p, max;
	while (hi > lo) {
		max = lo;
		for (p = lo + w; p <= hi; p += w) if (gcmp(p, max) > 0) max = p;
		swap_w(max, hi, w);
		hi -= w;
	}
}
/* qsort: the CRT's algorithm (median-of-three quicksort, short runs by selection), so equal elements end up where
 * the original puts them */
static void hle_qsort(XCtx *c) {
	u32 base = A(0), num = A(1), w = A(2), lo, hi, mid, loguy, higuy, lostk[32], histk[32];
	XCtx *save_c = cmp_ctx;
	u32 save_f = cmp_fn;
	int sp = 0;
	if (num < 2 || !w) CRET(0);
	cmp_ctx = c; cmp_fn = A(3);
	lo = base;
	hi = base + w * (num - 1);
	for (;;) {
		u32 size = (hi - lo) / w + 1;
		if (size <= 8) shortsort(lo, hi, w);
		else {
			mid = lo + (size / 2) * w;
			if (gcmp(lo, mid) > 0) swap_w(lo, mid, w);
			if (gcmp(lo, hi) > 0) swap_w(lo, hi, w);
			if (gcmp(mid, hi) > 0) swap_w(mid, hi, w);
			loguy = lo;
			higuy = hi;
			for (;;) {
				if (mid > loguy) do loguy += w; while (loguy < mid && gcmp(loguy, mid) <= 0);
				if (mid <= loguy) do loguy += w; while (loguy <= hi && gcmp(loguy, mid) <= 0);
				do higuy -= w; while (higuy > mid && gcmp(higuy, mid) > 0);
				if (higuy < loguy) break;
				swap_w(loguy, higuy, w);
				if (mid == higuy) mid = loguy;
			}
			higuy += w;
			if (mid < higuy) do higuy -= w; while (higuy > mid && gcmp(higuy, mid) == 0);
			if (mid >= higuy) do higuy -= w; while (higuy > lo && gcmp(higuy, mid) == 0);
			if (higuy - lo >= hi - loguy) {
				if (lo < higuy) { lostk[sp] = lo; histk[sp] = higuy; sp++; }
				if (loguy < hi) { lo = loguy; continue; }
			} else {
				if (loguy < hi) { lostk[sp] = loguy; histk[sp] = hi; sp++; }
				if (lo < higuy) { hi = higuy; continue; }
			}
		}
		if (--sp < 0) break;
		lo = lostk[sp];
		hi = histk[sp];
	}
	cmp_ctx = save_c; cmp_fn = save_f;
	CRET(0);
}
/* bsearch: the CRT's probing order (which of several equal keys it finds) */
static void hle_bsearch(XCtx *c) {
	u32 key = A(0), lo = A(1), num = A(2), w = A(3), hi, mid, half, r = 0;
	XCtx *save_c = cmp_ctx;
	u32 save_f = cmp_fn;
	int res;
	cmp_ctx = c; cmp_fn = A(4);
	if (!num) goto done;
	hi = lo + (num - 1) * w;
	while (lo <= hi) {
		if ((half = num / 2) != 0) {
			mid = lo + ((num & 1) ? half : half - 1) * w;
			if (!(res = gcmp(key, mid))) { r = mid; goto done; }
			if (res < 0) { hi = mid - w; num = (num & 1) ? half : half - 1; }
			else { lo = mid + w; num = half; }
		} else {
			if (num) r = gcmp(key, lo) ? 0 : lo;
			break;
		}
	}
done:
	cmp_ctx = save_c; cmp_fn = save_f;
	CRET(r);
}

/* ------------------------------------------------------------------------------------------------- math */
#define FA0 (c->fa[0])
#define FA1 (c->fa[1])
#define CI1(name_, expr_) static void hle_##name_(XCtx *c) { double x = FA0; CRETF(expr_); }
#define CI2(name_, expr_) static void hle_##name_(XCtx *c) { double x = FA1, y = FA0; CRETF(expr_); }
CI1(_CIsin, sin(x))
CI1(_CIcos, cos(x))
CI1(_CItan, tan(x))
CI1(_CIsqrt, sqrt(x))
CI1(_CIlog, log(x))
CI1(_CIlog10, log10(x))
CI1(_CIexp, exp(x))
CI1(_CIatan, atan(x))
CI1(_CIasin, asin(x))
CI1(_CIacos, acos(x))
CI2(_CIpow, pow(x, y))
CI2(_CIatan2, atan2(x, y))
CI2(_CIfmod, fmod(x, y))
static void hle_sin(XCtx *c) { CRETF(sin(D(0))); }
static void hle_cos(XCtx *c) { CRETF(cos(D(0))); }
static void hle_floor(XCtx *c) { CRETF(floor(D(0))); }
static void hle_ceil(XCtx *c) { CRETF(ceil(D(0))); }
static void hle_ldexp(XCtx *c) { CRETF(ldexp(D(0), (int)A(2))); }
static void hle_modf(XCtx *c) {
	double ip, fr = modf(D(0), &ip);
	M64(A(2)) = u64_of(ip);
	CRETF(fr);
}
static void hle__isnan(XCtx *c) { CRET(xrc_isnan(D(0)) ? 1 : 0); }
static void hle__finite(XCtx *c) { CRET(((u64_of(D(0)) >> 52) & 0x7ff) != 0x7ff ? 1 : 0); }
/* rand: the CRT's LCG */
static u32 holdrand = 1;
static void hle_srand(XCtx *c) { holdrand = A(0); CRET(0); }
static void hle_rand(XCtx *c) { holdrand = holdrand * 214013u + 2531011u; CRET((holdrand >> 16) & 0x7fff); }
static void hle__controlfp_s(XCtx *c) { if (A(0)) M32(A(0)) = 0x0009001fu; /* _CW_DEFAULT */ CRET(0); }

/* x87 helpers linked into LRFF13.exe itself: their entry points (as FFXIII's ovr_crt_math.c) */
static void XRC_CC m_ftol2(XCtx *c) {
	u64 v = xrc_i64(xrc_rint(FA0, 3));
	c->eax = (u32)v;
	c->edx = (u32)(v >> 32);
	xrc_cdecl_ret(c);
}
static void XRC_CC m_ftol2_sse(XCtx *c) { c->eax = cvt_trunc(FA0); xrc_cdecl_ret(c); }
void XRC_CC xrc_mss_cpu_probe(XCtx *c); /* hle_lr.c */
const XrcManual xrc_manual_lr_crt[] = {
	{0x00cf1ce0u, m_ftol2_sse, "_ftol2_sse"},
	{0x00cf1d16u, m_ftol2, "_ftol2"},
	{0x00de8000u, xrc_mss_cpu_probe, "Miles CPU probe (MSSMIXER section): no MMX / SSE"},
};
const unsigned xrc_manual_lr_crt_n = sizeof xrc_manual_lr_crt / sizeof xrc_manual_lr_crt[0];

/* ------------------------------------------------------------------------------------------ time, paths */
static unsigned long long clock0;
static unsigned long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
static void hle_clock(XCtx *c) { if (!clock0) clock0 = now_ms(); CRET((u32)(now_ms() - clock0)); }
static void hle__time64(XCtx *c) {
	u64 t = (u64)time(0);
	if (A(0)) M64(A(0)) = t;
	CRET64(t);
}
static void copy_part(u32 dst, const char *s, size_t n, size_t cap) {
	if (!dst) return;
	if (n >= cap) n = cap - 1;
	memcpy(xrc_mem + dst, s, n);
	M8(dst + (u32)n) = 0;
}
/* _splitpath(path, drive, dir, fname, ext) */
static void hle__splitpath(XCtx *c) {
	const char *p = S(A(0)), *d, *slash = 0, *dot = 0, *q;
	d = (p[0] && p[1] == ':') ? p + 2 : p;
	copy_part(A(1), p, (size_t)(d - p), 3);
	for (q = d; *q; q++) { if (*q == '/' || *q == '\\') slash = q + 1; }
	if (!slash) slash = d;
	copy_part(A(2), d, (size_t)(slash - d), 256);
	for (q = slash; *q; q++) if (*q == '.') dot = q;
	if (!dot) dot = q;
	copy_part(A(3), slash, (size_t)(dot - slash), 256);
	copy_part(A(4), dot, strlen(dot), 256);
	CRET(0);
}
/* _makepath(path, drive, dir, fname, ext) */
static void hle__makepath(XCtx *c) {
	char out[1024];
	size_t n = 0;
	const char *drv = A(1) ? S(A(1)) : "", *dir = A(2) ? S(A(2)) : "", *fn = A(3) ? S(A(3)) : "", *ext = A(4) ? S(A(4)) : "";
	if (*drv) n += (size_t)snprintf(out + n, sizeof out - n, "%c:", drv[0]);
	if (*dir) {
		size_t l = strlen(dir);
		n += (size_t)snprintf(out + n, sizeof out - n, "%s%s", dir, (dir[l - 1] == '/' || dir[l - 1] == '\\') ? "" : "\\");
	}
	n += (size_t)snprintf(out + n, sizeof out - n, "%s", fn);
	if (*ext) n += (size_t)snprintf(out + n, sizeof out - n, "%s%s", ext[0] == '.' ? "" : ".", ext);
	memcpy(S(A(0)), out, n + 1);
	CRET(0);
}
static void hle__fullpath(XCtx *c) {
	u32 abs = A(0), max = A(2), n;
	if (!abs) { max = 260; abs = xrc_heap_alloc(max, 0); }
	{ u32 a[4] = {A(1), max, abs, 0}; n = k32(c, "GetFullPathNameA", 4, a); }
	if (!n || n >= max) { set_errno(ERANGE_); CRET(0); }
	CRET(abs);
}
static void hle__chdir(XCtx *c) { u32 a[1] = {A(0)}; CRET(k32(c, "SetCurrentDirectoryA", 1, a) ? 0 : 0xffffffffu); }

/* _findfirst64i32 / _findnext64i32 / _findclose over FindFirstFileA: struct _finddata64i32_t is
 * {attrib, (pad), time_create, time_access, time_write (8 bytes each), size (4), name[260]} */
static u64 ft_time64(u32 ft) {
	u64 t = M64(ft);
	return t >= 116444736000000000ull ? (t - 116444736000000000ull) / 10000000ull : 0;
}
static void find_copy(u32 w, u32 fd) {
	M32(fd) = M32(w);
	M32(fd + 4) = 0;
	M64(fd + 8) = ft_time64(w + 4);
	M64(fd + 16) = ft_time64(w + 12);
	M64(fd + 24) = ft_time64(w + 20);
	M32(fd + 32) = M32(w + 32);
	memcpy(xrc_mem + fd + 36, xrc_mem + w + 44, 260);
}
static void hle__findfirst64i32(XCtx *c) {
	u32 h, path = A(0), fd = A(1); /* (the arguments before SCRATCH_BEGIN moves esp) */
	SCRATCH_BEGIN(c, 352);
	{ u32 a[2] = {path, scr_}; h = k32(c, "FindFirstFileA", 2, a); }
	if (h != 0xffffffffu) find_copy(scr_, fd);
	SCRATCH_END(c);
	if (h == 0xffffffffu) set_errno(ENOENT_);
	CRET(h);
}
static void hle__findnext64i32(XCtx *c) {
	u32 ok, h = A(0), fd = A(1); /* (the arguments before SCRATCH_BEGIN moves esp) */
	SCRATCH_BEGIN(c, 352);
	{ u32 a[2] = {h, scr_}; ok = k32(c, "FindNextFileA", 2, a); }
	if (ok) find_copy(scr_, fd);
	SCRATCH_END(c);
	if (!ok) set_errno(ENOENT_);
	CRET(ok ? 0 : 0xffffffffu);
}
static void hle__findclose(XCtx *c) { u32 a[1] = {A(0)}; CRET(k32(c, "FindClose", 1, a) ? 0 : 0xffffffffu); }

/* ------------------------------------------------------------------------------------ threads, startup, exit */
static void hle__beginthreadex(XCtx *c) {
	u32 a[6] = {A(0), A(1), A(2), A(3), A(4), A(5)};
	CRET(k32(c, "CreateThread", 6, a));
}
static void hle__endthreadex(XCtx *c) { u32 a[1] = {A(0)}; k32(c, "ExitThread", 1, a); CRET(0); }

static u32 g_argc, g_argv, g_envp, g_acmdln, g_fmode, g_commode;
/* the guest's argv / envp / _acmdln: the exe name, then the game's own arguments */
static void args_init(void) {
	int k, n = 1 + xrc_argc;
	char cmd[2048];
	size_t len;
	if (g_argv) return;
	g_argv = xrc_heap_alloc(4u * (u32)(n + 1), 1);
	for (k = 0; k < n; k++) {
		const char *s = k ? xrc_argv[k - 1] : "LRFF13.exe";
		u32 p = xrc_heap_alloc((u32)strlen(s) + 1, 0);
		memcpy(S(p), s, strlen(s) + 1);
		M32(g_argv + 4u * (u32)k) = p;
	}
	g_envp = xrc_heap_alloc(4, 1);
	len = (size_t)snprintf(cmd, sizeof cmd, "\"LRFF13.exe\"");
	for (k = 0; k < xrc_argc && len < sizeof cmd - 2; k++) len += (size_t)snprintf(cmd + len, sizeof cmd - len, " %s", xrc_argv[k]);
	g_acmdln = xrc_heap_alloc((u32)len + 1, 0);
	memcpy(S(g_acmdln), cmd, len + 1);
	if (!g_argc) g_argc = xrc_heap_alloc(4, 1);
	M32(g_argc) = (u32)n;
}
static u32 var(u32 *v, u32 init) {
	if (!*v) { *v = xrc_heap_alloc(4, 1); M32(*v) = init; }
	return *v;
}
/* data imports: the IAT slot holds the variable's address */
static u32 argv_var, acmdln_var;
u32 xrc_crt_data_import(const char *dll, const char *name) {
	if (!xrc_ieq(dll, "MSVCR100.dll") && !xrc_ieq(dll, "MSVCP100.dll")) return 0;
	if (!strcmp(name, "_fmode")) return var(&g_fmode, 0);
	if (!strcmp(name, "_commode")) return var(&g_commode, 0);
	args_init();
	if (!strcmp(name, "__argc")) return g_argc;
	if (!strcmp(name, "__argv")) return var(&argv_var, g_argv);
	if (!strcmp(name, "_acmdln")) return var(&acmdln_var, g_acmdln);
	if (!strcmp(name, "_FNan")) { /* _Dconst {0, 0x7fc0, ...}: the float quiet NaN */
		u32 p = xrc_heap_alloc(16, 1);
		M32(p) = 0x7fc00000u;
		return p;
	}
	return 0;
}
/* __getmainargs(&argc, &argv, &envp, dowildcard, &startinfo) */
static void hle___getmainargs(XCtx *c) {
	args_init();
	M32(A(0)) = M32(g_argc);
	M32(A(1)) = g_argv;
	M32(A(2)) = g_envp;
	CRET(0);
}
/* _initterm(begin, end): the C++ static constructors and the like */
static void hle__initterm(XCtx *c) {
	u32 p;
	for (p = A(0); p < A(1); p += 4) if (M32(p)) xrc_call_guest(c, M32(p), 0, 0);
	CRET(0);
}
static void hle__initterm_e(XCtx *c) {
	u32 p, r = 0;
	for (p = A(0); p < A(1) && !r; p += 4) if (M32(p)) r = xrc_call_guest(c, M32(p), 0, 0);
	CRET(r);
}
/* atexit / _onexit: run in reverse order by _cexit and exit */
static u32 onexit_fns[1024];
static unsigned nonexit;
static void hle__onexit(XCtx *c) {
	if (nonexit == sizeof onexit_fns / sizeof onexit_fns[0]) CRET(0);
	onexit_fns[nonexit++] = A(0);
	CRET(A(0));
}
static void hle___dllonexit(XCtx *c) {
	if (nonexit == sizeof onexit_fns / sizeof onexit_fns[0]) CRET(0);
	onexit_fns[nonexit++] = A(0);
	CRET(A(0));
}
static void run_onexit(XCtx *c) { while (nonexit) { u32 f = onexit_fns[--nonexit]; if (f) xrc_call_guest(c, f, 0, 0); } }
static void hle__cexit(XCtx *c) { run_onexit(c); CRET(0); }
static void hle_exit(XCtx *c) { u32 a[1] = {A(0)}; run_onexit(c); k32(c, "ExitProcess", 1, a); CRET(0); }
static void hle__exit(XCtx *c) { u32 a[1] = {A(0)}; k32(c, "ExitProcess", 1, a); CRET(0); }
static void hle_nop(XCtx *c) { CRET(0); }
static void hle__configthreadlocale(XCtx *c) { CRET(2); /* _DISABLE_PER_THREAD_LOCALE: the previous setting */ }
static void hle__lock(XCtx *c) { CRET(0); }   /* the guest's threads run one at a time (the runtime's GIL) */
static void hle__unlock(XCtx *c) { CRET(0); }

/* errors the CRT turns into termination */
static void hle__amsg_exit(XCtx *c) { xrc_fatal(c, "msvcr100: runtime error R60%02u", A(0)); }
static void hle__invoke_watson(XCtx *c) { xrc_fatal(c, "msvcr100: invalid parameter (_invoke_watson)"); }
static void hle__purecall(XCtx *c) { xrc_fatal(c, "msvcr100: pure virtual function call"); }
static void hle_terminate(XCtx *c) { xrc_fatal(c, "msvcr100: std::terminate"); }
static void hle__CxxThrowException(XCtx *c) {
	xrc_fatal(c, "C++ exception thrown (object %08x, throw info %08x): guest exception dispatch is not implemented yet", A(0), A(1));
}
static void hle_seh_handler(XCtx *c) { xrc_fatal(c, "msvcr100: %s reached: guest exception dispatch is not implemented yet", xrc_cur_import); }

/* -------------------------------------------------------------------------------------------- MSVCP100 */
/* iterator debugging: _Container_base12 {_Container_proxy *_Myproxy}, _Container_proxy {_Mycont, _Myfirstiter},
 * _Iterator_base12 {_Myproxy, _Mynextiter}. thiscall: this in ecx, the callee pops its arguments. */
static void orphan_all(u32 self) {
	u32 proxy = M32(self), it;
	if (!proxy) return;
	for (it = M32(proxy + 4); it; it = M32(it + 4)) M32(it) = 0;
	M32(proxy + 4) = 0;
}
static void hle_cb12_ctor(XCtx *c) { M32(c->ecx) = 0; TRET(c->ecx, 0); }
static void hle_cb12_dtor(XCtx *c) { orphan_all(c->ecx); TRET(0, 0); }
static void hle_cb12_orphan_all(XCtx *c) { orphan_all(c->ecx); TRET(0, 0); }
static void hle_cb12_swap_all(XCtx *c) {
	u32 self = c->ecx, right = A(0), t = M32(self);
	M32(self) = M32(right);
	M32(right) = t;
	if (M32(self)) M32(M32(self)) = self;
	if (M32(right)) M32(M32(right)) = right;
	TRET(0, 1);
}
static void hle_cb0_orphan_all(XCtx *c) { TRET(0, 0); }
static void hle_cb0_swap_all(XCtx *c) { TRET(0, 1); }
static void hle__Xlength_error(XCtx *c) { xrc_fatal(c, "msvcp100: std::length_error(\"%s\") thrown", A(0) ? S(A(0)) : ""); }
static void hle__Xout_of_range(XCtx *c) { xrc_fatal(c, "msvcp100: std::out_of_range(\"%s\") thrown", A(0) ? S(A(0)) : ""); }
static void hle_limits_uint_max(XCtx *c) { CRET(0xffffffffu); }
static void hle_limits_float_nan(XCtx *c) { CRETF(f64_of(0x7ff8000000000000ull)); } /* (bits: -ffast-math assumes no NaN) */

/* std::exception {vfptr, const char *_Mywhat, bool _Mydofree}; its vtable: scalar deleting destructor, what */
static u32 exc_vtable;
static void exc_set_what(u32 self, u32 s) {
	M32(self + 4) = 0; M8(self + 8) = 0;
	if (s) {
		u32 n = (u32)strlen(S(s)) + 1, p = xrc_heap_alloc(n, 0);
		memcpy(S(p), S(s), n);
		M32(self + 4) = p; M8(self + 8) = 1;
	}
}
static void exc_init(u32 self) {
	if (!exc_vtable) {
		exc_vtable = xrc_heap_alloc(8, 1);
		M32(exc_vtable) = xrc_import_addr("MSVCR100.dll", "??_Gexception@std@@UAEPAXI@Z");
		M32(exc_vtable + 4) = xrc_import_addr("MSVCR100.dll", "?what@exception@std@@UBEPBDXZ");
	}
	M32(self) = exc_vtable;
}
static void exc_free(u32 self) { if (M8(self + 8) && M32(self + 4)) xrc_heap_free(M32(self + 4)); M32(self + 4) = 0; M8(self + 8) = 0; }
/* exception(const char *const &) */
static void hle_exc_ctor_str(XCtx *c) { exc_init(c->ecx); exc_set_what(c->ecx, M32(A(0))); TRET(c->ecx, 1); }
/* exception(const exception &) */
static void hle_exc_ctor_copy(XCtx *c) {
	u32 o = A(0);
	exc_init(c->ecx);
	if (M8(o + 8)) exc_set_what(c->ecx, M32(o + 4));
	else { M32(c->ecx + 4) = M32(o + 4); M8(c->ecx + 8) = 0; }
	TRET(c->ecx, 1);
}
static void hle_exc_dtor(XCtx *c) { M32(c->ecx) = exc_vtable; exc_free(c->ecx); TRET(0, 0); }
static void hle_exc_sdtor(XCtx *c) {
	u32 self = c->ecx;
	exc_free(self);
	if (A(0) & 1) xrc_heap_free(self);
	TRET(self, 1);
}
static u32 unknown_exc;
static void hle_exc_what(XCtx *c) {
	if (M32(c->ecx + 4)) TRET(M32(c->ecx + 4), 0);
	if (!unknown_exc) { unknown_exc = xrc_heap_alloc(18, 0); memcpy(S(unknown_exc), "Unknown exception", 18); }
	TRET(unknown_exc, 0);
}

/* ----------------------------------------------------------------------------------------------- tables */
#define E(n) {"MSVCR100.dll", #n, hle_##n}
#define CR(name_, fn_) {"MSVCR100.dll", name_, fn_}
#define CP(name_, fn_) {"MSVCP100.dll", name_, fn_}
const XrcImport xrc_hle_msvcr100[] = {
	/* startup, exit */
	E(__getmainargs), E(_initterm), E(_initterm_e), E(_onexit), E(__dllonexit), E(_cexit), E(exit), E(_exit),
	CR("__set_app_type", hle_nop), CR("__setusermatherr", hle_nop), CR("_crt_debugger_hook", hle_nop),
	CR("?_type_info_dtor_internal_method@type_info@@QAEXXZ", hle_nop), E(_configthreadlocale), E(_controlfp_s),
	E(_lock), E(_unlock), E(_amsg_exit), E(_invoke_watson), E(_purecall), CR("?terminate@@YAXXZ", hle_terminate),
	E(_CxxThrowException), CR("__CxxFrameHandler3", hle_seh_handler), CR("_except_handler4_common", hle_seh_handler),
	CR("_XcptFilter", hle_seh_handler), E(_beginthreadex), E(_endthreadex),
	/* std::exception (MSVCR100 exports it) */
	CR("??0exception@std@@QAE@ABQBD@Z", hle_exc_ctor_str), CR("??0exception@std@@QAE@ABV01@@Z", hle_exc_ctor_copy),
	CR("??1exception@std@@UAE@XZ", hle_exc_dtor), CR("?what@exception@std@@UBEPBDXZ", hle_exc_what),
	CR("??_Gexception@std@@UAEPAXI@Z", hle_exc_sdtor),
	/* memory, strings */
	E(malloc), E(calloc), E(free), E(_aligned_malloc), E(_aligned_free), E(memcpy), E(memmove), E(memset), E(memcmp),
	E(memchr), E(memcpy_s), E(memmove_s), E(strcpy), E(strncpy), E(strncat), E(strcpy_s), E(strcat_s), E(strncmp),
	E(_stricmp), E(_strnicmp), E(strchr), E(strrchr), E(strstr), E(_mbsrchr), E(_mbsinc), E(_ismbblead), E(wcsstr),
	E(isupper), E(isdigit), E(toupper), E(abs), E(atoi), E(_atoi64), E(atof), E(qsort), E(bsearch), E(rand), E(srand),
	/* formatted text */
	E(printf), E(fprintf), E(sprintf), E(vsprintf), E(sprintf_s), E(vsprintf_s), E(_snprintf), E(_vsnprintf),
	E(_snprintf_s), E(_vsnprintf_s), E(sscanf_s), E(swscanf_s),
	/* files */
	E(fopen), E(fopen_s), E(fclose), E(fread), E(fwrite), E(fputs), E(fgets), E(ftell), E(fseek), E(_fileno),
	E(_filelength), E(__iob_func), E(_fullpath), E(_chdir), E(_splitpath), E(_makepath), E(_findfirst64i32),
	E(_findnext64i32), E(_findclose), E(_time64), E(clock),
	/* math */
	E(_CIsin), E(_CIcos), E(_CItan), E(_CIsqrt), E(_CIlog), E(_CIlog10), E(_CIexp), E(_CIatan), E(_CIasin), E(_CIacos),
	E(_CIpow), E(_CIatan2), E(_CIfmod), E(sin), E(cos), E(floor), E(ceil), E(ldexp), E(modf), E(_isnan), E(_finite),
	/* MSVCP100 */
	CP("??0_Container_base12@std@@QAE@XZ", hle_cb12_ctor), CP("??1_Container_base12@std@@QAE@XZ", hle_cb12_dtor),
	CP("?_Orphan_all@_Container_base12@std@@QAEXXZ", hle_cb12_orphan_all),
	CP("?_Swap_all@_Container_base12@std@@QAEXAAU12@@Z", hle_cb12_swap_all),
	CP("?_Orphan_all@_Container_base0@std@@QAEXXZ", hle_cb0_orphan_all),
	CP("?_Swap_all@_Container_base0@std@@QAEXAAU12@@Z", hle_cb0_swap_all),
	CP("?_Xlength_error@std@@YAXPBD@Z", hle__Xlength_error), CP("?_Xout_of_range@std@@YAXPBD@Z", hle__Xout_of_range),
	CP("?max@?$numeric_limits@I@std@@SAIXZ", hle_limits_uint_max),
	CP("?quiet_NaN@?$numeric_limits@M@std@@SAMXZ", hle_limits_float_nan),
	CP("?signaling_NaN@?$numeric_limits@M@std@@SAMXZ", hle_limits_float_nan),
};
const unsigned xrc_hle_msvcr100_n = sizeof xrc_hle_msvcr100 / sizeof xrc_hle_msvcr100[0];
