# Render submission path, frame timing, file decode, math (static, 2026-11-22, names3)

Static read only. Sources: RTTI (type descriptors, vtables found by scanning for COL pointers), the capstone disassembly of
`ffxiiiimg.exe.unpacked.exe` (the `tools/dis.sh` output drops some lines, e.g. the batch-pointer fetch in a41570, so use raw bytes for
details), string xrefs and callers. Everything marked "unverified" has not been observed at run time. Names went to
`tools/names/manual.tsv` (4th column = evidence; build_names.py ignores extra columns).

## Object graph (who owns what)

```
FrameworkDrawManager (FDM)   RTTI vtable 0218b688 (dtor only), ctor ac6410 / dtor ac7770
  +0x08  provider object (vf+0x1c used by ac5ee0/ac6040)
  +0x0c  X                    -> the big draw-manager object below
X  (built by a32460; allocator tag "CDev.Dw.Renderer.DrMgr"; no vtable store seen)
  +0x600c  layer* array       +0x6010 layer count
  +0x6020  GraphicsManager (GM)   <- 0x5107a00 points here (a30d80 stores `this` at a31722)
  +0x6c10 sub (a48420)  +0x6c4c (a4a8b0)  +0x6c84 (a36b80)  +0x6d10 (a50e70)  +0x6d7c (a3cac0)  +0x6e24 (a319c0)
  global 0x5107a04 = X, global 0x5107a00 = &X.GM
```
Evidence: a32460 does `lea ecx,[esi+0x6020]; call a30d80` and `mov [0x5107a04], esi`; a32a00 (this = X) works on `this+0x6040/0x6100/0x6180/0x6204`
= GM+0x20/0xe0/0x160/0x1e4, and calls GM methods with ecx = this+0x6020. ac6040 (FDM method) calls a32a00 with ecx = [FDM+0xc].
So the earlier name FrameworkDrawManager::DrawViews for a32a00 was the wrong class (fixed to Dw::Renderer::DrawManager).
GM offsets used by the draw code are therefore also X+0x6020+off (e.g. frustum at X+0x6204).

## Draw chain (runtime stack from shadows.md checked against the code)

```
DrawLayerManager::vf2 aceb60  UpdateLayerVisibility(mask, outMask)       RTTI vtable 00df72a8 slot 2
   -> sets layer[idx]+0x170 (visible byte) on X, ORs bits into *outMask, calls X::a320d0 (CountVisibleLayerNodes) if any
ac6040 FDM::DrawMainScene -> a32a00 X::DrawViews: copies camera/proj/frustum matrices, SetRenderState block via device vtbl+0xe4,
   loops views ([X+0x63b4] count, +0x63bc array) and calls SetCullFrustum a31990 (copies a frustum into GM+0x1e4 via 5a58c0)
a31bc0 X::BuildLayerViewLists -> a44cf0 AabbTree::BuildViewList (per visible layer, layer+0x34) -> gather callback [tree+0x100]
a6c5b0 Culling::GatherVisibleRecursive (recursive AABB tree walk, uses AabbViewTest a6ab50, pushes node pointers)
DrawableObject::vf8 a38550 DrawAllNodes   (RTTI vtable 02110ac4 slot 8; slot 7 a384e0 and slot 9 a385c0 are the same loop calling node vf+0x38 / +0x40)
   sets GM+0x5d0 = [obj+0xb0]+8 (or 0), GM+0x35c = [obj+0xb4]; loops [obj+0x178] x [obj+0x17c], stride 0xd0, calls [node.vtbl+0x3c]
ModelInstanceNode::vf15 a41570 DrawVisibleBatches                         RTTI vtable 02152434 slot 15 (slots 13/14/16/17 are sibling passes)
   layer test ([GM+0x2f4] & [node+8]) == [GM+0x2f8]; for each MeshInfo* in [node+0x84] (count [node+0x8c]):
   skip if [m+0x28] != 0; call a412b0 (frustum test on AABB m+0x50..m+0x6c; ret 1 = culled; [m+0x22]==0 = never cull);
   if not culled and [m+0x20] and [m+0xd1]: MeshInfo::DrawBatchA a46c20 (this = m)
a46c20 MeshInfo::DrawBatchA: if [GM+0x2e8] != [m+4] it resets pass state (memset 0x80 at 0x510c010, VS const regs c16..c18 <- GM+0xa0 x3
   via device vtbl+0x178, texture stage 15 (shadow map) and its sampler states, 75e1d0/75e2b0 sampler helpers), then
   GM+0x2e4 = m, sets world matrix constants (a45890), material (m+0x94), calls IndexedBatch::Draw a6d130 with ecx=[m+0x24], arg=m+0xac,
   increments GM+0x940, stores GM+0x2e8 = [m+4].
a6d130 IndexedBatch::Draw(arg): binds vertex streams (this+0x90 sub-object, a8f1c0), SetIndices (device vtbl+0x1a0 = slot 104) with
   [this+0xc8]->+0x18+idx*4 where idx=[0x510e95c], then DrawIndexedPrimitiveByPrimType a96750 (call at a6d1bb, verified) with prim type 4/5 from [this+0xb4].
```
Not on the direct path: a67080 (existing name ModelInstanceNode::CullAndBuildDrawList) is called from slot 13 a41410 with ecx = [m+0x28], so
its `this` is a mesh-owned sub-object, not a ModelInstanceNode. a46c20 / a47100 / a412b0 are MeshInfo methods (ecx = the batch pointer);
the manual names still say ModelInstanceNode::DrawBatchA/B (left, only a412b0 was fixed). shadows.md lists a6c5b0 between a46c20 and a41570 in
the stack: a41570 calls a46c20 directly (a41662), so that frame is from a different path or a misattributed return address.

## Struct layouts (offsets verified in code; "?" = purpose not proven)

GraphicsManager GM (at [0x5107a00] = X+0x6020; ctor a30d80, writes at a30d80 initialise +0x2e4/+0x2e8/+0x2f4/+0x2f8/+0x35c/+0x5d0/+0x694/+0x698/+0xbd8)
- +0x020 camera/view matrix (shadows.md; X+0x6040)
- +0x0a0 3 x vec4 uploaded to VS constants c16..c18 (a46c20: SetVertexShaderConstantF(dev,0x10,&GM+0xa0,3), vtbl+0x178)
- +0x0e0 projection matrix (X+0x6100); +0x160 matrix (X+0x6180) passed to AabbTree::BuildViewList as the view matrix
- +0x1d0 vec4 written by a32a00 from view width/height (X+0x61f0, reciprocals via fild/fdivr) ?
- +0x1e4 cull frustum (SetCullFrustum a31990 -> 5a58c0 copy; read by a412b0)
- +0x2e4 current MeshInfo* (a46c20 writes; a30aa0 / a30ac0 read fields of it); +0x2e8 key of the last drawn batch ([m+0x4])
- +0x2f0 ? (a46fe2); +0x2f4 layer mask, +0x2f8 required value (ac5ee0/ac5f50 write 0x90 / 0x80; a41570/a41410/a41470 test it)
- +0x35c = DrawableObject+0xb4; +0x5d0 = DrawableObject+0xb0 + 8 (or 0 when null)
- +0x3ac, +0x3ec matrices; +0x42c..0x438 shadow near/far/blend/far-scale; +0x68f shadows-enabled byte (shadows.md)
- +0x694, +0x698 ints (setters a30a60 / a30a80, only write when changed)
- +0x6ec index, +0x6f0 bool ([material+0xc6] != 0) (a46dc1-a46dcf), +0x43c/+0x444/+0x4b4 float tables indexed by +0x6ec
- +0x940 draw-call counter (incremented once per DrawBatchA)
- +0xbd8 flag choosing [0x5107a9c]+0x2c vs +0x34 (render-target set A/B) in a46c20

DrawableObject (RTTI vtable 02110ac4, 17 slots; slot 0 dtor a3a410, 7/8/9 DrawAllNodes variants)
- +0x0b0 ptr (+8 -> GM+0x5d0), +0x0b4 dword (-> GM+0x35c), +0x178 ModelInstanceNode array ptr, +0x17c count; element stride 0xd0 (= sizeof(ModelInstanceNode)).
  Allocation sizes: string "create model(Create DrawableObject) took %d[us]" in acc7f0.

ModelInstanceNode (RTTI vtable 02152434, 20 slots, ctor writes at a420e6/a4293b; allocator tag "CDev.Dw.Renderer.ModIns")
- +0x08 layer mask (tested against GM masks)
- +0x84 MeshInfo** array, +0x8c count (loop `mov esi,[edx+edi*4]`, a415b3-a415b9)
- vtable: slot 13 a41410, 14 a41470, 15 a41570 (main), 16 a41680 (DrawBatchB a47100), 17 a417a0 (DrawBatchC a47be0)

MeshInfo = the "batch" (RTTI `MeshInfo@ModelInstanceNode`, vtable 021523d4; vtbl+4 is called by slot 14)
- +0x04 pass/key id (compared to GM+0x2e8), +0x20 byte enabled, +0x22 byte (0 = never culled), +0x24 ptr to the IndexedBatch (geometry)
- +0x28 ptr, non-null means a sub-draw path (a65b60 / a67080) and the direct loop skips it
- +0x50..0x5c AABB min, +0x60..0x6c AABB max (vec4 each, read by a41570 and fed to a412b0)
- +0x90 mesh object (+0x24 -> +0x44 read by a30aa0), +0x94 material (+0x20 float, +0xc6/+0xc9 bytes, +0x78 read by a30ac0), +0x98, +0xa8 ?
- +0xac per-draw parameter block passed to IndexedBatch::Draw; +0xd0, +0xd1 bytes (+0xd1 = visible/active, tested with +0x20)

IndexedBatch (a6d130 `this`)
- +0x90 vertex stream sub-object (bound by a8f1c0), +0xb0 word (vertex count related: passed as count-1), +0xb4 byte (0 -> primitive type 4, nonzero -> 5),
  +0xc8 ptr to index-buffer set: +0x0c byte size, +0x10 index format (0: 16 bit, 1: 32 bit, divisor 2/4), +0x18 + frame*4 = IB per frame (frame = [0x510e95c])

AabbTree (class of layer+0x34; RTTI absent, allocator tag "CDev.Dw.Renderer.AabbTree" in a431a0/a43710/a44b10)
- +0x100 gather callback, +0x104 12-byte entries (qsorted by a42cf0), +0x114 count, +0x118 count2, +0x120 dirty byte, +0x124 vector<void*> result
  (begin +0, end +4, cap +8) filled by the callback; reserve a44990 / resize a44c50

Layer object (X+0x600c[i]): +0x20/+0x24 begin/end of a pointer vector, +0x34 AabbTree, +0x170 visible byte, +0x171 byte (view-list path enabled)

D3D wrapper globals: 0x510e944 = device object (vtable offsets are IDirect3DDevice9 slots x4: +0xe4 SetRenderState, +0x104 SetTexture,
+0x114 SetSamplerState, +0x178 SetVertexShaderConstantF, +0x1a0 SetIndices, Present +0x44), 0x510e95c = current frame index for per-frame
buffers, 0x510e960 = RenderInterface core (+0x74c query object, +0x158 byte), 0x510c010 = 0x80-byte scratch cleared per key change.

## Frame timing / main loop additions
- d12da0 is the application main loop (calls Steam::Init a04060, Framework::TickFrame ab93f0 at d130a0, Steam::Shutdown a04250); d12910
  (WndProc) also calls TickFrame (d12962).
- TickFrame order (ab93f0): Task::Debug a03170 / a033e0 on [0x5107370], stubs ab7790/ab77b0/ab77d0, ac2fe0 SetTargetFps on [0x511555c]
  (FrameClock), abcae0, ac3a40 / ac3a90 around ac3040 WaitForFrame, ac33b0 GetQuantizedDelta, ab9020 (FrameJobScheduler::Run a01a00).
- ac3040 FrameClock::WaitForFrame: GetTickCount-style QPC read (IAT 0xde9094, stored 0x24c3ad8), a96110, a96f20 Present (D3d9::Present,
  Present vtbl+0x44), a96090 GPU query wait, then FrameLimiter::WaitQpcSleep a96590 (twice). Globals 0x5115560-0x5115578 are the clock state.
- AppGame::FrameStep 4317c0 -> WaitFrame 42e2c0 (existing names) is the game-layer step; its only direct caller is 431e30.

## File loading
- FileController ctor is 74beb0 (the auto name FileControllerMethodDebugUSBFileSystem::ctor_dtor was wrong): it builds the methods
  FileSystem 020aa984, DebugFileSystem 020aa9dc, PatchFileSystem 020aaa34, DebugUSBFileSystem 020aaa8c, Image (74af90, vtable 020aaaec), and starts the
  IoThread (749990, thread name "FileRead", entry pushed at 74c2a4).
- FileControllerMethodImage ctor 74af90 creates thread "ZDecode" (entry DecodeThread 74ac50, push at 74b132). Global 0x24dab64 = the Image method
  instance, 0x24dab60 = file buffer allocator (both loaded at the top of DecodeThread).
- DecodeThread 74ac50: z_stream in esi (zalloc +0x20 = 749690, zfree +0x24 = 7496e0, next_in +0, avail_in +4, next_out +0xc, avail_out +0x10 = 0x10000
  chunks); waits [this+0x102a0] semaphore, pops jobs (74a6f0 under mutex [+0x1029c]), inflateInit_ c762a0 ("1.1.4"), loops inflate c762c0 (1 = stream end,
  -2/-3 = error, sets job error flag +0x18), inflateEnd c76150, frees buffers, sets [this+0x102ac] running flag.
- Image method fields seen: +0x10188 queue, +0x10190/+0x10194 queue head/tail, +0x1029c mutex, +0x102a0 semaphore, +0x102a4 signal object, +0x102a8,
  +0x102ac running flag, +0x102b0..+0x102bc state.
- vtable 020aaaec (Image) slots: 5 (74bac0) and 6 (74b630) both use fread 7498a0 under the mutex (read paths); slot 3 (749740) builds the weiss_data/sys path.

## Math (names in manual.tsv)
Vector4 Add/Sub/Cross/Dot3/Scale/Normalize, Matrix4x4 rotation builders (Y a 402ac0, Z 40b110, axis-angle 40ad60) and the packed-color converter 40abc0.
The range 0x409xxx-0x40a2d0 is NOT math: it is `Util::Scallion` (debug UI toolkit; RTTI ClickEvent/ActionEvent, 'vector<T> too long', 'CDev.Util.Scallion'),
and 4081d0/408190 are the ActionEvent / ClickEvent ctors. The old auto names `white::math::Matrix4x4::fn?` for 40ad60 / 40b110 came from a range guess.

## Not done / open
- Run-time confirmation of every layout (no game run).
- names4 (2026-11-22): 4086e0 = Matrix4x4::InverseAffine; 416340 / 4167e0 = Matrix4x4::FromEuler(_Aligned) (SinCos4 + xyz mask, writes 4 rows);
  416c30 / 416e70 = Matrix4x4::ToEuler(_Aligned) (3x atan2). b90790 = RaycastDetector::RaycastTriangleMesh with helpers ad4590 TriangleNormal, b09b50 TransformTriangle, b8d640 TransformRay.
- FrameClock field layout decoded in analysis/recon/frame_clock.md (2026-11-22).
