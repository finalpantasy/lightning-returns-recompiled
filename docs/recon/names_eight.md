# Naming pass 8 (names8)

`tools/names/names_eight.py` runs after `names_seven`. Rank 1.0 (above names7 at 0.9). Only unnamed functions or those with a names7 guess can be replaced.

Result: 63,810 to 63,812 of 66,348 named (+2 new, 7 weak names upgraded). Duplicate names: 2,696 before and after.

## Finding: the "no call edges" pool is gone
After names7 only 55 of the 5,459 target functions have no caller and only 18 of the unnamed have no edges at all (the earlier 649 mostly became callee7 names).
Scanning data sections for 4-byte pointers to all targets found only 59 referenced from data, nearly all noise: exception-unwind tables, constant blocks, zero-padded stride-table slots.
None has a string pointer in the adjacent word.

| source | named | upgraded from a names7 guess | evidence |
|---|---|---|---|
| vtloc8 | 6 | 4 | vtable of a function-local class, RTTI name `.?AVLocal@?1??func@Cls@ns@@` that `build_names.py` could not demangle: `Outer::func::Local::vfN` |
| node8 | 2 | 2 | the application constructor builds one node per update/draw pass; the function pointer sits next to a name string: `cb_<string>` |
| regcall8 | 1 | 1 | function pointer stored next to exactly one string: `cb_<string>` |
| datastr8 | 0 | 0 | function pointer with a string pointer in the adjacent data word |

One function was skipped because two strings conflicted. All 9 changes were checked against the data and agree.

## Leads
1. 25 local-class vtables are outside the vtable list because the demangler rejects the `?1??func@Cls@@` form. Fixing it ties them to RTTI classes properly.
2. The node table stores about 35 handlers; older generic names could be replaced with `cb_<string>` names.
3. A few shared handlers and string registrations need a manual look.
4. The remaining ~2,500 unnamed functions need a decompile-level pass (parameter use, API-call patterns), not more reference scanning.
