"""Third naming pass (runs after names_extra.run). Propagates names through the call graph and data refs. Never touches
a function that already has any name (so manual names are safe); all results are low confidence guesses by structure.
  callercls2  all callers are named methods of one class (chain-propagates over rounds)       Cls::h_<addr>      low
  leaf1       exactly one caller, which is named: Parent::sub_<addr>                          low
  callerns    callers span several classes but share a namespace of depth >= 2               ns::util_<addr>    low
  calleecls   >= 2 callees, all class-bearing callees in one class                            Cls::c_<addr>      low
  globcls     references a data global used only by methods of one class (>= 2 users)        Cls::g_<addr>      low
"""
import re, collections, sys

CLS_SRC = ('rtti_vf', 'string', 'manual', 'rtti', 'override', 'callercls', 'callercls2', 'leaf1', 'calleecls', 'globcls', 'callerns', 'lib')
SKIP_PFX = ('shared_stub', 'j_', 'w_', 'w2_', 'fn@', 'use_', 'log', 'ref_', 'str_', 'eh_', 'operator', 'std::')
RANK = {'callercls2': 1.2, 'leaf1': 1.0, 'callerns': 1.15, 'calleecls': 1.1, 'globcls': 1.25}


def cls_of_name(n):
    if n.startswith(SKIP_PFX) or '::' not in n or '<' in n or '(' in n: return None
    c = n.rsplit('::', 1)[0]
    return c if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_:]*', c) else None


def leaf_name(pn, addr):
    """Cls::meth_sub_<addr> when the parent is a real class method, Cls::sub_<addr> for synthetic parents."""
    c = cls_of_name(pn)
    if c:
        m = pn.rsplit('::', 1)[1]
        if m.endswith('?') or re.match(r'(h|util|c|g|sub)_[0-9a-f]+', m) or m in ('helper', 'fn'):
            return '%s::sub_%x' % (c, addr)
        m = re.sub(r'_sub_[0-9a-f]+$', '', m)
        return '%s::%s_sub_%x' % (c, m, addr)
    return '%s::sub_%x' % (re.sub(r'(::|_)sub_[0-9a-f]+$', '', pn).rstrip('?'), addr)


def run(c):
    names, funcs, callees, callers, dataref = c['names'], c['funcs'], c['callees'], c['callers'], c['dataref']
    for k, v in RANK.items(): c['RANK'][k] = v
    put = c['put']
    stats = collections.Counter()

    def cls(a):
        v = names.get(a)
        return cls_of_name(v[0]) if v and v[1] in CLS_SRC else None

    ref_users = collections.defaultdict(set)
    for f, ds in dataref.items():
        for d in ds: ref_users[d].add(f)

    for rnd in range(6):
        before = sum(stats.values())
        for f in funcs:
            if f in names: continue
            cs = callers.get(f, ())
            if not cs: continue
            cl = {cls(x) for x in cs}
            if len(cl) == 1 and None not in cl:
                if put(f, '%s::h_%x' % (next(iter(cl)), f), 'callercls2', 'low'): stats['callercls2'] += 1
        for f in funcs:
            if f in names: continue
            cs = list(callers.get(f, ()))
            if len(cs) == 1 and cs[0] in names and cs[0] != f:
                pn = names[cs[0]][0]
                if pn.startswith(('shared_stub', 'eh_handler')) or names[cs[0]][1] in ('strtag', 'wrap2', 'impuse', 'thunk'): continue
                if put(f, leaf_name(pn, f), 'leaf1', 'low'): stats['leaf1'] += 1
        for f in funcs:
            if f in names: continue
            cs = callers.get(f, ())
            if len(cs) < 2: continue
            cl = [cls(x) for x in cs]
            if None in cl: continue
            ns = {x.rsplit('::', 1)[0] if '::' in x else '' for x in cl}
            parts = [x.split('::')[:-0 or None] for x in cl]
            common = parts[0]
            for p in parts[1:]:
                k = 0
                while k < min(len(common), len(p)) and common[k] == p[k]: k += 1
                common = common[:k]
            if len(common) >= 2:
                if put(f, '%s::util_%x' % ('::'.join(common), f), 'callerns', 'low'): stats['callerns'] += 1
        for f in funcs:
            if f in names: continue
            ce = callees.get(f, ())
            if len(ce) < 2: continue
            cl = {cls(x) for x in ce} - {None}
            if len(cl) == 1 and sum(1 for x in ce if cls(x)) >= 2:
                if put(f, '%s::c_%x' % (next(iter(cl)), f), 'calleecls', 'low'): stats['calleecls'] += 1
        for f in funcs:
            if f in names: continue
            ds = dataref.get(f, ())
            votes = collections.Counter()
            for d in ds:
                us = ref_users.get(d, ())
                if not (2 <= len(us) <= 40): continue
                cl = {cls(u) for u in us if u != f}
                if len(cl) == 1 and None not in cl: votes[next(iter(cl))] += 1
            if len(votes) == 1:
                if put(f, '%s::g_%x' % (next(iter(votes)), f), 'globcls', 'low'): stats['globcls'] += 1
        print('names_more round', rnd, sum(stats.values()) - before, file=sys.stderr)
        if sum(stats.values()) == before: break
    print('names_more:', dict(stats), file=sys.stderr)
