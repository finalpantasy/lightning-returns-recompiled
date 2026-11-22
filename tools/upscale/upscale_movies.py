"""Every converted Lightning Returns movie upscaled with Real-ESRGAN, for the runtime to play instead.

upscale_movies.py <game dir> [--height 2160] [--model RealESRGAN_x4plus.pth] [--ffmpeg ffmpeg.exe] [--status status.txt]

Reads <game dir>/weiss_data/movie/xrc/<archive>@<offset>.mp4 (made by the runtime's converter: lr.exe --convert-movies)
and writes <name>.hd.mp4 beside each one (hle_bink.c plays it at its own size). Resumable: a movie whose .hd.mp4 was
made by this tool at this height is skipped; one made by the runtime's fast Lanczos pass is replaced. Movies whose
picture is the same as one already done (the voice-language copies: moviev / moviea) get that picture with their own
audio, without running the model again. Runs below normal priority. --status: a one-line progress file for the launcher.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (python -I leaves the script's folder off the path)
import esrgan_video as ev  # noqa: E402


def tag_of(ffmpeg, path):
    """The comment tag a .hd.mp4 carries (xrc-hd:...), '' if none."""
    out = subprocess.run([ffmpeg, "-hide_banner", "-i", path], capture_output=True, text=True, creationflags=ev.NO_WINDOW).stderr
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("comment") and ev.TAG_PREFIX in s:
            return s.split(":", 1)[1].strip()
    return ""


def fingerprint(ffmpeg, path):
    """The picture's identity: md5 of 60 decoded frames spread over its start, plus its duration."""
    out = subprocess.run([ffmpeg, "-v", "error", "-i", path, "-map", "0:v:0", "-vf", "select=not(mod(n\\,15))", "-frames:v", "60",
                          "-vsync", "0", "-f", "framemd5", "-"], capture_output=True, text=True, creationflags=ev.NO_WINDOW).stdout
    hashes = [l.split(",")[-1].strip() for l in out.splitlines() if l and not l.startswith("#")]
    dur = subprocess.run([ffmpeg, "-hide_banner", "-i", path], capture_output=True, text=True, creationflags=ev.NO_WINDOW).stderr
    dur = next((l.split("Duration:")[1].split(",")[0].strip() for l in dur.splitlines() if "Duration:" in l), "")
    return hashlib.md5(("|".join(hashes) + dur).encode()).hexdigest()


def frame_count(ffmpeg, path):
    w, h, rate = ev.probe(ffmpeg, path)
    out = subprocess.run([ffmpeg, "-hide_banner", "-i", path], capture_output=True, text=True, creationflags=ev.NO_WINDOW).stderr
    d = next((l.split("Duration:")[1].split(",")[0].strip() for l in out.splitlines() if "Duration:" in l), "0:0:0")
    hh, mm, ss = d.split(":")
    try:
        return int((int(hh) * 3600 + int(mm) * 60 + float(ss)) * float(rate))
    except ValueError:
        return 0


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("--height", type=int, default=2160)
    ap.add_argument("--model", default=os.path.join(here, "RealESRGAN_x4plus.pth"))
    ap.add_argument("--denoise", type=float, default=0.5)
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--status", default="")
    a = ap.parse_args()
    ev.low_priority()
    xdir = os.path.join(a.game_dir, "weiss_data", "movie", "xrc")
    tag = f"realesrgan:{os.path.splitext(os.path.basename(a.model))[0]}:{a.height}"
    names = sorted(f for f in os.listdir(xdir) if f.endswith(".mp4") and not any(s in f for s in (".hd.", ".raw.", ".tmp.")))

    def status(text):
        print(text, flush=True)
        if a.status:
            with open(a.status, "w", encoding="utf-8") as f:
                f.write(text + "\n")

    done_by_fp = {}  # fingerprint -> an .hd.mp4 made by this run's model and height
    for i, name in enumerate(names):
        src = os.path.join(xdir, name)
        hd = src[:-4] + ".hd.mp4"
        tmp = src[:-4] + ".hd.tmp.mp4"
        fp = fingerprint(a.ffmpeg, src)
        if os.path.exists(hd) and tag_of(a.ffmpeg, hd) == ev.TAG_PREFIX + tag:
            done_by_fp.setdefault(fp, hd)
            status(f"movie {i + 1}/{len(names)} {name}: already upscaled")
            continue
        if os.path.exists(tmp):
            os.remove(tmp)
        if fp in done_by_fp:  # the same picture: that one's video with this one's audio
            r = subprocess.run([a.ffmpeg, "-nostdin", "-v", "error", "-y", "-i", done_by_fp[fp], "-i", src, "-map", "0:v", "-map", "1:a?",
                                "-c", "copy", "-metadata", "comment=" + ev.TAG_PREFIX + tag, "-movflags", "+faststart", tmp],
                               creationflags=ev.NO_WINDOW | ev.BELOW_NORMAL).returncode
            how = "same picture as " + os.path.basename(done_by_fp[fp])
        else:
            total = frame_count(a.ffmpeg, src)
            t0 = time.time()

            def prog(n, fps, i=i, name=name, total=total):
                left = (total - n) / fps if fps > 0 and total > n else 0
                status(f"movie {i + 1}/{len(names)} {name}: frame {n}/{total} at {fps:.2f} fps, about {left / 60:.0f} min left on it")

            r = ev.upscale(src, tmp, a.height, a.model, a.denoise, a.ffmpeg, tag=tag, progress=prog)
            how = f"{time.time() - t0:.0f} s"
        if r == 0 and os.path.exists(tmp):
            os.replace(tmp, hd)
            done_by_fp[fp] = hd
            status(f"movie {i + 1}/{len(names)} {name}: upscaled ({how})")
        else:
            if os.path.exists(tmp):
                os.remove(tmp)
            status(f"movie {i + 1}/{len(names)} {name}: FAILED (exit {r})")
    status(f"all {len(names)} movies done")


if __name__ == "__main__":
    main()
