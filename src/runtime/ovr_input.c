/* ovr_input.c - a "fast" override (overrides.tsv) of the game's IsXInputDevice, 00744820.
 *
 * cdecl bool IsXInputDevice(const GUID *product): the DirectX SDK's check, run on every game controller DirectInput
 * enumerates. It asks WMI (Win32_PNPEntity) for each device whose id has "IG_" and compares its VID/PID with the
 * product GUID; true makes the game add an XInput slot (polled with XInputGetState, user = the slot's index), false
 * opens the device through DirectInput. The runtime has no WMI, so the translation always says false; and the runtime's
 * EnumDevices (hle_dinput.c) lists the XInput pads with a product GUID of its own, XRC_PAD_GUID. This answers true for
 * that GUID and runs the translation for any other.
 *
 * Without it the game never calls XInputGetState: its pad list stays empty (00745650 loops over it), so neither the
 * host's pads nor a scripted XRC_INPUT pad worked. */
#include <stdio.h>
#include "xrc_rt.h"

#define XRC_PAD_GUID1 0x50435258u /* (hle_dinput.c) */
#define XRC_PAD_GUID2 0x00004441u

void XRC_CC xo_00744820(XCtx *c) {
	static XFn orig;
	u32 g = M32(c->esp + 4);
	if (g && M32(g) == XRC_PAD_GUID1 && M32(g + 4) == XRC_PAD_GUID2) {
		c->eax = (c->eax & ~0xffu) | 1;
		xrc_cdecl_ret(c);
		return;
	}
	if (!orig) orig = xrc_translation_of(0x00744820u);
	orig(c);
}
