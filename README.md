# Lightning Returns Recompiled

A native 64-bit PC port of LIGHTNING RETURNS: FINAL FANTASY XIII, made by statically recompiling the game's x86
executable to C and running it on a new runtime with a Vulkan renderer. Windows, x64.

**This repository contains no game files.** You need your own legally bought Steam copy. Everything that comes from the
game (the translated C pages, tables, shaders, textures, movies, saves) is generated on your machine from your install and
is never committed. Not affiliated with or endorsed by Square Enix.

Name placeholder. Candidates: "Lightning Returns Recompiled" (current), "LR-XRC: Lightning Returns FFXIII Native PC Port",
"Lightning Returns Native: FFXIII Static Recompilation for PC".

> **Status:** work in progress, not released. Screenshots: TODO (real captures will be added before the first release).

## Features

- Vulkan renderer for the game's Direct3D 9 calls (shader model 3 bytecode translated at run time)
- Anti-aliasing: SMAA (default), FXAA at three levels, or off
- Lanczos supersampling (render above the display resolution and downscale) and contrast-adaptive sharpening (CAS)
- Frame pacing fix and a steadier frame limiter
- Faster loading (file I/O and load-path work)
- Native replacements for the hottest math helpers (matrix, vector, quaternion, CRT)
- Shadow stability: stops shadow pop-in from frame-to-frame visibility flicker
- In-game Enhancements menu (the "PC Graphics" row) that edits `lr.cfg` and applies most options at once
- Optional HD movies and textures through the scripts in `tools/upscale` (you run them on your own files)

## Build

Requirements: Windows 10/11 x64, your Steam copy of the game, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw),
Python 3, the [Vulkan headers](https://github.com/KhronosGroup/Vulkan-Headers), a shared LGPL build of
[FFmpeg](https://ffmpeg.org/), the [Odin compiler](https://odin-lang.org/) (translator and launcher), Valve's `steam_api64.dll`
(copy it from your Steam install).

1. **Get the translator.** The recompiler is [ps4star/XRC](https://github.com/ps4star/XRC) (The Unlicense). Clone it, apply
   `patches/odin-rexcode-i386.patch` from that repo to Odin's `core:rexcode`, and build:
   `odin build src -out:xrc.exe -o:speed -extra-linker-flags:/STACK:16777216`.
   See `patches/README.md` for what is still needed on top of the public translator for this game.
2. **Translate your own executable.** Copy the game's unpacked `LRFF13.exe` into a work folder, give XRC a function map and
   `xrc.cfg` for it, and run it (it writes `page_*.c` and `xrc_table.c`). The generated files stay in the work folder.
3. **Build the runtime and link:**
   ```
   LR_GEN=<translator output dir> FFMPEG=<ffmpeg dir> VULKAN_HEADERS=<vulkan-headers dir> \
   LLVM_MINGW=<llvm-mingw dir> sh build/build.sh
   ```
   `tools/ovr_math/gen_ovr_math.py` generates `src/runtime/ovr_math_gen.inc` from your pages (run it first, with
   `LR_GEN` set). Output is `out/lr.exe`.
4. **Run:** put `lr.exe`, the FFmpeg DLLs and `steam_api64.dll` next to the game's data (or point the runtime at it) and start
   it with Steam running.

Environment variables used by the helper scripts: `LR_GEN` (translator output), `LR_ROOT` (work folder for the name tools),
`LR_GAME_DIR` (your game install, texture extraction), `TEXCONV`, `SEEDVR2_DIR`, `GLSLANG`.

## Credits

- **ps4star** for the XRC translator and the WhiteRecomp project this builds on (the runtime design and the launcher).
- SMAA by Jorge Jimenez et al., stb_truetype by Sean Barrett, FXAA by Timothy Lottes, FidelityFX CAS by AMD. See
  `THIRD_PARTY.md`.

## License

To be decided (no LICENSE file yet).
