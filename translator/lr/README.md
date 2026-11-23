# Translator input for Lightning Returns

What XRC (ps4star's translator, Unlicense) needs for this game besides the executable. Nothing here is game code or game data: no
bytes, strings or instructions are copied from `LRFF13.exe`. `build/build_all.py` passes this folder to the translator as `--override`.

| File | What it is |
|---|---|
| `data/function_map.csv` | One row per function: start address, a generated name (`FUN_<address>`), size in bytes, thunk flag, and where the size came from. 66,355 rows. Addresses and sizes are measurements of the exe's layout. 70 rows carry a public symbol name instead: names of Windows DLL imports (`_timeGetTime`, `_DirectInput8Create`) and of the statically linked MSVC C runtime (`_memset`, `__CIsin`, `__ftol2`, ...). The translator uses the C runtime names to know which functions take x87 arguments. Rows marked `ours_extra`, `tailjmp_target` and `switch` are our own annotations (entry points found from vtables, jump-table owners, tail-jump targets); sizes are merged where a row turned out to be the middle of a function. |
| `overrides.tsv` | The functions replaced by runtime code (`fast`, `wrapped`, `manual`) or marked `unreachable`: address, kind, and a one-line reason naming the runtime file that implements it (`src/runtime/ovr_*.c`). |
| `xrc.cfg` | The addresses of the C runtime's `__SEH_prolog4` and `__SEH_epilog4` (`frame_prolog`, `frame_epilog`). |

The function map and overrides are for the Steam `LRFF13.exe` with SHA-256
`31a2bc59d305075f0e8eacee4f65f413a3205186b52b786d9f3a9db8aec1cf93` only.
