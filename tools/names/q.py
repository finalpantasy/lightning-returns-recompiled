"""Query the facts build_names.py saved (build/names_facts.pickle).
  python -I q.py s REGEX      functions referencing a string matching REGEX
  python -I q.py i NAME       functions calling an import (exact label, e.g. CreateFileA)
  python -I q.py f ADDR       one function: name, callers, callees, imports, strings
  python -I q.py c CLASSRX    vtables of classes matching CLASSRX
"""
import pickle, re, sys
d = pickle.load(open(__import__('os').environ.get('LR_ROOT', '.') + '/build/names_facts.pickle', 'rb'))
N, CE, CR, IM, ST = d['names'], d['callees'], d['callers'], d['fimps'], d['fstrs']


def nm(a):
    return N[a][0] if a in N else ''

mode, arg = sys.argv[1], sys.argv[2]
if mode == 's':
    rx = re.compile(arg)
    for f in sorted(ST):
        hits = [s for _, s in ST[f] if rx.search(s)]
        if hits: print('%08x %-40s %s' % (f, nm(f)[:40], ' | '.join(h[:70] for h in hits[:3])))
elif mode == 'i':
    for f in sorted(IM):
        if arg in IM[f]: print('%08x %-50s callers %d' % (f, nm(f)[:50], len(CR.get(f, ()))))
elif mode == 'f':
    f = int(arg, 16)
    print('%08x %s  nins %d' % (f, nm(f), d['nins'].get(f, 0)))
    print(' callers:', ' '.join('%x:%s' % (c, nm(c)[:30]) for c in sorted(CR.get(f, ()))[:20]))
    print(' callees:', ' '.join('%x:%s' % (c, nm(c)[:30]) for c in sorted(CE.get(f, ()))[:30]))
    print(' imports:', sorted(set(IM.get(f, []))))
    print(' strings:', [s[:80] for _, s in ST.get(f, [])][:12])
elif mode == 'c':
    rx = re.compile(arg)
    for vt, (cls, off, nb, fns) in sorted(d['vtables'].items()):
        if rx.search(cls): print('%08x %s +%x nvf %d first %s' % (vt, cls, off, len(fns), ' '.join('%x' % x for x in fns[:6])))
