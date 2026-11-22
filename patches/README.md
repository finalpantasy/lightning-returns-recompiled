# Patches to the translator

No patch files are published yet. The floating-point (x87) and entry-point changes used to translate
Lightning Returns were developed against a private snapshot of the translator, so they cannot be released as a
diff. Against the public XRC (github.com/ps4star/XRC) the known gaps on this game are:

- about 20 functions fail with "x87 underflow" / "x87 live across call" (CRT math helpers, `_ftol2` pattern);
- about 66 vtable-only functions are not discovered; add them to your `function_map.csv` as size-0 rows;
- `function_map.csv` needs a header line: `address,name,size,thunk,size_source`;
- `xrc.cfg`: `frame_prolog 00cf2210` and `frame_epilog 00cf2255` (the game's `__SEH_prolog4` / epilog).

Port the x87 patch to the public code and put it here as `x87-*.patch` when it is ready.
