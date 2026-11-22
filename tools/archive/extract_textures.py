"""Every texture in LR's data archives as a .dds named by the runtime's key, so the HD set covers the whole game
without collecting textures in play (vk_d3d9.c tex_check / tex_dump).

The archives (weiss_data\\sys\\white_img*.win32.bin, zone\\white_z*_img*.win32.bin, dlc\\*) are zlib streams at 0x800
boundaries. Texture pixels sit in a raw blob (the IMGB) next to the resource file (SEDBRES or WPD) whose GTEX headers
describe them: format, mips, width and height (big endian), and a mip table of (offset, size) into the IMGB. The IMGB
is right before its resource file in most of the data and right after it in parts (UI); it is the one whose size is
the end of the last mip, rounded up to less than 0x800 (each raw blob belongs to one resource file).
The list files are encrypted; they are not needed.

Key = tex_hash(level-0 bytes, fmt << 32 ^ w << 16 ^ h) (texhash.dll, the runtime's function), fmt = the D3D9 format.
Output: <out>\\<key>.dds, level 0 only, the game's mip count in reserved words (offset 32 = levels, 36 = 'XRCL'),
exactly as the runtime's dump writes them, so upscale_textures.py takes them as they are.

  python -I extract_textures.py --out <dir> [--check <dump dir>,...] [archives or folders ...]
"""
import argparse
import ctypes
import glob
import os
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get("LR_GAME_DIR", "") + "/weiss_data"  # your Steam install folder of the game
TH = ctypes.CDLL(os.path.join(HERE, "texhash.dll"))
TH.tex_hash.restype = ctypes.c_uint64
TH.tex_hash.argtypes = (ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64)


def fourcc(s):
    return struct.unpack("<I", s)[0]


# GTEX format -> (D3D9 format, bytes per 4x4 block or per texel, is block compressed)
FMT = {0x18: (fourcc(b"DXT1"), 8, True), 0x19: (fourcc(b"DXT3"), 16, True), 0x1A: (fourcc(b"DXT5"), 16, True),
       0x03: (21, 4, False), 0x04: (22, 4, False)}


def level0_size(f, w, h):
    _, b, bc = FMT[f]
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * b if bc else w * h * b


def dds(fmt, w, h, levels, body):
    hd = bytearray(128)
    bc = fmt not in (21, 22)
    struct.pack_into("<4sIIIII", hd, 0, b"DDS ", 124, 0x1007 | (0x80000 if bc else 0x8), h, w, len(body) if bc else w * 4)
    struct.pack_into("<I", hd, 28, 1)
    struct.pack_into("<II", hd, 32, levels, fourcc(b"XRCL"))
    struct.pack_into("<I", hd, 76, 32)
    if bc:
        struct.pack_into("<II", hd, 80, 4, fmt)
    else:
        struct.pack_into("<II I IIII", hd, 80, 0x41 if fmt == 21 else 0x40, 0, 32, 0xFF0000, 0xFF00, 0xFF,
                         0xFF000000 if fmt == 21 else 0)
    struct.pack_into("<I", hd, 108, 0x1000)
    return bytes(hd) + body


def streams(path):
    """(offset, data) for each zlib stream at a 0x800 boundary"""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        off = 0
        while off < size:
            f.seek(off)
            hdr = f.read(2)
            if len(hdr) < 2:
                return
            if hdr[0] == 0x78 and hdr[1] in (0x01, 0x5E, 0x9C, 0xDA):
                f.seek(off)
                d = zlib.decompressobj()
                out = []
                ok = True
                try:
                    while not d.eof:
                        chunk = f.read(1 << 20)
                        if not chunk:
                            break
                        out.append(d.decompress(chunk))
                except zlib.error:
                    ok = False
                if ok and d.eof:
                    used = f.tell() - off - len(d.unused_data)
                    yield off, b"".join(out)
                    off += (used + 0x7FF) & ~0x7FF
                    continue
            off += 0x800


def extent(d):
    """end of the last mip in the IMGB over every GTEX header of a resource file (0: none)"""
    m = 0
    i = d.find(b"GTEX")
    while i >= 0:
        if i + 20 <= len(d):
            mips, cube = d[i + 7], d[i + 9]
            mt = struct.unpack_from(">I", d, i + 16)[0]
            for j in range(mips * (6 if cube else 1)):
                if mt < 0x100 and i + mt + 8 * j + 8 <= len(d):
                    o, sz = struct.unpack_from(">II", d, i + mt + 8 * j)
                    m = max(m, o + sz)
        i = d.find(b"GTEX", i + 4)
    return m


def fits(e, blob):
    return e and 0 <= len(blob) - e < 0x800


def gtex_headers(d):
    i = d.find(b"GTEX")
    while i >= 0:
        if i + 32 <= len(d):
            f, mips, typ, cube = d[i + 6], d[i + 7], d[i + 8], d[i + 9]
            w, h, depth, mt = struct.unpack_from(">HHHI", d, i + 10)
            if mt < 0x100 and i + mt + 8 <= len(d):
                o0, s0 = struct.unpack_from(">II", d, i + mt)
                yield f, mips, typ, cube, w, h, depth, o0, s0
        i = d.find(b"GTEX", i + 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--check", default="", help="dump dirs: report how many of their textures the archives give")
    ap.add_argument("--dry", action="store_true", help="write nothing")
    ap.add_argument("paths", nargs="*")
    a = ap.parse_args()
    try:
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)  # BELOW_NORMAL
    except Exception:
        pass
    paths = a.paths or [os.path.join(GAME, "sys"), os.path.join(GAME, "zone"), os.path.join(GAME, "dlc")]
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(x for x in glob.glob(os.path.join(p, "**", "*.bin"), recursive=True) if "white_" in os.path.basename(x).lower())
        else:
            files.append(p)
    os.makedirs(a.out, exist_ok=True)
    want = set()
    for dd in filter(None, a.check.split(",")):
        want |= {n[:16] for n in os.listdir(dd) if n.endswith(".dds")}
    seen, stats, t0 = set(), {"gtex": 0, "written": 0, "nopixels": 0, "badfmt": 0, "skipped": 0}, time.time()
    badfmt = {}
    for path in files:
        last = None  # the last raw blob no resource file has claimed
        pending = None  # a resource file whose IMGB was not right before it: (bytes, extent)
        n0 = len(seen)
        for off, d in streams(path):
            if d[:8] != b"SEDBRES " and d[:4] != b"WPD\0":
                if pending and fits(pending[1], d):
                    take(pending[0], d, seen, stats, badfmt, a)
                    pending, last = None, None
                else:
                    last = d
                continue
            e = extent(d)
            if not e:
                continue
            if pending:
                stats["nopixels"] += pending[0].count(b"GTEX")
                pending = None
            if last is not None and fits(e, last):
                take(d, last, seen, stats, badfmt, a)
                last = None
            else:
                pending = (d, e)
        if pending:
            stats["nopixels"] += pending[0].count(b"GTEX")
        msg = f"{os.path.basename(path)}: {len(seen) - n0} new textures, {len(seen)} total, {time.time() - t0:.0f} s"
        if want:
            msg += f"; dumped ones found {len(want & seen)}/{len(want)}"
        print(msg, flush=True)
    print(stats, "unknown formats:", badfmt)


def take(d, blob, seen, stats, badfmt, a):
    """the textures of resource file d, pixels from IMGB blob"""
    for f, mips, typ, cube, w, h, depth, o0, s0 in gtex_headers(d):
        stats["gtex"] += 1
        if f not in FMT:
            badfmt[f] = badfmt.get(f, 0) + 1
            stats["badfmt"] += 1
            continue
        if cube or depth > 1 or w < 32 or h < 32 or w > 4096 or h > 4096:
            stats["skipped"] += 1
            continue
        n = level0_size(f, w, h)
        if o0 + n > len(blob):
            stats["nopixels"] += 1
            continue
        body = blob[o0:o0 + n]
        fmt = FMT[f][0]
        key = "%016x" % TH.tex_hash(body, n, (fmt << 32) ^ (w << 16) ^ h)
        if key in seen:
            continue
        seen.add(key)
        if a.dry:
            continue
        out = os.path.join(a.out, key + ".dds")
        if not os.path.exists(out):
            with open(out, "wb") as o:
                o.write(dds(fmt, w, h, mips, body))
            stats["written"] += 1


if __name__ == "__main__":
    main()
