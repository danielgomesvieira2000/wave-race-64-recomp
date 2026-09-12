// Two hooks into the game, for the modern water renderer.
//
// Neither replaces anything: each wraps the original function, which is called
// from inside it, so with the water renderer set to Original the only cost is
// the wrapper. See docs/WATER.md.
//
// Both are registered by address in src/overlays.cpp rather than by overriding
// a weak symbol, because the addresses are what the runtime dispatches on.

#include "recomp.h"

#include "wr64/water.h"

extern "C" void SysMain_SendGfxTaskSetMesg(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8009345C(uint8_t* rdram, recomp_context* ctx);

namespace wr64 {

// 0x8009345C, the game's complete race initializer. It also runs for a restart
// of the same course, where neither the course id nor the global frame counter
// changes -- so the renderer cannot infer "new race" from what it sees, and is
// told instead. The histories it clears are the wake and foam fields, which
// otherwise carry the previous race's marks into the first frames of the next.
//
// The reset happens after the original, not before: the original loads the
// course, and resetting before it would clear state that it then repopulates.
void water_race_init_hook(uint8_t* rdram, recomp_context* ctx) {
    func_8009345C(rdram, ctx);
    water::reset_for_race();
}

// 0x80046CF8, where the game hands a finished graphics task to the OS. This is
// the moment the frame's geometry and camera are final and nothing has been
// submitted yet, which is the only point at which a consistent snapshot of the
// game's state can be taken for the renderer to use.
//
// `OSTask::t.data_ptr` -- the display list this task will run -- is the 32-bit
// cartridge pointer at offset 0x30 of the task, and it is what the renderer
// matches its snapshot against on the other thread. See GAME-INTERNALS.md.
void water_task_submit_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t display_list = MEM_W(0x30, ctx->r4);
    water::publish_frame(rdram, display_list);
    SysMain_SendGfxTaskSetMesg(rdram, ctx);
}

}  // namespace wr64
