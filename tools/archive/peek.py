"""peek: inflate the zlib streams at 0x800 boundaries of an LR white_*.bin archive; print each stream's size and magic"""
import sys, zlib, collections
path, limit = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 60
f = open(path, "rb")
off, n, magics = 0, 0, collections.Counter()
while n < limit:
    f.seek(off)
    hdr = f.read(2)
    if len(hdr) < 2:
        break
    if hdr[0] == 0x78 and hdr[1] in (0x01, 0x5E, 0x9C, 0xDA):
        f.seek(off)
        d = zlib.decompressobj()
        out = bytearray()
        while not d.eof:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            out += d.decompress(chunk)
        used = f.tell() - off - len(d.unused_data)
        m = bytes(out[:16])
        magics[m[:4]] += 1
        print(f"{off:#012x} c={used:9d} u={len(out):9d} {m.hex()} {m[:8]!r}")
        n += 1
        off += (used + 0x7FF) & ~0x7FF
    else:
        f.seek(off); b = f.read(16)
        print(f"{off:#012x} raw {b.hex()} {b[:8]!r}")
        n += 1
        off += 0x800
print(magics.most_common(10))
