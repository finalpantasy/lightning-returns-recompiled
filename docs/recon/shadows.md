# Shadow cascades (Lightning Returns, opening area)

Static findings (2026-11-21, shadow build only; lr.exe and build\win\merge untouched).

## Where the numbers come from
- Global `0x5107a00` = the GraphicsManager singleton (ctor `00a30d80`, size > 0x930). Shadow fields:
  - `+0x42c` near cascade range (ctor default 10.0, from `[0xdf3e48]`). Runtime 10.
  - `+0x430` far cascade range (ctor default 25.0). Runtime 80.
  - `+0x434` shadow blend / strength t (ctor default 0). The shadow pass clamps it to min(t,1) and treats t <= a threshold as 0.
  - `+0x438` far scale (ctor default 0.99). `shadowFadeParam` = (+0x42c, +0x430 * +0x438) = (10, 79.2).
  - `+0x68f` byte: shadows enabled (`00b03780`). `+0x6dc..0x6e8` = shadowSplitRange (ctor 0, set at runtime to 10).
  - `+0x3ac` / `+0x3ec` caster draw-tag lists, `+0xe0` camera projection matrix, `+0x20` camera.
- The values are DATA, not constants. `00b02260` (EffectInterface::ApplyShadowConfig(struct*)) copies a block into the manager:
  `[blk+8]` -> `00b037f0(0,f)` (+0x42c), `[blk+0xc]` -> `00b037f0(1,f)` (+0x430), `[blk+0x10]` -> `00b03870` (+0x434),
  `[blk+0x14]` -> `00b03890` (+0x438), more into +0x4b4[], +0x498[], +0x43c[].
  `00b02260` is called only from `0097ac10` (keyframe/curve evaluator, 7 curve channels at +0x3c, +0x78, ...),
  itself called from `0096d7a0` (EffectInterface update, time in ms, singleton `[0x50f93b8]`). So the near/far ranges and the
  strength follow scene/effect keyframes; the 10 / 80 are authored values for the opening area.
- Per frame the shadow pass `00ac6b00` (page 00ac) loads `+0x430`, `+0x42c`, `+0x434`, ... and calls `00a525c0`
  (ExecuteDrawPass, 3300 lines, builds the cascade boxes from the camera projection `+0xe0` and the ranges, runs the shadow-map
  draws and the screen shadow). Args: ranges `10, 10, 10*t, 80, 80`, lists +0x3ac/+0x3ec, camera +0x20/+0xe0.
  The near cascade is the camera slice 0..10 m fit with a box (~8 m across, scale ~0.24 per unit), the far one 0..79.2 m (~0.028).
  So the near-cascade edge IS the hard cutoff: everything the camera sees beyond +0x42c falls to the far cascade or none.
- PS constants (CTAB names, regs from the trace): `$s_viewShadowMatrixNear` regs 2-5, `Far` 6-9, `shadowMatrix0` 43-46,
  `shadowMatrix1` 47-50, `shadowFadeParam` (many regs), `shadowSplitRange`. Binders: ScreenShadowShaderDriver `00a85040` (resolve) /
  `00a84e70` (set fade = (+0x42c, +0x430*+0x438), split = +0x6dc), ShadowMapShaderDriver `00a853d0` / `00a85180`.

## Timer / 5 s toggle candidates (not proven)
- `+0x434` (blend t) multiplies the near range in the shadow pass; it is driven by the keyframe evaluator `0097ac10`, so a looping
  curve or a re-triggered effect would make the near shadow vanish and return. Names seen in the binary: weather steps
  `SHADOWMAP_RES_ADD/DEL`, draw tag `INTERVAL_SHADOW`, `DRAW_SHADOW`, `ShadowOnOffClip` (cutscene).
- No other timer was found in the fit path (`00ac6b00` has no tick compare; it only tests `[0x511558c]+0x2c/+0x2d` and
  `IsShadowEnabled`). Runtime trace (XRC_SHADOW_TRACE=1 prints `xrc: shadowgm ...` every 4th shadow pass) is the way to catch it.

## Override (shadow build only, default off)
`XRC_SHADOW_NEAR_MUL=<f>`: page 00ac is rebuilt from a patched copy (`build\win\shadow\patch_00ac.py` ->
`merge\math_src\page_00ac.c`): the load of `+0x42c` at `00ac6b9f` is multiplied by `xrc_shadow_near_mul()` (vk_d3d9.c). That scales
the range passed to `00a525c0` (it did NOT move the box in the test below). The same factor multiplies the pixel shader's shadowSplitRange and
shadowFadeParam.x in `shadow_patch`. Also `XRC_SHADOW_TRACE=1` logs the manager's shadow fields (`xrc_shadow_gm_trace`).
Removed the dead "Shadow resolution" menu row (the exe never reads -Shadow=).

## Runtime result (slot 28 = newest save, standing still, 1280x720 headless, run dirs in the session scratchpad sh2\A2 and sh2\B3)
- Slot 28 opens on an area-intro text page; it needs pad A (p0010) taps at ~72-96 s, gameplay shadow passes start about 100 s.
- GraphicsManager shadow fields stay constant for the whole run (146 samples, ~117 s): +0x42c=10, +0x430=80, +0x434=0, +0x438=0.99,
  +0x43c/+0x440=20000, +0x4a0=0.7, +0x4a8=1, +0x4ac=1, +0x4b0=20, +0x4bc=0.5. So the near/far ranges are not what blinks.
- shadowMatrix0Column0 alternates between two values 4 samples apart (-0.240121 / -0.240127): jitter, not a toggle.
- XRC_SHADOW_NEAR_MUL=3 ran ("xrc: shadow near mul 3") but shadowMatrix0/1 and $s_viewShadowMatrixNear came out IDENTICAL to the baseline,
  so the near box is NOT derived from +0x42c in 00a525c0's args. The override only changes the PS split/fade constants (cascade pick),
  not the box extent. The box fit is still unfound (not in 00ac6b00's arg block; look at 00a524f0 / 00a51240 / 00ac5ab0 and at what
  writes shadowMatrix0 regs 43-46 before the draw).
- Screenshots (every 2 s, 150-172 s) show long hard-edged ground shadow bars (fence/prop shadows) that appear and vanish between
  frames at the owner's spot, the same with NEAR_MUL=3.

## Render-target persistence and INTERVAL_SHADOW (checked 2026-11-21, run sh2\C1)
- vk_d3d9.c begins every render with loadOp LOAD / storeOp STORE for colour and depth (about line 2724 and 2741). CLEAR only comes from
  a game Clear (deferred pcl list, folded into the next render) or a few internal passes. A target the game does not clear keeps its
  contents. No per-frame recreation, aliasing or ring of render targets was found. Nothing to fix there.
- INTERVAL_SHADOW is a frame-phase marker name (table at 0x22b4f20, next to INTERVAL_FRAMEWORK, INTERVAL_VFX_SETTLE ...): a label for a
  section of the main-thread frame, not a redraw period.
- New trace line `xrc: shmapdraws f<frame> n=<draws> size=<px>` (XRC_SHADOW_TRACE=1): once gameplay runs, the 2048 shadow map is drawn
  EVERY frame (about 800 consecutive frames, no zero frames). The draw count steps between 170 and 181 (e.g. 174, 181 for 6 frames,
  174, then 177, 178, then down to 170), so the caster set changes over time.
- The same run shows "423 draws skipped (waiting for their pipeline)" with a fresh pipeline cache: a caster whose pipeline is still being
  built is skipped, which looks like casters (and their shadows) popping in. It grew only 423 -> 425 late in the run.
- Open: what the 170 <-> 181 steps are (caster culling / LOD), and whether the owner's blink matches them (needs frame-aligned shots).

## Per-frame shadow-caster diff (run sh2\D2, ~27 fps, trace level 1; XRC_SHADOW_TRACE=2 adds the old shtrace constant lines)
- `shdiff` lines: draws into the 2048 shadow map keyed by (VS hash, PS hash, np, nv) that appeared (+) / vanished (-) against the
  previous frame. The VB handle is NOT stable (skinned meshes alternate handles every frame), so it is printed but not in the key.
  Shot lines now carry the frame number ("... at frame N").
- Gameplay at the owner's spot, frames 8260-8432 (about 6 s): the draw count goes 171 -> 179 -> 172. One group of 3 skinned meshes
  (np/nv 624/396, 246/177, 92/70, VS 38, i.e. one character) appears at f8296 with the plain depth PS (244-byte ps_34), switches to the
  alpha-tested PS (596-byte ps_36: texld + cmp) at f8390, back at f8393, vanishes at f8405 and again at f8432. Another mesh (np 662,
  nv 548, VS 44, ps_44) appears f8260 / f8302 and vanishes f8418. So casters enter and leave the shadow pass on a few-second scale: this
  is a per-object cull or LOD in guest code, not a render-target loss.
- Not yet traced into guest code: what decides a caster joins the shadow list. Lead: the shadow pass lists +0x3ac / +0x3ec in
  00ac6b00, filled by the draw-tag (DRAW_SHADOW) submit; look for a bounding-sphere test against the shadow frustum or a distance
  fade. The near-cascade matrix box is also still unfound (NEAR_MUL did not change it).

## Caster culling found statically (2026-11-21, run sh2\E2 stack traces; NOT yet A/B-verified in game)
- XRC_SHADOW_TRACE=3 prints `shstack` (guest return addresses above the shadow draw). Chain for the 624/246 skinned meshes:
  DrawIndexedPrimitive <- IndexedBatch::Draw a6d130 <- ModelInstanceNode::DrawBatchA a46c20 <- a6c5b0 <- ModelInstanceNode::vf15 a41570
  <- DrawableObject::vf8 a38550 (loops nodes, stride 0xd0) <- h_a32a00 (FrameworkDrawManager draw-views loop) <- DrawLayerManager::vf2 aceb60.
- +0x3ac/+0x3ec of the GraphicsManager are 64-byte MATRICES (0x40 apart), passed to 00a525c0 with camera +0x20 / proj +0xe0: not caster lists.
  00a525c0 has no min/max or caster compares; the cascade box is analytic (8 frustum corners, sin/cos/pow via x87 CRT, rcpps/divps), so no
  caster-driven fit. Also uses rcpps (translation: exact 1/x).
- The cull is per batch in a41570: pass only if ([mgr+0x2f4] & [node+8]) == [mgr+0x2f8], then for each batch (if [batch+0x28]==0) the batch
  AABB (+0x50 / +0x60) goes to 00a412b0 (ret 1 = culled; [batch+0x22]==0 means never culled) which calls 005a4ad0 against the frustum
  stored at GraphicsManager+0x1e4. That frustum is set only by SetCullFrustum 00a31990, called from a32a00 (00a32ea6, 00a331d5), b01180,
  b03a00. 00a525c0 / 00ac6b00 never set it, so shadow-map casters are tested against whatever frustum was set last (the camera's view of the
  frame), not the light's. Casters outside the camera view or behind its near plane drop out of the shadow map: matches both the blink
  (skinned group at the screen edge enters/leaves) and a straight cutoff near the camera. Unverified: the frustum values (shcull trace line
  in E4 never ran, machine too slow / run stopped for the owner).
- ovr_math: not on this path (only 005a4ad0 is not overridden; XRC_OVR_MATH=0 A/B not run).
- Fix (default off): XRC_SHADOW_CULL_FIX=1 in the shadow build. patch_00a4.py patches a copy of page_00a4 so 00a412b0 returns "not culled"
  while render target 0 is a square R32F >= 512 (xrc_shadow_nocull in vk_d3d9.c). Built (merge\pages\page_00a4.o), linked NOT yet; needs
  `sr.ps1 -link` then a run with XRC_SHADOW_CULL_FIX=1 vs 0 (taps in sh2\taps.txt, ~170-250 s, shots 150+).
- XRC_SHADOW_TRACE=3 also logs `shcull` (cull frustum +0x1e4, camera, proj matrices) every 8th frame.
- Build script fixes: build_shadow.sh called build\win\build_merged.sh rt_one (wrote build\win\merge\rt_*.o; fixed, but one run of the old
  call rebuilt rt_hle_d3d9.o etc. there from current src, harmless) and lacked the newer ovr_math pages (0043 004b 005a 00aa); both fixed.

## A/B XRC_SHADOW_CULL_FIX (runs sh2\F0 = off, sh2\F1 = on, 280 s, TRACE=3, shots 192-228)
- Shadow-map draws per frame: off 170-181 (mean 174.4), on 288-300 (mean 294.7). So the camera-frustum cull removes ~120 batches per frame from the shadow pass.
- shdiff events: off 34 (np=624 group 19 +/- lines), on 25 (np=624: 11). The skinned group still comes and goes with the fix on: a second gate exists for it
  (layer mask test in 00a41570, or a different cull in 00a6c5b0 / AabbViewTest a6ab50). Not yet found.
- Screenshots shot_210 off vs on: the straight hard dark bar on the ground at the left is identical in both. The cutoff is not caused by the batch cull.
  Next suspect: near cascade box / split in 00a525c0.
- shcull at f4992 (fix on): the +0x1e4 "cull" matrix is near zero apart from a few entries (not the camera VP; the object is not a plain 4x4).

## Shadow4 findings (2026-11-21, runs sh2\G0b G1 G2 G3 G4 G5; slot 28)
- 00a41570 gates (XRC_SHADOW_GATE_TRACE=1): layer mask, [b+0x20], [b+0xd1], [b+0x28] never change after first sight. Not the blink gate.
  Added XRC_SHADOW_GATE_TRACE / XRC_SHADOW_GATE_FIX=<bits 1,2,4,8> (shadow build, default off); `shgap` log = a node not visited for >1 frame.
- Real second gate: the OBJECT-level gather. 00a44cf0 (per view, from a32a00) calls the visible-object gather 00a6c5b0, which tests each object's
  AABB against the CAMERA frustum with AabbViewTest 00a6ab50 (2 call sites: 00a6c5db, 00a6c6ab). Objects outside the camera view never reach
  a41570 (whole sets of ~8 consecutive nodes, stride 0xd0, vanished for 100-600 frames in G3 `shgap`, e.g. nodes a4467ba0.. f3166->f3798). The np=624 group is an NPC at the screen edge.
- Fix: XRC_SHADOW_OBJ_FIX=1 (page_00a6 patch `build\win\shadow\patch_00a6.py`, `xrc_shadow_objfix` in vk_d3d9.c): both tests report visible. Use with XRC_SHADOW_CULL_FIX=1.
  Result (G4, with trace): shgap = 0 (was 138 gap lines in G3). Shadow-map draws 592-600/frame (was 295). Skinned group np=624 events after f4000: G5 4855-4904 (one 21-frame drop), vs
  G2 11 events spread f3904-f4523. Improved, not zero: one short drop remains (not yet traced; maybe the other view's own list). Cost: twice the shadow draws.
- Bar: unchanged under CULL_FIX, SPLIT x0, FADE x3 (G1 sheet), and with NO_SHADOWS=1 (G0b: gameplay shots 200-280 show the player's shadow AND the bars with
  zero 2048-map draws). So the bar is not shadow-map content clipped by a cascade/cull; it is drawn some other way (baked ground shading or another shadow technique). Not solved.
- G4 (gate trace on) produced no shots (run slow/hung at ~f5576); G5 (no gate trace) ran to f7019 fine.

## Shadow4, part 2: the bar and the player's shadow come from the screen-shadow pass (2026-11-21, runs H1-H11)
- Render-target census (XRC_RT_TRACE=1, XRC_RT_TRACE_F0=8000, 60 gameplay frames, run H2/H4): 2048x2048 R32F = shadow map (~174 draws/frame, PS c5effada...);
  3840x2160 R32F = depth/G-buffer (skipping it blacks the scene, H3); 3840x2160 A8R8G8B8 = lit scene (16.8k draws/60 f); 1920x1080 A8R8G8B8 = light buffer
  (4 draws/frame: PS 38b59999 = the SCREEN SHADOW resolve (CTAB: $s_cameraDepthSampler, $s_directionSampler, $s_shadowDepthSampler, 8 PCF $s_shadowOffset0-7,
  $s_shadowRatio0/1, $s_viewBound, $s_viewShadowMatrixNear/FarColumn0-3), then de2c9513, d97db00e x2); 1920x1080 R32F; 1280x720..20x12 A16B16G16R16F chain = bloom/luminance.
  Disassembly of ps 38b59999: sample camera depth, rebuild view pos, project with Near and Far matrices, cmp-select against $s_viewBound, 8-tap PCF per cascade.
- Skipping only PS 38b59999 (XRC_PS_SKIP=38b59999, run H5): the bar AND the player's shadow are gone. Skipping the whole 1920x1080 RT (H6) darkens the scene. So the bar is in the screen shadow pass.
  Not the 2048 map draws: XRC_NO_SHADOWS only skipped map draws (the map kept stale content), which is why G0b still had shadows.
- That PS has NO shadowSplitRange/shadowFadeParam, which is why SPLIT_MUL/FADE_MUL/SCHED modes 2 and 4 never moved the bar. Its range constant is `$s_viewBound` =
  (0, 10, 80, 80) in the opening area (same numbers as GraphicsManager +0x42c/+0x430), printed by XRC_SHADOW_TRACE=1 as `shadow viewBound`.
- XRC_SHADOW_BOUND_MUL=<f>|<f0,f1,f2,f3> (default off) multiplies $s_viewBound in the PS constant block; XRC_SHADOW_SCHED bit 8 = x3, bit 16 = x0.3.
  A/B in one run (H10, SCHED, sheet H10_sheet.view.png, shots 127..167): mode 0 (135? no: 127, 143, 159) bar present; x3 (135, 167) bar gone, player shadow kept; x0.3 (151) bar present.
  X component-only variant 1,3,1,1 (run H11, H11_sheet.view.png) did not remove it (wide dark stripes still on the ground), so scale all four. Not a proven root cause: bar edge = near cascade
  end at 10 m; the proper fix is widening the near cascade box to match, not just the bound (the bound only chooses the cascade; the near map box is ~8 m).
- Other new switches (shadow build, default off): XRC_RT_TRACE / XRC_RT_TRACE_F0, XRC_RT_SKIP="WxH:fmt[,..]", XRC_PS_SKIP=<hex hash hi32,...>; XRC_VK_DUMP_SHADERS file names now carry _h<ps hash hi32>.

## Shadow5: the bar is not the cascade seam (2026-11-21, runs M0-M9, T0-T2; screen shadow PS 38b59999 disassembled)
- PS 38b59999 (dump shd2\ps_74_74_h38b59999.bin): view pos = depth * viewray (view z negative forward). `cmp` on -(z) - $s_viewBound.y picks the Far matrices when dist >= 10, else Near; a second `cmp` on -(z) - viewBound.w forces "lit" (1.0) past 80. 8-tap PCF, one shadow map sampler (s1) for both matrices. No blend at the seam.
- Near/far boxes (XRC_SHADOW_MTX_TRACE=1 prints c2-c9 `shmtx`): the near matrix maps the camera axis at d=10 m to v=0.03 (v reaches 0 at about 10.7 m), so the near box and the 10 m bound agree on axis. Not a "samples outside the near box" seam. (Corner rays at an assumed wide fov do leave the map.)
- A/B in one run (M1, sched modes + XRC_SHADOW_BOUND_ALT1/ALT2): bound y x0 (all far) and y x1000 (all near, w kept 80) BOTH keep the bar; x3 only SHORTENS it (the far end is cut where the near map ends). H10/H11's "x3 removes the bar" was a misread: the far part of the same shadow was clipped.
- Shadow map dump (XRC_SHADOW_MAPDUMP=<frame,..> or XRC_SHADOW_MAPDUMP_MODE=1, shmap_*.png in XRC_SHOT_DIR): the 2048 map holds a long, straight, very dark (nearest-to-light) bar in every frame, pointing a different way as the camera turns. So the ground bar is that caster's real shadow, drawn by the shadow-map pass, hard-edged because it is thin. Not shadow-resolve math.
- Caster not pinned: only 5 VS hashes draw into the map (bb5a8a04, 0c21c79e, bc4e62e1, 1fbafb4a, 5ee410f9; XRC_SHADOW_LIST=1). Skipping by hash class (XRC_SHADOW_SCHED bit 128), by np (XRC_SHADOW_SKIP_NP, bit 0x20000) and by VS (XRC_SHADOW_SKIP_VS, bit 0x2000000) gave inconsistent results because a mode change can land between the map pass and the resolve (dumps lag one mode) and the camera drifts between shots. Open: skip by VS in two separate fresh runs, or key the mode to a frame number, to name the draw.
- Verdict: no seam fix applies. XRC_SHADOW_BOUND_MUL=2/3 is not recommended (it shortens real shadows). New test switches in the shadow build, all default off: XRC_SHADOW_MTX_TRACE, XRC_SHADOW_BOUND_ALT1/ALT2 (+ sched bits 32/64), XRC_SHADOW_MAPDUMP(_MODE), XRC_SHADOW_LIST, XRC_SHADOW_SKIP_NP, XRC_SHADOW_SKIP_VS.
- Frame time (T0 vs T1, no trace, same spot, 100+ s steady): baseline 29.8 fps (33.5 ms), ~650 draws/frame; XRC_SHADOW_OBJ_FIX=1 + CULL_FIX=1: 19.8 fps (50.5 ms), ~1250 draws/frame; cost about +17 ms/frame (-34% fps), CPU main thread 21 -> 37 ms. (Run T2 was invalid: sr.ps1 leaks env between runs in one PowerShell session.)

## Shadow6: cheaper blink fix, XRC_SHADOW_HOLD (2026-11-21, runs sh2\S0-S3, D6-D8, E1-E4, B0-B3b, H0-H3, F1; slot 28, 260 s, TRACE=1)
- Result: `XRC_SHADOW_HOLD=900` (shadow build, default off). The object gather result (00a6c5b0, both AabbViewTest sites) is overridden to "visible" for any
  object that tested visible within the last N frames (per object pointer and site; table in vk_d3d9.c `xrc_shadow_hold`, page_00a6 patch). Blink events of the np=624
  group in steady state (>300 frames after gameplay start): baseline 12-22 per run, HOLD=900 1-2, OBJ_FIX+CULL_FIX 0-4. HOLD=300: 8 (too short, gaps are long).
  Shadow-map draws/frame: baseline 174-178, HOLD 248-251, OBJ+CULL fix 596-598. fps (steady, 12 samples/run, machine noisy +-10% between runs): baseline 22.8 / 20.7 / 20.7 / 16.3,
  HOLD 20.0 / 20.4 / 17.7, OBJ+CULL fix 13.2. Paired in time: HOLD 1-3.5% below baseline (inside run noise), the full fix 35% below. Screenshots (sheet sh2\HOLD_sheet.png, B2 | H2 | F1 at 240 s): no new artifacts.
- Why a uniform margin does not work (XRC_SHADOW_MARGIN=<units>, widens the gather AABB and the batch-cull half-extent; XRC_SHADOW_MSWEEP=1 cycles 0,3,6,12,25,50,1000 every 150 frames):
  the blinking NPC group needs ~50 units to be present all the time (np624 draws 3.2 at 0, 3.9 at 3, 4.0 at 6, 4.7 at 12, 5.3 at 25, 6.0 at 50), and shadow draws grow to 220/238/290/340/383 at those
  margins (594 = full fix). Most culled objects are far away (more than 50 units), the cull rate per frustum barely drops until 25-50. Margin 3 and 6 as plain runs: np624 events unchanged, fps -15%.
  Class-filtered margin (XRC_SHADOW_VT=02110ac4 = the model-instance child class, BMARGIN=0): np624 events 1-5, but still +90..110 draws; the batch cull needs no change (BMARGIN 50 no better).
- The gather has 4 frustum objects per frame (803835b0, 82f96544, 82fe2ba4, 83006524; child vtables 02110ac4 / 021bb5c4 / 021bb954 at [obj], flags [obj+0x94]). The AABB (min xyz +0, max xyz +0x10) of the
  parent tests at 00a6c5db is the node itself (this), at 00a6c6ab it is built from [child+0x24] x [child+0x64].
- Debug switches added: XRC_SHADOW_AABBLOG=1 (per-object visibility mask over the sweep, dumps at frames 5500 / 6500), XRC_SHADOW_BMARGIN, XRC_SHADOW_VT. Files: patch_00a6.py, patch_00a4.py (ext hook before the 005a4ad0 call in 00a412b0), vk_d3d9.c (backups .pre_shadow6).
- Not done: a light-direction-aware extension (needs the light vector in world space); not needed now.

## Shadowpop: owner's "half-screen shadow block pops in/out" (static only, 2026-11-22; notes in shadowpop\notes.md)
Reread of the data above plus vk_d3d9.c; no run was made. Answers:
1. Cascades: one 2048x2048 R32F map (draw census: ~174 draws/frame, all into it; no second pass or viewport split was ever logged). The resolve PS 38b59999 samples it (one sampler, s1) through two matrices, $s_viewShadowMatrixNear (c2-c5) and Far (c6-c9), chosen by $s_viewBound.y = 10; beyond viewBound.w = 80 it returns lit. Both matrices and the bound are set by 00a525c0 (ExecuteDrawPass), per frame, from the camera projection and GM +0x42c/+0x430 (keyframe-driven, constant 10/80 in slot 28). a525c0 itself issues no Clear/SetRenderTarget/SetViewport, so near/far boxes are analytic and the map render is a separate pass. Near and far matrices differ 9x in scale (0.28 vs 0.03 per m on x) yet read the same texture: how the near box lands in a map also holding the far view is NOT understood. An unexplained 2048x4096 R32F RT (R16F by DIST16) exists in every run and got no draws in the census: possible per-cascade atlas, check it.
2. Ranked causes (none proven):
   A. A large/long caster entering and leaving the far pass. The map always holds a long, very dark (near-light) bar/blade; its shadow is the "bar" of Shadow4/5. Culls (object gather a6c5b0, batch cull a412b0) only drop caster sets that are outside the camera frustum; large static casters did not flicker in any logged run, but the NPC/skinned groups did (every 100-600 frames, matching "random, no trigger"). Untested idea: a skinned group re-entering after being culled draws with stale bones and makes a huge triangle/blade. Sub-case: casters nearer the light than the map near plane are clipped (XRC_SHADOW_CLAMP, menu "Shadow near fix", OFF for the owner): a big canopy straddling that plane would pop its shadow. In case A the lit frame is the wrong one only if the caster belongs there.
   B. Shadow parameters at night change (GM +0x434 blend, +0x42c/+0x430, viewBound) via the keyframe evaluator 0097ac10/00b02260; slot-28 logs may be daytime, so the night values were never observed. Cheap to check: XRC_SHADOW_TRACE=1 prints shadow viewBound and shadowgm.
   C. Map/matrix desync (frames in flight, render thread): unlikely. Constants are captured per draw, commands are one queue in order, map and resolve are issued by the same guest pass; no indication of reuse across frames. Stale map on skipped passes: the map is drawn every frame (no zero frames in 800), loadOp LOAD but the game clears it (clear max = 1.0 in dumps).
   D. R32F precision/bias: the shadow map keeps R32F (it has a depth buffer, DIST16 skips it); only the 2048x4096 and 1920x1080 R32F targets are R16F. Not a fit for a pop of a whole 10-80 m range. Unlikely.
3. Data: toggling casters are small (np 624/246/92 skinned group, np 662, np 261). Big statics only change once at fixed offsets. No log shows a huge caster flickering, but no log is from the night scene. In M7/M8 map dumps some modes have max depth 0.566 instead of 1.0 (no clear texels left) = the map filled by a large surface; those are test skip modes, not natural.
4. Test plan (game must be free; same settings Shadowing=Advanced, no SHADOW_HOLD):
   - Owner now: Enhancements > Shadow stability On (XRC_SHADOW_HOLD=900). If the block goes away, cause A (object-gather culling). If it stays, not A. Also try Shadow near fix On: if gone, near-plane clipping. Each is a separate yes/no, change one at a time.
   - Run T1: shadow build, owner's night spot, XRC_SHADOW_TRACE=1, XRC_SHADOW_LIST=1, XRC_SHADOW_MAPDUMP=<8 frames>, shots on the same frames (several on/off frames). Compare map dumps: block frames with a big dark region or a different bar = caster (A); identical maps = resolve/params (B).
   - Run T2: XRC_SHADOW_TRACE=1 + XRC_SHADOW_MTX_TRACE=1 only; check if viewBound or c2-c9 change on block frames (B/C).
   - Run T3: XRC_SHADOW_OBJ_FIX=1 + CULL_FIX=1 (full fix, -34% fps): if the block still pops, it is not culling at all.
   - Run T4: XRC_RT_TRACE over a long window to see whether the 2048x4096 RT is drawn on block frames.
