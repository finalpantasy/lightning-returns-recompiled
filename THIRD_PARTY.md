# Third-party components

| Component | Where | License | Status |
|---|---|---|---|
| XRC translator by ps4star (github.com/ps4star/XRC) | not in this repo | The Unlicense | Fetched by you, see README |
| SMAA 1.x (Jimenez et al.), SMAA.hlsl, AreaTex.h, SearchTex.h | `src/runtime/smaa/` | MIT, `src/runtime/smaa/LICENSE.txt` (copyright lines name the SMAA authors) | Vendored with license |
| stb_truetype.h (Sean Barrett) | `src/runtime/third_party/` | MIT or Unlicense (dual, text at the end of the file) | Vendored with license |
| FXAA (Timothy Lottes, NVIDIA, FXAA 2 fast variant) | reimplemented in the Vulkan backend (not in this staged tree) | The original FXAA 2/3 source is public domain / NVIDIA permissive; check the header text before release | Not vendored |
| AMD FidelityFX CAS (method only) | `src/runtime/post/post.frag` | MIT; the shader is a re-implementation of the published method | No code copied, credit only |
| Vulkan headers | fetched (KhronosGroup/Vulkan-Headers) | Apache-2.0 / MIT | Not vendored |
| FFmpeg shared libs (libavcodec, libavformat, libavutil, libswresample) | fetched, LGPL build, dynamically linked | LGPL-2.1+ | Not vendored |
| Valve `steam_api64.dll` | not in this repo | Steamworks SDK terms | Not vendored; copy from your Steam install |
| Odin compiler (for the translator and launcher) | fetched | BSD-3-Clause | Not vendored |
