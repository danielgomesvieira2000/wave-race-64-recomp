#include "recomp.h"
#include "wr64/water.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" void SysMain_SendGfxTaskSetMesg(uint8_t *, recomp_context *);
extern "C" void SysUtils_Srand(uint8_t *, recomp_context *);
extern "C" void func_8009345C(uint8_t *, recomp_context *);

namespace wr64 {
void water_test_course_hook(uint8_t *rdram,recomp_context *ctx) {
    // Use the original complete race initializer before its asset loads and
    // player/camera setup; never change a loaded course from the renderer.
    if (const char *text=std::getenv("WR64_TEST_COURSE")) {
        char *end=nullptr;
        const long course=std::strtol(text,&end,10);
        if (end!=text && *end=='\0' && course>=0 && course<=8) MEM_W(0,int32_t(0x800D8170))=int32_t(course);
    }
    if (const char *text=std::getenv("WR64_TEST_PLAYERS"); text && std::strcmp(text,"2")==0) {
        MEM_W(0,int32_t(0x801CE608))=1; // GMODE_2P_VS
        MEM_H(0,int32_t(0x801CE60C))=2;
        MEM_H(0,int32_t(0x801CE60E))=2;
    } else if (const char *text=std::getenv("WR64_TEST_MODE"); text && std::strcmp(text,"trials")==0) {
        MEM_W(0,int32_t(0x801CE608))=0;
        MEM_H(0,int32_t(0x801CE60C))=1;
        MEM_H(0,int32_t(0x801CE60E))=1;
    }
    func_8009345C(rdram,ctx);
    // Also runs for a same-course restart, where course ID and the global
    // game counter may stay unchanged. Clear visual histories explicitly.
    water::reset_for_race();
}

void water_test_seed_hook(uint8_t *rdram,recomp_context *ctx) {
    const auto saved=ctx->r4;
    if (const char *text=std::getenv("WR64_TEST_SEED")) {
        char *end=nullptr;
        const unsigned long seed=std::strtoul(text,&end,0);
        if (end!=text && *end=='\0' && seed<=UINT32_MAX) ctx->r4=int32_t(seed);
    }
    SysUtils_Srand(rdram,ctx);
    ctx->r4=saved;
}
void water_task_submit_hook(uint8_t *rdram, recomp_context *ctx) {
    // OSTask::t.data_ptr is the 32-bit cartridge pointer at offset 0x30.
    // The game has completed geometry and camera updates at this boundary.
    const uint32_t list = MEM_W(0x30,ctx->r4);
    water::publish_frame(rdram,list);
    SysMain_SendGfxTaskSetMesg(rdram,ctx);
}
}
