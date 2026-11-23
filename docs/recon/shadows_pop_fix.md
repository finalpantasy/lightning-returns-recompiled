# Shadow pop: cause and the XRC_POP_ZSHADOW switch

Companion to `shadows.md` and `shadowpop_notes.md`.

## Symptom
Standing still in an outdoor night scene with Shadowing set to Advanced, the far cascade intermittently goes dark: the ground beyond a near horizontal line
turns shadowed for about 122 frames (roughly 2 s), then returns. It recurs without input and also shows once right after the pause menu closes.
It reproduced in 10 of 10 test runs.

## Cause
It is a change in the shadow map content, not in cascade parameters, matrices or precision. The map is one 2048x2048 R32F target. In "block" frames
a group of large alpha-tested static casters (foliage) has no effect on it.

The runtime has an optimization, `zout_drop` in the Vulkan backend. It uses the pixel-shader variant without the depth-output write unless the
depth target was read in the last 120 frames. A depth read is set by a depth LOAD at render start (a render split) or by sampling a depth texture.
A texture upload for a newly spawned character, or the menu closing, splits the shadow-pass render, which LOADs depth. For the next 120 frames the
foliage shader keeps its depth write, the casters write different depth into the map, and the resolve pass darkens the ground. After the flag expires
the map goes clean again.

The kept depth write is what the game's own shader does, so the dark state is the faithful one and the clean state is the optimization's output
(inferred).

## The switch
`XRC_POP_ZSHADOW=keep|drop` pins the decision for shadow-map targets (square, 512 px or larger, format 114) and ignores the depth-read age.

| value | result |
|---|---|
| keep | stable, full shadows, faithful to the game shader. Approved default |
| drop | stable and clean, but large tree and building shadows are cut off |
| (unset, old behavior) | pops when the 120-frame flag flips |

Before/after, 290 s runs with shadow hold off: old behavior blocked at about 132 s and 250 s; keep was permanently dark (stable); drop permanently clean.
Frame cost: medians 38.2 fps (old), 33.8 (keep), 35.3 (drop). Hidden-window runs are noisy, so treat it as within noise, with keep possibly about 4 fps lower.

## Notes
- Shadow stability (hold) does not affect this bug.
- The fix lives in the Vulkan backend, which is not in this repo. This document is a finding, not source.
- Make `keep` the code default; never ship `drop` as default.
