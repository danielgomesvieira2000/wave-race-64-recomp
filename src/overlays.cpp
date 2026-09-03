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

#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "librecomp/sections.h"

#include "recomp_overlays.inl"

namespace wr64 {

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

size_t overlay_section_count() {
    return ARRLEN(overlay_sections_by_index);
}

size_t code_section_count() {
    return ARRLEN(section_table);
}

}  // namespace wr64
