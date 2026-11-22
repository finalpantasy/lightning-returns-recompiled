# LRFF13.exe subsystem map

Built 2026-11-20 from `analysis/names.tsv` (tools/names/build_names.py) plus reading the disassembly
(`tools/dis.sh <addr>`). Query helper: `python -I tools/names/q.py s REGEX | i IMPORT | f ADDR | c CLASSRX`.
Confidence: **high** = read the code or the runtime depends on it; **med** = RTTI/strings/imports agree;
**low** = one hint (address range from RTTI clustering, call position).

Address ranges are where most of a subsystem's code sits (10th..90th percentile of its RTTI methods). MSVC
folds identical functions (COMDAT), so tiny shared stubs (`65b030`, `65b3b0`, `cf19fc` = `_purecall`,
`c42c30`, `44bcd0`, `a410e0`) appear in hundreds of vtables: ignore them in stacks.

**Warning:** the addresses in `src/runtime/ovr_game.c`, `ovr_simd.c`, `ovr_bvh.c`, `ovr_crt_sort.c`,
`ovr_unlocked.c` and most `overrides.tsv` "fast"/"wrapped" rows are **FFXIII** addresses (not applied in
`XRC_LR` builds; 00e8d220 is not even inside LR's .text, which ends at 00de7f04). Do not reuse them for LR.

## Layout overview (med)

| range | what |
|---|---|
| 00401000-00434e50 | white::AppGame, boot (Framework::OnInitialize 00434670, AppGame::InitializeResources 00432200) |
| 00437000-008870e0 | white::gui (2100 methods); white::field 00423a70-00619560; white::battle 0050b6d0-005851e0 |
| 005c7810-0095a510 | white::db (datasheets), white::scene 006391d0-00710700, white::cinema |
| 00742000-0075e000 | white::io: input, file controllers, CRT-style lowio, save data, allocator |
| 00762000-0078b000 | debug code that shipped: white::DebugMenu, debug::LoadViewer, debug::terminal |
| 008bef30-008cdde0 | white::PrototypeMenu (debug); white::sound 008d23c0-008f0390 |
| 0091c000-00a10000 | SQEX::CDev::Engine::Cut (cutscene scheduler, plugins), white::accclip |
| 00a02000-00a05000 | SQEX::Common: Thread, Timer, Steam wrapper |
| 00a39620-00aa6000 | SQEX::CDev::Engine::Dw: Renderer, RenderInterface::D3d9 |
| 00ab7000-00af1000 | SQEX::CDev::Engine::Fw: Framework, Storage, SceneObject |
| 00b06000-00b40000 | SQEX::CDev::Engine::Lay (Stella layout) |
| 00b58000-00bb0000 | Phieg physics: IK, rigid bodies, collision |
| 00ba9c60-00d5f620 | SQEX Sd sound (Layout, Driver), Vfx/Qix effects 00c23000-00cdb000 |
| 00c17000-00c20000 | Miles-derived sample converters and mixers (+ MSSMIXER section 00de8000) |
| 00c76000-00c7a000 | zlib 1.1.4 (inflate, inflate_blocks 00c78100, trees 00c79730 / 00c797d0) |
| 00cdd910-00cdf3f0 | Bink movie player (Framework::Movie::Detail) |
| 00cf1936-00cf2700 | import thunks (CRT math, memset/memcpy, XInput, D3D9/D3DX) |
| 00d12000-00d76000 | Phieg ClothDynamics, Framework window proc (00d12910) |

## Rendering submit (med)

- Dw::Renderer 00a39620-00a83e40 (284 RTTI methods); Dw::RenderInterface 00a934d0-00aa48a0.
- 00a99f60 RenderInterface::D3d9::CreateDevice: the only caller of Direct3DCreate9 (thunk 00cf2700). **med**
- D3d9 resource wrappers: TextureImp 00aa3930, SurfaceImp 00aa48f0, BufferImp 00a94020/00a94050,
  ShaderImp 00a953f0 (VS) / 00a95420 (PS); D3DX shader compile + Include handler 00aa5700..00aa5b60. **med**
- Vfx draws through its own RenderInterface::D3D9 (00cd5340.., CDevVfx::RenderInterface 00cd5590-00d733f0).
- Present / device calls are COM vtable calls through registers (no direct import), so name the caller from a
  runtime stack inside vk_d3d9's Present, not statically.
- 00acaf60 Framework::ReadGraphicsConfig: reads Configuration.ini keys (Graphics_Resolution, Presentation,
  Scaling, Shadowing, Lighting, Glare, DepthOfField, ColorCorrection, TextureFiltering, FrameRate) and also
  references the string "Debug_About". **med**

## Frame pacing and timers (med)

- SQEX::Common::Timer (vtable 0x0210adb4): Start 00a02f40 creates a thread (Thread::Create 00a02200:
  _beginthreadex + priority + ideal processor) running Timer::ThreadMain 00a02d90: timeBeginPeriod(min),
  timeSetEvent(period, 0, TimeProc 00a02d70, &sem, TIME_PERIODIC); loop { on_tick(arg); wait(sem) } until the
  stop flag (Stop 00a02d40). TimeProc only does ReleaseSemaphore(sem, 1). Reconstruction:
  `analysis/recon/timer_thread.c`. **high**
- Timer::Start callers: 007471c0 (white::io, beside the pad rescan 00747100) and 00a02fb0. **med**
- Main loop: AppGame::FrameStep 004317c0 -> AppGame::WaitFrame 0042e2c0 -> 00b55160 -> 00b54de0 (from the
  boot-stall notes: the main thread waits here on a semaphore). **low**
- Thread helpers: 00a02090 join (WaitForSingleObject + CloseHandle), 00a020c0 SleepEx, 00a020e0
  GetCurrentThreadId (588 callers), white::Semaphore::Wait 00726a50 (WaitForSingleObject INFINITE, used by
  every white:: worker), white thread entry 00725720. **high**
- QueryPerformanceCounter users sit in the loader (0049a310, 44 callers: a timing helper in Loader.cpp). **med**

## File I/O, archives, filelists (med)

- white::io::FileController (vtable 0x020aaae0: 0074aa10, 0074a3a0) dispatches to FileControllerMethod
  implementations (20 slots each): FileSystem / DebugFileSystem / PatchFileSystem / **DebugUSBFileSystem**
  (vtables 0x020aa984.., shared slots 007495f0, 007495b0, 0074a220, 0074ab40, 00749610..00749a60) and
  **FileControllerMethodImage** (0x020aaaec: the packed white_img archive reader; slots 00749890, 00749720,
  00749740, 0074b160, 0074bac0, 0074b630, 0074a740..0074a5d0). **med**
- 0074ac50 FileControllerMethodImage::DecodeThread: a worker that inflates compressed entries with zlib 1.1.4
  ("read texture stream decode error"). **med**
- 00749990 FileController::IoThread (worker waiting on a semaphore). **low**
- FileStream: 00747c50 ReadAll (read loop: exits only on read() < 0), 007478a0 GetFileSize, 007488f0
  LoadFile; the CRT-like chain 007498a0 -> 00751ea0 -> 007521d0 -> 0074c590 (plain ReadFile). Win32 opens:
  00756120 / 007561b0 / 00757d10 (CreateFileA), finds 00757f00 / 00759f30, _findfirst 0074c340. **med**
- Filelists: AppGame::InitializeResources 00432200 (2500 instructions) loads udp/filelist_patch_*,
  tgd/filelist_trial_*, sys/movielist*.bin, and mounts "dlc/" and "udp/" roots. Zones: 009edc50
  (zone/filelist_z%04d, db/script, db/ai .wdb), 007278d0 (zone/white_z%04d%s_img), ZoneManager 00727840-00728500
  with its AddFileListLoader / DeleteFileListLoader helpers. **med**

## DLC loader (med)

- Dlc::LoadFilelist 00426fa0: "dlc/%07d/filelist_p%07d%s_%s.%s.bin" and "dlc/%07d/white_p%07d%s_%s.%s.%s",
  read through 007488f0 FileStream::LoadFile. **high** (also the boot-stall spinner path)
- Its caller 0093ff60 is slot 0 of white::db::(anon)::GetDlcList (vtable 0x020f2258; a job object run on the
  worker thread 00725720 -> 004b68e0): it walks "img/dlc/" and "dlc/" and calls LoadFilelist per package.
  Neighbours 0093ef20-0093ff20 are its helpers. (The 73 "slots" after it are a separate function-pointer table
  0093efc0..0093f9e0 that the vtable walker ran into.) **med**
- white::VoiceDlcAllocator (vtable 0x00df70b4: 00426f50..) and btldlc/%s/%s/%s (00532dc0) for battle DLC. **med**
- GUI: white::gui::GuiDLCMenu, GuiDLCContentsList (0x020c3c54..0x020c468c, ~007e7f70-007fc280). **med**

## GUI menus (med)

- GuiMainConfigMenu (vtable 0x020ca61c) methods 00807320-0080e280, its list boxes 0080c9c0-0080e1a0
  (GuiVirtualListBox<GuiMainConfigListImpl>), HidListener, TwitterListener 0080e1e0-0080e920.
- Option widgets 00806a30-0080a8b0: GuiMainConfigOptionListNormal, ...ListBar, ...ConfigKeyAssign (key
  rebinding), ...ConfigPadImage, ...CreditRoll, ...OssCredit; GuiMainConfigSnsConfirm (Twitter/Facebook share).
- Neighbours: GuiMainScoreResultMenu 00800560-00802100, monster lists 00803260-008062e0, GuiMainEquipList
  0080f2f0-00811cf0. Save/load window: white::gui::GuiSaveLoadWin32 (vtable 0x02068044, GuiSaveLoadWin32.cpp
  0048c440 thumbnails). Title movie menu GuiTitleMovieViewMenu (0x020d38dc).
- Software keyboard: white::io::osk::TextInputWindow (0x020ab134). CodeInputMenu (0x00e015c4: code entry).

## Audio (med)

- SQEX::CDev::Engine::Sd: Driver 00be1170-00d5dc50, ISoundDriver/SoundDriver vtables 0x021f680c/0x021f6b4c
  (methods 00d5da40-00d5f620, "timerVsyncProcessor"); Layout 00ba9c60-00bd1460; RootSound::UpdateVSYNC
  00bef390. Output is XAudio2 (COM; "XAUDIO2_PROCESSOR"). **med**
- white::sound 008d23c0-008f0390 (BankService::createStreamBank 008ed450, AsyncRegistrant 008d0180, the
  "[BGMLayer]" field music logic), white::sound::java::Session (the sound scripting runs on the game's Java VM).
- Miles-style mixer code: CPU probe 00de8000 (MSSMIXER section), mixer picker 00c1f1b0, C converters
  00c17db0/00c17e80, MMX converters 00c17f60/00c180e0, SSE2 mixers 00c1e430/00c1e980, double-buffer read
  00c18810 (runtime overrides it). No "Miles"/AIL strings remain in the exe. **med**

## Input (high for pads)

- Pads: DirectInput8Create thunk 009fcc08 (callers 00744fd0, 00751060 keyboard, 00c6b3f0 mouse);
  EnumDevices callback 00745440 -> 00744e00 -> IsXInputDevice 00744820; XInput poll 00745650 (XInputGetState
  thunk 00cf267c); device-change counter 00744d50, rescan 00747100. **high**
- Keyboard: white::io::keyboard (Keyboard 0074f570.., KeyboardDevice 007506c0.., KeyboardDeviceNative
  00750f80-007516c0 with GetAsyncKeyState); HidPadKeyboardListener 00743c40. **med**
- Debug pad: white::debug::CheatPadWindow (0x00df3adc), SceneCameraControlByDebugPad (free camera). **med**

## Save / load (med)

- Storage back ends (vtables, 4 slots): StorageWindows 0x0218a9c0 (00ac9380 / 00ac9400 / 00ac94a0:
  CreateFileA + ReadFile) and StorageSteam 0x0218a9d4 (00ac9540 / 00ac9560 / 00ac9580: SteamRemoteStorage);
  StorageManager 00abb7c0-00abbe00 with a worker thread 00abbd60 (SetEvent). **med**
- white::main::SaveDataCreator (vtable 0x020ab7c4, 00754680-0075a800), AutoSaveDataCreator /
  DebugSaveDataCreator (0x00df7a94 / 0x00df77a4), Gf::SaveDataSerializer; quick save in ZoneInterface
  0072b100 ("hasQuickSaveData"). **med**
- Steam wrapper 00a03700-00a04250: Init 00a04060 (SteamAPI_Init + RestartAppIfNecessary), RemoteStorage
  calls 00a037e0-00a03ae0, achievements 00a03c50, Shutdown 00a04250. **high**

## Memory (high)

- operator new 0075dbc0 ("White.Global"), new "White.Local" 0075db80, tagged new 0075dc10, operator delete
  0075dc40 (heap pointer at block-4), MemoryHeap::Alloc 00b454a0 / Free 00b453c0, global heap [0x05111080].
  Reconstruction: `analysis/recon/operator_new_delete.c`.

## Debug features left in the exe (leads for settings)

- white::DebugMenu (00762c70-00766b20) with 15 debug_menu_* pages (battle, field, sound, cdev_lay...),
  white::PrototypeMenu (008bef30-008cdde0), debug::LoadViewer (memory/thread/load views, 0077ca70-0078a4d0),
  debug::terminal::Terminal with a keyboard input distributor (00770b70..), CheatPadWindow, a free camera,
  DebugFileSystem / DebugUSBFileSystem file back ends, DebugSaveDataCreator. None was checked for reachability.
- A Java VM: "White.Java" heap, "javacode [0x%08x] size %d", loadBattleResidentJavaClassFiles, sound and battle
  scripts as Java classes.
- SNS: Twitter / Facebook sharing in the config menu ("[DEBUG]FB TOKEN=%s", "[DEBUG]TwitterCode=%s").

## Hot helpers (named 2026-11-20, static read; med unless noted)

Most-called functions that had no name. Names are in tools/names/manual.tsv; reconstructions in analysis/recon/hot_*.c.

- CRT (high): 00cf193c `__security_check_cookie` (10190 callers), 00cf181a `__ehvec_dtor`, 00cf195e `__ehvec_ctor`.
- Locking (high): white::Mutex Lock 00726e70 / Unlock 00726ea0 over WaitForSingleObject / ReleaseMutex wrappers
  007268b0 / 007268d0; GuiManager::Lock 007a2690 / Unlock 007a26a0 and EffectManager::Unlock 0096fd70 are thin wrappers on it.
- Math: Matrix4x4 SetIdentity 00402660, Assign 0040ac50, TransformVector4 00408210, Multiply 0040acb0; Vector4 Normalize 0040ab00 (SSE rsqrt).
- Logging: white::debug::Printf 0076a570 ("%X:%X> " prefix) and PrintfWithFile 0076a260 (file-tagged trace, 817 callers);
  Vfx Io::Printer GetInstance 00c22480 / Printf 00c225e0.
- Strings: StrLen 00435730, StrCopyN 004356e0, FixedString16 ctor 004012a0, FormatToBuffer 00435830 (own printf, 418 callers),
  std::string::_Tidy 00a5e960, std::vector::_Tidy 00431df0.
- Memory: GuiObject Alloc 007b6b70 / Free 007b6a70 (operator new/delete thunks 00792db0 / 0084ce30), Vfx Memory Alloc 00c228e0 / Free 00c22760.
- Handles: Scene::GetObjectByHandle 0065d970 (-1 invalid, -2 player, 256 slots with generation check).

## Hot functions 2 (prof_P1, 60 s gameplay, 2026-11-21)
Names in tools/names/manual.tsv, reconstructions in analysis/recon/hot2_*.c. % = self samples of CPU samples.
- Math, SSE already, override as a group (guest wastes ~half its insns on movss/unpcklps gathers): 00408320 Matrix4x4::Inverse, 005a44e0 Transpose (1.1%), 0040b1e0 CopyTo (0.8%), 00437270 MakeTranslation, 00416140 Quaternion::Multiply, 00416250 FromEuler, 00416020 SinCos4, 004164f0 Quaternion::ToMatrix, 00416a30 JointExpressionController::GetRotationQuat (2.0%), 004bb0b0 Aabb::ExpandByPoint (1.5%), 004bb1c0 Aabb::TransformByMatrix (1.2% self, 2.8% incl).
- Scene graph: 00aac130 Transform::ComposeLocalToWorld (2.0% self, 10.8% incl), calls the math group above; best combined override.
- D3D9 wrappers (guest side of be_draw2): 00a96750 DrawIndexedPrimitiveByPrimType (5.6%, the cost is the host draw, not the wrapper), 00a98b80 DrawPrimitiveUPByPrimType, 0075fe80 ApplyStreamSourceTable (1.9%, re-binds every call: cache last state), 00a92240 FillStreamSourceTable, 0075e1d0 / 0075e2b0 sampler state setters.
- Effects: 0097a2d0 LerpColorTable16K (0.7%, 16384-iteration scalar-double byte blend, easy SIMD).
- Renderer (low-confidence names): 00a46c20 / 00a47100 ModelInstanceNode::DrawBatchA/B, 00a45890 MeshDrawTag::SetupWorldMatrixConstants, 00a479f0 DrawTagList::DispatchAll, 00a525c0 FrameworkDrawManager::ExecuteDrawPass (3336 insns).
- Other: 00794470 GuiActionObject::UpdateTransformAndLayout (1.3%), 00a2ada0 Dw::Animation::SamplePose (0.9%), 00a015b0 / 00a01a00 spin-lock list + frame job scheduler, 00d5fc80 event handshake (0.9%, mostly blocked).

## UI and frame timing (med-high; details in analysis/recon/ui_timing.md)

- The GUI is dt-driven, not frame-counted. Framework dt (int units, 1/300000 s) -> GuiInterface::Update_FromUnits 0079fa70 (divide by 300000) -> GuiManager::Update 007b0470 (float seconds) -> widget list update 007a5050 / 007b1710 loop (`dt*1000*guiSpeed` ms to each widget, vtable +0x128) -> GuiProduct::Update_DispatchByState 0084cf00 -> GuiActionObject::UpdateTree 00795660 -> AdvanceActionTimers 007931e0 (8 slots, `t += dt*speed`). **high**
- No fixed per-frame step found in GUI updates (menu lists, credits, minimap, clock, field action sign checked). 60 fps risks: FieldInterface::vf8 005b8540 truncates dt to int ms; dt == 0 frames above 59.94 fps. **med**
- 00b55160 / 00b54de0 (formerly FramePacer::MainWait) are the Phieg physics frame step, fed with variable dt from AppGame::WaitFrame; 1/30 and 1/59.94 time steps are only defaults (b54e40, ab7570, b828e0 uses 1/60). **med**
- Shadow cascades: no alternate-frame or parity scheduling found. The only per-frame toggle in the draw manager is a double-buffer swap `+0x1ac ^= 1` at 00a541ab (light-like entry list, not shadows). **med (negative result)**
