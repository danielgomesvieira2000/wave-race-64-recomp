#include "wr64/rom.h"

#include <array>
#include <cstdio>
#include <fstream>

namespace wr64 {
namespace {

constexpr uint32_t kMagicZ64 = 0x80371240;
constexpr uint32_t kMagicN64 = 0x40123780;
constexpr uint32_t kMagicV64 = 0x37804012;

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

std::string trim_trailing(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) {
        s.pop_back();
    }
    return s;
}

std::string hex32(uint32_t v) {
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "0x%08X", v);
    return std::string(buf.data());
}

}  // namespace

std::string to_string(RomFormat format) {
    switch (format) {
        case RomFormat::Z64: return "z64 (big endian)";
        case RomFormat::N64: return "n64 (little endian)";
        case RomFormat::V64: return "v64 (byte swapped)";
        default:             return "unrecognized";
    }
}

bool read_header(const std::filesystem::path& path, RomHeader& out, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "cannot read " + path.string() + ": " + ec.message();
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }

    std::array<uint8_t, 64> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
    if (file.gcount() != static_cast<std::streamsize>(header.size())) {
        error = "file is too small to contain an N64 header";
        return false;
    }

    const uint32_t magic = read_be32(header.data());
    switch (magic) {
        case kMagicZ64: out.format = RomFormat::Z64; break;
        case kMagicN64: out.format = RomFormat::N64; break;
        case kMagicV64: out.format = RomFormat::V64; break;
        default:
            error = "not an N64 ROM: leading word is " + hex32(magic);
            return false;
    }

    // Every field below is read at its big-endian position, so it is only
    // meaningful for a z64 dump. verify() rejects the other two formats.
    out.size_bytes    = size;
    out.crc1          = read_be32(header.data() + 0x10);
    out.crc2          = read_be32(header.data() + 0x14);
    out.internal_name = trim_trailing(std::string(reinterpret_cast<char*>(header.data() + 0x20), 20));
    out.cartridge_id  = std::string(reinterpret_cast<char*>(header.data() + 0x3C), 2);
    out.region        = static_cast<char>(header[0x3E]);
    out.revision      = header[0x3F];
    return true;
}

bool verify(const RomHeader& header, std::vector<std::string>& problems) {
    problems.clear();

    if (header.format != RomFormat::Z64) {
        problems.push_back(
            "dump is " + to_string(header.format) +
            "; convert it to z64 (big endian) before going any further -- the "
            "recompiler and every splat address assume big endian");
        // The remaining fields were read at big-endian offsets, so nothing
        // below this point can be trusted.
        return false;
    }

    if (header.size_bytes != kTargetSizeBytes) {
        problems.push_back(
            "size is " + std::to_string(header.size_bytes) + " bytes, expected " +
            std::to_string(kTargetSizeBytes) + " (8 MiB)");
    }

    if (header.cartridge_id != kTargetCartridgeId) {
        problems.push_back(
            "cartridge id is '" + header.cartridge_id + "', expected '" +
            kTargetCartridgeId + "' -- this is not Wave Race 64");
    }

    if (header.region != kTargetRegion) {
        problems.push_back(
            std::string("region is '") + header.region + "', expected 'E' (USA)");
    }

    if (header.revision != kTargetRevision) {
        problems.push_back(
            "revision is " + std::to_string(header.revision) + ", expected " +
            std::to_string(kTargetRevision) +
            " (Rev A / v1.1) -- other revisions have different code addresses");
    }

    if (kTargetCrc1 != 0 || kTargetCrc2 != 0) {
        if (header.crc1 != kTargetCrc1 || header.crc2 != kTargetCrc2) {
            problems.push_back(
                "header CRC is " + hex32(header.crc1) + " " + hex32(header.crc2) +
                ", expected " + hex32(kTargetCrc1) + " " + hex32(kTargetCrc2));
        }
    }

    return problems.empty();
}

}  // namespace wr64
