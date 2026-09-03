// Phase 03: hand the generated section and overlay tables to librecomp.
//
// N64Recomp emits recomp_overlays.inl containing three things: a section_table
// describing every code section (ROM address, RAM address, size, its functions
// and its relocations), the total section count, and the list of which section
// indices are overlays.
//
// Those symbols are declared `static` in the generated file, so this is the one
// translation unit that can see them -- hence the #include of a .inl rather
// than linking against it.
//
// The overlay list is what makes the relocations meaningful: sections in it all
// load to the same address (0x802C5800 in this game) and are relocated as they
// are swapped in. Both prior public Wave Race 64 ports had to leave this empty
// because their ELF carried no relocation data.

#include <cstddef>
#include <cstdio>
#include <unordered_map>

#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "librecomp/sections.h"

#include "recomp_overlays.inl"
#include "runtime_funcs.inl"

extern "C" void osPiStartDma_recomp(uint8_t* rdram, recomp_context* ctx);


namespace wr64 {

void pi_start_dma_hook(uint8_t* rdram, recomp_context* ctx);

void register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections{};
    sections.code_sections = section_table;
    sections.num_code_sections = ARRLEN(section_table);
    sections.total_num_sections = num_sections;

    recomp::overlays::overlays_by_index_t overlays{};
    overlays.table = overlay_sections_by_index;
    overlays.len = ARRLEN(overlay_sections_by_index);

    recomp::overlays::register_overlays(sections, overlays);
}

void register_runtime_functions() {
    // Register the libultra functions the runtime provides at the addresses
    // they had on the cartridge.
    //
    // With use_lookup_for_all_function_calls every call is resolved by address
    // rather than by symbol, which is what makes overlay dispatch work. It also
    // leaves these invisible: N64Recomp renames them to <name>_recomp and never
    // puts them in a section table, because with direct calls nothing looked
    // them up by address. The first one the game called failed with "Failed to
    // find function at 0x800C6300", which is osDpSetStatus.
    //
    // This has to run from the game's on_init hook, not at startup:
    // init_overlays() begins with func_map.clear(), so anything registered
    // before it is silently discarded. librecomp calls init_overlays() well
    // before it calls on_init_callback.
    uint32_t pi_start_dma_addr = 0;
    for (const auto& entry : runtime_provided_funcs) {
        if (entry.func == osPiStartDma_recomp) {
            pi_start_dma_addr = entry.ram_addr;
        }
        recomp::overlays::add_loaded_function(static_cast<int32_t>(entry.ram_addr),
                                              entry.func);
    }

    // Register every function of every non-overlay section.
    //
    // init_overlays() does not populate the function map at all; it only records
    // where each section lives. Functions are added by load_overlay() when the
    // game DMAs a section in. That is sufficient when calls are direct, because
    // only overlays are ever looked up by address -- but with
    // use_lookup_for_all_function_calls every call is a lookup, and the resident
    // sections are never "loaded" through PI DMA, so nothing ever registers
    // them. main_segment holds most of the game.
    //
    // Overlay sections are deliberately skipped: several share one address, so
    // registering them here would install whichever happened to come last and
    // defeat the dynamic dispatch this is all in aid of. They are registered as
    // they are loaded, which is correct.
    //
    // A section is identified as an overlay by its address being shared, not by
    // consulting overlay_sections_by_index. That table's values are not section
    // indices -- they run 3..21 while main_segment is index 8 and codeseg is 11
    // -- so using it as an index set silently skips most of the game. Sharing an
    // address is also the property that actually matters here.
    std::unordered_map<uint32_t, int> address_uses;
    for (size_t i = 0; i < ARRLEN(section_table); ++i) {
        address_uses[section_table[i].ram_addr]++;
    }

    size_t registered = 0;
    for (size_t i = 0; i < ARRLEN(section_table); ++i) {
        const SectionTableEntry& section = section_table[i];
        if (address_uses[section.ram_addr] > 1) {
            continue;
        }
        for (size_t f = 0; f < section.num_funcs; ++f) {
            const FuncEntry& func = section.funcs[f];
            recomp::overlays::add_loaded_function(
                static_cast<int32_t>(section.ram_addr + func.offset), func.func);
            ++registered;
        }
    }

    // Redirect osPiStartDma through our wrapper, which announces each transfer
    // to the runtime. Every DMA the game makes passes through it, including the
    // overlay loads that bypass game_dma_copy entirely -- see patches/dma.cpp.
    //
    // This must come last. osPiStartDma is a reimplemented libultra function,
    // so it appears in both tables above, and the resident pass would otherwise
    // overwrite the hook with the raw implementation. That is exactly what was
    // happening: GameLoad_LoadOverlay ran, issued its DMA, and nothing was ever
    // announced, because the hook had been quietly replaced moments after it
    // was installed.

    if (pi_start_dma_addr != 0) {
        recomp::overlays::add_loaded_function(static_cast<int32_t>(pi_start_dma_addr),
                                              pi_start_dma_hook);
    }

    std::fprintf(stderr,
                 "[wr64] registered %zu runtime-provided and %zu resident functions"
                 " (PI DMA hook at 0x%08X)\n",
                 sizeof(runtime_provided_funcs) / sizeof(runtime_provided_funcs[0]),
                 registered, pi_start_dma_addr);
    std::fflush(stderr);
}

size_t overlay_section_count() {
    return ARRLEN(overlay_sections_by_index);
}

size_t code_section_count() {
    return ARRLEN(section_table);
}

}  // namespace wr64



