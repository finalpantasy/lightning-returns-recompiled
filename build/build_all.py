#!/usr/bin/env python3
"""build_all.py - builds lr.exe (Lightning Returns Recompiled) from this repository and YOUR OWN Steam copy of the game.

    python build/build_all.py --game "C:\\Program Files (x86)\\Steam\\steamapps\\common\\LIGHTNING RETURNS FINAL FANTASY XIII"

What it does (every step is skipped when its result is already up to date):
  1. checks <game>\\LRFF13.exe against the SHA-256 of the current Steam build (read only; the game folder is never written);
  2. finds the toolchain (llvm-mingw clang, FFmpeg dev build, Vulkan headers, Odin), or downloads it into build/_deps;
  3. clones ps4star's XRC translator (https://github.com/ps4star/XRC, The Unlicense) at a pinned commit, applies
     patches/xrc-lr.patch and builds it with Odin (patched core:rexcode, patches/odin-rexcode-i386.patch);
  4. translates your LRFF13.exe to C pages with translator/lr/ (function map, overrides, config);
  5. generates ovr_math_gen.inc from the translated pages, compiles the runtime and the pages with clang, links;
  6. writes lr.exe and the DLLs it loads into the output folder (default out/).

Nothing derived from the game is committed: pages, tables and objects stay in build/_work (ignored by git).
Compiles run at below-normal priority. First full build: roughly 30 to 60 minutes on 8+ cores (page_00cd alone is about
11 minutes), 3 GB of disk in build/_work. Re-runs only redo what changed.

Options: python build/build_all.py --help
"""
import argparse
import concurrent.futures as cf
import glob
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

# ---------------------------------------------------------------------------------------------------- pinned inputs
# The Steam build of LRFF13.exe the function map and overrides in translator/lr were made for (34,003,456 bytes, no DRM).
EXE_SIZE = 34003456
EXE_SHA256 = "31a2bc59d305075f0e8eacee4f65f413a3205186b52b786d9f3a9db8aec1cf93"
# ps4star's translator, pinned: patches/xrc-lr.patch is a diff against exactly this commit.
XRC_URL = "https://github.com/ps4star/XRC"
XRC_COMMIT = "0fdf2205623b8c9aa9fb3e6822f3508cefc677cd"
# Toolchain downloads (used only when the tool is not found; pass --no-download to forbid). sha256 None = not pinned.
LLVM_MINGW_URL = "https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-x86_64.zip"
LLVM_MINGW_SHA256 = "4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412"
ODIN_URL = "https://github.com/odin-lang/Odin/releases/download/dev-2026-11/odin-windows-amd64-dev-2026-11.zip"
ODIN_SHA256 = "259effedf2997293c8fa4a5e497496f8ebce9f826652d9dd353a9dd2113c31fd"
# BtbN's rolling "latest" tag: the file under this name is rebuilt from the n9.0 branch, so it cannot be hash-pinned.
FFMPEG_URL = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip"
VULKAN_URL = "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.330.zip"

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BELOW_NORMAL = 0x00004000 if os.name == "nt" else 0

# Flags: the runtime and the pages are compiled as the project's own builds do. -march=native tunes the exe to the CPU that
# builds it (use --march x86-64-v3 for an exe you will copy to another PC).
# Pages that hold the native-math functions keep -ffp-contract=off (the replacements are checked bit for bit against them).
MATH_PAGES = "0040 0041 0043 004b 005a 00aa 00cf".split()
LINK_LIBS = ["-lavformat", "-lavcodec", "-lavutil", "-lswresample", "-lgdi32", "-luser32", "-lwinmm", "-ldwmapi",
             "-Wl,-Bstatic", "-lpthread", "-Wl,-Bdynamic"]
# The exe sits above the guest memory block, which is at a fixed host address (src/include/xrc.h: XRC_FIXED_BASE).
LINK_FLAGS = ["-Wl,--image-base=0x200000000", "-Wl,--disable-dynamicbase", "-Wl,--disable-high-entropy-va"]


def say(msg):
    print("[build_all] " + msg, flush=True)


def die(msg):
    print("[build_all] ERROR: " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def exe_name(n):
    return n + (".exe" if os.name == "nt" else "")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def run(cmd, cwd=None, env=None, log=None, check=True):
    """Runs a command at below-normal priority. Output goes to `log` (a path) or the console."""
    e = dict(os.environ)
    if env:
        e.update(env)
    out = open(log, "w", encoding="utf-8", errors="replace") if log else None
    try:
        p = subprocess.run([str(c) for c in cmd], cwd=cwd, env=e, stdout=out, stderr=subprocess.STDOUT if out else None,
                           creationflags=BELOW_NORMAL)
    finally:
        if out:
            out.close()
    if check and p.returncode != 0:
        tail = ""
        if log:
            with open(log, encoding="utf-8", errors="replace") as f:
                tail = "".join(f.readlines()[-25:])
        die("command failed (%d): %s\n%s" % (p.returncode, " ".join(str(c) for c in cmd[:6]), tail))
    return p.returncode


def capture(cmd, cwd=None):
    p = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       errors="replace", creationflags=BELOW_NORMAL)
    return p.returncode, p.stdout


def download(url, dest, sha256=None):
    if os.path.isfile(dest) and (sha256 is None or sha256_file(dest) == sha256):
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    say("downloading %s" % url)
    req = urllib.request.Request(url, headers={"User-Agent": "lr-recompiled-build"})
    tmp = dest + ".part"
    with urllib.request.urlopen(req) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        got, last = 0, time.time()
        while True:
            blk = r.read(1 << 20)
            if not blk:
                break
            f.write(blk)
            got += len(blk)
            if time.time() - last > 5:
                say("  %d / %d MB" % (got >> 20, total >> 20))
                last = time.time()
    if sha256 and sha256_file(tmp) != sha256:
        os.remove(tmp)
        die("download of %s does not match the pinned SHA-256" % url)
    os.replace(tmp, dest)
    return dest


def unzip(zpath, dest):
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    say("unpacking %s" % os.path.basename(zpath))
    with zipfile.ZipFile(zpath) as z:
        z.extractall(dest)


def find_file(root, name):
    for dp, _, fns in os.walk(root):
        if name in fns:
            return os.path.join(dp, name)
    return None


# ---------------------------------------------------------------------------------------------------- toolchain
def find_clang(args, deps):
    cands = []
    for d in (args.llvm_mingw, os.environ.get("LLVM_MINGW")):
        if d:
            cands.append(os.path.join(d, "bin", exe_name("x86_64-w64-mingw32-clang")))
    w = shutil.which("x86_64-w64-mingw32-clang")
    if w:
        cands.append(w)
    # winget's per-user llvm-mingw (winget install MartinStorsjo.LLVM-MinGW.UCRT)
    la = os.environ.get("LOCALAPPDATA", "")
    cands += glob.glob(os.path.join(la, "Microsoft", "WinGet", "Packages", "MartinStorsjo.LLVM-MinGW*", "*", "bin",
                                    exe_name("x86_64-w64-mingw32-clang")))
    cands += glob.glob(os.path.join(deps, "llvm-mingw", "*", "bin", exe_name("x86_64-w64-mingw32-clang")))
    for c in cands:
        if os.path.isfile(c):
            return c
    if args.no_download:
        die("llvm-mingw (x86_64-w64-mingw32-clang) not found. Install it (winget install MartinStorsjo.LLVM-MinGW.UCRT) "
            "or pass --llvm-mingw <dir>.")
    z = download(LLVM_MINGW_URL, os.path.join(deps, "dl", os.path.basename(LLVM_MINGW_URL)), LLVM_MINGW_SHA256)
    unzip(z, os.path.join(deps, "llvm-mingw"))
    c = find_file(os.path.join(deps, "llvm-mingw"), exe_name("x86_64-w64-mingw32-clang"))
    if not c:
        die("downloaded llvm-mingw has no x86_64-w64-mingw32-clang")
    return c


def find_ffmpeg(args, deps):
    for d in (args.ffmpeg, os.environ.get("FFMPEG")):
        if d:
            if not os.path.isfile(os.path.join(d, "include", "libavcodec", "avcodec.h")):
                die("--ffmpeg %s has no include/libavcodec/avcodec.h (needs a shared FFmpeg 'dev' build with include/, lib/, bin/)" % d)
            return d
    dl = glob.glob(os.path.join(deps, "ffmpeg", "*", "include", "libavcodec", "avcodec.h"))
    if dl:
        return os.path.dirname(os.path.dirname(os.path.dirname(dl[0])))
    if args.no_download:
        die("FFmpeg dev build not found: pass --ffmpeg <dir> (a shared LGPL build with include/, lib/, bin/).")
    z = download(FFMPEG_URL, os.path.join(deps, "dl", os.path.basename(FFMPEG_URL)))
    unzip(z, os.path.join(deps, "ffmpeg"))
    dl = glob.glob(os.path.join(deps, "ffmpeg", "*", "include", "libavcodec", "avcodec.h"))
    if not dl:
        die("downloaded FFmpeg has no include/libavcodec/avcodec.h")
    return os.path.dirname(os.path.dirname(os.path.dirname(dl[0])))


def find_vulkan(args, deps):
    for d in (args.vulkan_headers, os.environ.get("VULKAN_HEADERS")):
        if d:
            if not os.path.isfile(os.path.join(d, "include", "vulkan", "vulkan.h")):
                die("--vulkan-headers %s has no include/vulkan/vulkan.h" % d)
            return d
    dl = glob.glob(os.path.join(deps, "vulkan-headers", "*", "include", "vulkan", "vulkan.h"))
    if dl:
        return os.path.dirname(os.path.dirname(os.path.dirname(dl[0])))
    if args.no_download:
        die("Vulkan headers not found: pass --vulkan-headers <KhronosGroup/Vulkan-Headers checkout>.")
    z = download(VULKAN_URL, os.path.join(deps, "dl", os.path.basename(VULKAN_URL)))
    unzip(z, os.path.join(deps, "vulkan-headers"))
    dl = glob.glob(os.path.join(deps, "vulkan-headers", "*", "include", "vulkan", "vulkan.h"))
    if not dl:
        die("downloaded Vulkan headers have no include/vulkan/vulkan.h")
    return os.path.dirname(os.path.dirname(os.path.dirname(dl[0])))


def find_odin(args, deps):
    for p in (args.odin, os.environ.get("ODIN")):
        if p:
            if not os.path.isfile(p):
                die("--odin %s is not a file (give the path to odin.exe)" % p)
            return p
    w = shutil.which("odin")
    if w:
        return w
    have = find_file(os.path.join(deps, "odin"), exe_name("odin")) if os.path.isdir(os.path.join(deps, "odin")) else None
    if have:
        return have
    if args.no_download:
        die("Odin not found. Install it from https://github.com/odin-lang/Odin/releases or pass --odin <odin.exe>, "
            "or pass --xrc <prebuilt patched xrc.exe> to skip building the translator.")
    z = download(ODIN_URL, os.path.join(deps, "dl", os.path.basename(ODIN_URL)), ODIN_SHA256)
    unzip(z, os.path.join(deps, "odin"))
    o = find_file(os.path.join(deps, "odin"), exe_name("odin"))
    if not o:
        die("downloaded Odin has no odin.exe")
    return o


def msvc_present():
    """Odin on Windows links with the Windows SDK and the MSVC libraries (Visual Studio Build Tools)."""
    if os.name != "nt":
        return True
    pf = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    return bool(glob.glob(os.path.join(pf, "Windows Kits", "10", "Lib", "*", "um", "x64", "kernel32.lib"))) and \
        bool(glob.glob(os.path.join(pf, "Microsoft Visual Studio", "*", "*", "VC", "Tools", "MSVC", "*", "lib", "x64"))
             or glob.glob(os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Microsoft Visual Studio", "*", "*",
                                       "VC", "Tools", "MSVC", "*", "lib", "x64")))


def git_apply(repo, patch, label):
    """Applies a patch (whitespace-tolerant: Odin's zip may carry CRLF); a patch that is already in place is accepted."""
    rc, _ = capture(["git", "apply", "--ignore-whitespace", "-R", "--check", patch], cwd=repo)
    if rc == 0:
        say("%s already applied" % label)
        return
    rc, out = capture(["git", "apply", "--ignore-whitespace", "--check", patch], cwd=repo)
    if rc != 0:
        die("cannot apply %s to %s:\n%s" % (os.path.basename(patch), repo, out))
    run(["git", "apply", "--ignore-whitespace", patch], cwd=repo)
    say("applied %s" % label)


def build_translator(args, odin_path, work):
    """Returns the path of the patched xrc executable."""
    if args.xrc:
        if not os.path.isfile(args.xrc):
            die("--xrc %s not found" % args.xrc)
        return os.path.abspath(args.xrc)
    xdir = os.path.join(work, "XRC")
    xexe = os.path.join(work, exe_name("xrc_lr"))
    stamp = os.path.join(work, "xrc_lr.stamp")
    want = "%s %s %s" % (XRC_COMMIT, sha256_file(os.path.join(REPO, "patches", "xrc-lr.patch")),
                         os.path.abspath(odin_path))
    if os.path.isfile(xexe) and os.path.isfile(stamp) and open(stamp).read() == want:
        return xexe
    if not shutil.which("git"):
        die("git not found (Git for Windows: winget install Git.Git)")
    if not msvc_present():
        say("WARNING: the Windows SDK / MSVC libraries were not found. Odin needs them to link the translator: install "
            "'Visual Studio Build Tools' with the 'Desktop development with C++' workload "
            "(winget install Microsoft.VisualStudio.2022.BuildTools --override \"--add Microsoft.VisualStudio.Workload.VCTools "
            "--includeRecommended --passive\"), or pass --xrc <prebuilt patched xrc.exe>.")
    # 1. the pinned translator source (autocrlf off: the patches are LF)
    if os.path.isdir(xdir):
        shutil.rmtree(xdir, onerror=lambda f, p, e: (os.chmod(p, 0o700), f(p)))
    say("cloning %s at %s" % (XRC_URL, XRC_COMMIT[:10]))
    run(["git", "-c", "core.autocrlf=false", "clone", "--quiet", XRC_URL, xdir])
    run(["git", "config", "core.autocrlf", "false"], cwd=xdir)
    run(["git", "checkout", "--quiet", XRC_COMMIT], cwd=xdir)
    git_apply(xdir, os.path.join(REPO, "patches", "xrc-lr.patch"), "patches/xrc-lr.patch (x87 port, ps4star's XRC + ours)")
    # 2. an Odin tree with the i386 decoder fixes (a private copy: your own Odin install is never modified)
    oroot_src = os.path.dirname(os.path.abspath(odin_path))
    oroot = os.path.join(work, "odin_root")
    if os.path.isdir(oroot):
        shutil.rmtree(oroot, onerror=lambda f, p, e: (os.chmod(p, 0o700), f(p)))
    os.makedirs(oroot)
    for n in os.listdir(oroot_src):
        s = os.path.join(oroot_src, n)
        if n in ("core", "base", "bin"):
            shutil.copytree(s, os.path.join(oroot, n))
        elif os.path.isfile(s) and n.lower().endswith((".exe", ".dll")):
            shutil.copy2(s, oroot)
    if not os.path.isdir(os.path.join(oroot, "core", "rexcode")):
        die("this Odin has no core:rexcode (needs a dev-2026-11 or newer build): %s" % oroot_src)
    run(["git", "init", "--quiet"], cwd=oroot)
    git_apply(oroot, os.path.join(xdir, "patches", "odin-rexcode-i386.patch"), "patches/odin-rexcode-i386.patch (core:rexcode)")
    shutil.rmtree(os.path.join(oroot, ".git"), onerror=lambda f, p, e: (os.chmod(p, 0o700), f(p)))
    # 3. build (the translator recurses deeply: 16 MB stack)
    say("building the translator with Odin")
    run([os.path.join(oroot, exe_name("odin")), "build", "src", "-out:" + xexe, "-o:speed",
         "-extra-linker-flags:/STACK:16777216"], cwd=xdir, env={"ODIN_ROOT": oroot}, log=os.path.join(work, "xrc_build.log"))
    with open(stamp, "w") as f:
        f.write(want)
    return xexe


# ---------------------------------------------------------------------------------------------------- compile
def stale(out, src, cmd):
    """True when `out` must be rebuilt: missing, older than its source, or built with a different command line."""
    stamp = out + ".cmd"
    if not (os.path.isfile(out) and os.path.isfile(stamp)):
        return True
    if open(stamp).read() != " ".join(cmd):
        return True
    return os.path.getmtime(out) < os.path.getmtime(src)


def compile_all(jobs, label, items):
    """items: [(cmd, src, out)], largest first. Compiles in parallel; stops at the first failure."""
    todo = [(c, s, o) for (c, s, o) in items if stale(o, s, c)]
    say("%s: %d to compile, %d up to date" % (label, len(todo), len(items) - len(todo)))
    if not todo:
        return
    t0 = time.time()
    done = [0]

    def one(it):
        cmd, src, out = it
        t1 = time.time()
        log = out + ".log"
        rc = run(cmd, log=log, check=False)
        if rc != 0:
            with open(log, encoding="utf-8", errors="replace") as f:
                return "FAILED %s\n%s" % (os.path.basename(src), "".join(f.readlines()[:30]))
        with open(out + ".cmd", "w") as f:
            f.write(" ".join(cmd))
        os.remove(log)
        done[0] += 1
        say("%s %d/%d %s (%d s)" % (label, done[0], len(todo), os.path.basename(src), time.time() - t1))
        return None

    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        for err in ex.map(one, todo):
            if err:
                ex.shutdown(wait=False, cancel_futures=True)
                die(err)
    say("%s done in %d s" % (label, time.time() - t0))


def main():
    ap = argparse.ArgumentParser(description="Build lr.exe from this repository and your own Steam copy of the game.")
    ap.add_argument("--game", required=True, help="the game's install folder (or the path of LRFF13.exe); only read")
    ap.add_argument("--out", default=os.path.join(REPO, "out"), help="output folder for lr.exe and the DLLs (default: out/)")
    ap.add_argument("--work", default=os.path.join(HERE, "_work"), help="scratch folder: translator, pages, objects (~3 GB)")
    ap.add_argument("--deps", default=os.path.join(HERE, "_deps"), help="where missing toolchain pieces are downloaded")
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 2), help="parallel compiles (default 4)")
    ap.add_argument("--llvm-mingw", help="llvm-mingw root (x86_64-w64-mingw32-clang in <dir>/bin); or env LLVM_MINGW")
    ap.add_argument("--ffmpeg", help="shared FFmpeg dev build (include/ lib/ bin/); or env FFMPEG")
    ap.add_argument("--vulkan-headers", help="KhronosGroup/Vulkan-Headers checkout; or env VULKAN_HEADERS")
    ap.add_argument("--odin", help="odin.exe (only to build the translator); or env ODIN")
    ap.add_argument("--xrc", help="a prebuilt translator (xrc built from patches/xrc-lr.patch): skips Odin and the clone")
    ap.add_argument("--steam-api", help="Valve's 64-bit steam_api64.dll to copy beside lr.exe (default: found in your Steam library)")
    ap.add_argument("--march", default="native", help="-march for the build (default native: tuned to this CPU)")
    ap.add_argument("--no-download", action="store_true", help="never download tools; fail when one is missing")
    ap.add_argument("--skip-hash-check", action="store_true",
                    help="build from a LRFF13.exe that is not the expected Steam build (the translator data will NOT match it)")
    ap.add_argument("--stop-after", choices=["translate", "compile"], help="stop after that step")
    ap.add_argument("--launcher", action="store_true", help="also build the launcher (needs Odin) into the output folder")
    args = ap.parse_args()

    work, deps, out = (os.path.abspath(p) for p in (args.work, args.deps, args.out))
    os.makedirs(work, exist_ok=True)
    t_start = time.time()

    # ---- 1. the game executable
    exe = args.game
    if os.path.isdir(exe):
        exe = os.path.join(exe, "LRFF13.exe")
    if not os.path.isfile(exe):
        die("LRFF13.exe not found at %s" % exe)
    size = os.path.getsize(exe)
    sha = sha256_file(exe)
    if sha != EXE_SHA256 or size != EXE_SIZE:
        msg = ("%s is not the expected Steam build of LRFF13.exe.\n  expected sha256 %s (%d bytes)\n  found    sha256 %s (%d bytes)\n"
               "In Steam: right-click the game > Properties > Installed Files > Verify integrity of game files, then run this "
               "again. A modified or repacked exe cannot be translated with the shipped function map." % (exe, EXE_SHA256, EXE_SIZE, sha, size))
        if not args.skip_hash_check:
            die(msg)
        say("WARNING: " + msg)
    else:
        say("LRFF13.exe verified (sha256 %s...)" % sha[:16])

    # ---- 2. toolchain
    clang = find_clang(args, deps)
    ffmpeg = find_ffmpeg(args, deps)
    vulkan = find_vulkan(args, deps)
    say("clang: %s" % clang)
    say("FFmpeg: %s" % ffmpeg)
    say("Vulkan headers: %s" % vulkan)
    clang_dir = os.path.dirname(clang)
    os.environ["PATH"] = clang_dir + os.pathsep + os.environ.get("PATH", "")
    CC = os.path.join(clang_dir, exe_name("x86_64-w64-mingw32-clang"))
    CCL = CC

    # ---- 3. translator
    if args.xrc:
        xrc = build_translator(args, None, work)
    else:
        xrc = build_translator(args, find_odin(args, deps), work)
    say("translator: %s" % xrc)

    # ---- 4. translate
    gen = os.path.join(work, "gen")
    pages_dir = os.path.join(gen, "src")
    table = os.path.join(gen, "xrc_table.c")
    stamp = os.path.join(gen, "translate.stamp")
    tdata = os.path.join(REPO, "translator", "lr")
    want = "%s|%s|%s|%s" % (sha, sha256_file(xrc),
                           "".join(sha256_file(os.path.join(tdata, n)) for n in ("xrc.cfg", "overrides.tsv", os.path.join("data", "function_map.csv"))),
                           "1")
    if os.path.isfile(table) and os.path.isfile(stamp) and open(stamp).read() == want:
        say("translation up to date (%s)" % pages_dir)
    else:
        if os.path.isdir(gen):
            shutil.rmtree(gen)
        os.makedirs(pages_dir)
        say("translating LRFF13.exe (about a minute, ~400 MB of C)")
        log = os.path.join(work, "xrc_translate.log")
        run([xrc, "--exe", exe, "--override", tdata, "--all", "--out", pages_dir, "--table", table,
             "--report", os.path.join(gen, "coverage.tsv")],
            cwd=os.path.dirname(xrc), env={"XRC_NO_SLOTS": "1"}, log=log)
        txt = open(log, encoding="utf-8", errors="replace").read()
        m = re.search(r"(\d+) functions\s+(\d+) ok .*?(\d+) hand-written\s+(\d+) unreachable\s+(\d+) need an override", txt)
        if not m:
            die("could not read the translator's summary; see %s" % log)
        nfun, nok, nman, nunr, nneed = (int(x) for x in m.groups())
        say("translated %d functions: %d ok, %d hand-written, %d unreachable, %d need an override" % (nfun, nok, nman, nunr, nneed))
        if nneed != 0 or nfun < 66000:
            die("unexpected translation result (need-an-override must be 0); see %s" % log)
        with open(stamp, "w") as f:
            f.write(want)
    if args.stop_after == "translate":
        return

    # ---- 5. ovr_math_gen.inc (straight-line C made from the translated pages; stays in the work folder)
    inc = os.path.join(gen, "ovr_math_gen.inc")
    if not os.path.isfile(inc) or os.path.getmtime(inc) < os.path.getmtime(table):
        say("generating ovr_math_gen.inc")
        with open(inc, "w", newline="\n") as f:
            p = subprocess.run([sys.executable, "-I", os.path.join(REPO, "tools", "ovr_math", "gen_ovr_math.py")],
                               env=dict(os.environ, LR_GEN=gen), stdout=f, creationflags=BELOW_NORMAL)
        if p.returncode != 0:
            os.remove(inc)
            die("gen_ovr_math.py failed (the translated pages did not have the expected functions)")

    # ---- 6. compile
    inc_dirs = ["-I" + os.path.join(REPO, "src", "include"), "-I" + os.path.join(REPO, "src", "runtime"), "-I" + gen]
    march = "-march=" + args.march
    fp = ["-ffast-math", "-fno-associative-math", "-mrecip=none"]
    common = ["-DXRC_LR", "-DXRC_SEG_FS=xrc_teb"]
    rtf = ["-m64", "-O1", march] + fp + ["-ffp-contract=off", "-Wno-overriding-option", "-Wall", "-DXRC_VULKAN"] + common + \
          ["-I" + os.path.join(ffmpeg, "include"), "-I" + os.path.join(vulkan, "include")] + inc_dirs
    pgf = ["-m64", march] + fp + ["-O1", "-w", "-fno-strict-aliasing", "-fwrapv"] + common + ["-I" + os.path.join(REPO, "src", "include")]
    obj = os.path.join(work, "obj")
    os.makedirs(os.path.join(obj, "rt"), exist_ok=True)
    os.makedirs(os.path.join(obj, "pages"), exist_ok=True)
    rt_items = []
    for f in sorted(glob.glob(os.path.join(REPO, "src", "runtime", "*.c")) + [os.path.join(REPO, "src", "lr_stubs", "match_table.c")]):
        o = os.path.join(obj, "rt", "rt_" + os.path.basename(f)[:-2] + ".o")
        rt_items.append(([CC] + rtf + ["-c", f, "-o", o], f, o))
    rt_items.sort(key=lambda it: -os.path.getsize(it[1]))
    # headers: a change to one rebuilds the runtime (cheap compared with the pages)
    newest_h = max(os.path.getmtime(h) for h in glob.glob(os.path.join(REPO, "src", "*", "*.h")) + [inc])
    for cmd, src, o in rt_items:
        if os.path.isfile(o) and os.path.getmtime(o) < newest_h:
            os.utime(o, (0, 0))
    compile_all(args.jobs, "runtime", rt_items)
    tbl_o = os.path.join(obj, "xrc_table.o")
    compile_all(1, "table", [([CC, "-m64", "-O0", "-w"] + common + inc_dirs + ["-c", table, "-o", tbl_o], table, tbl_o)])
    pg_items = []
    for f in glob.glob(os.path.join(pages_dir, "page_*.c")):
        p = os.path.basename(f)[5:-2]
        o = os.path.join(obj, "pages", "page_%s.o" % p)
        pg_items.append(([CC] + pgf + (["-ffp-contract=off"] if p in MATH_PAGES else []) + ["-c", f, "-o", o], f, o))
    pg_items.sort(key=lambda it: -os.path.getsize(it[1]))
    compile_all(args.jobs, "pages", pg_items)
    if args.stop_after == "compile":
        return

    # ---- 7. link
    os.makedirs(out, exist_ok=True)
    lr = os.path.join(out, exe_name("lr"))
    objs = sorted(glob.glob(os.path.join(obj, "rt", "rt_*.o"))) + [tbl_o] + sorted(glob.glob(os.path.join(obj, "pages", "page_*.o")))
    link_cmd = [CCL, "-m64", "-o", lr] + objs + LINK_FLAGS + ["-L" + os.path.join(ffmpeg, "lib")] + LINK_LIBS
    newest_o = max(os.path.getmtime(o) for o in objs)
    if not os.path.isfile(lr) or os.path.getmtime(lr) < newest_o:
        say("linking lr.exe")
        run(link_cmd, log=os.path.join(work, "link.log"))
    else:
        say("lr.exe up to date")

    # ---- 8. the DLLs beside it
    for pat in ("avcodec-*.dll", "avformat-*.dll", "avutil-*.dll", "swresample-*.dll"):
        found = glob.glob(os.path.join(ffmpeg, "bin", pat))
        if not found:
            die("FFmpeg bin/ has no %s" % pat)
        shutil.copy2(found[0], out)
    steam = args.steam_api
    if not steam:
        # Valve's redistributable 64-bit client DLL ships with every 64-bit Steam game: take the first one in your library
        gdir = os.path.dirname(os.path.abspath(exe))
        common_dir = os.path.dirname(gdir)
        for pat in (os.path.join(common_dir, "*", "steam_api64.dll"), os.path.join(os.path.dirname(common_dir), "..", "steam_api64.dll")):
            hits = sorted(glob.glob(pat))
            if hits:
                steam = hits[0]
                break
    if steam and os.path.isfile(steam):
        shutil.copy2(steam, os.path.join(out, "steam_api64.dll"))
        say("steam_api64.dll copied from %s" % steam)
    else:
        say("WARNING: steam_api64.dll not found. Copy Valve's 64-bit steam_api64.dll (from any 64-bit Steam game's folder) "
            "next to lr.exe, or re-run with --steam-api <file>.")

    # ---- 9. optional launcher
    if args.launcher:
        odin = find_odin(args, deps)
        say("building the launcher")
        run([odin, "build", ".", "-collection:shared=shared", "-subsystem:windows", "-o:speed",
             "-out:" + os.path.join(out, "LRLauncher.exe")], cwd=os.path.join(REPO, "launcher"), log=os.path.join(work, "launcher.log"))

    # ---- 10. build info (for the updater and bug reports)
    rc, head = capture(["git", "rev-parse", "HEAD"], cwd=REPO)
    with open(os.path.join(out, "BUILD_INFO.txt"), "w") as f:
        f.write("repo commit: %s\ntranslator: %s @ %s (+ patches/xrc-lr.patch)\nLRFF13.exe sha256: %s\nmarch: %s\n"
                % (head.strip() if rc == 0 else "unknown", XRC_URL, XRC_COMMIT, sha, args.march))
    say("done in %d s: %s (%.1f MB)" % (time.time() - t_start, lr, os.path.getsize(lr) / 1e6))
    say("Run it with the game's data beside it or in LR_GAME_DIR; see README.md.")


if __name__ == "__main__":
    main()
