"""Fifth naming pass (names5, 2026-11-22; runs last in build_names.py, only names functions that have no name yet).
  eh_unwind   SEH/C++ unwind funclets: no direct callers, no 'push ebp' prologue, <= 12 insns, first reads [ebp-N]
              (or the static-init guard clear: mov eax,[G]; and eax,-N; mov [G],eax; ret). One callee/jmp target ->
              eh_unwind_<target>; else eh_unwind_<addr>.                                        [low]
  wrap3       unnamed single-callee wrapper (<= 24 insns, no imports) of a function named by a strong source
              (manual rtti rtti_vf string lib import). Name w_<target>.                         [low]
  vdump       function whose every string is a '(x%08x)' debug-dump format: Vfx_DebugDump_<keys> [low]
  strref      function with a distinctive string and nothing else naming it: strref_<string>    [low]
"""
import re, collections

STRONG = ('manual', 'rtti', 'rtti_vf', 'string', 'lib', 'import')


def san(s, n=32):
    s = re.sub(r'[^A-Za-z0-9]+', '_', s).strip('_')
    return s[:n] if re.search(r'[A-Za-z]{3}', s) else None


def core(n):
    while n.startswith(('j_', 'w_')): n = n[2:]
    return n


def run(c):
    names, put, funcs, F = c['names'], c['put'], c['funcs'], c['F']
    callees, callers, nins, fimps, fstrs = c['callees'], c['callers'], c['nins'], c['fimps'], c['fstrs']
    st = collections.Counter()
    # ---- vdump (before strref so it wins)
    for f in funcs:
        if f in names: continue
        ss = [s for _, s in fstrs.get(f, [])]
        if len(ss) >= 2 and all('(x%08x)' in s or 'x%08x)' in s for s in ss):
            keys = []
            for s in ss:
                for k in re.findall(r'([A-Za-z0-9_\[\]%-]+)\((?:x%08x)\)', s):
                    k = re.sub(r'[^A-Za-z0-9]+', '', k.replace('%s', ''))
                    if k and k not in keys: keys.append(k)
            if keys and put(f, 'Vfx_DebugDump_' + '_'.join(keys[:5]), 'vdump', 'low', rank=1.25): st['vdump'] += 1
    # ---- eh_unwind funclets
    def is_funclet(f):
        ins = F.get(f, {}).get('ins')
        if not ins or len(ins) > 12 or callers.get(f): return False
        if ins[0][1] == 'push' and 'ebp' in ins[0][2]: return False
        head = ' '.join(t for _, _, t in ins[:3])
        return '[ebp - 0x' in head or 'lea' == ins[0][1] and 'ebp - ' in ins[0][2]
    def is_guard_clear(f):
        ins = F.get(f, {}).get('ins')
        if not ins or len(ins) != 4 or callers.get(f): return False
        return (ins[0][1] == 'mov' and ins[1][1] == 'and' and 'eax, -' in ins[1][2] and ins[2][1] == 'mov'
                and ins[3][1] == 'ret' and ins[0][2].endswith(']'))
    fl = [f for f in funcs if f not in names and f >= 0xd60000]
    pend = [f for f in fl if is_funclet(f)]
    for f in fl:
        if is_guard_clear(f):
            if put(f, 'eh_unwind_clear_static_init_flag_%08x' % f, 'eh_unwind', 'low', rank=1.12): st['eh_guard'] += 1
    for _ in range(3):  # targets named in an earlier round give better names
        for f in pend:
            if f in names and names[f][1] == 'eh_unwind' and not names[f][0].startswith('eh_unwind_0'): continue
            cs = callees.get(f, ())
            if len(cs) == 1:
                t = next(iter(cs))
                if t in names and not names[t][0].startswith('eh_unwind') and '?' not in names[t][0] and not core(names[t][0]).startswith('fn@'):
                    nm = 'eh_unwind_' + core(names[t][0])
                else: nm = 'eh_unwind_to_%08x' % t
            elif not cs: nm = 'eh_unwind_%08x' % f
            else: nm = 'eh_unwind_multi_%08x' % f
            old = names.get(f)
            if old and old[1] == 'eh_unwind' and old[0] == nm: continue
            if old and old[1] == 'eh_unwind': del names[f]
            if put(f, nm, 'eh_unwind', 'low', rank=1.12) and not old: st['eh_unwind'] += 1
    # ---- wrap3
    for _ in range(2):
        for f in funcs:
            if f in names or f >= 0xd60000 and is_funclet(f): continue
            cs = callees.get(f, ())
            if len(cs) == 1 and nins.get(f, 99) <= 24 and not fimps.get(f):
                t = next(iter(cs))
                if t in names and names[t][1] in STRONG:
                    if put(f, 'w_' + core(names[t][0]), 'wrap3', 'low', rank=1.75): st['wrap3'] += 1
    # ---- strref
    BAD = re.compile(r'^(invalid string position|string too long|vector<T> too long|%\w+|\W*)$')
    for f in funcs:
        if f in names: continue
        ss = [s for _, s in fstrs.get(f, []) if len(s) >= 5 and not BAD.match(s)]
        tags = [t for t in (san(s) for s in ss) if t]
        if tags:
            if put(f, 'strref_' + tags[0], 'strref', 'low', rank=1.0): st['strref'] += 1
    print('names_five:', dict(st))
