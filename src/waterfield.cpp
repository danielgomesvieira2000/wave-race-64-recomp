// See include/wr64/waterfield.h for what this is and why.

#include "wr64/waterfield.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// The game's water height at a world XZ. Recompiled; declaring it is all it
// takes to call it.
extern "C" void func_8004D30C(uint8_t* rdram, recomp_context* ctx);

namespace wr64::waterfield {
namespace {

// RDRAM is kept as host-endian 32-bit words, so a word is a plain copy and a
// halfword has its address XORed with 2 -- the same convention src/water.cpp
// and the generated MEM_H use. It matters here because the field is pairs of
// s16, so a decoder that ignores it reads height where age is.
uint32_t word(const uint8_t* rdram, uint32_t address) {
    uint32_t v;
    std::memcpy(&v, rdram + (address & 0x7FFFFF), sizeof v);
    return v;
}

float real(const uint8_t* rdram, uint32_t address) {
    float v;
    std::memcpy(&v, rdram + (address & 0x7FFFFF), sizeof v);
    return v;
}

const char* prefix() {
    static const char* value = [] {
        const char* p = std::getenv("WR64_WATER_FIELD");
        return (p != nullptr && p[0] != 0) ? p : nullptr;
    }();
    return value;
}

// How many calls into the hook to wait before dumping. The field starts flat
// and fills as the craft disturb it, so a dump on the first race frame is a
// dump of zeroes; the default is long enough for a boat to have been somewhere.
uint32_t delay() {
    static const uint32_t value = [] {
        const char* p = std::getenv("WR64_WATER_FIELD_DELAY");
        const long n = (p != nullptr) ? std::strtol(p, nullptr, 10) : 0;
        return n > 0 ? uint32_t(n) : 600u;
    }();
    return value;
}

// The game's own answer for the height at (x, z).
//
// The context is *copied* rather than borrowed. func_8004D30C adjusts $sp and
// saves $ra, which through a borrowed context would write into the live frame;
// a copy shares the same stack pointer, so the callee writes below it, which is
// what any ordinary call does.
//
// f_odd has to be repointed after the copy. It points into the context it
// belongs to -- librecomp sets it to &ctx->f0.u32h, or &ctx->f1.u32l under
// mips3 float mode -- so a struct copy leaves it aimed at the *original*
// context. Nothing fails until the callee touches an odd float register, which
// makes it exactly the kind of bug that shows up somewhere else entirely.
float height_at(uint8_t* rdram, const recomp_context* ctx, float x, float z) {
    recomp_context probe = *ctx;
    probe.f_odd = probe.mips3_float_mode ? &probe.f1.u32l : &probe.f0.u32h;
    probe.f12.fl = x;
    probe.f14.fl = z;
    func_8004D30C(rdram, &probe);
    return probe.f0.fl;
}

bool write_field(const std::string& path, const uint8_t* rdram) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    // Written in RDRAM order, host word endianness and all, and decoded on the
    // other side. Converting here would mean two places to get it wrong.
    const size_t written =
        std::fwrite(rdram + (kFieldAddress & 0x7FFFFF), 1, kFieldBytes, f);
    std::fclose(f);
    return written == kFieldBytes;
}

// Three grids, because they answer different questions.
//
//   lattice: probes on the *oblique* cell corners, which is the only grid a
//            decode of the array has to reproduce exactly. The first attempt
//            stepped 64 units along world x and z, and almost none of those
//            points land on a corner: the lattice is sheared, so a
//            world-aligned grid walks across cells rather than along them.
//            That gave small errors everywhere -- a median of 0.04 against a
//            field spanning 100 units -- which looked like a broken decode and
//            was a broken measurement.
//
//            u = x + z/sqrt(3), v = 2z/sqrt(3)  inverts to
//            x = u - v/2,       z = v * sqrt(3)/2
//
//   centre:  the middle of each cell, where the game has to be interpolating
//            between corners. The gap between this and the corners is that
//            interpolation, which the array alone cannot show.
//
//   fine:    eight units through one neighbourhood, for the shape of the
//            interpolation across a cell boundary.
struct Grid {
    const char* name;
    float half_extent;   // cells for the lattice grids, world units for fine
    float step;
    bool  lattice;
    float cell_offset;   // 0 at corners, 0.5 at cell centres
};

constexpr Grid kGrids[] = {
    { "lattice", 24.0f,  1.0f, true,  0.0f },
    { "centre",  24.0f,  1.0f, true,  0.5f },
    { "fine",   512.0f,  8.0f, false, 0.0f },
};

constexpr float kSqrt3Over2 = 0.8660254f;

bool write_probes(const std::string& path, uint8_t* rdram, const recomp_context* ctx,
                  float centre_x, float centre_z) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) return false;
    std::fprintf(f, "# The game's own water height, from func_8004D30C(x, z).\n"
                    "# A decode of D_80162420 that reproduces these everywhere can be\n"
                    "# trusted to build geometry from. See docs/GAME-INTERNALS.md.\n"
                    "grid,x,z,height\n");
    size_t count = 0;
    for (const Grid& grid : kGrids) {
        if (grid.lattice) {
            // The cell the craft is in, so the probes cover water it has been
            // disturbing. Same arithmetic as func_8004F3D4, before the wrap.
            const int centre_u = int(centre_x + centre_z * 0.57735026f) >> 6;
            const int centre_v = int(centre_z * 1.1547005f) >> 6;
            const int reach = int(grid.half_extent);
            for (int dv = -reach; dv <= reach; ++dv) {
                for (int du = -reach; du <= reach; ++du) {
                    const float u =
                        (float(centre_u + du) + grid.cell_offset) * float(kCellSize);
                    const float v =
                        (float(centre_v + dv) + grid.cell_offset) * float(kCellSize);
                    const float z = v * kSqrt3Over2;
                    const float x = u - v * 0.5f;
                    std::fprintf(f, "%s,%.4f,%.4f,%.6f\n", grid.name, x, z,
                                 height_at(rdram, ctx, x, z));
                    ++count;
                }
            }
            continue;
        }
        const float origin_x =
            std::floor((centre_x - grid.half_extent) / grid.step) * grid.step;
        const float origin_z =
            std::floor((centre_z - grid.half_extent) / grid.step) * grid.step;
        const int steps = int((grid.half_extent * 2.0f) / grid.step) + 1;
        for (int iz = 0; iz < steps; ++iz) {
            for (int ix = 0; ix < steps; ++ix) {
                const float x = origin_x + float(ix) * grid.step;
                const float z = origin_z + float(iz) * grid.step;
                std::fprintf(f, "%s,%.4f,%.4f,%.6f\n", grid.name, x, z,
                             height_at(rdram, ctx, x, z));
                ++count;
            }
        }
    }
    std::fclose(f);
    std::fprintf(stderr, "[wr64-field] %zu probes\n", count);
    return true;
}

}  // namespace

void maybe_dump(uint8_t* rdram, recomp_context* ctx, uint32_t tick, uint32_t course) {
    if (prefix() == nullptr) return;

    static bool done = false;
    static uint32_t calls = 0;
    if (done) return;
    if (++calls < delay()) return;

    // Where the player is, so the fine grid lands on water a craft has been
    // disturbing rather than on a flat corner of the field. Same offsets
    // src/water.cpp reads the craft world position from.
    const float craft_x = real(rdram, 0x80192690 + 0x44);
    const float craft_z = real(rdram, 0x80192690 + 0x4C);

    const std::string base = prefix();
    const bool field_ok  = write_field(base + "-field.bin", rdram);
    const bool probes_ok = write_probes(base + "-probe.csv", rdram, ctx, craft_x, craft_z);

    if (std::FILE* meta = std::fopen((base + "-meta.txt").c_str(), "w")) {
        std::fprintf(meta,
            "field_address   0x%08X\n"
            "field_rows      %u\n"
            "field_cols      %u\n"
            "cell_size       %u\n"
            "field_bytes     %u\n"
            "water_level     %d\n"
            "course          %u\n"
            "tick            %u\n"
            "call            %u\n"
            "craft0_x        %.3f\n"
            "craft0_z        %.3f\n",
            kFieldAddress, kFieldRows, kFieldCols, kCellSize, kFieldBytes,
            int32_t(word(rdram, kWaterLevelAddress)), course, tick, calls,
            craft_x, craft_z);
        std::fclose(meta);
    }

    done = true;
    std::fprintf(stderr, "[wr64-field] dumped to %s-{field.bin,probe.csv,meta.txt}%s\n",
                 base.c_str(), (field_ok && probes_ok) ? "" : "  (INCOMPLETE)");
    std::fflush(stderr);
}

}  // namespace wr64::waterfield
