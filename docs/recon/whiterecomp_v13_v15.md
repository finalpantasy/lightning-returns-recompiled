# WhiteRecomp v0.0.13 to v0.0.15: what to port

Source: the public commit history of the WhiteRecomp project (read-only), covering v0.0.13 through v0.0.15 and three follow-up commits.
Only the Odin launcher source is public. The game runtime ships inside a bundle, so those fixes are visible only as commit-message bullets,
not diffs. Anything marked "no diff" has to be re-derived.

## Ranked list

| # | what it does | status here | value | effort |
|---|---|---|---|---|
| 1 | Ultrawide 21:9 / 32:9: Hor+ FOV, HUD centred, 3D drawn at full width instead of pillarbox, extra resolution list | Not done. The renderer scales to fit with aspect kept. The approach (patch projection aspect, centre HUD) transfers, the addresses do not | High | High |
| 2 | Guest memory allocator rewrite (long-session crashes), no diff | Audit our guest heap for fragmentation and leaks | High if long sessions crash | Medium |
| 3 | Periodic ~100 ms hitches from unbuffered logging, no diff | Check for per-frame flushes in hot paths | High (pacing) | Low |
| 4 | HLE audit: thread slots reused, audio-loop freeze, save writes that fail, hardening, no diff | Audit our HLE against the same categories | Medium-High | Medium |
| 5 | Read-only constant folding in translated code, no diff | Translator change, not checked | Medium (speed) | Medium |
| 6 | Menu crash and a white-screen fix in one battle type, no diff | Game-specific, low priority | Low-Medium | Low |
| 7 | Function-map error that made a return land in the wrong place | Bug class worth checking in our function-map generator | Low-Medium | Low |
| 8 | Any SDL2-known controller shown to the game as an Xbox 360 pad | Not verified here | Medium | Medium |
| 9 | Steam Cloud save sync | Not needed unless a Steam-integrated build ships | Low-Medium | Medium |
| 10 | Windows moved fully to Vulkan + SDL2; per-program log, previous-log rotation, crash reports | The log rotation and crash report are the cheap part to copy | Low / Medium | Low for logs |
| 11-15 | Launcher items: exe icon, Bink2 cutscene path, build-id stamp, pipeline build thread default, subtitle language | Launcher-level or low value | Low | Low |

## Already covered here
- Anisotropic filtering: already present, with a menu row (16x, 8x, 4x, off).
- Hair and eyelash alpha fix: the alpha-to-coverage change was already ported.
- Frame pacing: diffs not visible; compare with our own frame-clock code (catch-up fix already in).
- Their "Effects Resolution" half-res row has no equivalent here (small, low value).
- Their lesson on pipelines: they tried synchronous pipeline creation and reverted because new pipelines stall draws. Ours is already asynchronous.

## Do first
1. Ultrawide and FOV. 2. Logging hitches. 3. Long-session allocator, thread-slot, audio-loop and save-write audit. 4. SDL2 gamepad mapping. 5. Constant folding in the translator.
