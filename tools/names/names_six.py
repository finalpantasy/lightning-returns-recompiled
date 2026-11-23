"""Sixth naming pass (names6, 2026-11-22; runs last in build_names.py, after names_five). Only ever names functions
that have no name yet, except jni_native, which also replaces weak guesses (range/leaf/callercls style, rank < 3.5).
Never touches manual, rtti, string, lib, import or thunk names. All results are static guesses from the exe bytes.

  jni_native   Script/native-API registration tables in .data: 12-byte records {name*, fn, class*} whose two strings are
               stored XOR 0xAB (e.g. "Party", "addAbilityBase"). Name = JavaNativeInterface<Class>::<method>.      [med]
  eh_action    unwind-map action funclets of MSVC C++ EH FuncInfo tables (magic 0x1993052x): eh_unwind_act_<addr>   [low]
  eh_catch     catch handler funclets (HandlerType.addressOfHandler): eh_catch_<addr>                              [low]
  nullsub      single 'ret' / 'ret N' function: nullsub_<addr>                                                     [low]
  getter/setter  leaf function that only reads or writes one field of this (ecx) or one global: get_fN / set_fN,
               get_fN_bitK, get_fN_maskM, get_fN_pM, get_gADDR, ret_const_V. Class prefix when the named callers
               agree (>= 3 named callers, >= 75% in one class), else a plain name plus the address.               [low]
  tramp        <= 4 insns that only load ecx/eax/imm and jmp a function that already has a good name:
               tramp_<target>                                                                                     [low]
  copyloop     leaf with 'rep movs' (memcpy_like) or 'rep stos' (memset_like) and <= 30 insns                      [low]
  refcount     leaf AddRef (inc [ecx+N] / lock inc / xadd +1, returns)                                              [low]
  algo         string hashes (FNV-1 prime 0x1000193, LCG 0x41c64e6d+0x3039), SHA-1/MD5 init, MT19937 init, Adler32,
               memcmp-like (cmps), atomic xchg/xadd/cmpxchg leaves                                                  [med/low]
  stridetbl    tables of function pointers at a fixed stride 8..64 bytes (>= 3 records): <loader>_tbl or
               StrideTbl_<base>_s<stride> ::slot_<i>                                                               [low]
  wrap/tramp   (see tramp) when the wrapper reloads ecx (calls a method on another object) the class is not claimed:
               calls_<Class>_<addr>; otherwise <Class>::wrap_<addr> / fn@<File>::wrap_<addr>                        [low]
  callerdom    unnamed function whose named callers are >= 80% (and >= 3) in one class, or all (>= 2) in one class,
               or >= 70% (and >= 2) in one source file (fn@File.cpp::helper_<addr>): Cls::helper_<addr>             [low]
  Run with NAMES_NO_COPY=1 to skip writing build/win/names.tsv and the facts pickle (only analysis/ is written).
"""
import re, struct, collections, sys

CLS_SRC = ('rtti_vf', 'string', 'manual', 'rtti', 'override', 'callercls', 'callercls2', 'leaf1', 'calleecls', 'globcls',
           'callerns', 'lib', 'jni_native', 'range', 'callerdom', 'dataref_cls')
SKIP_PFX = ('shared_stub', 'j_', 'w_', 'w2_', 'fn@', 'use_', 'log', 'ref_', 'str_', 'eh_', 'operator', 'std::', 'strref_',
            'tramp_', 'get_', 'set_', 'nullsub', 'cb_', 'DataTbl_', 'Tbl_')


def cls_of_name(n):
    if n.startswith(SKIP_PFX) or '::' not in n or '<' in n or '(' in n: return None
    c = n.rsplit('::', 1)[0]
    return c if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_:]*', c) else None


def parse(t):
    """'mov.d eax, [ecx + 0x680]' -> ('mov.d', ['eax', '[ecx+0x680]'])"""
    p = t.split(' ', 1)
    ops = [x.strip().replace(' ', '') for x in p[1].split(',')] if len(p) > 1 else []
    return p[0], ops


def run(c):
    names, put, funcs, F = c['names'], c['put'], c['funcs'], c['F']
    callees, callers, nins, fimps, fset = c['callees'], c['callers'], c['nins'], c['fimps'], c['fset']
    img, BASE, secs, RANK = c['img'], c['BASE'], c['sections'], c['RANK']
    RANK.update({'jni_native': 3.9, 'eh_action': 1.12, 'eh_catch': 1.12, 'nullsub6': 1.3, 'acc6': 1.25, 'tramp6': 1.3,
                 'copyloop6': 1.3, 'refcount6': 1.3, 'callerdom': 1.05, 'algo6': 1.4, 'stridetbl6': 1.12})
    st = collections.Counter()

    def u32(a):
        o = a - BASE
        return struct.unpack_from('<I', img, o)[0] if 0 <= o <= len(img) - 4 else 0
    DATA = [s for s in secs if s[0] == '.data'][0]
    RD = [s for s in secs if s[0] == '.rdata'][0]

    # ------------------------------------------------------------ jni_native
    def dec(a):
        o = a - BASE
        if not (0 <= o < len(img) - 80): return None
        e = img.find(b'\0', o, o + 80)
        if e - o < 2: return None
        s = bytes(b ^ 0xab for b in img[o:e])
        return s.decode('latin1') if re.fullmatch(rb'[A-Za-z_][A-Za-z0-9_]*', s) else None
    for a in range(DATA[1], DATA[2] - 12, 4):
        f = u32(a + 4)
        if f not in fset: continue
        n, cl = dec(u32(a)), dec(u32(a + 8))
        if not (n and cl): continue
        old = names.get(f)
        if old and old[3] >= 3.5: continue
        if put(f, 'JavaNativeInterface%s::%s' % (cl[:1].upper() + cl[1:], n), 'jni_native', 'med'): st['jni_native'] += 1

    # ------------------------------------------------------------ EH tables
    fi = []
    for a in range(RD[1], RD[2] - 28, 4):
        if u32(a) in (0x19930522, 0x19930521, 0x19930523):
            ms, pu, nt, pt = u32(a + 4), u32(a + 8), u32(a + 12), u32(a + 16)
            if 0 < ms < 600 and RD[1] <= pu < RD[2]: fi.append((a, ms, pu, nt, pt))
    for a, ms, pu, nt, pt in fi:
        for i in range(ms):
            t = u32(pu + 8 * i + 4)
            if t in fset and t not in names:
                if put(t, 'eh_unwind_act_%08x' % t, 'eh_action', 'low'): st['eh_action'] += 1
        if 0 < nt < 64 and RD[1] <= pt < RD[2]:
            for i in range(nt):
                te = pt + 20 * i
                nc, ph = u32(te + 12), u32(te + 16)
                if 0 < nc < 16 and RD[1] <= ph < DATA[2]:
                    for j in range(nc):
                        h = u32(ph + 16 * j + 12)
                        if h in fset and h not in names:
                            if put(h, 'eh_catch_%08x' % h, 'eh_catch', 'low'): st['eh_catch'] += 1

    # ------------------------------------------------------------ caller class
    def cls(a):
        v = names.get(a)
        return cls_of_name(v[0]) if v and v[1] in CLS_SRC else None

    def caller_class(f, minn=3, frac=0.75):
        cl = [cls(x) for x in callers.get(f, ()) if x != f]
        cl = [x for x in cl if x]
        if len(cl) < minn: return None
        k, n = collections.Counter(cl).most_common(1)[0]
        return k if n >= frac * len(cl) else None

    # ------------------------------------------------------------ leaf behavior
    RX_MEM = re.compile(r'^\[(\w+)(?:\+(0x[0-9a-f]+))?\]$')
    RX_ABS = re.compile(r'^\[(0x[0-9a-f]+)\]$')
    SZ = {'mov.b': 'u8', 'mov.w': 'u16', 'movzx.b': 'u8', 'movzx.w': 'u16', 'movsx.b': 's8', 'movsx.w': 's16'}

    def fld(op):
        m = RX_MEM.match(op)
        if m and m.group(1) == 'ecx': return int(m.group(2) or '0', 16)
        return None

    LOADS = ('mov.d', 'mov.b', 'mov.w', 'movzx.b', 'movzx.w', 'movsx.b', 'movsx.w')

    def classify(f):
        """-> (kind, suffix) or None for a small leaf"""
        ins = F.get(f, {}).get('ins')
        if not ins or len(ins) > 14: return None
        P = [parse(t) for _, _, t in ins]
        if not P[-1][0].startswith('ret'): return None
        if any(o.startswith(('j', 'call', 'rep')) for o, _ in P): return None
        ops = [o for o, _ in P]
        # fixed-point field read: fild + fmul of a field of this
        if 'fild.d' in ops and 'fmul.q' in ops and len(P) <= 14:
            for o, a in P:
                if o in LOADS and a[0] in ('eax', 'ecx', 'edx') and fld(a[1]) is not None:
                    return ('get', 'f%x_scaled' % fld(a[1]))
            return None
        if P[0] == ('push', ['ebp']) and len(P) > 3 and P[1] == ('mov', ['ebp', 'esp']):
            if P[-2] != ('pop', ['ebp']) or not P[-1][0].startswith('ret'): return None
            P = P[2:-2] + [P[-1]]
        if len(P) == 1: return ('nullsub', '')
        r = P[:-1]
        if len(r) > 6: return None
        # getter chain: load from [ecx+N] (or a global), optional [eax+M] loads, shifts and masks
        if r[0][0] in LOADS and r[0][1][0] in ('eax', 'al', 'ax'):
            src = r[0][1][1]
            fo = fld(src)
            am = RX_ABS.match(src)
            sz0 = SZ.get(r[0][0], '')
            if fo is None and not am: pass
            else:
                parts = [('f%x' % fo) if fo is not None else ('g%s' % am.group(1)[2:])]
                if sz0: parts.append(sz0)
                ok = True
                for o, a in r[1:]:
                    if o in LOADS and a[0] in ('eax', 'al', 'ax'):
                        m = RX_MEM.match(a[1])
                        if not (m and m.group(1) == 'eax'): ok = False; break
                        parts.append('p%x' % int(m.group(2) or '0', 16))
                        if SZ.get(o): parts.append(SZ[o])
                    elif o == 'shr' and a[0] == 'eax': parts.append('shr%d' % int(a[1], 16))
                    elif o == 'shl' and a[0] == 'eax': parts.append('shl%d' % int(a[1], 16))
                    elif o == 'sar' and a[0] == 'eax': parts.append('sar%d' % int(a[1], 16))
                    elif o == 'and' and a[0] in ('eax', 'al') and a[1].startswith('0x') or o == 'and' and a == ['eax', '1']:
                        if a[1] == '1' and parts[-1].startswith('shr'): parts[-1] = 'bit' + parts[-1][3:]
                        elif a[1] == '1': parts.append('bit0')
                        else: parts.append('mask' + a[1].replace('0x', ''))
                    else: ok = False; break
                if ok: return ('get', '_'.join(parts))
        if len(r) == 1 and r[0][0] == 'lea' and r[0][1][0] == 'eax':
            fo = fld(r[0][1][1])
            if fo is not None: return ('get', 'addr_f%x' % fo)
        if len(r) == 1 and r[0][0] == 'mov' and r[0][1][0] == 'eax' and r[0][1][1].startswith('0x'):
            return ('ret', 'const_%s' % r[0][1][1][2:])
        if len(r) == 1 and r[0][0] == 'fld.d':
            fo = fld(r[0][1][0])
            if fo is not None: return ('get', 'f%x_f32' % fo)
        if len(r) == 1 and r[0][0] == 'movss.d' and r[0][1][0] == 'xmm0':
            fo = fld(r[0][1][1])
            if fo is not None: return ('get', 'f%x_f32' % fo)
        if len(r) == 2 and r[0][0] in ('mov.d', 'mov.b', 'mov.w', 'movss.d') and r[1][0] == r[0][0]:
            m = RX_MEM.match(r[0][1][1])
            fo = fld(r[1][1][0])
            if m and m.group(1) in ('ebp', 'esp') and fo is not None and r[1][1][1] == r[0][1][0]:
                t = {'mov.d': '', 'mov.b': '_u8', 'mov.w': '_u16', 'movss.d': '_f32'}[r[0][0]]
                return ('set', 'f%x%s' % (fo, t))
            if m and m.group(1) in ('ebp', 'esp') and RX_ABS.match(r[1][1][0]) and r[1][1][1] == r[0][1][0]:
                return ('set', 'g%s' % RX_ABS.match(r[1][1][0]).group(1)[2:])
        return None

    for f in funcs:
        if f in names: continue
        k = classify(f)
        if not k: continue
        kind, suf = k
        if kind == 'nullsub':
            if put(f, 'nullsub_%08x' % f, 'nullsub6', 'low'): st['nullsub'] += 1
            continue
        pre = ''
        if suf.startswith(('f', 'addr_f')):
            cl = caller_class(f)
            pre = (cl + '::') if cl else ''
        nm = pre + kind + '_' + suf
        if not pre: nm += '_%08x' % f
        if put(f, nm, 'acc6', 'low'): st['accessor_' + kind] += 1

    # ------------------------------------------------------------ tramp: tiny reg-setup + jmp to a named function
    GOOD = ('manual', 'rtti', 'rtti_vf', 'string', 'lib', 'import', 'jni_native', 'callercls', 'callercls2', 'leaf1',
            'calleecls', 'globcls')

    RX_ECXW = re.compile(r'^(?:mov(?:\.d)?|lea|xor|pop|movzx(?:\.\w)?|add|sub) ecx\b')

    def ecx_written(f):
        return any(RX_ECXW.match(t) for _, _, t in F.get(f, {}).get('ins', []))

    def tramp():
        for f in funcs:
            if f in names: continue
            ins = F.get(f, {}).get('ins')
            if not ins or len(ins) > 4 or ins[-1][1] != 'jmp': continue
            a = ins[-1][0]
            b = img[a - BASE]
            if b == 0xe9: t = (a + 5 + struct.unpack_from('<i', img, a - BASE + 1)[0]) & 0xffffffff
            elif b == 0xeb: t = (a + 2 + struct.unpack_from('<b', img, a - BASE + 1)[0]) & 0xffffffff
            else: continue
            if t not in names or t == f: continue
            if not all(parse(x[2])[0] in ('mov', 'mov.d', 'lea', 'push', 'pop', 'add', 'sub') for x in ins[:-1]): continue
            tn = names[t]
            if tn[0].startswith(('eh_', 'shared_stub')): continue
            n = tn[0]
            if tn[1] in GOOD and not n.startswith('fn@'):
                while n.startswith(('tramp_', 'j_', 'w_')): n = n.split('_', 1)[1]
                if put(f, 'tramp_' + n, 'tramp6', 'low'): st['tramp'] += 1
                continue
            cl = cls_of_name(n)
            if cl:
                if ecx_written(f):  # calls a method on some other object (global or field): not a method of cl
                    if put(f, 'calls_%s_%x' % (cl.split('::')[-1], f), 'tramp6', 'low'): st['calls_cls'] += 1
                elif put(f, '%s::tramp_%x' % (cl, f), 'tramp6', 'low'): st['tramp_cls'] += 1
                continue
            m = re.match(r'fn@([A-Za-z0-9_]+\.(?:cpp|c|h|inl))', n)
            if m and not ecx_written(f) and put(f, 'fn@%s::tramp_%x' % (m.group(1), f), 'tramp6', 'low'): st['tramp_file'] += 1

    def wrapcls():
        for f in funcs:
            if f in names: continue
            cs = callees.get(f, ())
            if len(cs) != 1 or nins.get(f, 99) > 16 or fimps.get(f): continue
            t = next(iter(cs))
            if t == f or t not in names: continue
            tn = names[t]
            if tn[0].startswith(('eh_', 'shared_stub', 'operator', 'std::')): continue
            cl = cls_of_name(tn[0])
            if cl:
                if ecx_written(f):
                    if put(f, 'calls_%s_%x' % (cl.split('::')[-1], f), 'tramp6', 'low'): st['calls_cls'] += 1
                elif put(f, '%s::wrap_%x' % (cl, f), 'tramp6', 'low'): st['wrap_cls'] += 1
                continue
            m = re.match(r'fn@([A-Za-z0-9_]+\.(?:cpp|c|h|inl))', tn[0])
            if m and not ecx_written(f) and put(f, 'fn@%s::wrap_%x' % (m.group(1), f), 'tramp6', 'low'): st['wrap_file'] += 1

    # ------------------------------------------------------------ copy loops / refcounts
    for f in funcs:
        if f in names: continue
        ins = F.get(f, {}).get('ins')
        if not ins or len(ins) > 30 or callees.get(f) or fimps.get(f): continue
        txt = [t for _, _, t in ins]
        if any('movs' in t and 'rep' in t for t in txt):
            if put(f, 'memcpy_like_%08x' % f, 'copyloop6', 'low'): st['memcpy_like'] += 1
        elif any('stos' in t and 'rep' in t for t in txt):
            if put(f, 'memset_like_%08x' % f, 'copyloop6', 'low'): st['memset_like'] += 1
    for f in funcs:
        if f in names: continue
        ins = F.get(f, {}).get('ins')
        if not ins or len(ins) > 8: continue
        j = ' ; '.join(o + ' ' + ','.join(a) for o, a in (parse(t) for _, _, t in ins))
        if re.fullmatch(r'(lock )?inc(\.d)? \[ecx\+0x[0-9a-f]+\] ; ret', j) or re.fullmatch(
                r'mov\.d eax,\[ecx\+0x[0-9a-f]+\] ; inc eax ; mov\.d \[ecx\+0x[0-9a-f]+\],eax ; ret', j) or re.fullmatch(
                r'mov eax,1 ; lock xadd(\.d)? \[ecx\+0x[0-9a-f]+\],eax ; inc eax ; ret', j):
            cl = caller_class(f, 2)
            if put(f, (cl + '::' if cl else '') + 'AddRef_%08x' % f, 'refcount6', 'low'): st['refcount_addref'] += 1

    # ------------------------------------------------------------ algorithm constants and atomics
    for f in funcs:
        if f in names: continue
        ins = F.get(f, {}).get('ins')
        if not ins or len(ins) > 140: continue
        txt = ' '.join(t for _, _, t in ins)
        nm = None
        if len(ins) <= 30 and re.search(r'imul \w+, \w+, 0x1000193\b', txt) and 'xor' in txt:
            nm = 'StringHash_FNV1_%08x' % f
        elif len(ins) <= 30 and re.search(r'imul \w+, \w+, 0x41c64e6d\b', txt) and '0x3039' in txt:
            nm = 'StringHash_LCG_%08x' % f
        elif len(ins) <= 12 and '0x67452301' in txt and '0x10325476' in txt:
            nm = 'Sha1_Init_%08x' % f
        elif len(ins) <= 12 and '0x67452301' in txt:
            nm = 'Md5_or_Sha1_Init_%08x' % f
        elif len(ins) <= 40 and '0x6c078965' in txt:
            nm = 'Mt19937_Init_%08x' % f
        elif '0xfff1' in txt and not callees.get(f) and re.search(r'\bdiv\b', txt):
            nm = 'Adler32_%08x' % f
        elif re.search(r'\bcmps', txt) and len(ins) <= 40 and not callees.get(f):
            nm = 'memcmp_like_%08x' % f
        elif len(ins) <= 8 and not callees.get(f):
            if re.search(r'\bxchg\.d \[', txt): nm = 'atomic_xchg_%08x' % f
            elif re.search(r'\bxadd', txt): nm = 'atomic_add_%08x' % f
            elif re.search(r'\bcmpxchg', txt): nm = 'atomic_cas_%08x' % f
        if nm and put(f, nm, 'algo6', 'med' if nm.startswith(('StringHash', 'Sha1', 'Mt19937', 'Adler')) else 'low'):
            st['algo'] += 1

    # ------------------------------------------------------------ strided function-pointer tables (stride 8..64 bytes)
    slots = []
    for s_ in secs:
        if s_[0] not in ('.rdata', '.data'): continue
        for a in range(s_[1], s_[2] - 3, 4):
            if u32(a) in fset: slots.append(a)
    sl = set(slots)
    loaders = collections.defaultdict(set)
    for f, ds in c['dataref'].items():
        for d in ds: loaders[d].add(f)
    best = {}
    for a in slots:
        for stride in range(8, 0x41, 4):
            if a - stride in sl: continue
            n = 0
            b = a
            while b in sl: n += 1; b += stride
            if n >= 3:
                for i in range(n):
                    x = a + i * stride
                    if x not in best or best[x][2] < n: best[x] = (a, stride, n, i)
    for x, (a, stride, n, i) in sorted(best.items()):
        f = u32(x)
        if f in names or stride == 4: continue
        ld = [names[l][0] for l in loaders.get(a, ()) if l in names and not names[l][0].startswith(('fn@', 'shared_stub', 'eh_'))]
        ld = [l for l in ld if '?' not in l and '(' not in l]
        base = (ld[0] + '_tbl') if len(set(ld)) == 1 and ld else 'StrideTbl_%x_s%d' % (a, stride)
        if put(f, '%s::slot_%d' % (base, i), 'stridetbl6', 'low'): st['stride_tbl'] += 1

    # ------------------------------------------------------------ callerdom / callerfile
    def file_tag(a):
        v = names.get(a)
        if not v or v[1] in ('thunk', 'wrap2', 'wrap3', 'impuse'): return None
        m = re.match(r'fn@([A-Za-z0-9_]+\.(?:cpp|c|h|inl))', v[0])
        return m.group(1) if m else None
    for rnd in range(3):
        tramp()
        wrapcls()
        for f in funcs:
            if f in names: continue
            cl = caller_class(f, 3, 0.8)
            if cl and put(f, '%s::helper_%x' % (cl, f), 'callerdom', 'low'): st['callerdom'] += 1
        for f in funcs:
            if f in names: continue
            cl = caller_class(f, 2, 1.0)
            if cl and put(f, '%s::helper_%x' % (cl, f), 'callerdom', 'low'): st['callerdom2'] += 1
        for f in funcs:
            if f in names: continue
            cs = [x for x in callers.get(f, ()) if x != f]
            tg = [file_tag(x) for x in cs]
            tg = [t for t in tg if t]
            if len(tg) >= 2 and len(set(tg)) == 1 and len(tg) >= 0.7 * len(cs):
                if put(f, 'fn@%s::helper_%x' % (tg[0], f), 'callerdom', 'low'): st['callerfile'] += 1
    print('names_six:', dict(st), file=sys.stderr)
