# FrameClock (LRFF13.exe), static decode 2026-11-22

Object pointer: global `[0x511555c]` (FrameClock*), used by Framework::TickFrame ab93f0 (ab81f0, ab9485..ab97f9), ovr_frame.c `fps60_tick`.
Methods: ac3010 InitFps (this+0, this+4 = 59.94f from const df6860), ac2fe0 SetTargetFps (this+4 = arg, ret 4),
ac3040 WaitForFrame, ac33b0 GetQuantizedDelta (ret 4, out dt via arg).

## Object fields (this)
| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | f32 | current/measured fps (init 59.94) | ac3010 store; ac3040 reads [ebx], ovr_frame.c FC[0] |
| +0x04 | f32 | target fps (SetTargetFps) | ac2fe0; ac3040 reads [ebx+4]; n = (int)([0]/[4] + 0.5) frames to wait |
| +0x28 | u32 | ring index 0..3, `(i+1)&3` per GetQuantizedDelta call | ac33b0 ac3422 |
| +0x30 | u64 | QPC timestamp of the frame end (ac3040 stores ecx at ac3235 / ac32c8) | ac3040, ac33b0 reads it |
| +0x38 | u64 | previous quantized QPC stamp (floor(t30 / P) * P) | ac33b0 ac34cb |
| +0x40 | fn ptr | optional callback; ac2fb0 does `if (p) jmp p` | ac2fb0 |

## Globals
| addr | meaning | evidence |
|---|---|---|
| 0x24c3ad8/0x24c3adc | cached QueryPerformanceFrequency (u64), valid when bit0 of 0x24c3ae0 set (IAT 0xde9094 = QPF) | ac33b6..ac33e5 |
| 0x5115570/74 | u64 P = freq * 1001 / 60000: QPC ticks per 1/59.94 s frame; computed once in WaitForFrame (flag 0x5115578 bit0) | ac3094..ac30b5 (mul 0x3e9, div 0xea60) |
| 0x5115560/64 | u64 P / 10 (tenth of a frame, slack/threshold in the late-frame test); once, flag 0x5115578 bit1 | ac3330..ac335b (div by 0xa) |
| 0x5115568/6c | u64 running max of an elapsed/overshoot QPC measure in the wait loop (compared and stored at ac31a4..ac3215); exact meaning unconfirmed | ac3040 |
| 0x5115578 | init flags for WaitForFrame (bit0 P, bit1 P/10) | ac3046, ac3059, ac3330, ac3344 |
| 0x5115580/84 | u64 P again (freq*1001/60000) for GetQuantizedDelta; flag 0x5115588 bit0 | ac3471..ac349b |
| 0x5115588 | init flag for 0x5115580 | ac3427, ac3433 |
| 0x5115590 | ptr to config; +8 = max frame-rate float (mf); clamp = round(1/mf * 300000) (20020 at 14.985) | ovr_frame.c xo_00ac33b0 |
| 0x511558c | ptr to another config struct, +0x2c/+0x2d flags (shadows.md) | shadows.md |

## GetQuantizedDelta math (ac33b0)
diff = this.t30 - this.t38 (QPC); dt_units = diff * 300000 / freq (1/300000 s units); out = round((dt_units + 0x9c6) / 5005) * 5005
(0xd181753 magic divide by 0x1ac... then * 0x138d = 5005 = 300000/59.94). So dt is quantized to whole 1/59.94 s ticks, 5005 units each; this is
what ovr_frame.c fps60 replaces. t38 is set to floor(t30 / P) * P each call.

## WaitForFrame (ac3040)
Reads fps pair (this+0, this+4), turns the ratio into n frames, waits to the n * P boundary (QPC via IAT 0xde9090 = QPC read, Sleep 0xde90c4),
a96110 HasVblankCapability gates the vblank branch; stores the end time in this+0x30. Callee names: a96110, a96f20 D3d9::Present, a96090 WaitGpuQuery, a96590 FrameLimiter::WaitQpcSleep.

Confidence: field offsets and P constants high (direct constants); +0x28 ring and 0x5115568 meaning low-med.
