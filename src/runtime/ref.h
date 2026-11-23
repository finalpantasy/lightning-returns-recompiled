/* ref.h -- shared by the reference target's forwarders (ref_*.c, XRC_REFERENCE): calls into real 64-bit COM objects.
 *
 * The forwarders run the HLE function first (it keeps the runtime's own state and writes the guest-visible results),
 * then call the real DLL with the same arguments translated: values as they are, pointers to plain data as host
 * pointers into guest memory (xrc_mem + address: guest memory is host memory), guest objects as their real ones,
 * windows as their real windows; structures with pointer-sized fields are rebuilt. The real result wins where both
 * have one. */
#ifndef REF_H
#define REF_H
#include <stdint.h>
#include "xrc_rt.h"

/* a COM method by vtable slot with up to 11 arguments after `this`: x64 passes every one in a 64-bit slot, so the
 * unused ones are harmless; a float argument in one of the first three positions needs its own typed call */
typedef uintptr_t (*RefFn)(void *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                           uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static inline u32 ref_call(void *o, int slot, const uintptr_t *a) {
	RefFn f = (*(RefFn **)o)[slot];
	return (u32)f(o, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]);
}
static inline u32 ref_release(void *o) { uintptr_t a[11] = {0}; return o ? ref_call(o, 2, a) : 0; }
static inline void *ref_ptr(u32 g) { return g ? (void *)(xrc_mem + g) : 0; }

XrcHle ref_d3d9_wrap(const char *dll, const char *name);
XrcHle ref_user32_wrap(const char *dll, const char *name);
XrcHle ref_dinput_wrap(const char *dll, const char *name);
XrcHle ref_xaudio2_wrap(const char *dll, const char *name);
#include "xrc_quiet.h" /* (-DXRC_NO_TOOLS: nothing printed) */
#endif
