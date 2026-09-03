// Phase 04: tell the runtime whenever the game DMAs code into RAM.
//
// librecomp calls load_overlays exactly once, for the boot region:
//
//     load_overlays(0x1000, (int32_t)entrypoint, 1024 * 1024);
//
// Everything after that is the game's business, and until the runtime is told a
// section has been loaded, none of its functions are in the address map. With
// use_lookup_for_all_function_calls every call is resolved by address, so a call
// into an overlay nobody announced fails with
//
//     Failed to find function at 0x802C5800
//
// The obvious hook was game_dma_copy, which is what SysMain_Thread and
// GameLoad_LoadCodeseg use. It is the wrong one. Tracing every transfer through
// it showed codeseg and the asset chunks going by and nothing ever landing at
// 0x802C5800, because GameLoad_LoadOverlay does not use it: it reads an entry
// from gOverlayTable and calls osPiStartDma directly with the overlay's ROM
// range and a hardcoded destination of 0x802C5800.
//
// So the hook belongs one level down, at osPiStartDma, where both paths meet.
// That covers every transfer the game makes without having to know which
// routine issued it -- and without reimplementing either of them, since this
// wraps librecomp's own implementation rather than replacing it.

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/overlays.hpp"

#include <cstdio>

// librecomp's implementation, which does the actual transfer.
extern "C" void osPiStartDma_recomp(uint8_t* rdram, recomp_context* ctx);

namespace wr64 {

// osPiStartDma(mq, priority, direction, devAddr, dramAddr, size, retQueue).
// The first four are in registers; the rest are on the stack, at the offsets
// librecomp's own implementation reads them from.
void pi_start_dma_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t direction = static_cast<uint32_t>(ctx->r6);
    const uint32_t dev_addr = static_cast<uint32_t>(ctx->r7);
    const gpr      dram_addr = MEM_W(0x10, ctx->r29);
    const uint32_t size = static_cast<uint32_t>(MEM_W(0x14, ctx->r29));

    // Read the arguments before the call: the callee owns the context and is
    // free to leave the registers holding anything afterwards.
    osPiStartDma_recomp(rdram, ctx);

    // OS_READ is 0: a transfer from cartridge into RDRAM. A write back to the
    // cartridge cannot bring code in, so announcing it would be meaningless.
    if (direction != 0) {
        return;
    }

    // Normalise the device address the way librecomp does, then convert to a
    // ROM file offset, which is the space the generated section table uses.
    const uint32_t physical_addr = (dev_addr | recomp::rom_base) & 0x1FFFFFFFu;
    const uint32_t rom_offset = physical_addr - recomp::rom_base;

    load_overlays(rom_offset, static_cast<int32_t>(dram_addr), size);

    static int calls = 0;
    if (++calls <= 20) {
        std::fprintf(stderr, "[wr64-dma] #%d rom 0x%06X -> ram 0x%08X size 0x%X\n",
                     calls, rom_offset, static_cast<uint32_t>(dram_addr), size);
        std::fflush(stderr);
    }
}

}  // namespace wr64

// ---------------------------------------------------------------------------
// game_dma_copy(rom, ram, size)
//
// The hook above catches GameLoad_LoadOverlay, which calls osPiStartDma
// directly. It does not help the other seven call sites, which go through this
// function -- and those turned out to matter for a reason beyond announcing the
// load.
//
// The original is asynchronous in shape: it starts a PI transfer and then waits
// on a message queue for completion. Recompiled faithfully, the data arrives
// eventually, but the renderer thread reached its first overlay call before the
// loader thread had transferred anything, and the lookup failed on an overlay
// that was legitimately not there yet.
//
// This does the transfer synchronously with librecomp's own ROM read, so the
// data is in place before the call returns. That is stronger than the game
// asks for and removes the race entirely.
//
// Skipping the queues is deliberate: nothing sends to them any more, so nothing
// must wait on them either. The original's opening check skips its wait while
// the queue is empty, which it now always is; its closing wait would block
// forever with no transfer in flight to complete it.

namespace recomp {
void do_rom_read(uint8_t* rdram, gpr ram_address, uint32_t physical_addr, size_t num_bytes);
}

extern "C" void game_dma_copy(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t rom_arg = static_cast<uint32_t>(ctx->r4);
    const uint32_t ram_arg = static_cast<uint32_t>(ctx->r5);
    const uint32_t size = static_cast<uint32_t>(ctx->r6);

    // a1 is physical: the original passes it through osPhysicalToVirtual before
    // the DMA. do_rom_read writes through MEM_B, which wants a KSEG0 address.
    const gpr ram_addr = static_cast<gpr>(static_cast<int32_t>(ram_arg | 0x80000000u));

    const uint32_t physical_addr = (rom_arg | recomp::rom_base) & 0x1FFFFFFFu;
    const uint32_t rom_offset = physical_addr - recomp::rom_base;

    recomp::do_rom_read(rdram, ram_addr, physical_addr, size);
    load_overlays(rom_offset, static_cast<int32_t>(ram_addr), size);

    ctx->r2 = 0;
}
