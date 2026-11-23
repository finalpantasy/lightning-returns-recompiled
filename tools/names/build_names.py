"""Build analysis/names.tsv: a name for as many guest functions of LRFF13.exe as can be found statically.

Run (from anywhere): LR_ROOT=<work dir> python -I tools/names/build_names.py
Inputs : ffxiiiimg.exe.unpacked.exe (PE), analysis/xrc_coverage.tsv (function starts),
         build/xrc_gen/src/page_*.c (the translator's per-instruction disassembly comments),
         tools/names/manual.tsv (hand-curated names from runtime source and notes).
Outputs: analysis/names.tsv (address, name, source, confidence), analysis/names_xref.tsv (per function: callers,
         callees, imports, strings, vtables: the raw facts the names came from), build/win/names.tsv (copy the
         runtime loads by default).
Sources, strongest first: manual, import (thunks of imports), rtti (vtables, ctors, deleting dtors), string (a
"Class::method" log string inside the function), lib (zlib/Miles/CRT strings), file (__FILE__ .cpp of an assert),
range (between two functions of the same .cpp object or class), thunk (jmp/tiny wrapper of a named function).
"""
import os, re, struct, sys, glob, collections, pickle, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msvc_demangle import demangle_type, demangle_fn

ROOT = os.environ.get('LR_ROOT', '.')  # work dir: holds ffxiiiimg.exe.unpacked.exe, analysis/, build/xrc_gen/
EXE = ROOT + '/ffxiiiimg.exe.unpacked.exe'
COV = ROOT + '/analysis/xrc_coverage.tsv'
PAGES = ROOT + '/build/xrc_gen/src/page_*.c'
MANUAL = ROOT + '/tools/names/manual.tsv'
OUT = ROOT + '/analysis/names.tsv'
XREF = ROOT + '/analysis/names_xref.tsv'
CACHE = os.environ.get('NAMES_CACHE', ROOT + '/build/names_pages.pickle')

# ------------------------------------------------------------------ PE
data = open(EXE, 'rb').read()
e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, e_lfanew + 6)[0]
optsz = struct.unpack_from('<H', data, e_lfanew + 20)[0]
opt = e_lfanew + 24
BASE = struct.unpack_from('<I', data, opt + 28)[0]
SIZE = struct.unpack_from('<I', data, opt + 56)[0]
img = bytearray(SIZE)
img[0:0x1000] = data[0:0x1000]
sections = []
for i in range(nsec):
    o = opt + optsz + 40 * i
    name = data[o:o + 8].rstrip(b'\0').decode('latin1')
    vsize, va, rawsz, rawptr = struct.unpack_from('<IIII', data, o + 8)
    n = min(rawsz, vsize if vsize else rawsz)
    img[va:va + n] = data[rawptr:rawptr + n]
    sections.append((name, BASE + va, BASE + va + max(vsize, rawsz)))
SEC = {s[0]: s for s in sections}
TEXT = SEC['.text']


def u32(a):
    return struct.unpack_from('<I', img, a - BASE)[0]


def in_sec(a, s):
    return s[1] <= a < s[2]


def cstr(a, maxn=512):
    o = a - BASE
    if o < 0 or o >= SIZE: return None
    e = img.find(b'\0', o, o + maxn)
    if e < 0: return None
    return img[o:e].decode('latin1')

# imports: IAT slot -> dll!name
imports = {}
imp_rva = struct.unpack_from('<I', data, opt + 96 + 8)[0]
o = imp_rva
while True:
    oft, ts, fc, nm, ft = struct.unpack_from('<IIIII', img, o)
    if not nm: break
    dll = cstr(BASE + nm)
    t = oft or ft
    k = 0
    while True:
        v = struct.unpack_from('<I', img, t + 4 * k)[0]
        if not v: break
        if v & 0x80000000: fn = 'ord%d' % (v & 0xffff)
        else: fn = cstr(BASE + v + 2) or '?'
        imports[BASE + ft + 4 * k] = (dll, fn)
        k += 1
    o += 20

CPP_DEMANGLE = {
    '??2@YAPAXI@Z': 'operator new', '??3@YAXPAX@Z': 'operator delete', '??_U@YAPAXI@Z': 'operator new[]',
    '??_V@YAXPAX@Z': 'operator delete[]', '??0exception@std@@QAE@ABQBD@Z': 'std::exception::exception',
    '??1type_info@@UAE@XZ': 'type_info::~type_info', '?terminate@@YAXXZ': 'terminate',
}


XINPUT_ORD = {2: 'XInputGetState', 3: 'XInputSetState', 4: 'XInputGetCapabilities', 5: 'XInputEnable',
              7: 'XInputGetDSoundAudioDeviceGuids', 8: 'XInputGetBatteryInformation', 9: 'XInputGetKeystroke'}


def imp_label(dll, fn):
    fn = CPP_DEMANGLE.get(fn, fn)
    if fn.startswith('ord') and dll.lower().startswith('xinput'):
        fn = XINPUT_ORD.get(int(fn[3:]), dll + '!' + fn)
    elif fn.startswith('ord'):
        fn = dll.split('.')[0] + '!' + fn
    elif fn.startswith('?'):
        fn = demangle_fn(fn)
    return fn

# ------------------------------------------------------------------ functions
funcs = []
for line in open(COV):
    p = line.split('\t')
    if len(p) > 1 and re.fullmatch(r'[0-9a-f]{8}', p[0]): funcs.append(int(p[0], 16))
funcs = sorted(set(funcs))
fset = set(funcs)
import bisect


def fn_of(a):
    i = bisect.bisect_right(funcs, a) - 1
    return funcs[i] if i >= 0 else None

# ------------------------------------------------------------------ translated pages: per-function facts
RX_HDR = re.compile(r'^/\* ---- ([0-9a-f]{8}) ---- \*/')
RX_INS = re.compile(r'^\s*/\* ([0-9a-f]{8}) (\S+) (.*) \*/\s*$')
RX_HEX = re.compile(r'0x([0-9a-f]{5,8})\b')


def parse_pages():
    F = {}
    for path in sorted(glob.glob(PAGES)):
        cur = None
        with open(path, 'r', encoding='latin1') as fh:
            for line in fh:
                if line.startswith('/* ----'):
                    m = RX_HDR.match(line)
                    if m:
                        cur = {'ins': [], 'n': 0}
                        F[int(m.group(1), 16)] = cur
                    continue
                if cur is None or '/* 00' not in line and '/* 01' not in line: continue
                m = RX_INS.match(line)
                if not m: continue
                cur['ins'].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return F

if os.path.exists(CACHE) and os.path.getmtime(CACHE) > max(os.path.getmtime(p) for p in glob.glob(PAGES)):
    F = pickle.load(open(CACHE, 'rb'))
else:
    F = parse_pages()
    pickle.dump(F, open(CACHE, 'wb'), protocol=4)
print('functions in pages:', len(F), 'coverage:', len(funcs), file=sys.stderr)

# strings: NUL-terminated printable ASCII runs in .rdata / .data
RD, DA = SEC['.rdata'], SEC['.data']
PRINT = set(range(0x20, 0x7f)) | {9, 10, 13}


def string_at(a):
    if not (in_sec(a, RD) or in_sec(a, DA)): return None
    o = a - BASE
    if o > 0 and img[o - 1] in PRINT and img[o - 1] != 0: pass  # mid-string refs are fine (suffix)
    e = img.find(b'\0', o, o + 400)
    if e < 0 or e - o < 3: return None
    s = img[o:e]
    if any(c not in PRINT for c in s): return None
    return s.decode('latin1')

callees = collections.defaultdict(set)
callers = collections.defaultdict(set)
fimps = collections.defaultdict(list)
fstrs = collections.defaultdict(list)
vtstores = collections.defaultdict(list)  # fn -> [(insn index, vtable)]
dataref = collections.defaultdict(set)
nins = {}
tailjmp = {}

for f, d in F.items():
    ins = d['ins']
    nins[f] = len(ins)
    for idx, (a, mn, txt) in enumerate(ins):
        if mn in ('call', 'jmp'):
            mi = re.search(r'\[0x([0-9a-f]+)\]$', txt)
            if mi and int(mi.group(1), 16) in imports:
                dll, nm = imports[int(mi.group(1), 16)]
                fimps[f].append(imp_label(dll, nm))
                if mn == 'jmp' and len(ins) <= 2: tailjmp[f] = ('imp', imp_label(dll, nm))
                continue
            if re.search(r' [+-]0x[0-9a-f]+$', txt):
                b = img[a - BASE]
                if b == 0xe8: t = (a + 5 + struct.unpack_from('<i', img, a - BASE + 1)[0]) & 0xffffffff
                elif b == 0xe9: t = (a + 5 + struct.unpack_from('<i', img, a - BASE + 1)[0]) & 0xffffffff
                elif b == 0xeb: t = (a + 2 + struct.unpack_from('<b', img, a - BASE + 1)[0]) & 0xffffffff
                else: t = None
                if t is not None and t in fset and t != f:
                    if mn == 'call' or not (f <= t < f + 0x10000 and fn_of(t) == f):
                        callees[f].add(t); callers[t].add(f)
                    if mn == 'jmp' and idx == 0: tailjmp[f] = ('fn', t)
            continue
        for h in RX_HEX.findall(txt):
            v = int(h, 16)
            if v < 0x400000: continue
            s = string_at(v)
            if s is not None:
                fstrs[f].append((v, s))
            elif in_sec(v, RD) or in_sec(v, DA):
                dataref[f].add(v)
                if mn == 'mov' and txt.split(' ', 2)[1].startswith('['): vtstores[f].append((idx, v))
            if v in fset and mn in ('push', 'mov'):
                callers[v].add(f)  # address taken (callback)

# ------------------------------------------------------------------ RTTI
RX_TD = re.compile(rb'\.\?A[VU][^\0]{1,400}?@@\0')



tds = {}
for sec in (RD, DA):
    lo, hi = sec[1] - BASE, sec[2] - BASE
    for m in RX_TD.finditer(img, lo, hi):
        tds[BASE + m.start() - 8] = demangle_type(m.group(0)[:-1].decode('latin1'))
print('rtti type descriptors:', len(tds), file=sys.stderr)

cols = {}
lo, hi = RD[1] - BASE, RD[2] - BASE
for o in range(lo, hi - 20, 4):
    td = struct.unpack_from('<I', img, o + 12)[0]
    if td in tds:
        sig, off, cdoff = struct.unpack_from('<III', img, o)
        chd = struct.unpack_from('<I', img, o + 16)[0]
        if sig == 0 and off < 0x10000 and (in_sec(chd, RD)):
            nb = struct.unpack_from('<I', img, chd - BASE + 8)[0]
            cols[BASE + o] = (tds[td], off, nb)
print('rtti complete object locators:', len(cols), file=sys.stderr)

vtables = {}  # vtable addr -> (class, offset, nbases, [fns])
for o in range(lo, hi - 4, 4):
    v = struct.unpack_from('<I', img, o)[0]
    if v in cols:
        vt = BASE + o + 4
        fns = []
        a = vt
        while a < RD[2]:
            x = u32(a)
            if x not in fset: break
            if fns and a in cols: break
            fns.append(x)
            a += 4
            if a - BASE + 4 <= hi and u32(a) in cols: break  # next vtable's COL
        if fns: vtables[vt] = cols[v] + (fns,)
print('vtables:', len(vtables), file=sys.stderr)

# ------------------------------------------------------------------ naming
names = {}  # addr -> (name, source, conf, rank)
RANK = {'manual': 9, 'import': 8, 'rtti': 6, 'string': 7, 'lib': 7, 'file': 3, 'range': 2, 'thunk': 4, 'rtti_vf': 5,
        'override': 3.6, 'strtag': 2.8, 'wrap2': 1.6, 'callercls': 1.5, 'globcls': 1.4, 'impuse': 1.3}


def put(a, name, src, conf, rank=None):
    r = RANK[src] if rank is None else rank
    old = names.get(a)
    if old and old[3] >= r: return False
    names[a] = (name, src, conf, r)
    return True

# manual
if os.path.exists(MANUAL):
    for line in open(MANUAL, encoding='utf-8'):
        if line.startswith('#') or not line.strip(): continue
        p = line.rstrip('\n').split('\t')
        put(int(p[0], 16), p[1], 'manual', p[2] if len(p) > 2 else 'high')

# import thunks (jmp [IAT])
for f, (k, t) in list(tailjmp.items()):
    if k == 'imp': put(f, t, 'import', 'high')

# rtti: virtual functions; a function shared by several vtables goes to the class with the fewest bases
vfuse = collections.defaultdict(list)
for vt, (cls, off, nb, fns) in vtables.items():
    for i, f in enumerate(fns): vfuse[f].append((nb, off, cls, i))
for f, uses in vfuse.items():
    uses.sort()
    nb, off, cls, i = uses[0]
    classes = set(u[2] for u in uses)
    if len(classes) > 6 and nins.get(f, 99) <= 4:
        put(f, 'shared_stub_vf%d' % i, 'rtti_vf', 'low')
    else:
        conf = 'med' if len(classes) <= 3 else 'low'
        put(f, '%s::vf%d' % (cls, i) + ('' if off == 0 else '@%x' % off), 'rtti_vf', conf)

vt_class = {vt: v[0] for vt, v in vtables.items()}
DELETE = {'operator delete', 'operator delete[]', 'free', 'operator delete_heap', 'MemoryHeap::Free'}
del_fns = {a for a, v in names.items() if v[0] in DELETE}


def calls_delete(f):
    return any(x in DELETE for x in fimps.get(f, [])) or any(c in del_fns for c in callees.get(f, ()))

for f, st in vtstores.items():
    vts = [v for _, v in st if v in vt_class]
    if not vts: continue
    if f in vfuse and calls_delete(f):
        put(f, '%s::`scalar deleting dtor\'' % vt_class[vts[0]], 'rtti', 'med')
    elif f in vfuse:
        put(f, '%s::dtor' % vt_class[vts[0]], 'rtti', 'low')
    else:
        put(f, '%s::ctor_dtor' % vt_class[vts[-1]], 'rtti', 'low')
# deleting dtors that call the dtor (not inlined): slot 0, small, calls operator delete
for f, uses in vfuse.items():
    nb, off, cls, i = sorted(uses)[0]
    if i == 0 and nins.get(f, 99) <= 30 and calls_delete(f):
        put(f, '%s::`scalar deleting dtor\'' % cls, 'rtti', 'med')
        for c in callees.get(f, ()):
            if c not in del_fns and names.get(c, ('', '', '', 0))[0].endswith('ctor_dtor'):
                put(c, '%s::~%s' % (cls, cls.split('::')[-1]), 'rtti', 'med', rank=6.5)

# strings: "Class::method" in a log/assert string inside the function
RX_QN = re.compile(r'(?:^|[\[\( <:])((?:[A-Za-z_][A-Za-z0-9_]*::)+~?[A-Za-z_][A-Za-z0-9_]*)')
RX_CPP = re.compile(r'([A-Za-z0-9_]+)\.(cpp|c|h|inl)$', re.I)
LIBSTR = [
    ('invalid window size', 'zlib::inflate', 'med'), ('incorrect header check', 'zlib::inflate', 'med'),
    ('invalid stored block lengths', 'zlib::inflate_blocks', 'med'),
    ('too many length or distance symbols', 'zlib::inflate_blocks', 'med'),
    ('invalid literal/length code', 'zlib::inflate_codes_or_fast', 'low'),
    ('oversubscribed dynamic bit lengths tree', 'zlib::inflate_trees_bits', 'med'),
    ('incomplete literal/length tree', 'zlib::inflate_trees_dynamic', 'med'),
    ('deflate 1.', 'zlib::deflate', 'low'), ('inflate 1.', 'zlib::inflate', 'low'),
    ('BINKOPEN', 'Bink', 'low'),
]
file_of = {}
for f, ss in fstrs.items():
    qn = collections.Counter()
    for v, s in ss:
        m = RX_CPP.search(s.replace('\\', '/'))
        if m and ('\\' in s or '/' in s): file_of[f] = m.group(1) + '.' + m.group(2)
        for lib, nm, cf in LIBSTR:
            if s.startswith(lib): put(f, nm, 'lib', cf)
        for m in RX_QN.finditer(s):
            q = m.group(1)
            if len(q) < 6 or q.startswith('std::'): continue
            qn[q] += 1
        if re.match(r'AIL_[a-z]\w+', s):
            put(f, 'Miles::' + re.split(r'[^A-Za-z0-9_]', s)[0], 'lib', 'low')
    if qn:
        q, n = qn.most_common(1)[0]
        put(f, q, 'string', 'med' if len(qn) == 1 else 'low')
for f, fl in file_of.items():
    put(f, 'fn@' + fl, 'file', 'med')

# ranges: a run of functions between two of the same .cpp (nothing else in between) belongs to that object file
order = funcs
idx_of = {f: i for i, f in enumerate(order)}
anchors = sorted((idx_of[f], fl) for f, fl in file_of.items() if f in idx_of)
for (i0, f0), (i1, f1) in zip(anchors, anchors[1:]):
    if f0 == f1 and 0 < i1 - i0 <= 400:
        for k in range(i0 + 1, i1):
            put(order[k], 'fn@' + f0 + '?', 'range', 'low')
# classes: between two methods of the same class (rtti vf or string), nothing else named in between
cls_of = {}
for a, (nm, src, cf, r) in names.items():
    if src in ('rtti_vf', 'string', 'manual') and '::' in nm and not nm.startswith('shared_stub'):
        cls_of[a] = nm.rsplit('::', 1)[0]
canch = sorted((idx_of[a], c) for a, c in cls_of.items() if a in idx_of)
for (i0, c0), (i1, c1) in zip(canch, canch[1:]):
    if c0 == c1 and 1 < i1 - i0 <= 40:
        for k in range(i0 + 1, i1):
            put(order[k], c0 + '::fn?', 'range', 'low', rank=2.5)

# thunks: jmp to a named function, or a tiny wrapper whose only call is a named function (a few passes)
for _ in range(3):
    for f in funcs:
        if f in names and names[f][3] >= 4: continue
        t = tailjmp.get(f)
        if t and t[0] == 'fn' and t[1] in names:
            put(f, 'j_' + names[t[1]][0], 'thunk', names[t[1]][2]); continue
        cs = callees.get(f, ())
        if len(cs) == 1 and nins.get(f, 99) <= 8 and not fimps.get(f):
            c = next(iter(cs))
            if c in names and names[c][1] not in ('range', 'file', 'thunk'):
                put(f, 'w_' + names[c][0], 'thunk', 'low')
        elif not cs and nins.get(f, 99) <= 6 and len(set(fimps.get(f, []))) == 1:
            put(f, 'w_' + fimps[f][0], 'thunk', 'med')

# ------------------------------------------------------------------ pass 2 (names_extra.py): extra sources, filtered by source/confidence
import names_extra
names_extra.run(dict(names=names, put=put, funcs=funcs, fset=fset, nins=nins, callees=callees, callers=callers,
                     G=dict(img=img, BASE=BASE, u32=u32), fimps=fimps, fstrs=fstrs, dataref=dataref, vtables=vtables, idx_of=idx_of, order=order, tds=tds))

import names_more
names_more.run(dict(names=names, put=put, funcs=funcs, callees=callees, callers=callers, dataref=dataref, RANK=RANK))

import names_dataref
names_dataref.run(dict(names=names, put=put, funcs=funcs, dataref=dataref, RANK=RANK, G=dict(img=img, BASE=BASE), SECS=sections))

import names_five
names_five.run(dict(names=names, put=put, funcs=funcs, F=F, callees=callees, callers=callers, nins=nins, fimps=fimps, fstrs=fstrs))

import names_six
names_six.run(dict(names=names, put=put, funcs=funcs, F=F, callees=callees, callers=callers, nins=nins, fimps=fimps, fstrs=fstrs,
                   dataref=dataref, vtables=vtables, img=img, BASE=BASE, sections=sections, RANK=RANK, fset=fset, imports=imports))

import names_seven
names_seven.run(dict(names=names, put=put, funcs=funcs, F=F, callees=callees, callers=callers, nins=nins, fimps=fimps, fstrs=fstrs,
                     dataref=dataref, vtables=vtables, img=img, BASE=BASE, sections=sections, RANK=RANK, fset=fset, imports=imports))

import names_eight
names_eight.run(dict(names=names, put=put, funcs=funcs, F=F, callees=callees, callers=callers, nins=nins, fimps=fimps, fstrs=fstrs,
                     dataref=dataref, vtables=vtables, img=img, BASE=BASE, sections=sections, RANK=RANK, fset=fset, imports=imports))

# ------------------------------------------------------------------ write
def clean(n):
    return re.sub(r'[\t\r\n]', ' ', n)

with open(OUT, 'w', encoding='utf-8', newline='\n') as fh:
    fh.write('# address\tname\tsource\tconfidence  (tools/names/build_names.py; sources: manual import rtti rtti_vf '
             'string lib file range thunk override strtag wrap2 callercls globcls impuse dataref_*)\n')
    for a in sorted(names):
        n, s, c, _ = names[a]
        fh.write('%08x\t%s\t%s\t%s\n' % (a, clean(n), s, c))
with open(XREF, 'w', encoding='utf-8', newline='\n') as fh:
    fh.write('# address\tnins\tcallers\tcallees\timports\tstrings (first 4)\n')
    for f in funcs:
        ss = '|'.join(clean(s)[:60] for _, s in fstrs.get(f, [])[:4])
        fh.write('%08x\t%d\t%d\t%s\t%s\t%s\n' % (f, nins.get(f, 0), len(callers.get(f, ())),
                 ','.join('%x' % c for c in sorted(callees.get(f, ()))[:12]),
                 ','.join(sorted(set(fimps.get(f, []))))[:200], ss))
if not os.environ.get('NAMES_NO_COPY'):
    os.makedirs(ROOT + '/build/win', exist_ok=True)
    shutil.copyfile(OUT, ROOT + '/build/win/names.tsv')
by = collections.Counter(v[1] for v in names.values())
print('named %d of %d functions:' % (len(names), len(funcs)), dict(by.most_common()), file=sys.stderr)
if not os.environ.get('NAMES_NO_COPY'): pickle.dump({'callees': dict(callees), 'callers': dict(callers), 'fimps': dict(fimps), 'fstrs': dict(fstrs),
             'vtables': vtables, 'names': names, 'nins': nins, 'file_of': file_of, 'imports': imports},
            open(ROOT + '/build/names_facts.pickle', 'wb'), protocol=4)
