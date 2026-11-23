"""Seventh naming pass (names7, 2026-11-22; runs last in build_names.py, after names_six). Only names functions that have
no name yet. Every name carries its address so it is unique, and its evidence kind is the 'source' column (all low).
  solo7    the function has exactly one caller (data refs count) and that caller is named: Cls::helper_<addr> when the
           caller's class is known, else helper_of_<caller>_<addr>.                                       [low]
  chain7   same rule, applied again after solo7 (callee of a function named by solo7/chain7). Keeps the root caller.
  ns7      >= 2 named callers share a class (>= 60%) or a top-level namespace / name stem: <Ns>::util_<addr>.  [low]
  callee7  no better evidence, but the function calls named functions: calls_<best callee>_<addr>. Callee choice: highest
           naming rank (manual > rtti > string ...), generic runtime names (operator, eh_, std::...) skipped.   [low]
  caller7  >= 2 named callers with no common class/namespace: called_by_<first caller>_<addr> (one stem only if 50%+ agree).
  adj7     no call edges at all: address neighbors on both sides (distance <= 3 functions) are named by a strong source and
           share a class/file: <Cls>::adj_<addr>.                                                            [low]
"""
import re, collections

RANKOF = {}
GENERIC = ('operator', 'std::', 'eh_', 'nullsub', 'shared_stub', 'memcpy', 'memset', 'free', 'malloc', 'strlen', 'ref_', 'str_',
           'DataTbl_', 'Tbl_', 'get_', 'set_', 'ret_', 'cb_', 'log[', 'tramp_', 'zlib', 'Miles', 'CRT', 'MemoryHeap', 'white::Mutex', 'Mutex', 'calls_', 'called_by_', 'helper_of_', 'StrideTbl', 'use_', 'eh_')
OWN = ('solo7', 'chain7', 'ns7', 'callee7', 'caller7', 'adj7')
JUNK_SRC = ('eh_unwind', 'eh_action', 'eh_catch', 'strref', 'stridetbl6', 'vdump', 'nullsub6', 'import', 'algo6', 'copyloop6')


def cls_of(n):
    if '::' not in n or '<' in n or '(' in n or n.startswith(('shared_stub', 'j_', 'w_', 'w2_', 'fn@', 'eh_', 'std::', 'operator', 'calls_')):
        return None
    c = n.rsplit('::', 1)[0]
    return c if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_:]*', c) else None


def stem(n):
    """strip decoration so a guessed name is carried forward as plain as possible"""
    n = n.rstrip('?')
    for p in ('helper_of_', 'called_by_', 'calls_', 'j_', 'w_', 'w2_', 'ref_', 'str_', 'strref_', 'tramp_'):
        while n.startswith(p): n = n[len(p):]
    n = re.sub(r'(::|_)(helper|util|adj|sub|h|c|fn)_[0-9a-f]{6,8}$', '', n)
    n = re.sub(r'_[0-9a-f]{8}$', '', n)
    n = re.sub(r'[^A-Za-z0-9_:@.\[\]]+', '_', n)
    return n[:70]


def run(c):
    names, put, funcs = c['names'], c['put'], c['funcs']
    callees, callers = c['callees'], c['callers']
    st = collections.Counter()

    def mk(f, nm, src):
        return put(f, nm, src, 'low', rank=0.9)

    # ---- solo7 / chain7
    for rnd, src in ((0, 'solo7'), (1, 'chain7'), (2, 'chain7'), (3, 'chain7')):
        new = []
        for f in funcs:
            if f in names: continue
            rs = callers.get(f, ())
            if len(rs) != 1: continue
            r = next(iter(rs))
            if r == f or r not in names or names[r][1] in JUNK_SRC: continue
            rn = names[r][0]
            if names[r][1] in OWN and src == 'solo7': continue
            if rn.startswith(GENERIC) and not rn.startswith(('get_', 'set_')): continue
            k = cls_of(stem(rn)) if '::' in stem(rn) else None
            if names[r][1] == 'chain7':
                nm = 'helper_of_' + stem(rn) + '_%08x' % f
            elif k and not rn.startswith(('callerns',)):
                nm = '%s::helper_%08x' % (k, f)
            else:
                s = stem(rn)
                if not s: continue
                nm = 'helper_of_%s_%08x' % (s, f)
            new.append((f, nm, src))
        for f, nm, s in new:
            if mk(f, nm, s): st[s] += 1
        if not new: break

    # ---- ns7
    for f in funcs:
        if f in names: continue
        rn = [names[r][0] for r in callers.get(f, ()) if r in names and r != f and names[r][1] not in JUNK_SRC and not names[r][0].startswith(GENERIC)]
        if len(rn) < 2: continue
        cl = collections.Counter(cls_of(stem(n)) or '' for n in rn)
        cl.pop('', None)
        top = None
        if cl:
            k, v = cl.most_common(1)[0]
            if v * 10 >= len(rn) * 6: top = k
        if top is None:
            pre = collections.Counter(stem(n).split('::')[0].split('_')[0] for n in rn)
            pre.pop('', None)
            if pre:
                k, v = pre.most_common(1)[0]
                if v * 10 >= len(rn) * 6 and len(k) >= 4 and not k.startswith(('sub', 'fn@', 'Framework')): top = k
        if top and '::' not in top and not top.startswith('JavaNativeInterface'): top = None
        if top and mk(f, '%s::util_%08x' % (top, f), 'ns7'): st['ns7'] += 1

    # ---- callee7
    def good(n):
        return not n.startswith(GENERIC) and '?' not in n
    for f in funcs:
        if f in names: continue
        best = None
        for t in sorted(callees.get(f, ())):
            v = names.get(t)
            if not v or v[1] in OWN or v[1] in JUNK_SRC or not good(v[0]): continue
            if best is None or v[3] > best[3]: best = v
        if best and mk(f, 'calls_%s_%08x' % (stem(best[0]), f), 'callee7'): st['callee7'] += 1

    # ---- caller7
    for f in funcs:
        if f in names: continue
        rn = [names[r][0] for r in sorted(callers.get(f, ())) if r in names and r != f and names[r][1] not in OWN and names[r][1] not in JUNK_SRC and not names[r][0].startswith(GENERIC)]
        if not rn: continue
        s = collections.Counter(stem(n) for n in rn)
        s.pop('', None)
        if not s: continue
        k, v = s.most_common(1)[0]
        if len(rn) == 1 or v * 2 >= len(rn):
            if mk(f, 'called_by_%s_%08x' % (k, f), 'caller7'): st['caller7'] += 1

    # ---- adj7
    order = funcs
    idx = {f: i for i, f in enumerate(order)}
    STR = ('rtti_vf', 'rtti', 'string', 'manual', 'lib', 'file', 'jni_native')
    def tag(a):
        v = names.get(a)
        if not v or v[1] not in STR: return None
        return cls_of(v[0]) or (v[0][3:].split('.')[0] if v[0].startswith('fn@') else None)
    for f in funcs:
        if f in names: continue
        i = idx[f]
        l = r = None
        for k in range(1, 4):
            if i - k >= 0 and l is None: l = tag(order[i - k])
            if i + k < len(order) and r is None: r = tag(order[i + k])
        if l and l == r and mk(f, '%s::adj_%08x' % (l, f), 'adj7'): st['adj7'] += 1
    print('names_seven:', dict(st), 'total', sum(st.values()))
