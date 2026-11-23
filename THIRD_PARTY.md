# Third-party components

Our own code (everything not listed here) is under The Unlicense, see `UNLICENSE`. The components below keep their own licenses.

| Component | Where | License | Status |
|---|---|---|---|
| XRC translator by ps4star (github.com/ps4star/XRC) | not in this repo | The Unlicense | Fetched by you, see README |
| Original runtime and launcher by ps4star (WhiteRecomp, github.com/ps4star/WhiteRecomp): the HLE layer, Vulkan renderer, recompiled-code runtime, `launcher/` and its `iv_*` libraries | `src/runtime/`, `src/include/xrc.h`, `launcher/` | Released with ps4star's permission, same terms as this repo (The Unlicense) | Included, modified |
| SMAA 1.x (Jimenez et al.): `SMAA.hlsl`, `AreaTex.h`, `SearchTex.h` | `src/runtime/smaa/` | MIT, text in `src/runtime/smaa/LICENSE.txt` (the copyright lines name the SMAA authors) | Vendored with license |
| stb_truetype.h v1.28 (Sean Barrett) | `src/runtime/third_party/` | MIT or Unlicense (dual, text at the end of the file) | Vendored with license |
| FXAA 2 "fast" variant (Timothy Lottes, NVIDIA) | `src/runtime/vk_d3d9.c`, `fxaa_ps_code` | Our own hand-assembled Shader Model 3 bytecode that implements the published FXAA 2 algorithm; no FXAA source file is vendored. See the FXAA note below | Credit only |
| AMD FidelityFX CAS (method only) | `src/runtime/post/post.frag` | MIT; the shader is a re-implementation of the published method | No code copied, credit only |
| Liberation Sans Bold (Red Hat / Google), Noto Sans Regular (The Noto Project Authors), Orbitron Bold (Matt McInerney) | `launcher/shared/iv_system/ren/assets/fonts/`, test copies under `launcher/shared/*/testdata/` | SIL Open Font License 1.1, `OFL-1.1.txt` and `LICENSES.txt` in the fonts folder | Vendored with license; the fonts keep their OFL |
| Odin compiler, core and vendor libraries (`vendor:raylib`, `vendor:stb`, `vendor:vulkan` bindings used by `launcher/`) | fetched with Odin, not vendored | zlib license (Odin); raylib is zlib, stb is MIT or public domain, Vulkan headers are Apache-2.0 / MIT | Not vendored |
| Vulkan headers | fetched (KhronosGroup/Vulkan-Headers) | Apache-2.0 / MIT | Not vendored |
| FFmpeg shared libs (libavcodec, libavformat, libavutil, libswresample) | fetched, LGPL build, dynamically linked | LGPL-2.1+ | Not vendored |
| SDL2 (loaded at run time on non-Windows hosts only) | not in this repo | zlib | Not vendored |
| Valve `steam_api64.dll` | not in this repo | Steamworks SDK terms | Not vendored; copy from your Steam install |

## FXAA note

The FXAA header that NVIDIA publishes (FXAA 3.11) reads: "NVIDIA FXAA 3.11 by TIMOTHY LOTTES. COPYRIGHT (C) 2010, 2011 NVIDIA CORPORATION.
ALL RIGHTS RESERVED. TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, THIS SOFTWARE IS PROVIDED *AS IS* AND NVIDIA AND ITS SUPPLIERS
DISCLAIM ALL WARRANTIES ..." and then the standard liability disclaimer. It carries no further conditions and no redistribution clause.
This repository does not contain that file or any FXAA source: `fxaa_ps_code` in `vk_d3d9.c` is Shader Model 3 bytecode we assembled by hand
from the FXAA 2 algorithm, with the algorithm described in the comment above it. We credit Timothy Lottes and NVIDIA for the algorithm.
If you copy NVIDIA's FXAA source into a build, keep its header.
