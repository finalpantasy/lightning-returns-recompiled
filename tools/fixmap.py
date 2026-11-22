# function_map fix for MSVC switches: drop rows that are switch case targets (and never call targets), and make the
# function holding each switch jmp a row whose size covers its cases (up to its jump table, which MSVC places at the end)
import struct, sys, csv, bisect
exe, fmap, out = sys.argv[1], sys.argv[2], sys.argv[3]
b = open(exe, 'rb').read()
pe = struct.unpack_from('<I', b, 0x3c)[0]; ns = struct.unpack_from('<H', b, pe+6)[0]; osz = struct.unpack_from('<H', b, pe+20)[0]
base = struct.unpack_from('<I', b, pe+24+28)[0]; sec = pe+24+osz
code = []
for i in range(ns):
    n, vs, rva, rs, ro = struct.unpack_from('<8sIIII', b, sec+40*i)
    if struct.unpack_from('<I', b, sec+40*i+36)[0] & 0x20000000: code.append((base+rva, base+rva+min(vs, rs), ro))
def off(a):
    for lo, hi, ro in code:
        if lo <= a < hi: return ro + a - lo
def rd(a):
    o = off(a); return None if o is None else struct.unpack_from('<I', b, o)[0]
calls = set(); sw = {}
for lo, hi, ro in code:
    for o in range(ro + 16, ro + (hi - lo) - 7):
        op = b[o]
        if op == 0xe8: calls.add((lo + o - ro + 5 + struct.unpack_from('<i', b, o+1)[0]) & 0xffffffff)
        elif op == 0xff and b[o+1] == 0x24 and b[o+2] in (0x85, 0x8d, 0x95, 0x9d, 0xad, 0xb5, 0xbd):
            j = lo + o - ro; tbl = struct.unpack_from('<I', b, o+3)[0]
            if off(tbl) is None or not (j < tbl < j + 0x10000): continue
            bound = None
            if b[o-2] == 0x77 and b[o-5] == 0x83 and 0xf8 <= b[o-4] <= 0xff: bound = b[o-3] + 1
            elif b[o-6] == 0x0f and b[o-5] == 0x87 and b[o-9] == 0x83 and 0xf8 <= b[o-8] <= 0xff: bound = b[o-7] + 1
            ts = []
            for k in range(bound or 512):
                t = rd(tbl + 4*k)
                if t is None or off(t) is None or not (j - 0x10000 < t < tbl): break
                ts.append(t)
            if ts: sw[j] = (tbl, ts)
cases = {t for _, ts in sw.values() for t in ts}
rows = list(csv.reader(open(fmap, newline='')))
hdr, body = rows[0], [r for r in rows[1:] if r]
keep = {int(r[0], 16): r for r in body if not (int(r[0], 16) in cases and int(r[0], 16) not in calls)}
dropped = len(body) - len(keep)
starts = sorted(keep)
added = grown = 0
def fstart(j):
    # MSVC: functions start 16-aligned after int3 / zero padding; walk back to it
    a = j & ~15
    while a > j - 0x10000:
        o = off(a)
        if o is not None and b[o-1] in (0xcc,) and b[o] not in (0xcc,): return a
        a -= 16
for j, (tbl, ts) in sorted(sw.items()):
    i = bisect.bisect_right(starts, j) - 1
    s = starts[i] if i >= 0 else None
    p = fstart(j)
    if s is None or (p is not None and p > s and int(keep[s][2]) and s + int(keep[s][2]) <= j):
        s = p  # the holding function is not in the map (or the row before ends before the jmp)
    if s is None: continue
    need = tbl - s
    if s not in keep:
        keep[s] = ['%08x' % s, 'FUN_%08x' % s, str(need), '0', 'switch']; bisect.insort(starts, s); added += 1
    elif int(keep[s][2]) < need:
        keep[s][2] = str(need); keep[s][4] = 'switch'; grown += 1
# rows inside a jump table are data (the analyzer took the table for code): drop them, and start the function MSVC
# placed right after the table (16-aligned, push ebp / mov ebp, esp), which a table row would otherwise swallow
tdrop = tadd = 0
for j, (tbl, ts) in sorted(sw.items()):
    end = tbl + 4 * len(ts)
    for a in [a for a in starts if tbl <= a < end and a not in calls]:
        del keep[a]; starts.remove(a); tdrop += 1
    c = (end + 15) & ~15
    o = off(c)
    if o is not None and c not in keep and b[o:o+3] == bytes([0x55, 0x8b, 0xec]) and all(b[off(x)] in (0xcc, 0x90) for x in range(end, c)):
        nxt = starts[bisect.bisect_right(starts, c)] if bisect.bisect_right(starts, c) < len(starts) else c + 16
        keep[c] = ['%08x' % c, 'FUN_%08x' % c, str(nxt - c), '0', 'aftertable']; bisect.insort(starts, c); tadd += 1
print('table rows dropped', tdrop, 'after-table functions added', tadd)
print('switches', len(sw), 'case targets', len(cases), 'rows', len(body), 'dropped', dropped, 'added', added, 'grown', grown)
with open(out, 'w', newline='') as f:
    w = csv.writer(f, lineterminator='\n'); w.writerow(hdr)
    for a in sorted(keep): w.writerow(keep[a])
