"""Fourth naming pass (runs after names_more.run). Names unnamed functions that are referenced from data as 4-byte pointers.
Scans .rdata/.data for runs of consecutive function-start pointers (tables) and for lone pointers beside a string pointer.
Only ever names functions that have no name yet, so manual/rtti/string names are never overwritten. All low confidence.
  dataref_str   pointer (run length <= 2) with a string pointer at -4/+4/-8: name from that string   cb_<string>
  dataref_cls   table where >= 2 named entries (rtti/manual/string) agree on one class              Cls::tbl<base>::slot_<i>
  dataref_load  table base is loaded by one named function                                         <loader>_tbl::slot_<i>
  dataref_strt  table has a string-pointer table directly before/after it                          Tbl_<firststr>::slot_<i>
  dataref_tbl   any other run of >= 3 function pointers                                             DataTbl_<base>::slot_<i>
"""
import re, sys, collections

STRONG = ('manual', 'string', 'rtti', 'rtti_vf', 'import', 'lib')


def san(s, n=36):
    s = re.sub(r'[^A-Za-z0-9]+', '_', s).strip('_')
    return s[:n] if re.search(r'[A-Za-z]{3}', s) else None


def cls_of(n):
    if '::' not in n or '<' in n or '(' in n or n.startswith(('shared_stub', 'fn@', 'std::')): return None
    c = n.rsplit('::', 1)[0]
    return c if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_:]*', c) else None


def run(c):
    names, funcs, dataref, put = c['names'], c['funcs'], c['dataref'], c['put']
    G = c['G']; img, BASE = G['img'], G['BASE']
    c['RANK'].update({'dataref_str': 1.18, 'dataref_cls': 1.17, 'dataref_load': 1.16, 'dataref_strt': 1.15, 'dataref_tbl': 1.1})
    fset = set(funcs)
    import struct
    def u32(a):
        o = a - BASE
        return struct.unpack_from('<I', img, o)[0] if 0 <= o <= len(img) - 4 else 0
    # section ranges: use the string_at-capable data sections
    secs = [s for s in c['SECS'] if s[0] in ('.rdata', '.data')]
    PRINT = set(range(0x20, 0x7f))
    def stri(a):
        if not any(s[1] <= a < s[2] for s in secs): return None
        o = a - BASE
        e = img.find(b'\0', o, o + 120)
        if e - o < 4 or any(ch not in PRINT for ch in img[o:e]): return None
        return img[o:e].decode('latin1')
    loaders = collections.defaultdict(set)
    for f, ds in dataref.items():
        for d in ds: loaders[d].add(f)
    # find runs
    runs = []
    for s in secs:
        a = s[1]
        while a < s[2] - 4:
            if u32(a) in fset:
                b = a
                while b < s[2] - 4 and u32(b) in fset: b += 4
                runs.append((a, (b - a) // 4)); a = b
            else: a += 4
    stats = collections.Counter()
    for base, n in runs:
        ents = [u32(base + 4 * i) for i in range(n)]
        un = [i for i, f in enumerate(ents) if f not in names]
        if not un: continue
        # string table adjacent?
        before = []; p = base - 4
        while stri(u32(p)) is not None and len(before) < 400: before.append(u32(p)); p -= 4
        after = []; p = base + 4 * n
        while stri(u32(p)) is not None and len(after) < 400: after.append(u32(p)); p += 4
        strtab = None
        if len(before) >= 2: strtab = before[::-1]
        elif len(after) >= 2: strtab = after
        if n <= 2:
            for i in un:
                p = base + 4 * i; nm = None
                for off in (-4, 4, -8, 8):
                    sv = stri(u32(p + off)) if off not in (4,) or i == n - 1 else None
                    if sv and san(sv): nm = san(sv); break
                if nm and put(ents[i], 'cb_' + nm, 'dataref_str', 'low'): stats['dataref_str'] += 1
            if n < 3 and not strtab: continue
        votes = collections.Counter(cls_of(names[f][0]) for f in ents if f in names and names[f][1] in STRONG)
        votes.pop(None, None)
        top = votes.most_common(1)
        if top and top[0][1] >= 2 and top[0][1] * 2 >= sum(1 for f in ents if f in names):
            tn, src = '%s::tbl%x' % (top[0][0], base), 'dataref_cls'
        else:
            ld = [names[f][0] for f in loaders.get(base, ()) if f in names and not names[f][0].startswith(('fn@', 'shared_stub'))]
            ld = [x for x in ld if '?' not in x and '(' not in x]
            if len(ld) == 1 or (ld and len(set(ld)) == 1):
                tn, src = ld[0] + '_tbl', 'dataref_load'
            elif strtab and san(stri(strtab[0]) or ''):
                tn, src = 'Tbl_' + san(stri(strtab[0])), 'dataref_strt'
            elif n >= 3:
                tn, src = 'DataTbl_%x' % base, 'dataref_tbl'
            else: continue
        for i in un:
            nm = '%s::slot_%d' % (tn, i)
            if strtab and len(strtab) == n and san(stri(strtab[i]) or ''): nm += '_' + san(stri(strtab[i]), 28)
            if put(ents[i], nm, src, 'low'): stats[src] += 1
    print('names_dataref: runs %d' % len(runs), dict(stats), file=sys.stderr)
