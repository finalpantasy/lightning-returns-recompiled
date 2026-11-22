r"""HD textures for Lightning Returns (the runtime's XRC_TEX_DIR).

The runtime (XRC_TEX_DUMP=<dir>) writes each texture's top level as <hash>.dds. This makes <out>\<hash>.dds for each:
4x-UltraSharpV2 (picked in the model bake-off, build\compare\texture_models), sized to 4x for textures up to 512
texels a side and 2x for larger ones (the few 4096-wide backdrops go to 8192); BC7 with all its mips (texconv,
DirectXTex). Normal maps (mostly blue) go through 4x-Normal-RG0-BC1 on X,Y with Z rebuilt and normalized; two-channel
RG maps (olive: B near 0) through the same net on R,G with B resized; DXT5nm maps (flat color, detail in alpha) are
only resized. Alpha is resized (bicubic): the bake-off's network alpha was blotchier. Textures with one mip level (the runtime records it: UI, fonts, logos -- drawn with the
runtime's HD UI option only) are made 2x: glyph atlases ('text': tightly packed, two-tone) by bicubic and an edge
curve, so no glyph bleeds into its neighbor; other UI art by the network, its alpha by bicubic and the edge curve
(network-made alpha gave jagged outlines). Lookup tables and dither patterns are kept (classify). Resumable: textures already in <out> are skipped. Runs below normal priority.

  ..\\vsr\\img\\venv\\Scripts\\python.exe upscale_textures.py --dump <dump dir> --out <tex dir> [--limit N] [--max 8192]
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esrgan_video as ev  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(os.path.dirname(HERE), "texsr", "models")
TEXCONV = os.environ.get("TEXCONV", "texconv.exe")  # DirectXTex texconv, fetched separately


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
    if r.returncode:
        raise RuntimeError(" ".join(cmd) + "\n" + r.stdout[-800:] + r.stderr[-800:])


def read_rgba_dds(path):
    b = open(path, "rb").read()
    h, w = struct.unpack_from("<II", b, 12)
    off = 148 if b[84:88] == b"DX10" else 128
    return np.frombuffer(b, np.uint8, h * w * 4, off).reshape(h, w, 4).copy()


def write_rgba_dds(path, a):
    h, w = a.shape[:2]
    hd = bytearray(128)
    hd[0:4] = b"DDS "
    struct.pack_into("<IIIIII", hd, 4, 124, 0x100F, h, w, w * 4, 0)
    struct.pack_into("<I", hd, 28, 1)
    struct.pack_into("<IIIIIIII", hd, 76, 32, 0x41, 0, 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000)
    struct.pack_into("<I", hd, 108, 0x1000)
    with open(path, "wb") as f:
        f.write(hd)
        f.write(np.ascontiguousarray(a, np.uint8).tobytes())


def is_normal_map(a, fourcc):
    rgb = a[..., :3].reshape(-1, 3).astype(np.float32)
    m, s = rgb.mean(0), rgb.std(0)
    if m[2] > 190 and 90 < m[0] < 170 and 90 < m[1] < 170:
        return True
    if fourcc == b"DXT5" and s.max() < 6 and a[..., 3].std() > 20:  # (DXT5nm: x in alpha, flat color)
        return True
    return False


def target_size(w, h, cap):
    s = 4 if max(w, h) <= 512 else 2
    tw, th = w * s, h * s
    if max(tw, th) > cap:
        k = cap / max(tw, th)
        tw, th = max(4, int(tw * k)), max(4, int(th * k))
    return tw // 4 * 4 or 4, th // 4 * 4 or 4


TILE = 192  # (transformer models (UltraSharpV2) need small tiles; set from --tile)


@torch.inference_mode()
def esrgan(net, x, tw=None, th=None, pad=16):
    """x: 1x3xHxW on the GPU, 0..1 -> 1x3xTHxTW (default 4x), in tiles with reflected borders; each tile's
    4x output is resized straight to the target scale. A tile that comes out non-finite in half is redone in float."""
    x = x.float()
    _, c, h, w = x.shape
    tw, th = tw or w * 4, th or h * 4
    sx, sy = tw / w, th / h
    xp = F.pad(x, (pad, pad, pad, pad), mode="reflect" if min(h, w) > pad else "replicate")
    out = torch.zeros((1, c, th, tw), device=x.device)
    for y0 in range(0, h, TILE):
        for x0 in range(0, w, TILE):
            y1, x1 = min(y0 + TILE, h), min(x0 + TILE, w)
            t = xp[:, :, y0:y1 + 2 * pad, x0:x1 + 2 * pad]
            r = net(t.half()).float()
            if not torch.isfinite(r).all():
                net.float()
                r = net(t).float()
                net.half()
            r = r[:, :, pad * 4:(y1 - y0 + pad) * 4, pad * 4:(x1 - x0 + pad) * 4]
            oy0, oy1, ox0, ox1 = round(y0 * sy), round(y1 * sy), round(x0 * sx), round(x1 * sx)
            if r.shape[2] != oy1 - oy0 or r.shape[3] != ox1 - ox0:
                r = F.interpolate(r, size=(oy1 - oy0, ox1 - ox0), mode="bicubic", align_corners=False, antialias=True)
            out[:, :, oy0:oy1, ox0:ox1] = r
    return out.clamp_(0, 1)


def renorm(v):
    """RG of a tangent-space normal map -> unit vectors (Z rebuilt)"""
    xy = v[:, :2] * 2 - 1
    z = (1 - (xy ** 2).sum(1, keepdim=True)).clamp(0).sqrt()
    n = torch.cat((xy, z), 1)
    return n / n.norm(dim=1, keepdim=True).clamp(min=1e-6) * .5 + .5


def load(path, dev):
    from spandrel import ModelLoader
    net = ModelLoader().load_from_file(path).model.eval().half().to(dev)
    net.requires_grad_(False)
    return net


def resize(x, tw, th):
    return F.interpolate(x.float(), size=(th, tw), mode="bicubic", align_corners=False, antialias=True).clamp_(0, 1)


DETAIL = 0.7  # (set from --detail)


def detail(y, x, strength=None):
    """Back-projection: the network smooths away some of the original's fine contrast (rock streaks read painted in
    game). Box-shrink the result to the original size (as the mips are made), take what it lost against the original,
    and add that back, resized. At 1.0 the shrunk result equals the original; below it, part of the original's DXT
    block noise stays out. y, x: 1xCxHxW in 0..1 (target, original)."""
    s = DETAIL if strength is None else strength
    if s <= 0:
        return y
    h, w = x.shape[-2:]
    down = F.interpolate(y.float(), size=(h, w), mode="area")
    th, tw = y.shape[-2:]
    lost = F.interpolate(x.float() - down, size=(th, tw), mode="bicubic", align_corners=False)
    return (y.float() + s * lost).clamp_(0, 1)


SHARPEN = 0.0  # (set from --sharpen)


def sharpen(y, amount=None, sigma=1.0):
    """Unsharp mask at the target size: y + amount * (y - gaussian(y)). 0 = off."""
    k = SHARPEN if amount is None else amount
    if k <= 0:
        return y
    r = int(3 * sigma + 0.5)
    t = torch.arange(-r, r + 1, device=y.device, dtype=torch.float32)
    g = torch.exp(-t * t / (2 * sigma * sigma)); g = g / g.sum()
    c = y.shape[1]
    b = F.pad(y.float(), (r, r, r, r), mode="replicate")
    b = F.conv2d(b, g.view(1, 1, 1, -1).repeat(c, 1, 1, 1), groups=c)
    b = F.conv2d(b, g.view(1, 1, -1, 1).repeat(c, 1, 1, 1), groups=c)
    return (y.float() + k * (y.float() - b)).clamp_(0, 1)


FLAT = None  # (set from --flat lo,hi)
PACKED_BICUBIC = False  # (set from --packed-bicubic)


def gblur(x, sigma):
    r = int(3 * sigma + 0.5)
    t = torch.arange(-r, r + 1, device=x.device, dtype=torch.float32)
    g = torch.exp(-t * t / (2 * sigma * sigma)); g = g / g.sum()
    c = x.shape[1]
    b = F.pad(x.float(), (r, r, r, r), mode="replicate")
    b = F.conv2d(b, g.view(1, 1, 1, -1).repeat(c, 1, 1, 1), groups=c)
    return F.conv2d(b, g.view(1, 1, -1, 1).repeat(c, 1, 1, 1), groups=c)


def flat_guard(y, x):
    """The DXT 4x4 blocks of a flat area (plaster, painted walls) are below what the eye sees in the original, but the
    network and the detail step turn them into hard white squares. Where the original is flat (the local spread of its
    blurred luminance under FLAT lo..hi), the result fades to the blurred original, bicubic-resized; detailed areas are
    left as they are. y, x: 1x3xHxW in 0..1 (target, original). Returns (result, fraction faded)."""
    if not FLAT:
        return y, 0.0
    lo, hi = FLAT
    xb = gblur(x, 1.0)
    lum = (0.299 * xb[:, 0:1] + 0.587 * xb[:, 1:2] + 0.114 * xb[:, 2:3])
    m = gblur(lum, 3.0)
    sd = (gblur(lum * lum, 3.0) - m * m).clamp_(min=0).sqrt_()
    w = ((sd - lo) / (hi - lo)).clamp_(0, 1)
    th, tw = y.shape[-2:]
    w = F.interpolate(w, size=(th, tw), mode="bilinear", align_corners=False)
    base = resize(xb, tw, th)
    return (w * y.float() + (1 - w) * base).clamp_(0, 1), float((1 - w).mean())


def edge_curve(x, lo=0.35, hi=0.65):
    """sharpens soft (resized) two-tone edges back to a narrow ramp: smoothstep(lo, hi)"""
    t = ((x - lo) / (hi - lo)).clamp_(0, 1)
    return t * t * (3 - 2 * t)


def is_text_atlas(a):
    """a glyph atlas: nearly every texel at one of two levels (ink or background) in luminance or alpha"""
    f = a.astype(np.float32)
    for ch in (f[..., :3].mean(-1), f[..., 3]):
        lo, hi = np.percentile(ch, 2), np.percentile(ch, 98)
        if hi - lo < 64:
            continue
        mid = (ch > lo + 0.15 * (hi - lo)) & (ch < hi - 0.15 * (hi - lo))
        if mid.mean() < 0.12 and (ch > (lo + hi) / 2).mean() > 0.02:
            return True
    return False


def upscale_ui(net, dev, a):
    """one-level textures (UI): 2x, capped at 4096"""
    h, w = a.shape[:2]
    s = 2 if max(w, h) <= 2048 else 1
    tw, th = w * s, h * s
    x = torch.from_numpy(a).to(dev).permute(2, 0, 1).unsqueeze(0).float().div_(255)
    big = F.interpolate(x, size=(th, tw), mode="bicubic", align_corners=False).clamp_(0, 1)
    if is_text_atlas(a):
        # each channel: back to two-tone edges between its own ink and background levels
        out = []
        for c in range(4):
            ch = big[:, c:c + 1]
            lo, hi = x[:, c].min(), x[:, c].max()
            if hi - lo < 0.1:
                out.append(ch)
                continue
            out.append(lo + (hi - lo) * edge_curve((ch - lo) / (hi - lo)))
        y = torch.cat(out, 1)
        kind = "UI text atlas: bicubic + edge curve"
    else:
        rgb = esrgan(net, x[:, :3], tw, th)
        al = big[:, 3:4]
        amin, amax = x[:, 3].min(), x[:, 3].max()
        if amax - amin > 0.1:
            # (alpha: mostly 0 / 1 with soft edges -> crisp edges; partial alpha regions (glows) stay as resized)
            fa = x[:, 3].flatten()
            if ((fa > 0.1) & (fa < 0.9)).float().mean() < 0.15:
                al = amin + (amax - amin) * edge_curve((al - amin) / (amax - amin))
        y = torch.cat((rgb, al), 1)
        kind = "UI art: esrgan, alpha bicubic + edge curve"
    y = y[0].permute(1, 2, 0).mul_(255).round_().clamp_(0, 255).to(torch.uint8).cpu().numpy()
    return y, kind


def classify(a):
    """'skip' (data: lookup tables, dither patterns, smooth gradients -- nothing to gain, and their values matter),
    'packed' (unrelated masks in the channels: each channel on its own), else 'image'."""
    f = a.astype(np.float32)
    used = [c for c in range(4) if f[..., c].std() > 3]
    if not used:
        return "skip"
    hf = []
    for c in used:
        ch = f[..., c]
        d1 = (np.abs(np.diff(ch, axis=1)).mean() + np.abs(np.diff(ch, axis=0)).mean()) / 2
        d2 = (np.abs(ch[:, 2:] - ch[:, :-2]).mean() + np.abs(ch[2:] - ch[:-2]).mean()) / 2
        if d1 > 4 and d1 > 1.8 * d2:  # (alternating texel to texel: a dither or checker pattern)
            return "skip"
        hf.append(d1)
    if max(hf) < 1.0:  # (only smooth gradients)
        return "skip"
    for c in used:  # (random per-texel noise, a lookup the shaders sample: neighbors unrelated -- the net makes cells)
        ch = f[..., c] - f[..., c].mean()
        if c < 3 and (ch[:, :-1] * ch[:, 1:]).mean() < 0.15 * ch.var():
            return "skip"
    g = f[..., :3].mean(-1)
    if g.std() > 3:
        d1 = (np.abs(np.diff(g, axis=1)).mean() + np.abs(np.diff(g, axis=0)).mean()) / 2
        if d1 < 0.05 * g.std():  # (soft noise masks, gradients: the net invents ripples; resized instead)
            return "smooth"
    rgb = [c for c in used if c < 3]
    for i in range(len(rgb)):
        for j in range(i + 1, len(rgb)):
            if np.corrcoef(f[..., rgb[i]].ravel(), f[..., rgb[j]].ravel())[0, 1] < 0.5:
                return "packed"
    return "image"


def gray_up(net, x1, tw, th):
    """one channel (1x1xHxW) through the network as gray"""
    return esrgan(net, x1.expand(-1, 3, -1, -1).contiguous(), tw, th).mean(1, keepdim=True)


def upscale_one(net, nnet, dev, a, fourcc, cap, reuse=None):
    """reuse: an earlier result's top level (HxWx4 uint8) for this texture: only the detail step is redone on its
    network-made RGB (color textures; anything else returns None, 'kept')."""
    h, w = a.shape[:2]
    tw, th = target_size(w, h, cap)
    cls = classify(a)
    if cls == "skip":
        return None, "data texture: kept"
    x = torch.from_numpy(a).to(dev).permute(2, 0, 1).unsqueeze(0).float().div_(255)
    if reuse is not None:
        m = a[..., :3].reshape(-1, 3).mean(0)
        olive = m[2] < 40 and abs(m[0] - 128) < 20 and abs(m[1] - 128) < 30
        if cls == "smooth" or is_normal_map(a, fourcc) or olive or reuse.shape[:2] != (th, tw):
            return None, "reuse: not a color net texture, left as it was"
        r = torch.from_numpy(reuse).to(dev).permute(2, 0, 1).unsqueeze(0).float().div_(255)
        rgb, fl = flat_guard(sharpen(detail(r[:, :3], x[:, :3])), x[:, :3])
        y = torch.cat((rgb, r[:, 3:4]), 1)
        y = y[0].permute(1, 2, 0).mul_(255).round_().clamp_(0, 255).to(torch.uint8).cpu().numpy()
        return y, f"reuse + detail {DETAIL} + sharpen {SHARPEN}" + (f" + flat guard {fl:.0%}" if FLAT else "")
    al = resize(x[:, 3:4], tw, th)  # (alpha: bicubic, sharper than the network's in the bake-off)
    m = a[..., :3].reshape(-1, 3).mean(0)
    olive = m[2] < 40 and abs(m[0] - 128) < 20 and abs(m[1] - 128) < 30  # (two-channel RG normal maps, B ~0)
    if cls == "smooth":
        y = resize(x, tw, th)
        kind = "smooth: bicubic"
    elif is_normal_map(a, fourcc) and fourcc == b"DXT5" and a[..., 3].std() > 20 and a[..., :3].std() < 6:
        y = resize(x, tw, th)
        kind = "DXT5nm normal map: bicubic"
    elif is_normal_map(a, fourcc) or olive:
        inp = x[:, :3].clone()
        inp[:, 2] = 0
        o = esrgan(nnet, inp, tw, th)
        if olive and not is_normal_map(a, fourcc):
            y = torch.cat((o[:, :2], resize(x[:, 2:3], tw, th), al), 1)
            kind = "RG map: normal net on R,G; B bicubic"
        else:
            y = torch.cat((renorm(o), al), 1)
            kind = "normal map: normal net, Z rebuilt"
    else:
        if cls == "packed" and PACKED_BICUBIC:  # (masks: the net turns their DXT block noise into hard squares)
            rgb = resize(x[:, :3], tw, th)
        elif cls == "packed":
            rgb = torch.cat([gray_up(net, x[:, c:c + 1], tw, th) for c in range(3)], 1)
        else:
            rgb = esrgan(net, x[:, :3], tw, th)
        rgb, fl = flat_guard(sharpen(detail(rgb, x[:, :3])), x[:, :3])
        y = torch.cat((rgb, al), 1)
        kind = ("net, each channel alone" if cls == "packed" else "net") + (f", flat guard {fl:.0%}" if FLAT else "")
    y = y[0].permute(1, 2, 0).mul_(255).round_().clamp_(0, 255).to(torch.uint8).cpu().numpy()
    return y, kind


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max", type=int, default=8192)  # (only textures above 2048 reach it: 2x)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--ui-only", action="store_true", help="only one-level (UI) textures")
    ap.add_argument("--no-ui", action="store_true", help="skip one-level (UI) textures")
    ap.add_argument("--only", default="", help="comma-separated hashes to make (tests)")
    ap.add_argument("--model", default=os.path.join(MODELS, "ultrasharpv2", "4x-UltraSharpV2.safetensors"))
    ap.add_argument("--normal-model", default=os.path.join(MODELS, "rundev", "4x-Normal-RG0-BC1.pth"))
    ap.add_argument("--tile", type=int, default=192)
    ap.add_argument("--detail", type=float, default=0.7, help="back-projection strength (0 = off, 1 = full)")
    ap.add_argument("--sharpen", type=float, default=0.0, help="unsharp mask amount at the target size (0 = off)")
    ap.add_argument("--flat", default="", help="lo,hi: fade flat areas (local luminance spread below hi) to the blurred original")
    ap.add_argument("--packed-net", action="store_true", help="packed mask textures through the net (default: bicubic; the net turns their DXT noise into hard squares)")
    ap.add_argument("--cpu", action="store_true", help="run on the CPU (with --reuse: no network work)")
    ap.add_argument("--reuse", default="", help="earlier output dir: redo only the detail step on its color textures")
    a = ap.parse_args()
    global TILE, DETAIL, SHARPEN, FLAT, PACKED_BICUBIC
    PACKED_BICUBIC = not a.packed_net
    TILE = a.tile
    DETAIL = a.detail
    SHARPEN = a.sharpen
    if a.flat:
        FLAT = tuple(float(v) for v in a.flat.split(","))
    ev.low_priority()
    os.makedirs(a.out, exist_ok=True)
    done = lambda n: os.path.exists(os.path.join(a.out, n)) or os.path.exists(os.path.join(a.out, n[:-4] + ".skip"))
    names = sorted(n for n in os.listdir(a.dump) if n.lower().endswith(".dds") and not done(n))
    if a.only:
        names = [n for n in names if n[:16] in a.only.split(",")]
    if a.limit:
        names = names[: a.limit]
    print(f"{len(names)} textures to make", flush=True)
    dev = torch.device("cpu" if a.cpu else "cuda")
    torch.backends.cudnn.benchmark = False
    net = load(a.model, dev)
    nnet = load(a.normal_model, dev)
    t0 = time.time()
    with tempfile.TemporaryDirectory(dir=a.out) as tmp:  # (same drive: the finished file is moved)
        for i, n in enumerate(names):
            src = os.path.join(a.dump, n)
            try:
                hdr = open(src, "rb").read(128)
                fourcc = hdr[84:88]
                levels, tag = struct.unpack_from("<II", hdr, 32)
                ui = tag == 0x4C435258 and levels < 2  # ('XRCL': the game's mip count; one level: UI, fonts, logos)
                if ui and a.no_ui:
                    continue
                if not ui and a.ui_only:
                    continue
                run([TEXCONV, "-nologo", "-y", "-m", "1", "-f", "R8G8B8A8_UNORM", "-ft", "dds", "-o", tmp, src])
                img = read_rgba_dds(os.path.join(tmp, n))
                if ui and (classify(img) == "skip" or is_normal_map(img, fourcc)):
                    y, kind = None, "UI data texture: kept"
                elif ui and not a.reuse:
                    y, kind = upscale_ui(net, dev, img)
                elif a.reuse:  # (UI: its size differs from target_size, so it is copied as it was)
                    prev = os.path.join(a.reuse, n)
                    if not os.path.exists(prev):
                        y, kind = None, "reuse: none made before"
                    else:
                        os.makedirs(os.path.join(tmp, "r"), exist_ok=True)
                        run([TEXCONV, "-nologo", "-y", "-m", "1", "-f", "R8G8B8A8_UNORM", "-ft", "dds", "-o", os.path.join(tmp, "r"), prev])
                        rp = os.path.join(tmp, "r", n)
                        y, kind = (None, "reuse: UI") if ui else upscale_one(net, nnet, dev, img, fourcc, a.max, read_rgba_dds(rp))
                        os.remove(rp)
                        if y is None and kind != "data texture: kept":  # (not a color texture: the earlier file as it was)
                            import shutil
                            shutil.copyfile(prev, os.path.join(a.out, n))
                            print(f"[{i + 1}/{len(names)}] {n} ({kind})", flush=True)
                            os.remove(os.path.join(tmp, n))
                            continue
                else:
                    y, kind = upscale_one(net, nnet, dev, img, fourcc, a.max)
                os.remove(os.path.join(tmp, n))
                if y is None:  # (kept as it is: a marker, so it is not looked at again)
                    open(os.path.join(a.out, n[:-4] + ".skip"), "wb").close()
                    print(f"[{i + 1}/{len(names)}] {n} {img.shape[1]}x{img.shape[0]} ({kind})", flush=True)
                    continue
                big = os.path.join(tmp, "big_" + n)
                write_rgba_dds(big, y)
                run([TEXCONV, "-nologo", "-y", "-m", "0", "-f", "BC7_UNORM", "-ft", "dds", "-o", tmp, big])
                os.replace(os.path.join(tmp, "big_" + n), os.path.join(a.out, n))
                print(f"[{i + 1}/{len(names)}] {n} {img.shape[1]}x{img.shape[0]} -> {y.shape[1]}x{y.shape[0]} ({kind}) {time.time() - t0:.0f}s", flush=True)
            except Exception as e:  # (one bad texture: the rest go on)
                print(f"[{i + 1}/{len(names)}] {n} failed: {e}", flush=True)
            torch.cuda.empty_cache()


if __name__ == "__main__":
    main()
