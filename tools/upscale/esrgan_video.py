"""Upscale a movie with Real-ESRGAN x4plus on the GPU (PyTorch, fp16), frames piped through ffmpeg.

esrgan_video.py IN.mp4 OUT.mp4 --height 2160 [--model RealESRGAN_x4plus.pth] [--ffmpeg ffmpeg.exe] [--frames N]

The network is built here (RRDBNet, as basicsr defines it) so only torch is needed. Each 1280x720 frame is run 4x
(5120x2880) and resized to the target height with an antialiased bicubic filter, then encoded with NVENC H.264
(yuv420p, BT.601 limited range, as the runtime expects). The audio streams are copied.
"""
import argparse
import os
import subprocess
import sys
import time

import torch
import torch.nn as nn
import torch.nn.functional as F


class RDB(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
        self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
        self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
        self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
        self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=True)

    def forward(self, x):
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.rdb1, self.rdb2, self.rdb3 = RDB(nf, gc), RDB(nf, gc), RDB(nf, gc)

    def forward(self, x):
        return self.rdb3(self.rdb2(self.rdb1(x))) * 0.2 + x


class RRDBNet(nn.Module):
    def __init__(self, nf=64, nb=23, gc=32):
        super().__init__()
        self.conv_first = nn.Conv2d(3, nf, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(nf, gc) for _ in range(nb)])
        self.conv_body = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_hr = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_last = nn.Conv2d(nf, 3, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=True)

    def forward(self, x):
        feat = self.conv_first(x)
        feat = feat + self.conv_body(self.body(feat))
        feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode="nearest")))
        feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode="nearest")))
        return self.conv_last(self.lrelu(self.conv_hr(feat)))


class SRVGGNetCompact(nn.Module):
    """realesr-general-x4v3: 32 3x3 convs with PReLU at the input's size, then pixel shuffle, plus the input upscaled."""

    def __init__(self, nf=64, nc=32, up=4):
        super().__init__()
        self.up = up
        layers = [nn.Conv2d(3, nf, 3, 1, 1), nn.PReLU(nf)]
        for _ in range(nc):
            layers += [nn.Conv2d(nf, nf, 3, 1, 1), nn.PReLU(nf)]
        layers += [nn.Conv2d(nf, 3 * up * up, 3, 1, 1)]
        self.body = nn.ModuleList(layers)
        self.upsampler = nn.PixelShuffle(up)

    def forward(self, x):
        out = x
        for m in self.body:
            out = m(out)
        return self.upsampler(out) + F.interpolate(x, scale_factor=self.up, mode="nearest")


def load_net(model, denoise):
    """The network for model (a .pth); for realesr-general-x4v3, denoise in [0, 1] blends in the wdn weights
    (1 = the plain model, keeps grain; 0 = the denoising one), as Real-ESRGAN's own -dn option does."""
    def sd_of(p):
        sd = torch.load(p, map_location="cpu", weights_only=True)
        return sd.get("params_ema", sd.get("params", sd))
    base = os.path.basename(model)
    if base.startswith("realesr-general"):
        net, sd = SRVGGNetCompact(), sd_of(model)
        wdn = os.path.join(os.path.dirname(model), "realesr-general-wdn-x4v3.pth")
        if denoise < 1 and "wdn" not in base and os.path.exists(wdn):
            sd2 = sd_of(wdn)
            sd = {k: denoise * v + (1 - denoise) * sd2[k] for k, v in sd.items()}
    else:
        net, sd = RRDBNet(), sd_of(model)
    net.load_state_dict(sd)
    return net


def probe(ffmpeg, path):
    """(width, height, rate) of the first video stream, read from ffmpeg's banner."""
    out = subprocess.run([ffmpeg, "-hide_banner", "-i", path], capture_output=True, text=True).stderr
    for line in out.splitlines():
        if "Video:" in line:
            for tok in line.replace(",", " ").split():
                if "x" in tok and tok.split("x")[0].isdigit() and tok.split("x")[1].isdigit():
                    w, h = map(int, tok.split("x"))
                    break
            rate = line.split(" fps")[0].split()[-1]
            return w, h, rate
    raise SystemExit(f"no video stream in {path}")


NO_WINDOW, BELOW_NORMAL = 0x08000000, 0x00004000
TAG_PREFIX = "xrc-hd:"


def low_priority():
    """This process (and so its CUDA work) below normal priority: the PC stays usable."""
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), BELOW_NORMAL)


def popen(cmd, **kw):
    flags = (NO_WINDOW | BELOW_NORMAL) if os.name == "nt" else 0
    return subprocess.Popen(cmd, creationflags=flags, **kw)


def upscale(inp, out, height, model, denoise, ffmpeg, frames=0, cq=21, tag="realesrgan", progress=None):
    """inp's video run through the model and resized to height; its audio copied; written to out. 0 on success."""
    w, h, rate = probe(ffmpeg, inp)
    oh = height
    ow = (w * oh // h) & ~1
    dev = torch.device("cuda")
    torch.backends.cudnn.benchmark = True
    net = load_net(model, denoise).half().to(dev).eval().to(memory_format=torch.channels_last)

    dec = [ffmpeg, "-nostdin", "-v", "error", "-i", inp, "-map", "0:v:0", "-f", "rawvideo", "-pix_fmt", "rgb24"]
    if frames:
        dec += ["-frames:v", str(frames)]
    dec += ["-"]
    enc = [ffmpeg, "-nostdin", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{ow}x{oh}", "-r", rate, "-i", "-",
           "-i", inp, "-map", "0:v", "-map", "1:a?", "-c:a", "copy",
           "-vf", "scale=out_range=tv:out_color_matrix=bt601,format=yuv420p",
           "-c:v", "h264_nvenc", "-preset", "p6", "-rc", "vbr", "-cq", str(cq), "-b:v", "0", "-maxrate", "60M", "-bufsize", "120M",
           "-profile:v", "high", "-colorspace", "smpte170m", "-color_range", "tv",
           "-metadata", "comment=" + TAG_PREFIX + tag, "-movflags", "+faststart"]
    if frames:
        enc += ["-frames:v", str(frames), "-shortest"]
    enc += [out]
    pd = popen(dec, stdout=subprocess.PIPE)
    pe = popen(enc, stdin=subprocess.PIPE)
    fsize = w * h * 3
    n, t0 = 0, time.time()
    with torch.inference_mode():
        while True:
            buf = pd.stdout.read(fsize)
            if len(buf) < fsize:
                break
            x = torch.frombuffer(bytearray(buf), dtype=torch.uint8).to(dev, non_blocking=True)
            x = x.view(1, h, w, 3).permute(0, 3, 1, 2).half().div_(255).contiguous(memory_format=torch.channels_last)
            y = net(x)
            y = F.interpolate(y.float(), size=(oh, ow), mode="bicubic", antialias=True, align_corners=False)
            y = y.clamp_(0, 1).mul_(255).round_().to(torch.uint8)[0].permute(1, 2, 0).contiguous()
            pe.stdin.write(y.cpu().numpy().tobytes())
            n += 1
            if progress and n % 50 == 0:
                progress(n, n / max(time.time() - t0, 1e-6))
    pe.stdin.close()
    pd.wait()
    r = pe.wait()
    del net
    torch.cuda.empty_cache()
    return r if n else 1


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--height", type=int, default=2160)
    ap.add_argument("--model", default=os.path.join(here, "RealESRGAN_x4plus.pth"))
    ap.add_argument("--denoise", type=float, default=0.5, help="realesr-general-x4v3 only: 1 keeps grain, 0 removes noise")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--frames", type=int, default=0, help="stop after N frames (a test)")
    ap.add_argument("--cq", type=int, default=21)
    a = ap.parse_args()
    low_priority()
    t0 = time.time()
    r = upscale(a.inp, a.out, a.height, a.model, a.denoise, a.ffmpeg, a.frames, a.cq,
                progress=lambda n, fps: print(f"{n} frames, {fps:.2f} fps", file=sys.stderr, flush=True))
    print(f"done in {time.time() - t0:.1f} s -> {a.out} (exit {r})", file=sys.stderr)
    sys.exit(r)


if __name__ == "__main__":
    main()
