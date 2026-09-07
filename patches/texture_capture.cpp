// Process-only source texture capture fixtures. These overrides are inactive
// unless WR64_TEXTURE_DUMP and explicit WR64_TEXTURE_CAPTURE_* options are set.
// Race variants run before the original complete initializer. Scene fixtures
// use native initializers before the main loop's asset/overlay-load boundary.
// Native asset selection, relocation and display-list creation remain together.
#include "recomp.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void func_801EBD28(uint8_t*, recomp_context*);
extern "C" void func_801EC304(uint8_t*, recomp_context*);
extern "C" void func_801EC3AC(uint8_t*, recomp_context*);
extern "C" void func_801EC500(uint8_t*, recomp_context*);
extern "C" void func_801EC5B4(uint8_t*, recomp_context*);
extern "C" void func_801EC650(uint8_t*, recomp_context*);
extern "C" void func_801EC6EC(uint8_t*, recomp_context*);
extern "C" void func_801EC780(uint8_t*, recomp_context*);
extern "C" void func_801EB91C(uint8_t*, recomp_context*);
extern "C" void func_801EC0D4(uint8_t*, recomp_context*);
extern "C" void func_80077F5C(uint8_t*, recomp_context*);
extern "C" void func_80092CF0(uint8_t*, recomp_context*);

namespace wr64 {
namespace {
int32_t initialized_race_tick = -1;
bool postrace_entered = false;
bool capture_enabled() {
    const char* path = std::getenv("WR64_TEXTURE_DUMP");
    return path && *path;
}

int bounded_option(const char* name, int maximum) {
    const char* text = std::getenv(name);
    if (!text || !*text) return -1;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno || end == text || *end || value < 0 || value > maximum) {
        std::fprintf(stderr, "[texture-capture] ignoring invalid %s\n", name);
        return -1;
    }
    return static_cast<int>(value);
}
} // namespace

void texture_capture_rider_select_hook(uint8_t* rdram, recomp_context* ctx) {
    const char* scene = capture_enabled() ? std::getenv("WR64_TEXTURE_CAPTURE_SCENE") : nullptr;
    if (scene && std::strcmp(scene, "overview") == 0) {
        const int round = bounded_option("WR64_TEXTURE_CAPTURE_ROUND", 7);
        const int difficulty = bounded_option("WR64_TEXTURE_CAPTURE_DIFFICULTY", 2);
        // Normal/hard have a -1 sentinel at round seven; expert has eight
        // courses. Never feed that sentinel to the native course loader.
        if (round >= 0 && difficulty >= 0 && (difficulty == 2 || round < 7)) {
            MEM_W(0, int32_t(0x801CE608)) = 4;
            MEM_H(0, int32_t(0x801CE60C)) = 1;
            MEM_H(0, int32_t(0x801CE60E)) = 4;
            MEM_W(0, int32_t(0x801CB334)) = round;
            MEM_W(0, int32_t(0x801CB338)) = difficulty;
            func_801EB91C(rdram, ctx);
            std::fprintf(stderr, "[texture-capture] scene=overview expected_state=30 actual_state=%d round=%d course=%d\n",
                int(MEM_W(0, int32_t(0x800DAB24))), round, int(MEM_W(0, int32_t(0x800D8170))));
            return;
        }
    }
    using Initializer = void (*)(uint8_t*, recomp_context*);
    struct Scene { const char* name; int state; Initializer initialize; };
    static constexpr Scene scenes[] = {
        {"options", 0x3c, func_801EC304},
        {"names", 0x3e, func_801EC3AC},
        {"records", 0x42, func_801EC500},
        {"conditions", 0x44, func_801EC5B4},
        {"audio", 0x48, func_801EC650},
        {"erase-page", 0x46, func_801EC6EC},
        {"save-load-page", 0x40, func_801EC780},
    };
    if (scene) {
        for (const Scene& candidate : scenes) {
            if (std::strcmp(scene, candidate.name) != 0) continue;
            // Enter through the native complete initializer. The capture runner
            // releases every button before the page appears and issues no
            // further input, including no save, erase, or name-edit actions.
            candidate.initialize(rdram, ctx);
            std::fprintf(stderr, "[texture-capture] scene=%s expected_state=%d actual_state=%d\n",
                candidate.name, candidate.state, int(MEM_W(0, int32_t(0x800DAB24))));
            return;
        }
        std::fprintf(stderr, "[texture-capture] ignoring invalid capture scene\n");
    }
    func_801EBD28(rdram, ctx);
}

void texture_capture_before_race(uint8_t* rdram) {
    if (!capture_enabled()) return;

    const int rider = bounded_option("WR64_TEXTURE_CAPTURE_RIDER", 3);
    const int alternate = bounded_option("WR64_TEXTURE_CAPTURE_ALTERNATE", 1);
    const int difficulty = bounded_option("WR64_TEXTURE_CAPTURE_DIFFICULTY", 2);
    const char* mode = std::getenv("WR64_TEXTURE_CAPTURE_MODE");

    if (mode) {
        int game_mode = -1;
        int players = 1;
        int riders = 1;
        if (std::strcmp(mode, "trials") == 0) game_mode = 0;
        else if (std::strcmp(mode, "versus") == 0) {
            game_mode = 1;
            players = riders = 2;
        } else if (std::strcmp(mode, "championship") == 0) {
            game_mode = 4;
            riders = 4;
        } else if (std::strcmp(mode, "stunt") == 0) game_mode = 11;
        if (game_mode >= 0) {
            // Game_801CE608 from the Rev A native initializer.
            MEM_W(0, int32_t(0x801CE608)) = game_mode;
            MEM_H(0, int32_t(0x801CE60C)) = players;
            MEM_H(0, int32_t(0x801CE60E)) = riders;
        } else {
            std::fprintf(stderr, "[texture-capture] ignoring invalid capture mode\n");
        }
    }

    if (rider >= 0) {
        // D_800DA9B0 maps race slots to rider identities. Keep it a permutation
        // rather than assigning the same identity to multiple race slots.
        for (int slot = 0; slot < 4; ++slot)
            MEM_W(slot * 4, int32_t(0x800DA9B0)) = (rider + slot) % 4;
        MEM_W(0, int32_t(0x800D48DC)) = 0;
        MEM_W(0, int32_t(0x800D48E0)) = 1;
    }
    if (alternate >= 0) {
        MEM_H(0, int32_t(0x801CE6F4)) = alternate;
        MEM_H(2, int32_t(0x801CE6F4)) = alternate;
    }
    if (difficulty >= 0) MEM_W(0, int32_t(0x801CB338)) = difficulty;
}

void texture_capture_after_race(uint8_t* rdram) {
    if (!capture_enabled()) return;
    initialized_race_tick = MEM_W(0, int32_t(0x80151960));
    postrace_entered = false;

    // The native rider loader assembles this terminated DMA request list.
    // Logging the table gives the capture report independently observed bank
    // coverage without treating a loaded bank as proof all its textures drew.
    std::fprintf(stderr, "[texture-capture] race course=%d mode=%d players=%d difficulty=%d\n",
        int(MEM_W(0, int32_t(0x800D8170))), int(MEM_W(0, int32_t(0x801CE608))),
        int(MEM_H(0, int32_t(0x801CE60C))), int(MEM_W(0, int32_t(0x801CB338))));
    for (int slot = 0; slot < 8; ++slot) {
        const uint32_t start = MEM_W(slot * 16, int32_t(0x800DB330));
        const uint32_t end = MEM_W(slot * 16 + 4, int32_t(0x800DB330));
        if (start == 0 && end == 0) break;
        if (start >= end || end > 0x800000) {
            std::fprintf(stderr, "[texture-capture] unexpected rider DMA entry slot=%d\n", slot);
            break;
        }
        std::fprintf(stderr, "[texture-capture] rider-bank slot=%d rom_start=%08x rom_end=%08x\n",
            slot, start, end);
    }
}

void texture_capture_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!capture_enabled() || initialized_race_tick < 0) return;
    // Wait for the original race overlay and all four actors to initialize.
    // This runs after building the game display list and before the main loop's
    // native asset/overlay loads. Submission is too late for scene transitions.
    const int32_t tick = MEM_W(0, int32_t(0x80151960));
    if (tick - initialized_race_tick >= 60 && MEM_W(0, int32_t(0x800DAB24)) == 0x28) {
        const int power = bounded_option("WR64_TEXTURE_CAPTURE_POWER", 5);
        if (power >= 0) {
            MEM_W(0x12c, int32_t(0x801C2938)) = power;
            MEM_W(0x138, int32_t(0x801C2938)) = power;
        }
        const int misses = bounded_option("WR64_TEXTURE_CAPTURE_MISSES", 4);
        if (misses >= 0) {
            MEM_W(0, int32_t(0x800D8174)) = 5;
            MEM_W(0x134, int32_t(0x801C2938)) = misses;
        }
    }
    if (postrace_entered) return;
    const char* scene = std::getenv("WR64_TEXTURE_CAPTURE_POSTRACE");
    if (!scene || (std::strcmp(scene, "results") != 0 && std::strcmp(scene, "finish-hud") != 0)) return;
    const int requested_delay = bounded_option("WR64_TEXTURE_CAPTURE_FINISH_DELAY", 1200);
    const int delay = requested_delay >= 180 ? requested_delay : 180;
    if (tick - initialized_race_tick < delay || MEM_W(0, int32_t(0x800DAB24)) != 0x28 ||
        MEM_W(0, int32_t(0x801CE620)) != 4 || MEM_W(0, int32_t(0x801982F0)) != 4 ||
        MEM_W(0, int32_t(0x801CE638)) != 1) return;
    postrace_entered = true;
    // The stock input route includes Dolphin Park's introduction before the
    // process-only race course override. Match championship progression to
    // this fixture's actual race instead of taking the tutorial return path.
    MEM_W(0, int32_t(0x801CB330)) = MEM_W(0, int32_t(0x800D8170));
    // Seed valid completed-race records spanning every decimal digit. The
    // native results initializer retains ownership of ranking and asset loads.
    // +0x2f4 is completion; +0x2ec is retirement/disqualification and must be
    // zero or the results drawer emits RETIRE instead of the time.
    // The capture runner uses a portable bundle, isolating any record saves.
    constexpr int32_t times[] = {123456, 234567, 345678, 456789};
    const int requested_place = bounded_option("WR64_TEXTURE_CAPTURE_FINISH_PLACE", 3);
    const int first_place = requested_place >= 0 ? requested_place : 0;
    const recomp_context saved = *ctx;
    for (int rider = 0; rider < 4; ++rider) {
        const int32_t address = int32_t(0x801C2938) + rider * 0x378;
        const int place = (rider + first_place) % 4;
        MEM_W(0, address) = 3;
        MEM_W(4, address) = place;
        MEM_W(0x19c, address) = times[place];
        MEM_W(0x2ec, address) = 0;
        MEM_W(0x2f4, address) = 1;
        for (int lap = 0; lap < 3; ++lap) MEM_W(0x178 + lap * 4, address) = times[place] / 3;
        const float finish_order = float(40000 - place * 10000);
        uint32_t finish_order_bits;
        std::memcpy(&finish_order_bits, &finish_order, sizeof(finish_order_bits));
        MEM_W(0x32c, address) = finish_order_bits;
    }
    if (std::strcmp(scene, "results") == 0) {
        func_80077F5C(rdram, ctx);
        func_801EC0D4(rdram, ctx);
    }
    *ctx = saved;
    std::fprintf(stderr, "[texture-capture] postrace=%s tick=%d actual_state=%d finish_flags=%d,%d,%d,%d retire_flags=%d,%d,%d,%d\n",
        scene, tick, int(MEM_W(0, int32_t(0x800DAB24))),
        int(MEM_W(0x2f4, int32_t(0x801C2938))), int(MEM_W(0x2f4 + 0x378, int32_t(0x801C2938))),
        int(MEM_W(0x2f4 + 2 * 0x378, int32_t(0x801C2938))), int(MEM_W(0x2f4 + 3 * 0x378, int32_t(0x801C2938))),
        int(MEM_W(0x2ec, int32_t(0x801C2938))), int(MEM_W(0x2ec + 0x378, int32_t(0x801C2938))),
        int(MEM_W(0x2ec + 2 * 0x378, int32_t(0x801C2938))), int(MEM_W(0x2ec + 3 * 0x378, int32_t(0x801C2938))));
}

void texture_capture_draw_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t begin = uint32_t(ctx->r4);
    func_80092CF0(rdram, ctx);
    const char* clear_title = capture_enabled() ? std::getenv("WR64_TEXTURE_CAPTURE_TITLE_CLEAR") : nullptr;
    if (clear_title && std::strcmp(clear_title, "1") == 0 && MEM_W(0, int32_t(0x800DAB24)) == 2) {
        const uint32_t end = uint32_t(ctx->r2);
        if (begin >= 0x80000000u && end >= begin && end <= 0x80800000u && end - begin <= 0x100000u) {
            for (uint32_t address = begin; address + 8 <= end; address += 8) {
                // func_i0_802C5800 appends this title-logo display list after
                // func_8009328C has drawn the course. Omit only this call so
                // the moving Dolphin Park arch can be inspected unobstructed.
                if (uint32_t(MEM_W(0, int32_t(address))) == 0x06000000u &&
                    uint32_t(MEM_W(4, int32_t(address))) == 0x0805af88u) {
                    MEM_W(0, int32_t(address)) = 0; // G_SPNOOP
                    MEM_W(4, int32_t(address)) = 0;
                    static bool logged = false;
                    if (!logged) std::fprintf(stderr, "[texture-capture] title logo omitted for course-sign review\n");
                    logged = true;
                }
            }
        }
    }
    texture_capture_frame(rdram, ctx);
}
} // namespace wr64
