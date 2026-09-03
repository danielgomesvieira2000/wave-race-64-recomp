// Phase 04: replacements for the overlay-dispatch functions stubbed in phase 02.
//
// N64Recomp emits every recompiled function as RECOMP_FUNC, which under Clang
// is `extern inline __attribute__((weak, noinline))`. A strong definition of the
// same symbol therefore wins at link time, which is the mechanism N64Recomp
// intends for patching game functions -- no regeneration, no editing generated
// code.
//
// Phase 02 stubbed three functions because they `jal` directly into the overlay
// window at 0x802C5800, which cannot be resolved statically: resolve_jal never
// treats a function in a relocatable section as a candidate from another
// section, since which overlay is resident is a runtime fact.
//
// Stubbing them was not free, and phase 04 found the bill. In SysMain_Thread:
//
//     800471FC: jal func_80092CF0
//     80047200:   lw $a0, %lo(gDisplayListHead)($a0)   ; delay slot: head in
//     80047208: jal SysMain_GfxFullSync
//     8004720C:   sw $v0, %lo(gDisplayListHead)($at)   ; delay slot: return out
//
// func_80092CF0 takes the display list head, appends whatever the resident
// overlay draws, and returns the advanced pointer -- which is written straight
// back into gDisplayListHead. An empty stub leaves v0 unset, so the head became
// null and SysMain_GfxFullSync wrote through it and faulted.

#include "recomp.h"

extern "C" {

// Returns the display list unchanged: nothing is drawn, but the pointer stays
// valid and the frame completes.
//
// This is a deliberate interim, not the fix. The real implementation has to
// dispatch to the resident overlay through the runtime's overlay lookup, and
// until it does, everything these 20 overlay entry points draw is missing.
// Making it explicit here is the point -- the alternative is a null pointer
// crash that says nothing about why.
void func_80092CF0(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    // a0 is the incoming display list head; v0 is the returned one.
    ctx->r2 = ctx->r4;
}

}  // extern "C"
