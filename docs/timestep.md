# LR frame timestep and world clock (static analysis)

Source: ffxiiiimg.exe.unpacked.exe (VA = RVA + 0x400000), disassembly via tools/dis.sh and capstone. Confidence tags: high = read the code; med = inferred from call shape; low = guess.

## Answer

Partly. The game does not use a fixed 1/30 or 1/60 step and does not step the clock per frame. It measures real time (QPC), rounds it to whole 1/59.94 s ticks, and clamps each frame to 1/14.985 s = 66.7 ms (20020 units). The world clock adds that clamped delta every frame. So the clock is fps-independent from about 15 fps up to uncapped speeds, and it loses time only on frames longer than 66.7 ms (a sustained rate below ~15 fps, or hitches). A steady 22 fps (45 ms frames) does not slow the clock. A 22 fps average built from many 70-500 ms hitches would.

The "00e8d490" address is not code in this exe: 0xE8D490 is inside .rdata (.text ends at 0xDE7F04). src\runtime\ovr_game.c and its three entries in overrides.tsv (00e8d220, 00e8d560, 00e8d5a0) are left over from the FFXIII port; none of those addresses exist as code in LR. The LR equivalent of the "frame time rounded to 1/60 ticks" function is 00ac33b0.

## Units

All game dt values are int "units" of 1/300000 s (constant 300000.0 at 0xdf18e0). One 1/59.94 s tick = 5005 units. Frame = 59.94 Hz NTSC tick, not 60.

## 1. Delta time (high)

Objects: FrameClock singleton at [0x511555c] (ctor 00ac2f40), ApplicationConfiguration at [0x5115590] (ctor 00ac4bd0; embedded at Framework+8).

- 00ac3040 FrameClock wait: per frame, tick P = QPCfreq*1001/60000 (= QPC ticks per 1/59.94 s; 166833 at the runtime's 10 MHz). n = (int)(FC[0]/FC[4] + 0.5) ticks minimum. target = last + max(n*P, ceil((now-last)/P)*P). It waits (Sleep(0)/Sleep(1) loop, see 00a96590) until now >= target, then stores target in FC+0x30. That branch runs when 00a96110 is true (a once-cached capability flag at [0x510e964], from a function pointer at [0x510e960]+0xc). If false, 00ac3240 is used: last = real QPC, and it returns without waiting when the frame was shorter than P and n == 1. FC[0] is 0 in the ctor and I found no writer, so n = 0 and only the "round the frame up to whole ticks" part applies (med; needs a runtime dump to confirm).
- 00ac33b0 delta: reads FC+0x30, quantizes it down to a multiple of P, takes the difference to the previous quantized stamp (FC+0x38), converts to units and rounds to a multiple of 5005:
  `dt = ((delta*300000/freq + 0x9c6) / 0x138d) * 0x138d` (0x9c6 = 2502 = half a tick). Quantizing absolute stamps means no drift: the sum of dt tracks real time. Faster than 59.94 fps gives dt = 0 on most frames and 5005 on tick boundaries.
- 00ab93f0 Framework per-frame tick (caller: AppGame::vf54 004268b0, WndProc d12910). Sequence at ab94b9..ab9634:
  1. FrameClock[4] = config target fps (config+4, default 29.97 = float at 0xdf7918) via ac2fe0.
  2. ac3040 wait, then ac33b0 -> raw dt -> Framework+0x30.
  3. clamp: `maxdt = (int)((1/config[+8]) * 300000)`; config+8 default = float at 0x218b2b8 = **14.985 fps -> 20020 units = 66.7 ms**. `dt = min(raw, maxdt)` (ab95e7..ab960f).
  4. If config+0xc (int, setter ac4c50) is nonzero it replaces dt (fixed step override; nothing in the exe seen setting it).
  5. dt *= !paused (config+0x14 byte), dt += config+0x18 (extra-time carry, then zeroed), dt = (int)(dt * config+0x1c) (time scale, default 1.0 at 0x2180630).
  6. Passes &dt to the task dispatcher ab9020 -> game update functions.
- Frame-rate setting: ReadGraphicsConfig 00acaf60 only builds the debug/system menu with strings Graphics_FrameRate / _Variable / _Stability (no 30 / 60 values). The target fps is config+4 (29.97 by default) and FC[4] follows it each frame. I did not find where "Variable" vs "Stability" sets it (not resolved).

## 2. World clock (high for the update, med for who feeds it)

- FieldObjectManager::Update 005cef80 takes int dt_ms; at 005cf209..005cf239 it converts `units = ms * 300000000 / 1000000` and calls the world timer update `5e3c00(&units)` on the object at [mgr+0x18e74].
- 005e3c00 (world timer update, this = world timer): `[this+0x98:0x9c] = (mult - [this+0x28]) * 0x271a` plus `dt_units * mult` (64-bit allmul at 005e3d70) plus [this+0xa0]; then [this+0x50:0x54] (the base time, u64) += that (005e3f02). `mult` is the sum/product of 8 ints at this+0xc.. (005e3cbb loop); the 0x400 floor applies in the flag-8 case. No per-frame cap other than the dt clamp above. World time = [this+0x50] (5e2ff0 getter; overridden by 3 slots at +0x78..+0x88 when set).
- Time getters, 64-bit units: 5e2b70 (day number, mod 0xffff), 5e2bb0 hours (24t / 1.08e9 mod 24), 5e2ca0 minutes (24t / 18e6 mod 60), 5e2d90 seconds (24t / 300000 mod 60), 5e2dd0 (24t/300000*24 as double).  Setter 5e3110 (SetBaseTime, mirrors to DB +0x2860 at [0x50f79d8]), offset add 5e3190, 5e36c0 (day-length ratio from DB param 0x77).
- Open item: the ms value 005b5050 passes to FieldObjectManager::Update is `[[0x234f130]+0x4c]`. I did not find the writer of that field (not a plain store with the usual pattern). It is almost certainly the post-clamp dt / 300, but I did not prove it. Also FieldInterface::vf8 005b8540 converts the same dt to ms with truncation (`dt*1000/300000`, magic 0x6fd91d85): 5005 -> 16 ms (95.9 percent of real), 10010 -> 33, 15015 -> 50, 20020 -> 66. That drives other ms countdowns (list at +0x120 via 5b3dd0), not the world time; at 60 fps those timers run about 4 percent slow, at lower fps nearly exact (the opposite direction of the suspected slowdown).
- Scale check: if t is in real 1/300000 s units, 24t/1.08e9 gives hours, so t = 45e6 units per game hour = 150 real s per game hour at mult = 1. The real speed depends on `mult`, which I did not resolve.

## 3. Behavior by fps

| fps | dt per frame | clock |
| --- | --- | --- |
| 60 (cap) | 1 tick (5005) | tracks real time |
| 22 steady | 45 ms rounded to 3 ticks, no drift | tracks real time |
| 15 to 60 | n ticks | tracks real time |
| below 14.985, or any frame over 66.7 ms | 20020 (clamp) | loses the excess (clock slower than wall) |
| 200 to 1000 | wait path: capped at 59.94; no-wait path: 0 on most frames, 5005 on tick boundaries | tracks real time; jittery dt |

The stuttery dt=0 frames only matter if the no-wait branch (00ac3240 with n == 1) is taken. I could not tell statically which branch the runtime takes (depends on the flag from the render context function at [0x510e960]+0xc).

## 4. Runtime HLE timers (read, high)

- QueryPerformanceFrequency = 10,000,000 (hle_kernel32.c:463). QueryPerformanceCounter = CLOCK_MONOTONIC in 100 ns units starting at 60 s (qpc_now, hle_kernel32.c:437). Real time: not scaled, not paused during GIL waits (wall clock). XRC_TIME_SCALE / XRC_TIME_SCALE_AT only if set in a tools build (xrc_tool_env).
- timeGetTime: ms of the same clock minus a 60 s offset (hle_system.c:35). timeBeginPeriod passes through on Windows; timeGetDevCaps returns 1 ms. timeSetEvent = one guest thread per timer sleeping with xrc_sleep to the next due time; late timers do not catch up (hle_system.c:60). The game uses it only for Timer::ThreadMain (00a02d90; analysis\recon\timer_thread.c), not for dt.
- Sleep(n>0): GIL released, high-resolution waitable timer (~0.5 ms) on Windows (xrc_thread.c:376); Sleep(0) only yields if GIL waiters exist. This makes the game's 00a96590 Sleep(0)/Sleep(1) loop spin-yield, not sleep.
- ovr_game.c (XRC_FRAME_LIMIT pacer, idle work) targets the stale 00e8xxxx FFXIII addresses and is not part of the LR frame path.

## Recommended fix (if hitches over 66.7 ms are confirmed)

Do not raise the global clamp (it protects physics and animation from huge steps). Instead keep the world clock on real time:
1. Preferred: manual override of 005e3c00 in the runtime (new file, not edited runtime *.c): remember xrc_qpc() at the previous call; if the incoming dt units are > 0 (not paused) and equal the clamp (20020 * time scale), replace them by `min(real_elapsed_units, 2 s)` using the real elapsed time since the last call (real_ms * 300). Leave paused frames (dt = 0) alone.
2. Alternative: override ac4b40 (config min-fps getter, `fld [ecx+8]`) to return 5.0f (clamp 200 ms). Simpler but changes all game logic steps on hitches.
3. First confirm with a one-line log in ab93f0's clamp (count frames where raw > maxdt) during the 22 fps soak; if the count is near zero, the clock slowdown is not from the clamp and the weak evidence was the save slot.

## Suggested names (not written to names.tsv)

- 00ac2f40 FrameClock::ctor; 00ac2fe0 FrameClock::SetTargetFps (shared ICF body with ApplicationConfiguration::vf1)
- 00ac3040 FrameClock::WaitForFrame (the limiter; calls 00a96590 FrameLimiter::WaitQpcSleep, which is already named)
- 00ac33b0 FrameClock::GetQuantizedDelta (1/59.94 s tick rounding; the LR analog of "00e8d490")
- 00ac4bd0 ApplicationConfiguration::ctor (defaults 29.97 / 14.985 / 1.0); 00ac4b40 GetMinFps; 0040a920 GetTargetFps; 00ac4c50 SetFixedDeltaOverride; 00ac4c70 SetExtraTime; 00c5b9d0 GetExtraTimePtr; 00ac4b50/00ae2130 paused byte
- 00ab93f0 Framework::TickFrame (clamp, pause, scale); 00ab7e90 Framework::vf2 = Update
- 005cef80 FieldObjectManager::Update(dt_ms) (already named field::api::EncountFail by string; wrong)
- 005e3c00 FieldWorldTimer::Update(const int* dtUnits); 005e2ff0 GetWorldTime; 005e3110 SetBaseTime; 005e3190 AddTimeOffset; 005e2b70 GetDayNumber; 005e2bb0 GetHour; 005e2ca0 GetMinute; 005e2d90 GetSecond; 005e36c0 GetDayProgress
- 005a02f0 FieldInterface::UpdateWithDt (reads *dt / 300000 as float seconds; callers 427670, 5b8540)
