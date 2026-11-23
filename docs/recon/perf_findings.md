# Performance findings: pipeline stutter and long sessions

Findings only. The fixes live in runtime files that are not in this repo.

## Pipeline creation is not the stutter source
- The Vulkan backend already builds pipelines on a worker pool, waits at most 2 ms, then skips the draw. Pipeline keys are recorded to `<cache>.keys` and prewarmed on the next boot.
- Creation costs about 1 ms each. The old synchronous in-draw path hit a 98 ms worst frame on an empty cache.
- An empty cache means about 1,200 skipped draws (visible as pop-in). Merging a shipped key list (`XRC_VK_PW_SEED=<file>`) cut that to 0-7 on a fresh cache.
  A boot-time burst gave no gain.
- To ship this: record `.keys` from a full playthrough and load it from beside the exe by default. Only a short route list exists so far.
- About 100 frames per 8,000 still exceed 20 ms, even warm or synchronous: that is game work or streaming, the next fluidity target.

## Long-session audit of the HLE layer
Prompted by WhiteRecomp's long-session fixes (see `whiterecomp_v13_v15.md`).
- Thread table: 64 slots that are never reused, with stacks leaked; every timer callback thread consumes one.
- Guest heap: big blocks are never split or merged and `VirtualFree` is a no-op, so fragmentation can exhaust it.
- Save move: one `MoveFileEx` attempt with no retry; `WriteFile` always reports success.
- A prototype frees thread stack/TEB/TLS on exit, raises the table to 256, coalesces big blocks on allocation failure and retries the save move 8 times
  (25 ms, doubling). Not yet tested in a long run.
- Test plan: count threads over a long run with thread tracing, sample memory every 10 s, save twice.
