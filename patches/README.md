# Patches to the translator

The Lightning Returns build uses ps4star's public translator, [XRC](https://github.com/ps4star/XRC) (The Unlicense), pinned to commit
`0fdf2205623b8c9aa9fb3e6822f3508cefc677cd` (2026-11-21, "Translate fs: in PE code as the current thread's TEB").

- `xrc-lr.patch` is a diff against that commit (`src/xrc_fp.odin` only). It is Unlicense, like XRC itself, and all credit for the
  translator belongs to ps4star. It ports the x87 handling this game needs:
  - values left on the x87 stack below a call's arguments stay live across a call whose x87 effect was solved
    (`x87_deriving`: the relaxation applies only to the function being translated);
  - CRT `ftol` helpers take `st(0)` only and leave the values below it alone (`x87_deep`);
  - the plain CRT math functions (`floor`, `sqrt`, `pow`, `fmod`, `sin`, ...) are known to take doubles on the stack and return in `st(0)`;
  - a computed tail call (`jmp reg` / `jmp [mem]`) and a forwarder get a guessed, flexible result.

  Without it about 20 functions fail with "x87 underflow" / "x87 live across call".
- The other things this game needs are data, not code, and live in `translator/lr/`: the function map (66k functions, including the
  vtable-only entry points XRC does not find by itself, with a header row), `overrides.tsv`, and `xrc.cfg` (the `__SEH_prolog4` / epilog
  addresses). See [translator/lr/README.md](../translator/lr/README.md).
- Odin's `core:rexcode` needs `patches/odin-rexcode-i386.patch` from the XRC repository (i386 decoder fixes). `build/build_all.py`
  applies both patches to private copies; your own Odin install is never modified.

Apply by hand:

```
git clone https://github.com/ps4star/XRC && cd XRC
git checkout 0fdf2205623b8c9aa9fb3e6822f3508cefc677cd
git apply /path/to/xrc-lr.patch
```
