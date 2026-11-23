# Credits

## Authors

1. **ps4star** (https://github.com/ps4star): wrote the XRC translator (https://github.com/ps4star/XRC) and the original runtime and
   launcher this project builds on, first for his Final Fantasy XIII port WhiteRecomp (https://github.com/ps4star/WhiteRecomp). The window,
   widgets and `iv_*` UI libraries in `launcher/`, and the HLE layer, Vulkan renderer and recompiled-code runtime in `src/runtime/`, start
   from his work. Thank you.
2. **finalpantasy** (https://github.com/finalpantasy): the Lightning Returns port work in this repository: SMAA and the post-processing
   passes, the shadow fixes, the frame-pacing fix, the in-game F10 menu options, the native math overrides, loading speedups, the typed
   guest structs and the function naming, and the tools in `tools/`.

## Third-party code and data

SMAA (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez), stb_truetype (Sean Barrett), FXAA (Timothy
Lottes, NVIDIA), FidelityFX Contrast Adaptive Sharpening (AMD), the Odin language and its vendor libraries (Ginger Bill and contributors),
and the fonts Liberation Sans (Red Hat / Google), Noto Sans (The Noto Project Authors) and Orbitron (Matt McInerney). Licenses and where each
one is used: [THIRD_PARTY.md](THIRD_PARTY.md).

Lightning Returns: Final Fantasy XIII is a product of Square Enix. This project is not affiliated with or endorsed by Square Enix.
