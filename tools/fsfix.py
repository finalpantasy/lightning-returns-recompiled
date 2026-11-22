# Rebase fs: memory operands onto the guest thread's TEB in the generated C.
#
# ff xrc drops the fs: segment (its decoder reports Capstone's register number, not the 5 x_addr checks for),
# so `mov ecx, fs:[2Ch]` reads guest address 0x2c. fs:[0] (the SEH chain) mostly survives that; fs:[18h]
# (TEB self) and fs:[2Ch] (the static TLS array) do not. fs:[0] is rebased too: flat, every guest thread shared one
# SEH chain head at guest address 0, and the game's asserts (`mov ds:[0], 0`) wrote over it -- rebased, guest
# address 0 belongs to nothing, so the runtime can guard it. Run after every `ff xrc` translation:
#   python -I tools/fsfix.py analysis/fsrefs.txt build/xrc_gen/src [image]
# analysis/fsrefs.txt is `lrrecomp fsrefs LRFF13.exe` output: "<addr> <disp hex> <instruction>" per line.
# Only displacements below 0x1000 are patched (larger ones are junk decodes).
import os, re, sys

refs, src = sys.argv[1], sys.argv[2]
want = []
for line in open(refs):
    p = line.split()
    if len(p) < 3:
        continue
    addr, disp = int(p[0], 16), int(p[1], 16)
    if 0 <= disp < 0x1000:
        want.append((addr, disp))

done = 0
cache, dirty = {}, set()


def page(n):
    if n not in cache:
        path = os.path.join(src, "page_%04x.c" % n)
        cache[n] = open(path, encoding="utf-8").read().split("\n") if os.path.exists(path) else None
    return cache[n]


for addr, disp in want:
    tag = "/* %08x " % addr
    # (the page of the function: an instruction just past a 64 KB boundary is in the page its function starts in)
    for n in (addr >> 16, (addr >> 16) - 1, (addr >> 16) - 2):
        lines = page(n)
        if not lines:
            continue
        hit = False
        for i, l in enumerate(lines):
            if l.lstrip().startswith(tag):
                hit = True
                for j in range(i + 1, min(i + 4, len(lines))):
                    old = "_ea = (0x%xu);" % disp
                    if old in lines[j]:
                        lines[j] = lines[j].replace(old, "_ea = (xrc_teb + 0x%xu); /* fs: */" % disp)
                        done += 1
                        dirty.add(n)
                        break
                    if "xrc_teb" in lines[j]:
                        done += 1
                        break
        if hit:
            break
# Second pass, from the image's bytes: fsrefs.txt misses some sites (e.g. SEH epilogues in code its scan never
# reached), so every remaining small-displacement operand whose instruction carries the 0x64 (fs) prefix is
# rebased too. A plain `mov ds:[0], 0` (an assert) has no prefix and stays flat. The image: the third argument,
# default ffxiiiimg.exe.unpacked.exe in the current directory (the project root).
img_path = sys.argv[3] if len(sys.argv) > 3 else "ffxiiiimg.exe.unpacked.exe"
extra = 0
if os.path.exists(img_path):
    import struct
    img = open(img_path, "rb").read()
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    nsec, optsz = struct.unpack_from("<H", img, pe + 6)[0], struct.unpack_from("<H", img, pe + 20)[0]
    base = struct.unpack_from("<I", img, pe + 24 + 28)[0]
    secs = []
    for k in range(nsec):
        o = pe + 24 + optsz + 40 * k
        vsz, va, rsz, raw = struct.unpack_from("<IIII", img, o + 8)
        secs.append((base + va, base + va + max(vsz, rsz), raw, rsz))

    def has_fs(a):
        for lo, hi, raw, rsz in secs:
            if lo <= a < hi and a - lo < rsz:
                off = raw + a - lo
                for b in img[off:off + 5]:  # (legacy prefixes come first, in any order)
                    if b == 0x64:
                        return True
                    if b not in (0x26, 0x2E, 0x36, 0x3E, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3):
                        return False
        return False

    ins = re.compile(r"^\s+/\* ([0-9a-f]{8}) ")
    ea = re.compile(r"_ea = \((0x[0-9a-f]{1,3})u\);")
    for fn in sorted(os.listdir(src)):
        if not (fn.startswith("page_") and fn.endswith(".c")):
            continue
        n = int(fn[5:9], 16)
        lines = page(n)
        cur = None
        for j, l in enumerate(lines):
            m = ins.match(l)
            if m:
                cur = int(m.group(1), 16)
                continue
            if cur is None or "_ea = (0x" not in l:
                continue
            m = ea.search(l)
            if m and has_fs(cur):
                lines[j] = l.replace(m.group(0), "_ea = (xrc_teb + %su); /* fs: */" % m.group(1))
                extra += 1
                dirty.add(n)
for n in dirty:
    open(os.path.join(src, "page_%04x.c" % n), "w", encoding="utf-8", newline="\n").write("\n".join(cache[n]))
print("fs: operands rebased (or already): %d of %d; from the image bytes: %d more" % (done, len(want), extra))
