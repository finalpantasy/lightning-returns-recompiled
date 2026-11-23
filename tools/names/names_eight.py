"""Eighth naming pass (names8, 2026-11-22; runs last in build_names.py, after names_seven). Targets: functions with no name,
and functions whose name is one of the relation guesses made by names_seven (sources solo7 chain7 ns7 callee7 caller7 adj7).
Every other name is left alone. New names are low confidence, rank 1.0 (above names7's 0.9). Evidence kinds (= source column):
  vtloc8   vtable of a function-local class (RTTI name like .?AVLocal@?1??func@Cls@ns@@..) that build_names could not demangle:
           Cls::func::Local::vf<NN>
  regcall8 function address passed as an argument next to exactly one string argument of the same call (push str / push fn /
           call register): cb_<string>
  node8    function address stored into a record and a string stored a few instructions later with no call between
           (the AppGame update/draw node table): cb_<string>
  datastr8 function pointer in .rdata/.data with a string pointer in the adjacent word: cb_<string>
A function seen with two different strings is skipped. Names are made unique by suffixing the 8-digit address on a clash.
"""
import re, struct, collections

WEAK = ('solo7', 'chain7', 'ns7', 'callee7', 'caller7', 'adj7')
RX = re.compile(r'0x([0-9a-f]{5,8})\b')


def san(s, n=40):
    s = re.sub(r'[^A-Za-z0-9]+', '_', s).strip('_')
    return s[:n] if len(re.findall(r'[A-Za-z]', s)) >= 4 else None


def run(c):
    names, put, funcs, F, fset = c['names'], c['put'], c['funcs'], c['F'], c['fset']
    img, BASE, secs, vtables = c['img'], c['BASE'], c['sections'], c['vtables']
    c['RANK']['vtloc8'] = c['RANK']['regcall8'] = c['RANK']['node8'] = c['RANK']['datastr8'] = 1.0
    tg = {f for f in funcs if f not in names or names[f][1] in WEAK}
    used = {v[0] for v in names.values()}
    st = collections.Counter(); st['targets'] = len(tg)
    rd = [s for s in secs if s[0] in ('.rdata', '.data')]
    rdata = [s for s in secs if s[0] == '.rdata'][0]

    def u32(a):
        o = a - BASE
        return struct.unpack_from('<I', img, o)[0] if 0 <= o <= len(img) - 4 else 0

    def sat(a):
        if not any(s[1] <= a < s[2] for s in rd): return None
        o = a - BASE
        e = img.find(b'\0', o, o + 120)
        if e - o < 4: return None
        b = img[o:e]
        if any(ch < 0x20 or ch > 0x7e for ch in b): return None
        return b.decode('latin1')

    def give(f, nm, src):
        if f not in tg: return False
        if nm in used: nm = '%s_%08x' % (nm, f)
        if nm in used: return False
        old = names.get(f)
        if put(f, nm, src, 'low', rank=1.0):
            if old: used.discard(old[0])
            used.add(nm); st[src] += 1
            if old: st['upgraded_' + src] += 1
            return True
        return False

    # ---- vtloc8
    vtset = set(vtables)
    for a in range(rdata[1], rdata[2] - 8, 4):
        col = u32(a)
        if not (rdata[1] <= col < rdata[2]) or a + 4 in vtset: continue
        sig, off, cd, td = struct.unpack_from('<IIII', img, col - BASE)
        if sig != 0 or off > 0x10000: continue
        o = td - BASE + 8
        if not (0 <= o < len(img)) or img[o:o + 4] not in (b'.?AV', b'.?AU'): continue
        raw = img[o + 4:img.find(b'\0', o)].decode('latin1')
        m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)@\?[0-9A-Z]+\?\?([A-Za-z_][A-Za-z0-9_]*|\?[0-9A-Z])@((?:[A-Za-z_][A-Za-z0-9_]*@)+)@', raw)
        if not m: continue
        loc, fn, outer = m.group(1), m.group(2), [x for x in m.group(3).split('@') if x]
        fn = {'?0': 'ctor', '?1': 'dtor'}.get(fn, fn)
        base = '::'.join(outer[::-1] + [fn, loc])
        v, i = a + 4, 0
        while u32(v + 4 * i) in fset and v + 4 * i < rdata[2] - 4:
            give(u32(v + 4 * i), '%s::vf%d' % (base, i), 'vtloc8'); i += 1

    # ---- regcall8 / node8 : scan code
    hits = collections.defaultdict(lambda: collections.defaultdict(set))  # fn -> src -> {string}
    for r, d in F.items():
        ins = d['ins']
        n = len(ins)
        segstart = 0
        strs = {}   # index -> string
        fns = []
        for i, (a, mn, txt) in enumerate(ins):
            if mn in ('call', 'ret', 'jmp'):
                segstart = i + 1
                continue
            if mn not in ('push', 'mov', 'lea', 'mov.d'): continue
            for h in RX.findall(txt):
                v = int(h, 16)
                if v in fset and v in tg: fns.append((i, v))
                elif v >= 0x400000:
                    s = sat(v)
                    if s and san(s): strs[i] = (s, segstart)
        for i, f in fns:
            # regcall: arguments of the next call within 6 instructions
            j = i + 1
            while j < n and j <= i + 6 and ins[j][1] not in ('call', 'ret', 'jmp'): j += 1
            if j < n and ins[j][1] == 'call':
                lo = i
                while lo > 0 and ins[lo - 1][1] not in ('call', 'ret', 'jmp') and lo > i - 8: lo -= 1
                ss = {strs[k][0] for k in range(lo, j) if k in strs}
                if len(ss) == 1:
                    hits[f]['regcall8'].add(next(iter(ss))); continue
            # node: nearest string after the fn store in the same call-free run
            for k in range(i + 1, min(n, i + 19)):
                if ins[k][1] in ('call', 'ret', 'jmp'): break
                if k in strs:
                    if '[' in ins[k][2]: hits[f]['node8'].add(strs[k][0])
                    break
    for f, d in hits.items():
        for src in ('regcall8', 'node8'):
            ss = d.get(src)
            if not ss: continue
            allss = set().union(*d.values())
            if len(allss) != 1: st['skipped_ambiguous'] += 1; break
            give(f, 'cb_' + san(next(iter(ss))), src); break

    # ---- datastr8
    for s in rd:
        for a in range(s[1], s[2] - 8, 4):
            v = u32(a)
            if v in tg and v not in names or (v in tg and names[v][1] in WEAK):
                for off in (-4, 4):
                    sv = sat(u32(a + off))
                    if sv and san(sv) and u32(a + off) not in fset:
                        give(v, 'cb_' + san(sv), 'datastr8'); break
    import sys
    print('names_eight:', dict(st), file=sys.stderr)
