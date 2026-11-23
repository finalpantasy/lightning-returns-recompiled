/* img_decode.c -- image file decoding for the D3DX texture loaders (host side, no dependencies): PNG (with its own
 * inflate) to 8-bit RGBA. */
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"

/* ============================================================================================ inflate (RFC 1951) */
typedef struct {
	const u8 *in;
	size_t n, pos;
	u32 bits, nb;
	u8 *out;
	size_t on, ocap;
	int err;
} Inf;
static u32 getbits(Inf *s, int need) {
	u32 v;
	while ((int)s->nb < need) {
		if (s->pos >= s->n) { s->err = 1; return 0; }
		s->bits |= (u32)s->in[s->pos++] << s->nb;
		s->nb += 8;
	}
	v = s->bits & ((1u << need) - 1u);
	s->bits >>= need;
	s->nb -= (u32)need;
	return v;
}
static int put(Inf *s, u8 b) {
	if (s->on == s->ocap) {
		size_t nc = s->ocap ? s->ocap * 2 : 65536;
		u8 *p = (u8 *)realloc(s->out, nc);
		if (!p) return 0;
		s->out = p;
		s->ocap = nc;
	}
	s->out[s->on++] = b;
	return 1;
}
typedef struct { short count[16], sym[320]; } Huff;
static int build(Huff *h, const short *len, int n) {
	short offs[16];
	int k, left = 1;
	memset(h->count, 0, sizeof h->count);
	for (k = 0; k < n; k++) h->count[len[k]]++;
	if (h->count[0] == n) return 0;
	for (k = 1; k < 16; k++) { left <<= 1; left -= h->count[k]; if (left < 0) return -1; }
	offs[1] = 0;
	for (k = 1; k < 15; k++) offs[k + 1] = (short)(offs[k] + h->count[k]);
	for (k = 0; k < n; k++) if (len[k]) h->sym[offs[len[k]]++] = (short)k;
	return left;
}
static int decode(Inf *s, const Huff *h) {
	int code = 0, first = 0, index = 0, l;
	for (l = 1; l < 16; l++) {
		code |= (int)getbits(s, 1);
		if (s->err) return -1;
		if (code - h->count[l] < first) return h->sym[index + (code - first)];
		index += h->count[l];
		first += h->count[l];
		first <<= 1;
		code <<= 1;
	}
	s->err = 1;
	return -1;
}
static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static int codes(Inf *s, const Huff *lc, const Huff *dc) {
	for (;;) {
		int sym = decode(s, lc);
		if (sym < 0) return -1;
		if (sym < 256) { if (!put(s, (u8)sym)) return -1; }
		else if (sym == 256) return 0;
		else {
			int len, d;
			size_t dist;
			sym -= 257;
			if (sym >= 29) return -1;
			len = lbase[sym] + (int)getbits(s, lext[sym]);
			d = decode(s, dc);
			if (d < 0 || d >= 30) return -1;
			dist = (size_t)dbase[d] + getbits(s, dext[d]);
			if (s->err || dist > s->on) return -1;
			while (len--) if (!put(s, s->out[s->on - dist])) return -1;
		}
	}
}
static int fixed(Inf *s) {
	static Huff lc, dc;
	static int done;
	if (!done) {
		short l[288];
		int k;
		for (k = 0; k < 144; k++) l[k] = 8;
		for (; k < 256; k++) l[k] = 9;
		for (; k < 280; k++) l[k] = 7;
		for (; k < 288; k++) l[k] = 8;
		build(&lc, l, 288);
		for (k = 0; k < 30; k++) l[k] = 5;
		build(&dc, l, 30);
		done = 1;
	}
	return codes(s, &lc, &dc);
}
static int dynamic(Inf *s) {
	static const short ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	short len[320];
	Huff lc, dc;
	int nlen = (int)getbits(s, 5) + 257, ndist = (int)getbits(s, 5) + 1, ncode = (int)getbits(s, 4) + 4, k;
	if (nlen > 286 || ndist > 30) return -1;
	for (k = 0; k < ncode; k++) len[ord[k]] = (short)getbits(s, 3);
	for (; k < 19; k++) len[ord[k]] = 0;
	if (build(&lc, len, 19) != 0) return -1;
	for (k = 0; k < nlen + ndist;) {
		int sym = decode(s, &lc), rep, v = 0;
		if (sym < 0) return -1;
		if (sym < 16) { len[k++] = (short)sym; continue; }
		if (sym == 16) { if (!k) return -1; v = len[k - 1]; rep = 3 + (int)getbits(s, 2); }
		else if (sym == 17) rep = 3 + (int)getbits(s, 3);
		else rep = 11 + (int)getbits(s, 7);
		if (k + rep > nlen + ndist) return -1;
		while (rep--) len[k++] = (short)v;
	}
	if (len[256] == 0) return -1;
	if (build(&lc, len, nlen) < 0 || build(&dc, len + nlen, ndist) < 0) return -1;
	return codes(s, &lc, &dc);
}
/* zlib stream -> malloc'd bytes */
static u8 *zinflate(const u8 *in, size_t n, size_t *outn) {
	Inf s;
	int last;
	memset(&s, 0, sizeof s);
	if (n < 2 || (in[0] & 15) != 8) return 0;
	s.in = in + 2;
	s.n = n - 2;
	do {
		int type;
		last = (int)getbits(&s, 1);
		type = (int)getbits(&s, 2);
		if (s.err) break;
		if (type == 0) {
			u32 l;
			s.bits = 0; s.nb = 0;
			if (s.pos + 4 > s.n) { s.err = 1; break; }
			l = (u32)s.in[s.pos] | (u32)s.in[s.pos + 1] << 8;
			s.pos += 4;
			if (s.pos + l > s.n) { s.err = 1; break; }
			while (l--) if (!put(&s, s.in[s.pos++])) { s.err = 1; break; }
		} else if (type == 1) { if (fixed(&s)) s.err = 1; }
		else if (type == 2) { if (dynamic(&s)) s.err = 1; }
		else s.err = 1;
	} while (!last && !s.err);
	if (s.err) { free(s.out); return 0; }
	*outn = s.on;
	return s.out;
}

/* ============================================================================================ PNG */
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static int paeth(int a, int b, int c) {
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
/* PNG -> RGBA8 (malloc'd; 0 on failure). max: bytes available (the chunks are walked to IEND). Supports bit depth 8
 * (and 16, reduced) of gray / RGB / palette / gray+alpha / RGBA, and palette depths 1-8; no interlacing. */
u8 *xrc_png_decode(const u8 *d, size_t max, u32 *pw, u32 *ph) {
	static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
	u32 w = 0, h = 0, bpp, stride, x, y;
	int depth = 0, ctype = 0, inter = 0, chans;
	u8 pal[256][4], *idat = 0, *raw, *out;
	size_t ni = 0, pos = 8, rn;
	memset(pal, 255, sizeof pal);
	if (max < 8 || memcmp(d, sig, 8)) return 0;
	while (pos + 12 <= max) {
		u32 len = be32(d + pos);
		const u8 *t = d + pos + 4, *cd = d + pos + 8;
		if (pos + 12 + len > max) break;
		if (!memcmp(t, "IHDR", 4)) { w = be32(cd); h = be32(cd + 4); depth = cd[8]; ctype = cd[9]; inter = cd[12]; }
		else if (!memcmp(t, "PLTE", 4)) { u32 k; for (k = 0; k < len / 3 && k < 256; k++) { pal[k][0] = cd[3 * k]; pal[k][1] = cd[3 * k + 1]; pal[k][2] = cd[3 * k + 2]; } }
		else if (!memcmp(t, "tRNS", 4) && ctype == 3) { u32 k; for (k = 0; k < len && k < 256; k++) pal[k][3] = cd[k]; }
		else if (!memcmp(t, "IDAT", 4)) { idat = (u8 *)realloc(idat, ni + len); memcpy(idat + ni, cd, len); ni += len; }
		else if (!memcmp(t, "IEND", 4)) break;
		pos += 12 + len;
	}
	if (!w || !h || !idat || inter) { free(idat); return 0; }
	chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
	bpp = (u32)((chans * depth + 7) / 8);
	stride = (w * (u32)chans * (u32)depth + 7) / 8;
	raw = zinflate(idat, ni, &rn);
	free(idat);
	if (!raw || rn < (size_t)(stride + 1) * h) { free(raw); return 0; }
	/* unfilter in place */
	for (y = 0; y < h; y++) {
		u8 *row = raw + (size_t)y * (stride + 1), f = row[0], *cur = row + 1, *prev = y ? raw + (size_t)(y - 1) * (stride + 1) + 1 : 0;
		for (x = 0; x < stride; x++) {
			int a = x >= bpp ? cur[x - bpp] : 0, b = prev ? prev[x] : 0, c = prev && x >= bpp ? prev[x - bpp] : 0;
			switch (f) {
			case 1: cur[x] = (u8)(cur[x] + a); break;
			case 2: cur[x] = (u8)(cur[x] + b); break;
			case 3: cur[x] = (u8)(cur[x] + ((a + b) >> 1)); break;
			case 4: cur[x] = (u8)(cur[x] + paeth(a, b, c)); break;
			}
		}
	}
	out = (u8 *)malloc((size_t)w * h * 4);
	for (y = 0; y < h; y++) {
		const u8 *row = raw + (size_t)y * (stride + 1) + 1;
		for (x = 0; x < w; x++) {
			u8 *o = out + ((size_t)y * w + x) * 4;
			if (depth < 8) { /* palette or gray, packed */
				u32 bit = x * (u32)depth, v = (row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1u);
				if (ctype == 3) memcpy(o, pal[v], 4);
				else { u8 g = (u8)(v * 255 / ((1u << depth) - 1u)); o[0] = o[1] = o[2] = g; o[3] = 255; }
				continue;
			}
			{
				const u8 *px = row + (size_t)x * bpp;
				int st = depth == 16 ? 2 : 1;
				switch (ctype) {
				case 0: o[0] = o[1] = o[2] = px[0]; o[3] = 255; break;
				case 2: o[0] = px[0]; o[1] = px[st]; o[2] = px[2 * st]; o[3] = 255; break;
				case 3: memcpy(o, pal[px[0]], 4); break;
				case 4: o[0] = o[1] = o[2] = px[0]; o[3] = px[st]; break;
				default: o[0] = px[0]; o[1] = px[st]; o[2] = px[2 * st]; o[3] = px[3 * st]; break;
				}
			}
		}
	}
	free(raw);
	*pw = w;
	*ph = h;
	return out;
}

/* ============================================================================================ PNG writing
 * xrc_png_write: 32-bit BGRX rows (pitch bytes apart) as an RGB PNG; stored (uncompressed) deflate blocks (screenshots:
 * XRC_SHOT) */
static unsigned png_crc_tab[256];
static unsigned png_crc(unsigned c, const unsigned char *p, size_t n) {
	size_t i;
	if (!png_crc_tab[1]) for (i = 0; i < 256; i++) { unsigned k, v = (unsigned)i; for (k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; png_crc_tab[i] = v; }
	for (i = 0; i < n; i++) c = png_crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c;
}
static void png_chunk(FILE *f, const char *type, const unsigned char *p, size_t n) {
	unsigned char b[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n};
	unsigned c;
	fwrite(b, 1, 4, f); fwrite(type, 1, 4, f); if (n) fwrite(p, 1, n, f);
	c = png_crc(0xffffffffu, (const unsigned char *)type, 4); c = png_crc(c, p, n) ^ 0xffffffffu;
	b[0] = (unsigned char)(c >> 24); b[1] = (unsigned char)(c >> 16); b[2] = (unsigned char)(c >> 8); b[3] = (unsigned char)c;
	fwrite(b, 1, 4, f);
}
void xrc_png_write(const char *path, const unsigned char *bgrx, int pitch, unsigned w, unsigned h) {
	size_t raw = (size_t)h * (1 + 3 * (size_t)w), nblk = raw / 65535 + 1, n = 2 + raw + 5 * nblk + 4, o = 0, i;
	unsigned char *z = (unsigned char *)malloc(n), *r = (unsigned char *)malloc(raw), hdr[13];
	unsigned x, y, s1 = 1, s2 = 0;
	FILE *f = fopen(path, "wb");
	if (!f || !z || !r) { if (f) fclose(f); free(z); free(r); return; }
	for (y = 0; y < h; y++) {
		unsigned char *d = r + (size_t)y * (1 + 3 * (size_t)w);
		const unsigned char *sp = bgrx + (size_t)y * (size_t)pitch;
		*d++ = 0;
		for (x = 0; x < w; x++, sp += 4) { *d++ = sp[2]; *d++ = sp[1]; *d++ = sp[0]; }
	}
	z[o++] = 0x78; z[o++] = 0x01;
	for (i = 0; i < raw;) {
		size_t k = raw - i > 65535 ? 65535 : raw - i;
		z[o++] = i + k >= raw ? 1 : 0;
		z[o++] = (unsigned char)k; z[o++] = (unsigned char)(k >> 8); z[o++] = (unsigned char)~k; z[o++] = (unsigned char)(~k >> 8);
		memcpy(z + o, r + i, k); o += k; i += k;
	}
	for (i = 0; i < raw; i++) { s1 = (s1 + r[i]) % 65521u; s2 = (s2 + s1) % 65521u; }
	x = (s2 << 16) | s1;
	z[o++] = (unsigned char)(x >> 24); z[o++] = (unsigned char)(x >> 16); z[o++] = (unsigned char)(x >> 8); z[o++] = (unsigned char)x;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	hdr[0] = (unsigned char)(w >> 24); hdr[1] = (unsigned char)(w >> 16); hdr[2] = (unsigned char)(w >> 8); hdr[3] = (unsigned char)w;
	hdr[4] = (unsigned char)(h >> 24); hdr[5] = (unsigned char)(h >> 16); hdr[6] = (unsigned char)(h >> 8); hdr[7] = (unsigned char)h;
	hdr[8] = 8; hdr[9] = 2; hdr[10] = hdr[11] = hdr[12] = 0;
	png_chunk(f, "IHDR", hdr, 13); png_chunk(f, "IDAT", z, o); png_chunk(f, "IEND", 0, 0);
	fclose(f); free(z); free(r);
}
