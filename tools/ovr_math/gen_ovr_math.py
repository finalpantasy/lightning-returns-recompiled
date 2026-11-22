#!/usr/bin/env python3
"""Generates src/runtime/ovr_math_gen.inc: straight-line C for the hot SSE helpers of LRFF13.exe, one statement per
guest instruction, read from the translator's page sources (build/xrc_gen/src/page_XXXX.c comments).
The guest stack frame (ebp-relative locals) is a local byte array; packed ops use the same xv_* / ex_* helpers the
translation uses, so the results are the translation's, bit for bit.  Usage: gen_ovr_math.py > ovr_math_gen.inc"""
import re

import os
SRC = os.environ.get('LR_GEN', 'build/xrc_gen') + '/src/page_%s.c'
# name, addr, kind ('this' = thiscall ecx, 'cdecl', 'reg' = register convention, no prologue), ret n (stdcall arg words)
FUNCS = [
    ('TransformVector4', '00408210', 'this', 2),
    ('Normalize',        '0040ab00', 'this', 1),
    ('Copy4x4',          '0040b1e0', 'this', 1),
    ('Fn00416140',       '00416140', 'this', 2),
    ('Fn004164f0',       '004164f0', 'cdecl', 0),
    ('SinCos4',          '00416020', 'reg', 0),
    ('Fn00416250',       '00416250', 'cdecl', 0),
    ('Transpose',        '005a44e0', 'this', 1),
    ('ExpandByPoint',    '004bb0b0', 'this', 2),
    ('TransformPoint',   '004bada0', 'this', 2),
    ('MakeTranslation',  '00437270', 'cdecl', 0),
    ('Inverse',          '00408320', 'this', 1),
    ('TransformByMatrix', '004bb1c0', 'this', 2, ('004bb1f7', '004bb5fc')),
    ('GetRotationQuat',  '00416a30', 'this', 1, ('00416a49', '00416a4e', '00416a50', '00416a57', '00416b7a', '00416bb5', '00416bb7', '00416bb8')),
    ('Multiply',         'MULTIPLY', 'this', 2),
    ('Fn00b8edb0',       '00b8edb0', 'this', 5, (), True),
    ('ComposeLocalToWorld', '00aac130', 'this', 3, ('00aac14c', '00aac14d', '00aac4aa', '00aac4f0')),
]
# callees a generated function can call directly: guest address -> (core name, argument words, who pops them: 'callee' for ret n, 'caller' for cdecl)
CALLEES = {
    0x416250: ('Fn00416250', 2, 'caller'),
    0x416140: ('Fn00416140', 2, 'callee'),
    0x4bb0b0: ('ExpandByPoint', 2, 'callee'),
    0x4bada0: ('TransformPoint', 2, 'callee'),
    0x416a30: ('GetRotationQuat', 1, 'callee'),
    0x4164f0: ('Fn004164f0', 2, 'caller'),
    0x408210: ('TransformVector4', 2, 'callee'),
    0x40b1e0: ('Copy4x4', 1, 'callee'),
    0x40acb0: ('Multiply', 2, 'callee'),
    0x5a44e0: ('Transpose', 1, 'callee'),
}
# 0040acb0 Matrix4x4::Multiply as a core (the hand-written gen_Multiply of ovr_math.c, with the same scratch use)
MULTIPLY = '''/* 0040acb0 Multiply as a core: out.row[r] = this.TransformVector4(rhs.row[r]) for r = 3..0 into temporaries, then the rows are copied to out. */
#define G_Multiply_READS 0xff
#define G_Multiply_WRITES 0x3f
static inline __attribute__((always_inline)) u32 core_Multiply(XCtx *c, X128 *X, u32 r_ecx, u32 esp0) {
	u8 *const xm = XM;
	u32 s = esp0 - 0x100u, out = M32(esp0 + 4), rhs = M32(esp0 + 8), tmp = s + 0x20u;
	u64 hi;
	int r;
	_Pragma("clang loop unroll(full)")
	for (r = 3; r >= 0; r--) {
		M32(s + 4) = tmp + 16u * (u32)r;
		M32(s + 8) = rhs + 16u * (u32)r;
		(void)core_TransformVector4(c, X, r_ecx, s);
	}
	_Pragma("clang loop unroll(full)")
	for (r = 0; r < 4; r++) {
		u64 lo_ = ld64(xm + tmp + 16u * (u32)r), hi_ = ld64(xm + tmp + 16u * (u32)r + 8);
		st64(xm + out + 16u * (u32)r, lo_);
		st64(xm + out + 16u * (u32)r + 8, hi_);
	}
	hi = ld64(xm + tmp + 0x30u + 8);
	X[0].v = _mm_castsi128_ps(_mm_cvtsi64_si128((long long)hi));
	return out;
}
'''
REGS = ('eax', 'ecx', 'edx', 'esi', 'edi')
CORE_INFO = {}  # core name -> (reads, writes) xmm sets


def disasm(addr):
    on = False
    out = []
    for l in open(SRC % addr[:4]).read().split('\n'):
        if l.startswith('/* ---- '):
            on = (l.split()[2] == addr)
            continue
        m = re.match(r'    /\* ([0-9a-f]{8}) (\S+) (.*) \*/$', l)
        if on and m:
            out.append((m.group(1), m.group(2), m.group(3)))
    return out


def imm(s):
    return int(s, 0)


def mem(op):
    """'[eax + 0xc]' -> ('g', C expr) guest memory, ('l', off) frame local, ('a', off) [ebx + off] argument"""
    m = re.match(r'\[(0x[0-9a-f]+)\]$', op)
    if m:
        return ('g', '0x%xu' % imm(m.group(1)))
    m = re.match(r'\[(eax|ecx|edx|esi|edi) \+ (eax|ecx|edx|esi|edi)\*(\d)(?: \+ (0x[0-9a-f]+))?\]$', op)
    if m:
        return ('x', m.group(1), m.group(2), int(m.group(3)), imm(m.group(4)) if m.group(4) else 0)
    m = re.match(r'\[(\w+)(?: ([+-]) (0x[0-9a-f]+))?\]$', op)
    assert m, op
    r, sg, d = m.group(1), m.group(2), m.group(3)
    off = imm(d) if d else 0
    if sg == '-':
        off = -off
    if r == 'ebp' and off > 0:
        return ('a', off)  # plain push ebp / mov ebp, esp frame: [ebp + 8] is the first argument
    if r == 'ebp':
        assert -0x200 <= off < 0
        return ('l', off)
    if r == 'ebx':
        return ('a', off)
    assert r in ('eax', 'ecx', 'edx', 'esi', 'edi'), op
    return ('r', r, off)


def gen(name, addr, kind, nret, skip=(), standalone=False):
    ins = disasm(addr)
    body = []
    written = set()
    readfirst = set()

    def rd(n):
        if n not in written:
            readfirst.add(n)

    def wr(n):
        written.add(n)

    def X(s):
        return int(s[3:])

    if kind != 'reg':
        k = next(i for i, t in enumerate(ins) if t[2].startswith('mov ebp, esp'))
        assert ins[k + 1][2].startswith('sub esp, '), ins[k + 1]
        e = max(i for i, t in enumerate(ins) if t[2] == 'mov esp, ebp')
        assert all(t[1] in ('pop', 'mov', 'ret') for t in ins[e:]) and ins[-1][1] == 'ret', ins[e:]
        ins = ins[k + 2:e]
    else:
        assert ins[-1][1] == 'ret'
        ins = ins[:-1]

    def calltgt(a, txt):
        return (int(a, 16) + 5 + imm(txt.split()[1])) & 0xffffffff

    callmode = any(t[1] == 'call' and t[0] not in skip and calltgt(t[0], t[2]) != 0x416020 for t in ins)
    jt = set()
    for i_, t in enumerate(ins):
        if t[1] in ('je', 'jmp'):
            jt.add((int(ins[i_ + 1][0], 16) + imm(t[2].split()[1])) & 0xffffffff)
    pushes = []  # compile-time model of the guest stack: names of the C variables holding the pushed values
    npush = [0]

    def addr_of(mm):
        if mm[0] == 'l' and callmode:
            return 'xm + (u32)(fb - %du)' % (-mm[1])
        if mm[0] == 'l':
            return 'L + %d' % (0x100 + mm[1])
        if mm[0] == 'a':
            return 'xm + (u32)(esp0 + %du)' % (mm[1] - 4)
        if mm[0] == 'x':
            return 'xm + (u32)(r_%s + r_%s * %du + %du)' % (mm[1], mm[2], mm[3], mm[4])
        if mm[0] == 'r':
            if kind == 'reg':
                return 'p_%s + %d' % (mm[1], mm[2])
            return 'xm + (u32)(r_%s + %du)' % (mm[1], mm[2] & 0xffffffff)
        return 'xm + (u32)(%s)' % mm[1]

    def is_mem(s):
        return s.startswith('[')

    lea = {}
    spush = []
    maxsv = sum(1 for t in ins if t[1] == 'push' and t[2] in ('push esi', 'push edi')) if not callmode else 0
    for a, mn, txt in ins:
        m = re.match(r'(\S+) (.*)$', txt)
        op = m.group(1)
        args = [s.strip() for s in re.split(r', (?![^\[]*\])', m.group(2))] if m else []
        c = '/* %s %s */ ' % (a, txt)
        if int(a, 16) in jt:
            body.append('L_%s:;' % a)
        if a in skip:
            body.append(c + '/* stack protector / saved esi: not needed */')
            continue
        if callmode and op in ('je', 'jmp'):
            nxt = ins[[t[0] for t in ins].index(a) + 1][0]
            tg = (int(nxt, 16) + imm(args[0])) & 0xffffffff
            body.append(c + ('if (zf) goto L_%08x;' % tg if op == 'je' else 'goto L_%08x;' % tg))
            continue
        if callmode and op in ('cmp.b', 'test.b'):
            mm = mem(args[0]); assert args[1] == 'cl' or args[1].isdigit()
            rhs = '(u8)r_ecx' if args[1] == 'cl' else '%su' % args[1]
            if op == 'cmp.b':
                body.append(c + 'zf = (*(%s) == %s);' % (addr_of(mm), rhs))
            else:
                body.append(c + 'zf = ((*(%s) & %s) == 0);' % (addr_of(mm), rhs))
            continue
        if callmode and op == 'mov' and args[0] == 'cl' and args[1].isdigit():
            body.append(c + 'r_ecx = (r_ecx & ~0xffu) | %su;' % args[1])
            continue
        if callmode and op == 'mov' and args[0] in REGS and args[1] in REGS:
            body.append(c + 'r_%s = r_%s;' % (args[0], args[1]))
            continue
        if callmode and op == 'lea' and re.match(r'\[(esi|edi|eax|ecx|edx) \+ 0x[0-9a-f]+\]$', args[1]):
            mm = mem(args[1])
            body.append(c + 'r_%s = r_%s + %du;' % (args[0], mm[1], mm[2]))
            continue
        if callmode and op == 'lea' and args[1].startswith('[ebp'):
            mm = mem(args[1]); assert mm[0] == 'l'
            body.append(c + 'r_%s = fb - %du;' % (args[0], -mm[1]))
            continue
        if callmode and op == 'push' and args[0] in REGS:
            v = 'p%d' % npush[0]; npush[0] += 1
            body.append(c + '%s = r_%s;' % (v, args[0]))
            pushes.append(v)
            continue
        if callmode and op == 'add' and args[0] == 'esp':
            n = imm(args[1]) // 4
            del pushes[len(pushes) - n:]
            body.append(c + '/* the caller drops the arguments */')
            continue
        if callmode and op == 'call':
            tgt = calltgt(a, txt)
            cn, na, who = CALLEES[tgt]
            av = pushes[::-1][:na]
            assert len(av) == na
            for k_, v in enumerate(av):
                body.append('	st32(xm + (u32)(s + %du), %s);' % (4 + 4 * k_, v))
            body.append(c + 'r_eax = core_%s(c, X, r_ecx, s);' % cn)
            if who == 'callee':
                del pushes[len(pushes) - na:]
            rd_, wr_ = CORE_INFO[cn]
            for r_ in rd_: rd(r_)
            for r_ in wr_: wr(r_)
            continue
        if not callmode and op == 'lea' and re.match(r'\[(eax|ecx|edx|esi|edi) \+ (eax|ecx|edx|esi|edi)\*\d(?: \+ 0x[0-9a-f]+)?\]$', args[1]):
            mm = mem(args[1])
            body.append(c + 'r_%s = r_%s + r_%s * %du + %du;' % (args[0], mm[1], mm[2], mm[3], mm[4]))
            continue
        if op == 'lea' and args[1].startswith('[ebp'):
            mm = mem(args[1])
            assert mm[0] == 'l'
            lea[args[0]] = mm[1]
            body.append(c + '/* pointer to the local */')
            continue
        if op == 'call':
            tgt = (int(a, 16) + 5 + imm(args[0] if args else txt.split()[1])) & 0xffffffff
            assert tgt == 0x416020 and 'eax' in lea and 'ecx' in lea, txt
            body.append(c + 'core_SinCos4(c, X, L + %d, L + %d);' % (0x100 + lea['eax'], 0x100 + lea['ecx']))
            written.update(range(8))
            continue
        if not callmode and op in ('push', 'pop') and args[0] in ('esi', 'edi'):
            if op == 'push':
                v = 'sv%d' % len(spush); spush.append(v)
                body.append(c + '%s = r_%s;' % (v, args[0]))
            else:
                v = spush.pop()
                body.append(c + 'r_%s = %s;' % (args[0], v))
            continue
        if op == 'movzx.w' and args[0] in REGS and is_mem(args[1]):
            body.append(c + 'r_%s = ld16(%s);' % (args[0], addr_of(mem(args[1]))))
            continue
        if op == 'imul' and args[0] in REGS and args[1] in REGS:
            body.append(c + 'r_%s = r_%s * r_%s;' % (args[0], args[0], args[1]))
            continue
        if op in ('mov', 'mov.d') and args[0] in REGS and is_mem(args[1]):
            mm = mem(args[1])
            assert mm[0] == 'a' or callmode, txt
            body.append(c + 'r_%s = ld32(%s);' % (args[0], addr_of(mm)))
            continue
        if op in ('movss.d', 'movss') and args[0].startswith('xmm') and is_mem(args[1]):
            d = X(args[0]); wr(d); p = addr_of(mem(args[1]))
            body.append(c + 'X[%d].v = _mm_load_ss((const float *)(%s));' % (d, p))
            continue
        if op in ('movss.d', 'movss') and is_mem(args[0]):
            s = X(args[1]); rd(s); p = addr_of(mem(args[0]))
            body.append(c + '_mm_store_ss((float *)(%s), X[%d].v);' % (p, s))
            continue
        if op == 'movss' and args[0].startswith('xmm') and args[1].startswith('xmm'):
            d, s = X(args[0]), X(args[1]); rd(d); rd(s); wr(d)
            body.append(c + 'X[%d].v = _mm_move_ss(X[%d].v, X[%d].v);' % (d, d, s))
            continue
        if op in ('movq.q', 'movq') and args[0].startswith('xmm') and is_mem(args[1]):
            d = X(args[0]); wr(d); p = addr_of(mem(args[1]))
            body.append(c + 'X[%d].v = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)(%s)));' % (d, p))
            continue
        if op in ('movq.q', 'movq') and is_mem(args[0]):
            s = X(args[1]); rd(s); p = addr_of(mem(args[0]))
            body.append(c + '_mm_storel_epi64((__m128i *)(%s), _mm_castps_si128(X[%d].v));' % (p, s))
            continue
        if op in ('movaps.x', 'movdqa.x') and args[0].startswith('xmm') and is_mem(args[1]):
            d = X(args[0]); wr(d); p = addr_of(mem(args[1]))
            body.append(c + 'X[%d].v = _mm_loadu_ps((const float *)(%s));' % (d, p))
            continue
        if op in ('movaps.x', 'movdqa.x') and is_mem(args[0]):
            s = X(args[1]); rd(s); p = addr_of(mem(args[0]))
            body.append(c + '_mm_storeu_ps((float *)(%s), X[%d].v);' % (p, s))
            continue
        if op in ('movaps', 'movdqa'):
            d, s = X(args[0]), X(args[1]); rd(s); wr(d)
            body.append(c + 'X[%d] = X[%d];' % (d, s))
            continue
        if op == 'xorps' and args[0] == args[1]:
            d = X(args[0]); wr(d)
            body.append(c + 'X[%d].v = _mm_setzero_ps();' % d)
            continue
        if op in ('unpcklps', 'unpckhps', 'andps', 'andnps', 'orps', 'xorps', 'pand'):
            d, s = X(args[0]), X(args[1]); rd(d); rd(s); wr(d)
            fn = {'pand': 'andps'}.get(op, op)
            body.append(c + 'X[%d] = xv_%s(X[%d], X[%d]);' % (d, fn, d, s))
            continue
        if op == 'shufps':
            d, s = X(args[0]), X(args[1]); k = imm(args[2]) & 0xff; rd(d); rd(s); wr(d)
            body.append(c + 'X[%d] = XV_SHUFPS(X[%d], X[%d], 0x%02x);' % (d, d, s, k))
            continue
        if op in ('mulps', 'addps', 'subps'):
            d, s = X(args[0]), X(args[1]); rd(d); rd(s); wr(d)
            body.append(c + 'X[%d] = ex_%s(X[%d], X[%d]);' % (d, op, d, s))
            continue
        if op in ('maxps', 'minps'):
            d, s = X(args[0]), X(args[1]); rd(d); rd(s); wr(d)
            body.append(c + 'X[%d] = ex_%s(X[%d], X[%d]);' % (d, op, d, s))
            continue
        if op in ('movlhps', 'movhlps'):
            d, s = X(args[0]), X(args[1]); rd(d); rd(s); wr(d)
            body.append(c + ('X[%d].v = _mm_movelh_ps(X[%d].v, X[%d].v);' if op == 'movlhps' else 'X[%d].v = _mm_movehl_ps(X[%d].v, X[%d].v);') % (d, d, s))
            continue
        if op == 'rcpps':
            d, s = X(args[0]), X(args[1]); rd(s); wr(d)
            body.append(c + 'X[%d] = xv_rcpps(X[%d], X[%d]);' % (d, d, s))
            continue
        if op in ('mulps.x', 'addps.x', 'subps.x'):
            d = X(args[0]); rd(d); wr(d); p = addr_of(mem(args[1]))
            body.append(c + '{ X128 t_; t_.v = _mm_loadu_ps((const float *)(%s)); X[%d] = ex_%s(X[%d], t_); }' % (p, d, op[:-2], d))
            continue
        if op == 'rsqrtps':
            d, s = X(args[0]), X(args[1]); rd(s); wr(d)
            body.append(c + 'X[%d] = xv_rsqrtps(X[%d], X[%d]);' % (d, d, s))
            continue
        if op == 'cvtdq2ps':
            d, s = X(args[0]), X(args[1]); rd(s); wr(d)
            body.append(c + 'X[%d] = xv_cvtdq2ps(X[%d], X[%d]);' % (d, d, s))
            continue
        if op == 'cvtps2dq':
            d, s = X(args[0]), X(args[1]); rd(s); wr(d)
            body.append(c + 'if (__builtin_expect(XRC_SSE_RC() == 0 && ((_mm_getcsr() >> 13) & 3) == 0, 1)) X[%d].vi = _mm_cvtps_epi32(X[%d].v); else { X128 s_ = X[%d], r_ = X[%d]; int k_; for (k_ = 0; k_ < 4; k_++) r_.u[k_] = cvt_round((double)s_.f[k_]); X[%d] = r_; }' % (d, s, s, d, d))
            continue
        if op == 'paddd':
            d, s = X(args[0]), X(args[1]); rd(s); rd(d); wr(d)
            body.append(c + 'X[%d].vi = _mm_add_epi32(X[%d].vi, X[%d].vi);' % (d, d, s))
            continue
        if op == 'cmpps':
            d, s = X(args[0]), X(args[1]); k = imm(args[2]); rd(d); rd(s); wr(d)
            body.append(c + 'X[%d] = xv_cmpps%d(X[%d], X[%d]);' % (d, k, d, s))
            continue
        raise SystemExit('unhandled %s %s' % (addr, txt))
    if callmode and any(t[1] in ('je', 'jmp') for t in ins):
        readfirst = set(range(8))  # a register written on one path only keeps the caller's value on the other
    CORE_INFO[name] = (set(readfirst), set(written))
    out = []
    out.append("/* %s %s: generated from the translator's page source. */" % (addr, name))
    out.append('#define G_%s_READS 0x%02x' % (name, sum(1 << i for i in readfirst)))
    out.append('#define G_%s_WRITES 0x%02x' % (name, sum(1 << i for i in written)))
    if kind == 'reg':
        out.append('static inline __attribute__((always_inline)) u32 core_%s(XCtx *c, X128 *X, u8 *p_eax, u8 *p_ecx) {' % name)
        out.append('	u32 r_eax = c->eax; u8 *const xm = XM; u32 r_ecx = 0, esp0 = 0; u8 *L = 0;')
    elif callmode:
        out.append('static inline __attribute__((always_inline)) u32 core_%s(XCtx *c, X128 *X, u32 r_ecx, u32 esp0) {' % name)
        out.append('	u8 *const xm = XM; u32 r_eax = c->eax, r_edx = c->edx, r_esi = c->esi, r_edi = c->edi; u32 fb = (esp0 - 12u) & ~0xfu, s = fb - 0x200u; int zf = 0;')
        out.append('	u32 %s;' % ', '.join('p%d' % i for i in range(max(npush[0], 1))))
        out.append('	u8 *L = 0; (void)L; (void)zf; (void)r_edx; (void)r_esi; (void)r_edi; (void)s; (void)fb;')
    else:
        out.append('static inline __attribute__((always_inline)) u32 core_%s(XCtx *c, X128 *X, u32 r_ecx, u32 esp0) {' % name)
        out.append('	u8 *const xm = XM; u8 L[0x200] __attribute__((aligned(16))); u32 r_eax = c->eax, r_edx = c->edx, r_esi = c->esi, r_edi = c->edi;')
        out.append('	(void)r_edx; (void)r_esi; (void)r_edi;')
        if maxsv:
            out.append('	u32 %s;' % ', '.join('sv%d' % i for i in range(maxsv)))
    out.append('	(void)xm; (void)L; (void)r_ecx; (void)esp0; (void)c; (void)r_eax;')
    if kind == 'reg':
        out.append('	(void)p_eax; (void)p_ecx;')
    for b in body:
        out.append('\t' + b)
    if standalone:
        out.append('\tc->ecx = r_ecx; c->edx = r_edx; c->esi = r_esi; c->edi = r_edi;')
    out.append('\treturn r_eax;')
    out.append('}')
    w = ['static void XRC_CC gen_%s(XCtx *c) {' % name, '\tX128 X[8]; u32 eax_; u8 *const xm_ = XM; (void)xm_;',
         ''.join(chr(9) + 'X[%d] = c->x[%d];' % (i_, i_) + chr(10) for i_ in sorted(CORE_INFO[name][0])) + chr(9) + '(void)0;',
         ('\teax_ = core_%s(c, X, xm_ + c->eax, xm_ + c->ecx);' if kind == 'reg' else '\teax_ = core_%s(c, X, c->ecx, c->esp);') % name,
         ''.join(chr(9) + 'c->x[%d] = X[%d];' % (i_, i_) + chr(10) for i_ in sorted(CORE_INFO[name][1])) + chr(9) + '(void)0;']
    if kind != 'reg':
        w.append('\tc->eax = eax_;')
    else:
        w.append('\t(void)eax_;')
    w.append('\tXRC_CDECL_RET(c);' if nret == 0 else '\tXRC_STDCALL_RET(c, %d);' % nret)
    w.append('}')
    return '\n'.join(out + w)


print('/* ovr_math_gen.inc - GENERATED by tools/ovr_math/gen_ovr_math.py; do not edit. */')
for f in FUNCS:
    if f[1] == 'MULTIPLY':
        CORE_INFO['Multiply'] = (set(range(8)), set(range(6)))
        print(MULTIPLY)
        continue
    print(gen(*f))
    print()
