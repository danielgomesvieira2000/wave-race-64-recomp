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
    uint8_t     revision = 0;      // header offset 0x3F          (1 = Rev A)
    uint64_t    size_bytes = 0;
};

// What the recompilation pipeline is pinned to. Every splat segment address,
// every symbol and every generated function assumes this exact dump.
inline constexpr char        kTargetCartridgeId[] = "WR";
inline constexpr char        kTargetRegion  = 'E';
inline constexpr uint8_t     kTargetRevision = 1;      // Rev A, a.k.a. v1.1
inline constexpr uint64_t    kTargetSizeBytes = 8u * 1024u * 1024u;

// CRC1/CRC2 of the reference dump. Zero means "not yet pinned": fill these in
// from `WaveRace64Recomp --identify <rom>` against a dump you have verified
// yourself, then rebuild. Until then only the structural checks above apply.
inline constexpr uint32_t    kTargetCrc1 = 0x00000000;
inline constexpr uint32_t    kTargetCrc2 = 0x00000000;

std::string to_string(RomFormat format);

// Reads the first 64 bytes and the file size. Does not load the ROM.
bool read_header(const std::filesystem::path& path, RomHeader& out, std::string& error);

// Structural verification against the pinned target above. Populates `problems`
// with one human-readable line per mismatch; returns true when there are none.
bool verify(const RomHeader& header, std::vector<std::string>& problems);

}  // namespace wr64
