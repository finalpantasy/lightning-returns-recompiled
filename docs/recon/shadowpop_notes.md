# shadowpop: half-screen shadow block that pops (static analysis, 2026-11-22)

Sources: analysis/recon/shadows.md, render_path.md, src/runtime/vk_d3d9.c, earlier run logs in the Claude scratchpad sh2\* (run.err files).
Nothing was run. See the section appended to shadows.md for the ranked causes and the test plan.

Data facts used
- One 2048x2048 R32F map is sampled (s1) by PS 38b59999 for BOTH cascades; the resolve picks Near/Far matrices by $s_viewBound (0,10,80,80). a525c0 only sets states/shaders/constants (device vtbl offsets 0xe4 SetRenderState, 0x114 SetSamplerState, 0x104 SetTexture, 0x170/0x1ac shaders, 0x178 VS constants; no SetRenderTarget/Clear/SetViewport), so the map render is elsewhere (the DrawViews layer loop).
- A second R32F RT, 2048x4096, is created in every run and made R16F by XRC_VK_DIST16 ("drawn with no depth buffer" test). It received no draws in the 60-frame census (H4). Purpose unknown.
- GraphicsManager shadow fields (+0x42c 10, +0x430 80, +0x434 0, +0x438 0.99) were constant in slot 28 (daytime? unverified), but they come from keyframe curves (00b02260 <- 0097ac10), so the owner's night scene may differ.
- Casters that toggle in steady state (shdiff, runs B0 D2 F0 S0 B1 B2 E1 H2): small groups only (np 624/246/92 skinned NPC, np 662, np 261). Large static casters (vs bb5a8a04 np 800-1932, vs 0c21c79e np 1176-2292, vs bc4e62e1 np 1663/599) change at the SAME frame offsets in every run (about +1600, +1700, +2900 frames after gameplay starts), i.e. scripted/streamed scene changes, once each, not random flicker.
