#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wr64 {

// Byte order of an N64 dump, identified from the first word of the header.
enum class RomFormat {
    Unknown,
    Z64,  // big endian,     0x80371240 -- the only format the toolchain accepts
    N64,  // little endian,  0x40123780
    V64,  // byte swapped,   0x37804012
};

// The fields of the 64-byte N64 cartridge header that identify a dump.
struct RomHeader {
    RomFormat   format = RomFormat::Unknown;
    uint32_t    crc1 = 0;          // header offset 0x10
    uint32_t    crc2 = 0;          // header offset 0x14
    std::string internal_name;     // header offset 0x20, 20 bytes, space padded
    std::string cartridge_id;      // header offset 0x3C, 2 bytes  ("WR" for Wave Race)
    char        region = '\0';     // header offset 0x3E          ('E' = USA)
    uint8_t     revision = 0;      // header offset 0x3F          (1 = Rev A, 0 = v1.0)
    uint64_t    size_bytes = 0;
};

// What the recompilation pipeline is pinned to. Every splat segment address,
// every symbol and every generated function assumes this exact dump.
//
// This project targets Wave Race 64 (USA) (Rev A), revision 1, a.k.a. v1.1.
//
// It briefly targeted v1.0 instead, when that was the only dump available. That
// is no longer so, and Rev A is the right target by a wide margin: it is the
// revision every piece of existing Wave Race 64 reverse engineering was built
// for, and our dump is byte-identical to the one LLONSIT's decomp pins, so its
// segment map and symbol corpus apply unmodified. docs/PHASE01-FINDINGS.md
// records what we measured about v1.0 in the meantime -- the two revisions are
// related by piecewise offsets, not a constant -- which is why targeting v1.0
// would have cost weeks of sequence alignment for no benefit.
inline constexpr char        kTargetCartridgeId[] = "WR";
inline constexpr char        kTargetRegion  = 'E';
inline constexpr uint8_t     kTargetRevision = 1;      // Rev A / v1.1
inline constexpr uint64_t    kTargetSizeBytes = 8u * 1024u * 1024u;

// CRC1/CRC2 from the header of the pinned dump:
//
//   sha1  508dfc2d4caa42b6f6de5263d0aed5e44ac7966a
//
// That sha1 is the value LLONSIT's decomp pins in waverace64.us.rev1.sha1, so
// a dump matching it is known-good rather than merely self-consistent. Setting
// both CRC values to zero disables the check and leaves only structural checks.
inline constexpr uint32_t    kTargetCrc1 = 0x492F4B61;
inline constexpr uint32_t    kTargetCrc2 = 0x04E5146A;

std::string to_string(RomFormat format);

// Reads the first 64 bytes and the file size. Does not load the ROM.
bool read_header(const std::filesystem::path& path, RomHeader& out, std::string& error);

// Structural verification against the pinned target above. Populates `problems`
// with one human-readable line per mismatch; returns true when there are none.
bool verify(const RomHeader& header, std::vector<std::string>& problems);

}  // namespace wr64
