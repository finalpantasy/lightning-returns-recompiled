"""Extra naming sources run by build_names.py after the base passes (static analysis only).
Each source labels its names (source, confidence) so weak ones can be filtered:
  override  derived-class vtable slot overriding a base slot that has a real (string/manual) method name  [med]
  strtag    unnamed function whose strings give a log tag "[TAG]" / dotted id / lone identifier            [low/med]
  wrap2     function (<=24 ins) with exactly one callee that is named by a strong source; name w2_<callee>  [low]
  callercls unnamed function whose callers are all methods of one class (private helper)                  [low]
  impuse    unnamed small function whose only external effect is one import: use_<import>                  [low]
"""
import re, collections

STRONG = ('manual', 'import', 'rtti', 'rtti_vf', 'string', 'lib', 'override')
RX_VFN = re.compile(r'::vf\d+(@[0-9a-f]+)?$')


def san(s, n=40):
    return re.sub(r'[^A-Za-z0-9_.\[\]]+', '_', s).strip('_')[:n]


def run(c):
    names, put, funcs, nins = c['names'], c['put'], c['funcs'], c['nins']
    callees, callers, fimps, fstrs, dataref = c['callees'], c['callers'], c['fimps'], c['fstrs'], c['dataref']
    vtables = c['vtables']
    g = c['G']
    img, BASE, u32, tds = g['img'], g['BASE'], g['u32'], c['tds']
    stats = collections.Counter()

    def mark(src, ok):
        if ok: stats[src] += 1

    def is_real(a):
        v = names.get(a)
        return v and v[1] in ('string', 'manual', 'lib') and '::' in v[0]

    # ---- override: via RTTI class hierarchy descriptor
    by_cls = {}
    for vt, (cls, off, nb, fns) in vtables.items():
        if off == 0: by_cls[cls] = (vt, fns)
    for vt, (cls, off, nb, fns) in vtables.items():
        if off != 0: continue
        col = u32(vt - 4)
        chd = u32(col + 16)
        n = u32(chd + 8); arr = u32(chd + 12)
        if n < 2 or n > 64: continue
        # walk bases; the first base (index 1) is the primary base (same vtable offset 0)
        td = u32(u32(arr + 4))
        base = tds.get(td)
        if base not in by_cls: continue
        bfns = by_cls[base][1]
        for i, f in enumerate(fns):
            if i >= len(bfns) or bfns[i] == f: continue
            if is_real(bfns[i]) and f in names and names[f][1] == 'rtti_vf' and RX_VFN.search(names[f][0]):
                meth = names[bfns[i]][0].rsplit('::', 1)[1]
                mark('override', put(f, '%s::%s' % (cls, meth), 'override', 'med'))

    # ---- strtag
    for f in funcs:
        if f in names or not fstrs.get(f): continue
        ss = [s for _, s in fstrs[f]]
        tags = collections.Counter()
        for s in ss:
            m = re.match(r'((?:\[[A-Za-z0-9_ ]{2,16}\]){1,3})', s)
            if m: tags[m.group(1)] += 1
        cand = None
        if tags:
            t = tags.most_common(1)[0][0]
            cand = ('log' + san(t.replace('][', '_')), 'med' if len(tags) == 1 else 'low')
        else:
            ids = [s for s in ss if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.]{3,40}', s)]
            if ids and len(set(ids)) == 1:
                cand = ('ref_' + ids[0], 'low')
            elif len(set(ss)) == 1 and len(ss[0]) >= 8:
                cand = ('str_' + san(ss[0], 28), 'low')
        if cand: mark('strtag', put(f, cand[0], 'strtag', cand[1]))

    # ---- wrap2: tiny function with exactly one callee which has a strong name
    for _ in range(2):
        for f in funcs:
            if f in names and names[f][3] >= 1.6: continue
            cs = callees.get(f, ())
            if len(cs) == 1 and nins.get(f, 99) <= 24 and not fimps.get(f):
                cc = next(iter(cs))
                if cc in names and names[cc][1] in STRONG and not names[cc][0].startswith(('shared_stub', 'operator', 'MemoryHeap', 'free')):
                    mark('wrap2', put(f, 'w2_' + names[cc][0], 'wrap2', 'low'))

    # ---- callercls / globcls: need a class per method
    def cls_of(a):
        v = names.get(a)
        if v and v[1] in ('rtti_vf', 'string', 'manual', 'rtti', 'override') and '::' in v[0] and not v[0].startswith('shared_stub'):
            return v[0].rsplit('::', 1)[0].lstrip('j_').lstrip('w_') if False else v[0].rsplit('::', 1)[0]
        return None
    for f in funcs:
        if f in names: continue
        if 'CxxFrameHandler3' in ' '.join(fimps.get(f, ())) and nins.get(f, 99) <= 12:
            mark('impuse', put(f, 'eh_handler_stub', 'impuse', 'med')); continue
        cl = {cls_of(x) for x in callers.get(f, ())}
        if len(callers.get(f, ())) >= 1 and len(cl) == 1 and None not in cl:
            mark('callercls', put(f, '%s::helper?' % next(iter(cl)), 'callercls', 'low'))
    # ---- impuse
    for f in funcs:
        if f in names: continue
        im = set(fimps.get(f, ()))
        if len(im) == 1 and nins.get(f, 99) <= 40 and not callees.get(f):
            mark('impuse', put(f, 'use_' + next(iter(im)), 'impuse', 'low'))
    print('extra sources:', dict(stats), file=__import__('sys').stderr)
