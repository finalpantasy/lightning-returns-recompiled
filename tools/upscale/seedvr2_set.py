"""A SeedVR2 7B texture set for A/B tests: the color textures that upscale_textures.py sent through its color net
(the "(net)" lines of its log) are redone with SeedVR2 (RGB; alpha bicubic when it varies), BC7 with all mips like
production. Everything else (normal maps, per-channel masks, data textures, .skip markers) is copied from an existing
set. One SeedVR2 CLI process per texture, one GPU job at a time.
usage (tools\\vsr\\img\\venv python, -I): seedvr2_set.py --dump DIR --log PROD_LOG --base SET_DIR --out DIR"""
import argparse, os, re, shutil, subprocess, sys, tempfile, time
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upscale_textures import TEXCONV, read_rgba_dds, write_rgba_dds, run  # noqa: E402

SV = os.environ.get("SEEDVR2_DIR", os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "vsr", "seedvr2"))
DIT = "seedvr2_ema_7b_fp8_e4m3fn_mixed_block35_fp16.safetensors"
LINE = re.compile(r"\] (\w+\.dds) (\d+)x(\d+) -> (\d+)x(\d+) \(net\)")

ap = argparse.ArgumentParser()
ap.add_argument("--dump", required=True)
ap.add_argument("--log", required=True)
ap.add_argument("--base", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--timeout", type=int, default=400)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
net = {}
for ln in open(a.log, encoding="utf-8", errors="replace"):
    m = LINE.search(ln)
    if m:
        net[m.group(1)] = (int(m.group(4)), int(m.group(5)))
for n in os.listdir(a.base):  # (everything that is not a color net texture: as the base set has it)
    if n not in net and not os.path.exists(os.path.join(a.out, n)) and os.path.isfile(os.path.join(a.base, n)):
        shutil.copyfile(os.path.join(a.base, n), os.path.join(a.out, n))
todo = sorted(n for n in net if not os.path.exists(os.path.join(a.out, n)))
print(f"{len(net)} color textures, {len(todo)} to make", flush=True)
env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
t0 = time.time()
with tempfile.TemporaryDirectory(dir=a.out) as tmp:
    for i, n in enumerate(todo):
        tw, th = net[n]
        try:
            run([TEXCONV, "-nologo", "-y", "-m", "1", "-f", "R8G8B8A8_UNORM", "-ft", "dds", "-o", tmp, os.path.join(a.dump, n)])
            img = read_rgba_dds(os.path.join(tmp, n))
            os.remove(os.path.join(tmp, n))
            src, dst = os.path.join(tmp, "in.png"), os.path.join(tmp, "out.png")
            Image.fromarray(img[..., :3]).save(src)
            if os.path.exists(dst):
                os.remove(dst)
            cmd = [SV + r"\venv\Scripts\python.exe", SV + r"\repo\inference_cli.py", src, "--output", dst, "--output_format", "png",
                   "--model_dir", SV + r"\models", "--dit_model", DIT, "--resolution", str(min(tw, th)), "--max_resolution", str(max(tw, th)),
                   "--batch_size", "1", "--vae_encode_tiled", "--vae_decode_tiled", "--cuda_device", "0"]
            p = subprocess.Popen(cmd, env=env, cwd=SV + r"\repo", creationflags=0x4000, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding="utf-8", errors="replace")
            try:
                log, _ = p.communicate(timeout=a.timeout)
            except subprocess.TimeoutExpired:
                subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
                log = "timeout"
            if p.returncode != 0 or not os.path.exists(dst):  # (failed: the base set's file stays in its place)
                shutil.copyfile(os.path.join(a.base, n), os.path.join(a.out, n))
                print(f"[{i + 1}/{len(todo)}] {n} SeedVR2 failed ({'timeout' if log == 'timeout' else 'error'}): base file kept", flush=True)
                continue
            o = Image.open(dst).convert("RGB")
            if o.size != (tw, th):
                o = o.resize((tw, th), Image.BICUBIC)
            y = np.dstack([np.array(o), np.full((th, tw), 255, np.uint8)])
            if img[..., 3].min() != img[..., 3].max():
                y[..., 3] = np.array(Image.fromarray(img[..., 3]).resize((tw, th), Image.BICUBIC))
            big = os.path.join(tmp, "big_" + n)
            write_rgba_dds(big, y)
            run([TEXCONV, "-nologo", "-y", "-m", "0", "-f", "BC7_UNORM", "-ft", "dds", "-o", tmp, big])
            os.replace(big, os.path.join(a.out, n))
            print(f"[{i + 1}/{len(todo)}] {n} {img.shape[1]}x{img.shape[0]} -> {tw}x{th} (SeedVR2 7B) {time.time() - t0:.0f}s", flush=True)
        except Exception as e:  # (one bad texture: the rest go on)
            print(f"[{i + 1}/{len(todo)}] {n} failed: {e}", flush=True)
