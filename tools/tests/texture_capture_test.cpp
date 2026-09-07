// Verify capture overrides cannot affect normal runs or write unchecked values.
// Standalone: clang++ -std=c++20 -fsanitize=address,undefined \
// -Ilib/N64ModernRuntime/N64Recomp/include patches/texture_capture.cpp \
// tools/tests/texture_capture_test.cpp -o build/texture-qa/capture-test
#include "recomp.h"
#include <cassert>
#include <vector>

namespace wr64 {
void texture_capture_before_race(uint8_t*);
void texture_capture_after_race(uint8_t*);
void texture_capture_rider_select_hook(uint8_t*, recomp_context*);
void texture_capture_frame(uint8_t*, recomp_context*);
void texture_capture_draw_hook(uint8_t*, recomp_context*);
}

static int initialized_state = -1;
#define NATIVE_INITIALIZER(name, state) \
    extern "C" void name(uint8_t* rdram, recomp_context*) { \
        initialized_state = state; MEM_W(0, int32_t(0x800DAB24)) = state; }
NATIVE_INITIALIZER(func_801EBD28, 0x0a)
NATIVE_INITIALIZER(func_801EC304, 0x3c)
NATIVE_INITIALIZER(func_801EC3AC, 0x3e)
NATIVE_INITIALIZER(func_801EC500, 0x42)
NATIVE_INITIALIZER(func_801EC5B4, 0x44)
NATIVE_INITIALIZER(func_801EC650, 0x48)
NATIVE_INITIALIZER(func_801EC6EC, 0x46)
NATIVE_INITIALIZER(func_801EC780, 0x40)
NATIVE_INITIALIZER(func_801EB91C, 0x1e)
NATIVE_INITIALIZER(func_801EC0D4, 0x34)
extern "C" void func_80077F5C(uint8_t*, recomp_context* ctx) { ctx->r2 = 456; }
extern "C" void func_80092CF0(uint8_t*, recomp_context* ctx) { ctx->r2 = ctx->r4 + 24; }

int main() {
    std::vector<uint8_t> memory(8 * 1024 * 1024, 0x5a);
    uint8_t* rdram = memory.data();
    setenv("WR64_TEXTURE_CAPTURE_RIDER", "2", 1);
    setenv("WR64_TEXTURE_CAPTURE_ALTERNATE", "1", 1);
    setenv("WR64_TEXTURE_CAPTURE_DIFFICULTY", "2", 1);
    setenv("WR64_TEXTURE_CAPTURE_MODE", "versus", 1);
    unsetenv("WR64_TEXTURE_DUMP");
    const auto initial = memory;
    wr64::texture_capture_before_race(rdram);
    wr64::texture_capture_after_race(rdram);
    assert(memory == initial);

    setenv("WR64_TEXTURE_DUMP", "capture-test-only", 1);
    wr64::texture_capture_before_race(rdram);
    assert(MEM_W(0, int32_t(0x801CE608)) == 1);
    assert(MEM_H(0, int32_t(0x801CE60C)) == 2);
    assert(MEM_H(0, int32_t(0x801CE60E)) == 2);
    assert(MEM_W(0, int32_t(0x801CB338)) == 2);
    assert(MEM_W(0, int32_t(0x800D48DC)) == 0);
    assert(MEM_W(0, int32_t(0x800D48E0)) == 1);
    for (int slot = 0; slot < 4; ++slot)
        assert(MEM_W(slot * 4, int32_t(0x800DA9B0)) == (2 + slot) % 4);
    assert(MEM_H(0, int32_t(0x801CE6F4)) == 1);
    assert(MEM_H(2, int32_t(0x801CE6F4)) == 1);

    const auto valid = memory;
    setenv("WR64_TEXTURE_CAPTURE_RIDER", "999999999999999999999999999999", 1);
    setenv("WR64_TEXTURE_CAPTURE_ALTERNATE", "2", 1);
    setenv("WR64_TEXTURE_CAPTURE_DIFFICULTY", "2junk", 1);
    setenv("WR64_TEXTURE_CAPTURE_MODE", "unknown", 1);
    wr64::texture_capture_before_race(rdram);
    assert(memory == valid);
    setenv("WR64_TEXTURE_CAPTURE_RIDER", "-1", 1);
    wr64::texture_capture_before_race(rdram);
    assert(memory == valid);
    setenv("WR64_TEXTURE_CAPTURE_SCENE", "audio", 1);
    wr64::texture_capture_rider_select_hook(rdram, nullptr);
    assert(initialized_state == 0x48);
    setenv("WR64_TEXTURE_CAPTURE_SCENE", "invalid", 1);
    wr64::texture_capture_rider_select_hook(rdram, nullptr);
    assert(initialized_state == 0x0a);
    setenv("WR64_TEXTURE_CAPTURE_SCENE", "overview", 1);
    setenv("WR64_TEXTURE_CAPTURE_ROUND", "7", 1);
    setenv("WR64_TEXTURE_CAPTURE_DIFFICULTY", "0", 1);
    // Normal/hard's round-seven table entry is a -1 course sentinel.
    wr64::texture_capture_rider_select_hook(rdram, nullptr);
    assert(initialized_state == 0x0a);
    setenv("WR64_TEXTURE_CAPTURE_DIFFICULTY", "2", 1);
    wr64::texture_capture_rider_select_hook(rdram, nullptr);
    assert(initialized_state == 0x1e);
    assert(MEM_W(0, int32_t(0x801CB334)) == 7);
    setenv("WR64_TEXTURE_CAPTURE_SCENE", "audio", 1);
    unsetenv("WR64_TEXTURE_DUMP");
    wr64::texture_capture_rider_select_hook(rdram, nullptr);
    assert(initialized_state == 0x0a);
    setenv("WR64_TEXTURE_CAPTURE_POSTRACE", "results", 1);
    recomp_context ctx{};
    const auto disabled = memory;
    wr64::texture_capture_frame(rdram, &ctx);
    assert(memory == disabled);
    setenv("WR64_TEXTURE_DUMP", "capture-test-only", 1);
    MEM_W(0, int32_t(0x80151960)) = 1200;
    MEM_W(0, int32_t(0x800DB330)) = 0;
    MEM_W(4, int32_t(0x800DB330)) = 0;
    wr64::texture_capture_after_race(rdram);
    MEM_W(0, int32_t(0x800DAB24)) = 0x28;
    MEM_W(0, int32_t(0x801CE620)) = 4;
    MEM_W(0, int32_t(0x801982F0)) = 4;
    MEM_W(0, int32_t(0x801CE638)) = 1;
    ctx.r4 = 0x12345678;
    ctx.r2 = 0x76543210;
    MEM_W(0, int32_t(0x80151960)) = 1379;
    wr64::texture_capture_frame(rdram, &ctx);
    assert(MEM_W(0, int32_t(0x800DAB24)) == 0x28);
    MEM_W(0, int32_t(0x80151960)) = 1380;
    wr64::texture_capture_frame(rdram, &ctx);
    assert(MEM_W(0, int32_t(0x800DAB24)) == 0x34);
    assert(ctx.r4 == 0x12345678 && ctx.r2 == 0x76543210);
    assert(MEM_W(0x19c + 3 * 0x378, int32_t(0x801C2938)) == 456789);
    assert(MEM_W(0x2ec + 3 * 0x378, int32_t(0x801C2938)) == 0);
    assert(MEM_W(0x2f4 + 3 * 0x378, int32_t(0x801C2938)) == 1);
    // The delayed fourth-place fixture must preserve the native finish-HUD
    // transition while still presenting both buoy states during the lead-in.
    setenv("WR64_TEXTURE_CAPTURE_POSTRACE", "finish-hud", 1);
    setenv("WR64_TEXTURE_CAPTURE_FINISH_DELAY", "360", 1);
    setenv("WR64_TEXTURE_CAPTURE_FINISH_PLACE", "3", 1);
    setenv("WR64_TEXTURE_CAPTURE_POWER", "5", 1);
    setenv("WR64_TEXTURE_CAPTURE_MISSES", "2", 1);
    MEM_W(0, int32_t(0x80151960)) = 2000;
    wr64::texture_capture_after_race(rdram);
    MEM_W(0, int32_t(0x800DAB24)) = 0x28;
    MEM_W(0x2f4, int32_t(0x801C2938)) = 0;
    MEM_W(0, int32_t(0x80151960)) = 2359;
    wr64::texture_capture_frame(rdram, &ctx);
    assert(MEM_W(0x2f4, int32_t(0x801C2938)) == 0);
    assert(MEM_W(0x12c, int32_t(0x801C2938)) == 5);
    assert(MEM_W(0x138, int32_t(0x801C2938)) == 5);
    assert(MEM_W(0x134, int32_t(0x801C2938)) == 2);
    assert(MEM_W(0, int32_t(0x800D8174)) == 5);
    MEM_W(0, int32_t(0x80151960)) = 2360;
    wr64::texture_capture_frame(rdram, &ctx);
    assert(MEM_W(0, int32_t(0x800DAB24)) == 0x28);
    assert(MEM_W(4, int32_t(0x801C2938)) == 3);
    assert(MEM_W(0x19c, int32_t(0x801C2938)) == 456789);
    assert(MEM_W(4 + 0x378, int32_t(0x801C2938)) == 0);
    assert(MEM_W(0x2f4, int32_t(0x801C2938)) == 1);
    assert(ctx.r4 == 0x12345678 && ctx.r2 == 0x76543210);
    // The title fixture removes precisely one known appended display-list
    // call. Other calls and the returned end pointer remain untouched.
    ctx.r4 = int32_t(0x80001000);
    MEM_W(0, ctx.r4) = 0x06000000;
    MEM_W(4, ctx.r4) = 0x0805af88;
    MEM_W(8, ctx.r4) = 0x06000000;
    MEM_W(12, ctx.r4) = 0x0106f168;
    MEM_W(16, ctx.r4) = 0x04000000;
    MEM_W(20, ctx.r4) = 0x0805af88;
    MEM_W(0, int32_t(0x800DAB24)) = 2;
    setenv("WR64_TEXTURE_CAPTURE_TITLE_CLEAR", "1", 1);
    unsetenv("WR64_TEXTURE_DUMP");
    wr64::texture_capture_draw_hook(rdram, &ctx);
    assert(MEM_W(0, ctx.r4) == 0x06000000);
    setenv("WR64_TEXTURE_DUMP", "capture-test-only", 1);
    wr64::texture_capture_draw_hook(rdram, &ctx);
    assert(MEM_W(0, ctx.r4) == 0 && MEM_W(4, ctx.r4) == 0);
    assert(MEM_W(8, ctx.r4) == 0x06000000 && MEM_W(12, ctx.r4) == 0x0106f168);
    assert(MEM_W(16, ctx.r4) == 0x04000000 && MEM_W(20, ctx.r4) == 0x0805af88);
    assert(ctx.r2 == int32_t(0x80001018));
    return 0;
}
