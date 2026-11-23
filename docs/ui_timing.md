# UI and frame timing map (static, no game runs)

Goal: find what in the UI, layout animation and shadow code depends on frame count instead of dt, for a 60 fps fix. Names are in tools/names/manual.tsv (section "UI and frame timing"). Confidence: high = read the code, med = inferred from call shape.

## Result in one paragraph

The GUI is time-driven, not frame-counted. The Framework dt (int units of 1/300000 s, already clamped to 20020 and quantized to 5005) is converted to float seconds once (GuiInterface 0079fa70: `cvtdq2ps; divsd [0xdf18e0]`). Every GUI widget then receives dt in milliseconds (`dt_sec * 1000.0 * guiSpeed`; [0xdf1bb8] is the double 1000.0, guiSpeed is GuiManager+0xacc). Layout tween timers advance by `dt * speed` and compare against a duration. I found no GUI update function that adds a fixed step per frame. The scan covered GUI vf74/vf90-92/vf1 updates and every function in 0x43xxxx-0x99xxxx with a member inc/dec plus a compare; each hit was a list or state counter (once per event), not a per-frame timer. The remaining frame-rate risks are (a) the 16 ms truncation in FieldInterface::vf8 (already in timestep.md), (b) dt == 0 frames above 59.94 fps (physics skips them, tweens do not move), (c) the physics step defaults (section 4).

## 1. dt path into the GUI (high)

| Addr | Proposed name | Evidence |
| --- | --- | --- |
| 0079fa70 | GuiInterface::Update_FromUnits (vf7) | loads int [arg], `cvtdq2ps`, `divsd 300000.0`, calls GuiApi 0079a2d0 |
| 0079faa0 | GuiInterface::UpdateWidgetList_FromUnits (vf8) | same conversion, calls 0079a2f0 then 007a5050 |
| 007b0470 | GuiManager::Update(float sec) | 1486 insns; every sub-manager gets `[ebp-0x16c]` (dt) |
| 007a5050 | GuiManager::UpdateWidgetList(float sec) | loop over list at +0x4c4, `vcall +0x128(dt*1000*[+0xacc])` |
| 0084cf00 | GuiProduct::Update_DispatchByState (vtable slot 0x128) | forwards dt to the layout frame (+0x34, slot +4), then by state byte [+0x40] (2..5) to slots 0x168/0x16c/0x170/0x174 |
| 00795660 | GuiActionObject::UpdateTree (vf1) | calls 00794470 (UpdateTransformAndLayout), then recurses to children (+8, next at +0x14) with the same dt |
| 007931e0 | GuiActionObject::AdvanceActionTimers | up to 8 slots (stride 0x10), `t += dt * [this+0xa8]`, wrap or clamp at the slot duration [+0xc] |

Other consumers confirmed as dt-based (the instruction pattern scales by the dt argument): GuiMainMenuList::vf1 00892ae0 (cursor and scroll `pos +/-= dt/k*c`, clamped), GuiCreditRoll::vf74 007df190, GuiNaviMap::vf74 0084a160, ClockMenu::vf74 0086b6a0, GuiMissionTargetIcon::vf74 00841c30, GuiMainEquipFrame::vf1 00890220.

Field prompts (GuiFieldActionSign, vtable 020c56b4, init 007eee10): the animation is played by layout object slots (`vcall +0x2c` with ids such as 0x44) and advanced by the GuiProduct dt path. Its state functions 007efa70 and 007efd90 only do once-per-state setup (`[+0xc0]` is a "setup done" flag, incremented once). No frame counting there.

GuiManager::Update extras, all dt: a time accumulator `[+0x878] = fmod(acc + dt, 1000.0)` (007b187e, used for pulses); a fade countdown `[+0x15d8] -= dt` (007b1912); dt re-converted to int units (`dt * 300000`) for the manager at [0x23509a8] (007b18e1).

## 2. Frame-rate checklist

1. FieldInterface::vf8 005b8540: `dt*1000/300000` truncated to int ms (5005 -> 16 ms, 4 percent slow at 60 fps). Drives the ms countdown list at +0x120. Fix: carry the remainder.
2. Quantized dt (ac33b0): above 59.94 fps most frames have dt = 0. GuiManager::Update still runs with dt 0 (GuiManager+0x9c0 is a separate pause flag), so tweens do not move on those frames; physics skips them.
3. AppGame target fps selector 0042cf70: mode [this+0x324] 0 -> ApplicationConfiguration vf+0x50 picks one of two doubles (0xdf7910 or 0xdf7908), mode 1 -> 29.97 (float 0xdf7918), otherwise float 0xdf6860; the result goes to the config setter (vf+4). The double constants are the 60 fps lever (not verified at runtime).
4. No fixed per-frame UI step was found. If a prompt still looks 30 fps at runtime, suspect the layout resource frame rate (data), not code.

## 3. Reconstructions

```c
/* 0079fa70 GuiInterface::Update_FromUnits (vf7) */
void GuiInterface_Update_FromUnits(GuiInterface* self, const int* dt_units) {
    GuiApi_Update((float)((double)(float)*dt_units / 300000.0));   /* 0079a2d0 -> 007b0470 */
}

/* 007a5050 GuiManager::UpdateWidgetList */
void GuiManager_UpdateWidgetList(GuiManager* m, float dt_sec) {
    for (Widget* w = m->widgetHead /*+0x4c4*/; w; w = w->next /*+0x8*/) {
        bool paused = m->pauseA /*+0x9c0*/ || m->pauseB /*+0x9c4*/;
        if (paused && !(w->flags /*+0x38*/ & 0x80)) continue;    /* 0x80: updates while paused */
        if (w->flags & 0x200) continue;                          /* 0x200: skip */
        w->vf_0x128((float)((double)m->guiSpeed /*+0xacc*/ * ((double)dt_sec * 1000.0)));
    }
}

/* 0084cf00 GuiProduct::Update_DispatchByState (vtable +0x128), dt in ms */
void GuiProduct_Update(GuiProduct* p, float dt_ms) {
    if (!p->frame /*+0x34*/) return;
    p->frame->vf_0x04(dt_ms);                        /* GuiActionObject::UpdateTree */
    switch (p->state /*+0x40, signed byte*/) {
    case 2: p->vf_0x168(dt_ms); break;
    case 3: p->vf_0x16c(dt_ms); break;
    case 4: p->vf_0x170(dt_ms); break;
    case 5: p->vf_0x174(dt_ms); break;
    }
}

/* 007931e0 GuiActionObject::AdvanceActionTimers: 8 slots x 16 bytes at this+0xb8 */
bool GuiActionObject_AdvanceActionTimers(GuiActionObject* o, float dt) {
    if (!o->slots) return false;
    float step = o->speed /*+0xa8*/ * dt;  bool anyActive = false;
    for (int i = 0; i < 8; i++) {
        Slot* s = &o->slots[i];            /* def +0x0, t +0x4 (float), flags +0x8, state +0xc */
        if (!s->def) continue;
        if (s->flags & 8)  { s->state |= 1; }                          /* force finish */
        else if (s->flags & 0x10) { s->t = s->def->duration; s->state = (s->state & ~2) | 1; }
        else if (s->flags & 1) {            /* looping */
            s->t += step;
            if (s->t > s->def->duration) { s->t -= s->def->duration; /* flags&2: ping-pong toggle of state bit 2 */ }
        } else if (!(s->state & 1)) {       /* one shot */
            s->t += step;
            if (s->t > s->def->duration) { s->t = s->def->duration; s->state |= 1; }
        }
        if ((s->state & 1) && !(s->state & 4)) anyActive = true;
    }
    return anyActive;
}
```

The AdvanceActionTimers reconstruction is simplified from the disassembly (the flag-8, flag-0x10 and ping-pong branches are shown in outline; check 007931fa..00793335 before relying on bit details).

## 4. Physics step (Phieg): the "FramePacer" names were wrong

The functions previously named FramePacer::MainWait (b55160) and MainWait_inner (b54de0) are the Phieg physics frame step, not a pacer. AppGame::WaitFrame 0042e2c0 passes the frame dt (int units) to b55160, which converts it to float seconds (`/300000`) and calls b54de0. That returns without stepping when dt == 0 or when state bits 8 or 10 of [this+0xd0] are set. Otherwise it calls b54cb0 (StepAll: b7ce70(dt), SetTimeStep(dt) at b7d130 = `[world+0x4]->[+0x40] = dt`, then two more stages). So physics runs with the real variable dt.

Fixed values exist only as defaults: b54e40 (create, called from AppGame::FrameStep 004317c0) sets 1/30 (float 0xdf3568), Framework vf12 ab7570 resets it to 1/59.94 (0x2097c40), and the sub-world constructor b828e0 stores 1/60 (0x2074a84) at +4 and increments a step counter at +0x21c. Whether the solver sub-steps by that 1/60 independent of dt is not resolved; read b86ff0 and b7d300 before trusting physics at 120 fps. Also 0054b644 (BattleMagicControlMcrsSP1::vf2) uses a 1/60 constant; not read further.

## 5. Shadow-map scheduling (negative result, med)

- No parity or alternate-frame logic found for shadow cascades. I searched the renderer range (0xa0xxxx-0xb1xxxx) for `and reg,1` / `test ...,1` preceded by a global load or counter, and read all Shadow-named code. ShadowMapShaderDriver 00a853d0 / 00a85180 and ScreenShadowShaderDriver 00a85040 / 00a84e70 only bind constants (shadowMatrix0/1, shadowSplitRange, shadowFadeParam, randomize offsets). ShadowFilter 00a51120 is a ctor/dtor shell. The Graphics_Shadowing_Standard/Advanced strings are debug-menu items in ReadGraphicsConfig 00acaf60.
- The only per-frame toggle in the draw manager is `FrameworkDrawManager+0x1ac ^= 1` at 00a541ab (end of ExecuteDrawPass 00a525c0). It double-buffers a per-frame entry list that helper 00a51650 appends to (capped at 0x20 entries via the count at +0x1b0, reset to 0 at 00a541b2); a525c0 consumes it at 00a53cac. Entries hold a position, a float gate against [0xdf403c] and a normalized direction, which looks like dynamic lights, not shadow cascades (low-med). It is a swap, not an every-other-frame skip.
- Conclusion: a 30 fps look in shadows does not come from a code-level alternate-frame update in the places I could find. It may come from cutscene ShadowClip data (vtable 021f0f2c) or quality preset tables. A runtime check (trace writes to the shadowMatrix0 constants) is needed before changing anything.

## 6. Names added to manual.tsv (15; backup tools/names/manual.tsv.bak_ui)

00b55160 and 00b54de0 (renamed from FramePacer), 00b54cb0, 00b7d130, 00b54e40, 00ab7570, 007b0470, 007a5050, 0079fa70, 0079faa0, 0079a2d0, 0079a2f0, 00795660, 007931e0, 0084cf00.
