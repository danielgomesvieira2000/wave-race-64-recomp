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
    uint8_t     revision = 0;      // header offset 0x3F          (0 = v1.0, 1 = Rev A)
    uint64_t    size_bytes = 0;
};

// What the recompilation pipeline is pinned to. Every splat segment address,
// every symbol and every generated function assumes this exact dump.
//
// This project targets the original US release, v1.0 (revision 0), not Rev A.
// That is a deliberate departure from the existing Wave Race 64 reverse
// engineering, all of which targets Rev A: LLONSIT's decomp declares support
// for "US, Rev1" only. We target v1.0 because that is the dump available to
// this project, and because the revisions appear to share a link layout -- the
// decomp's Rev A `entry` segment is at vram 0x80046800, which is exactly the
// entry point in the v1.0 cartridge header. Rev A therefore remains useful as
// a symbol donor even though it is not the build target. See docs/PLAN.md.
inline constexpr char        kTargetCartridgeId[] = "WR";
inline constexpr char        kTargetRegion  = 'E';
inline constexpr uint8_t     kTargetRevision = 0;      // v1.0, the original US release
inline constexpr uint64_t    kTargetSizeBytes = 8u * 1024u * 1024u;

// CRC1/CRC2 of the reference dump, taken from the header of the dump this
// project was pinned against:
//
//   sha1  887ab588c2ecc64c52fb2065f06b0a1ee4af13dc
//   md5   ae480013f39d4aec86eea1b4995600d1
//
// These identify the dump; they do not prove it is a good one. Cross-check the
// sha1 against a No-Intro DAT if you want independent confirmation. Setting
// both values to zero disables the CRC check and leaves only the structural
// checks above.
inline constexpr uint32_t    kTargetCrc1 = 0x7DE11F53;
inline constexpr uint32_t    kTargetCrc2 = 0x74872F9D;

std::string to_string(RomFormat format);

// Reads the first 64 bytes and the file size. Does not load the ROM.
bool read_header(const std::filesystem::path& path, RomHeader& out, std::string& error);

// Structural verification against the pinned target above. Populates `problems`
// with one human-readable line per mismatch; returns true when there are none.
bool verify(const RomHeader& header, std::vector<std::string>& problems);

}  // namespace wr64
