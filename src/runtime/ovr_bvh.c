/* ovr_bvh.c - a "fast" override (overrides.tsv) of the collision broadphase's tree builder, 00d785d0: one recursive
 * step of a bounding volume hierarchy build over the dynamic collision objects, rebuilt every frame (~1.5% of the main
 * thread). The original is unoptimized compiler output (every float through its stack frame before a comiss, three
 * copies of the partition loop); this does the same operations in the same order, so the tree comes out the same,
 * NaNs and signed zeros included, and the recursion and the leaves (00d77e10) are native. Only the node vector's grow
 * (00d782c0, when the tree has more nodes than ever before) is called in the guest.
 *
 * thiscall u32 Builder::Build(u32 parent, u32 first, s32 n, u32 reuse) -> the node's index. The builder (this):
 *   +0x04/+0x08  std::vector of 64-byte nodes (begin, end), +0x20 the nodes in use
 *   +0x10        -> the object-index array (reordered in place), +0x14 -> one vec4 point per object (the split's)
 *   +0x28        160-byte per-object entries (the leaf's), +0x2c the 112-byte object records (box at +0x40 / +0x50)
 * A node: box min +0x00 / max +0x10, children +0x20 / +0x22, object +0x24 (-1: internal), parent +0x26, the skip
 * index +0x2a (the first node after the subtree: written by a later pass, 00d77e90, read by the reuse check).
 * One call: takes a node; one object makes a leaf; else the union of the objects' boxes and the bounds of their
 * points; with reuse, a subtree whose box has not moved (|d max| + |d min|, the largest axis, within [0x10a523c])
 * keeps last build's nodes (the counter jumps past them); else a midpoint split on the points' longest axis, a
 * two-ended partition of the indices, the halves recursed (one side empty: a leaf of one object instead).
 * Quirks kept: the node's address is taken once (a grow during the recursion leaves the original writing the
 * children's indices to the old buffer: so does this), and a reused subtree's box is not stored.
 *
 * XRC_FAST_CHECK=1 (tools builds, with ovr_simd.c's): each call from the guest is also run by the translation from the
 * same state, and the nodes, the index range, the node count and the result compared (a mismatch reported, the first
 * few in detail); the translation's result is kept. XRC_NO_FAST=1: the translation only. Checked (2026-11-19, Ch4
 * field): 16383 builds, up to 632 objects, 0 mismatches -- the bytes a node never gets from the build not compared
 * (stack garbage in the original too: see checked()). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"
#include "guest_types.h"

#define FN_BUILD 0x00d785d0u
#define FN_GROW 0x00d782c0u    /* thiscall vector<Node>::_Insert_n(Node *where, u32 count, const Node *value) */
#define K_EPS 0x010a523cu       /* the reuse check's tolerance */
#define K_HALF 0x0109b130u      /* 0.5 */

typedef struct B { XCtx *c; u32 self, tmpl; } B;

/* comiss a, b: the branch conditions (unordered: CF = ZF = 1) */
static inline int c_b(float a, float b) { return xrc_isnanf(a) || xrc_isnanf(b) || a < b; }   /* jb  */
static inline int c_a(float a, float b) { return !xrc_isnanf(a) && !xrc_isnanf(b) && a > b; } /* ja  */
static inline int c_be(float a, float b) { return xrc_isnanf(a) || xrc_isnanf(b) || a <= b; } /* jbe */
static inline int neg(u32 a, u32 b) { return (s32)(a - b) < 0; } /* sub a, b; js */

/* a new node: the vector grown by one first if full; the old count (the node's index) */
static u32 node_new(B *b) {
	u32 v = GADDR(BvhBuilder, b->self, nodes_begin), beg = GF(BvhBuilder, b->self, nodes_begin), k;
	s32 cap = beg ? (s32)(GF(BvhBuilder, b->self, nodes_end) - beg) >> 6 : 0;
	if ((s32)GF(BvhBuilder, b->self, node_count) >= cap) {
		u32 a[3] = {GF(BvhBuilder, b->self, nodes_end), 1, b->tmpl};
		b->c->ecx = v;
		xrc_call_guest(b->c, FN_GROW, 3, a);
	}
	k = GF(BvhBuilder, b->self, node_count);
	GF(BvhBuilder, b->self, node_count) = k + 1;
	return k;
}

/* 00d77e10 (cdecl: node, object, parent, its 160-byte entry, its record): a leaf of one object */
static void leaf(B *b, u32 node, u32 o, u32 parent) {
	u32 aux = GF(BvhBuilder, b->self, entries) + o * 160u, rec = GF(BvhBuilder, b->self, records) + o * 0x70u;
	X128 t;
	GF(BvhNode, node, child_b) = 0xffff;
	GF(BvhNode, node, child_a) = 0xffff;
	GF(BvhNode, node, parent) = (u16)parent;
	GF(BvhNode, node, object) = (u16)o;
	XLD(t, GADDR(BvhRecord, rec, box_min)); XST(node, t);
	XLD(t, GADDR(BvhRecord, rec, box_max)); XST(GADDR(BvhNode, node, box_max), t);
	GF(BvhNode, node, unk_2e) = GF(BvhEntry, aux, unk_44);
	GF(BvhNode, node, unk_30) = GF(BvhRecord, rec, unk_66);
	GF(BvhNode, node, unk_32) = GF(BvhEntry, aux, unk_4b);
	GF(BvhNode, node, unk_38) = GF(BvhEntry, aux, unk_54);
	GF(BvhNode, node, unk_34) = GF(BvhEntry, aux, unk_4a);
	GF(BvhNode, node, unk_2c) = 0xffff;
	GF(BvhNode, node, unk_3c) = GF(BvhEntry, aux, unk_4c);
}

static u32 build(B *b, u32 parent, u32 first, s32 n, u32 reuse) {
	u32 self = b->self, k = node_new(b);
	u32 ia = GF(BvhVector, GF(BvhBuilder, self, indices), begin) + first * 4u, pts = GF(BvhVector, GF(BvhBuilder, self, points), begin), recs = GF(BvhBuilder, self, records);
	u32 node = (k << 6) + GF(BvhBuilder, self, nodes_begin), o, ax;
	X128 bmin, bmax, pmin, pmax, t, hv, cen, ext;
	float cv;
	s32 i, lo, hi;
	if (n <= 1) { leaf(b, node, M32(ia), parent); return k; }
	o = M32(ia);
	XLD(bmin, GADDR(BvhRecord, recs + o * 0x70u, box_min)); XLD(bmax, GADDR(BvhRecord, recs + o * 0x70u, box_max));
	XLD(pmin, pts + (o << 4)); pmax = pmin;
	reuse &= 1;
	for (i = 1; i < n; i++) {
		o = M32(ia + (u32)i * 4u);
		XLD(t, GADDR(BvhRecord, recs + o * 0x70u, box_min)); bmin = xv_minps(bmin, t);
		XLD(t, GADDR(BvhRecord, recs + o * 0x70u, box_max)); bmax = xv_maxps(bmax, t);
		XLD(t, pts + (o << 4)); pmin = xv_minps(pmin, t); pmax = xv_maxps(pmax, t);
	}
	if (reuse) { /* the subtree kept if its box moved no more than the tolerance */
		X128 omin, omax, z, d, a0, a1, s, m;
		memset(&z, 0, sizeof z);
		XLD(omax, GADDR(BvhNode, node, box_max)); XLD(omin, node);
		d = xv_subps(omax, bmax); a1 = xv_maxps(xv_subps(z, d), d);
		d = xv_subps(omin, bmin); a0 = xv_maxps(xv_subps(z, d), d);
		s = xv_addps(a0, a1);
		m = xv_maxps(XV_SHUFPS(s, s, 0xaa), s);
		m = xv_maxps(m, XV_SHUFPS(m, m, 0x55));
		if (!c_b(MF32(K_EPS), m.f[0])) {
			s32 skip = (s16)GF(BvhNode, node, skip);
			if (skip >= (s32)(k + 1)) GF(BvhBuilder, self, node_count) = (u32)skip;
			return k;
		}
	}
	XST(node, bmin); XST(GADDR(BvhNode, node, box_max), bmax);
	hv.f[0] = hv.f[1] = hv.f[2] = hv.f[3] = MF32(K_HALF);
	cen = xv_mulps(xv_addps(pmax, pmin), hv);
	ext = xv_subps(pmax, pmin);
	if (c_a(ext.f[0], ext.f[2]) || c_a(ext.f[1], ext.f[2])) ax = c_a(ext.f[0], ext.f[1]) ? 0 : 1;
	else ax = 2;
	cv = cen.f[ax];
	/* the partition: indices with their point's coordinate <= cv to the front */
#define P(j_) MF32(pts + (M32(ia + (u32)(j_) * 4u) << 4) + ax * 4u)
	lo = 0; hi = n - 1;
	if (neg(0, (u32)hi)) {
		do {
			if (neg((u32)lo, (u32)hi)) {
				do {
					float w;
					int up;
					if (c_b(cv, P(lo))) break;
					w = P(hi);
					lo++;
					up = !c_b(w, cv);
					hi -= neg((u32)lo, (u32)hi) & up;
				} while (neg((u32)lo, (u32)hi));
			}
			while (neg((u32)lo, (u32)hi) && !c_be(P(hi), cv)) hi--;
			if (neg((u32)lo, (u32)hi)) {
				u32 x = M32(ia + (u32)hi * 4u), y = M32(ia + (u32)lo * 4u);
				M32(ia + (u32)lo * 4u) = x;
				M32(ia + (u32)hi * 4u) = y;
				lo++; hi--;
			}
		} while (neg((u32)lo, (u32)hi));
	}
#undef P
	if (lo == 0) { /* nothing on the low side: a leaf of the first object (the left child not set) */
		u32 e = node_new(b);
		leaf(b, (e << 6) + GF(BvhBuilder, self, nodes_begin), M32(ia), k);
	} else GF(BvhNode, node, child_a) = (u16)build(b, k, first, lo, reuse);
	if (n - lo != 0) GF(BvhNode, node, child_b) = (u16)build(b, k, first + (u32)lo, n - lo, reuse);
	else { /* nothing on the high side: a leaf of the object at lo (the right child not set) */
		u32 e = node_new(b);
		leaf(b, (e << 6) + GF(BvhBuilder, self, nodes_begin), M32(ia + (u32)lo * 4u), k);
	}
	GF(BvhNode, node, object) = 0xffff;
	GF(BvhNode, node, parent) = (u16)parent;
	return k;
}

static void native(XCtx *c) {
	u32 sp = c->esp, r;
	B b;
	b.c = c;
	b.self = c->ecx;
	b.tmpl = (sp - 0x60u) & ~15u; /* (the grow's 64-byte value: whatever is there, like the original's stack local) */
	c->esp = b.tmpl - 0x10u;
	r = build(&b, M32(sp + 4), M32(sp + 8), (s32)M32(sp + 12), M32(sp + 16));
	c->esp = sp;
	c->eax = r;
	xrc_stdcall_ret(c, 4);
}

static int fast_mode = -1, in_orig;
static unsigned long long n_checked, n_bad, n_skipped;
static u32 n_max; /* (the most objects in a checked build) */

/* ours, then the translation from the same state; the translation's result kept */
static void checked(XCtx *c, XFn orig) {
	u32 self = c->ecx, first = arg_u32(c, 2), n = arg_u32(c, 3);
	u32 beg = GF(BvhBuilder, self, nodes_begin), end = GF(BvhBuilder, self, nodes_end), cap = GF(BvhBuilder, self, nodes_cap), cnt0 = GF(BvhBuilder, self, node_count), ia = GF(BvhVector, GF(BvhBuilder, self, indices), begin) + first * 4u;
	u32 nb = cap - beg, ni = (s32)n > 0 ? n * 4u : 0, r1, cnt1, end1, k;
	XCtx c0 = *c;
	static unsigned long long n_calls;
	if ((++n_calls & (n_calls - 1)) == 0) fprintf(stderr, "xrc: BVH build check: %llu calls (%llu checked, %llu skipped, %llu mismatches; this one %d objects)\n", n_calls, n_checked, n_skipped, n_bad, (int)n);
	u8 *n0 = (u8 *)malloc(nb ? nb : 1), *i0 = (u8 *)malloc(ni ? ni : 1), *n1 = (u8 *)malloc(nb ? nb : 1), *i1 = (u8 *)malloc(ni ? ni : 1);
	memcpy(n0, xrc_mem + beg, nb); memcpy(i0, xrc_mem + ia, ni);
	native(c);
	/* (a grow within the capacity -- the vector refilled after a clear, every frame -- is undone with the end; one that
	 * moved the buffer cannot be) */
	if (GF(BvhBuilder, self, nodes_begin) != beg || GF(BvhBuilder, self, nodes_cap) != cap) { n_skipped++; goto done; }
	r1 = c->eax; cnt1 = GF(BvhBuilder, self, node_count); end1 = GF(BvhBuilder, self, nodes_end);
	memcpy(n1, xrc_mem + beg, nb); memcpy(i1, xrc_mem + ia, ni);
	memcpy(xrc_mem + beg, n0, nb); memcpy(xrc_mem + ia, i0, ni); GF(BvhBuilder, self, node_count) = cnt0; GF(BvhBuilder, self, nodes_end) = end;
	*c = c0;
	in_orig = 1; orig(c); in_orig = 0;
	if (GF(BvhBuilder, self, nodes_begin) != beg || GF(BvhBuilder, self, nodes_cap) != cap) { n_skipped++; goto done; }
	n_checked++;
	if ((s32)n > (s32)n_max) { n_max = n; if (n_max >= 2) fprintf(stderr, "xrc: BVH build check: a build of %u objects checked (%llu so far, %llu mismatches)\n", n, n_checked, n_bad); }
	/* (not compared: the bytes of a node the build does not write -- a grown node's come from the grow's 64-byte value,
	 * an uninitialized stack local in the original (seen: a return address), here other stack bytes. An internal node
	 * (object 0xffff) has nothing written past its parent (+0x28..+0x3f); a leaf (00d77e10) not +0x28..+0x2b (written
	 * after the build by 00d77e90), +0x36..+0x37 and +0x39..+0x3b) */
	for (k = 0; k + 64 <= nb; k += 64) {
		u8 *o = xrc_mem + beg + k;
		if (GF(BvhNode, beg + k, object) == 0xffff) memcpy(n1 + k + 0x28, o + 0x28, 0x18);
		else { memcpy(n1 + k + 0x28, o + 0x28, 4); memcpy(n1 + k + 0x36, o + 0x36, 2); memcpy(n1 + k + 0x39, o + 0x39, 3); }
	}
	if (r1 != c->eax || cnt1 != GF(BvhBuilder, self, node_count) || end1 != GF(BvhBuilder, self, nodes_end) || memcmp(n1, xrc_mem + beg, nb) || memcmp(i1, xrc_mem + ia, ni)) {
		if (++n_bad <= 10) {
			fprintf(stderr, "xrc: FAST MISMATCH %08x (BVH build, %u objects): result %u / %u, nodes in use %u / %u (ours / original)\n",
			        FN_BUILD, n, r1, c->eax, cnt1, GF(BvhBuilder, self, node_count));
			if (memcmp(i1, xrc_mem + ia, ni)) fprintf(stderr, "xrc:   the index order differs\n");
			for (k = 0; k + 64 <= nb; k += 64)
				if (memcmp(n1 + k, xrc_mem + beg + k, 64)) {
					unsigned j;
					fprintf(stderr, "xrc:   node %u: ours", k / 64);
					for (j = 0; j < 64; j += 4) fprintf(stderr, " %08x", *(const u32 *)(n1 + k + j));
					fprintf(stderr, "\nxrc:   node %u: orig", k / 64);
					for (j = 0; j < 64; j += 4) fprintf(stderr, " %08x", M32(beg + k + j));
					fprintf(stderr, "\n");
					break;
				}
		}
	}
	if ((n_checked & (n_checked - 1)) == 0) fprintf(stderr, "xrc: BVH build check: %llu builds checked (up to %u objects), %llu mismatches, %llu skipped (the vector grew)\n", n_checked, n_max, n_bad, n_skipped);
done:
	free(n0); free(i0); free(n1); free(i1);
}

/* 00d785d0 */
void XRC_CC xo_00d785d0(XCtx *c) {
	static XFn orig;
	if (!orig) orig = xrc_translation_of(FN_BUILD);
	if (fast_mode < 0) fast_mode = getenv("XRC_NO_FAST") ? 2 : xrc_tool_env("XRC_FAST_CHECK") ? 1 : 0;
	if (fast_mode == 2 || in_orig) { orig(c); return; } /* (in_orig: the translation's own recursion, when checking) */
	if (fast_mode == 1) { checked(c, orig); return; }
	native(c);
}
