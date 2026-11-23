# Naming pass 7 (names7)

`tools/names/names_seven.py` runs last in `build_names.py`, after `names_six`. It only names functions that have no name yet.
Every name carries its 8-digit address, so names are unique. Evidence kind is the `source` column; confidence is low for all of them.

Result: 60,889 to 63,810 of 66,348 functions named (+2,921). 2,538 stay unnamed.

| source | count | rule |
|---|---|---|
| caller7 | 1,466 | `called_by_<stem of most common named caller>_<addr>` (50% or more of named callers agree) |
| callee7 | 947 | `calls_<best-ranked named callee>_<addr>` (generic callees such as operator, CRT, allocator, mutex, imports are skipped) |
| solo7 | 235 | exactly one named caller: `Cls::helper_<addr>` or `helper_of_<caller>_<addr>` |
| ns7 | 184 | 60% or more of 2+ named callers share a class or namespace: `Cls::util_<addr>` |
| chain7 | 56 | the solo rule applied again to callees of solo7 names |
| adj7 | 33 | no call edges; address neighbors on both sides share one class or file: `Cls::adj_<addr>` |

Callers whose own name is weak (unwind stubs, string-reference names, import thunks) are ignored as evidence.

## Checks
- No existing entry changed; the TSV format is unchanged.
- No duplicate names among the new ones.
- 20 new names spot-checked against the call graph from the translated disassembly: all 20 had the caller or callee the name claims.
  One weak case: a vtable-slot reference counts as a caller, so a function can be named after a class that only references it.
- These are call-graph facts, not proof of purpose. Read `called_by_X` as "neighbor of X", not "is X". About half the new names are relation names, not class-method names.

## Leads
1. About 650 functions had no call edges before the pass; scan data sections for pointers to them and for jump tables inside named switch functions.
2. Promote `helper_` and `util_` names to a real class when a vtable-class constructor is the only caller.
3. Re-run chain naming after more strong names are added by hand (about 1,500 functions chain off weak names).
4. Matching the sequel's PC exe against this one for shared library code is the only remaining source of real names.
