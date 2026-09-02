// Wave Race 64: Recompiled -- entry point.
//
// Phase 00 scaffolding. Today this binary identifies and verifies the ROM the
// user supplies; the runtime and the recompiled game code are wired in by
// phases 02 and 03 (see docs/PLAN.md), behind the WR64_WITH_* build options.

#include "wr64/rom.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

void print_usage(const char* argv0) {
    std::printf(
        "Wave Race 64: Recompiled\n"
        "\n"
        "Usage:\n"
        "  %s --identify <rom.z64>   Print header fields and verify the dump\n"
        "  %s --version              Print build configuration\n"
        "\n"
        "This program contains no game data. Supply your own legally obtained\n"
        "Wave Race 64 (USA) (Rev A) dump.\n",
        argv0, argv0);
}

void print_version() {
    std::printf("Wave Race 64: Recompiled -- phase 00 scaffolding\n");
    std::printf("  recompiled game code : %s\n",
#if WR64_WITH_RECOMPILED
        "linked");
#else
        "not built (configure with -DWR64_WITH_RECOMPILED=ON)");
#endif
    std::printf("  runtime + RT64       : %s\n",
#if WR64_WITH_RUNTIME
        "linked");
#else
        "not built (configure with -DWR64_WITH_RUNTIME=ON)");
#endif
}

int identify(const char* path) {
    wr64::RomHeader header;
    std::string error;

    if (!wr64::read_header(path, header, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::printf("%-16s %s\n", "file",     path);
    std::printf("%-16s %s\n", "format",   wr64::to_string(header.format).c_str());
    std::printf("%-16s %llu bytes\n", "size",
                static_cast<unsigned long long>(header.size_bytes));
    std::printf("%-16s %s\n", "name",     header.internal_name.c_str());
    std::printf("%-16s %s\n", "cart id",  header.cartridge_id.c_str());
    std::printf("%-16s %c\n", "region",   header.region ? header.region : '?');
    std::printf("%-16s %u\n", "revision", header.revision);
    std::printf("%-16s 0x%08X 0x%08X\n", "header crc", header.crc1, header.crc2);

    std::vector<std::string> problems;
    if (wr64::verify(header, problems)) {
        std::printf("\nThis dump matches the pinned target.\n");
        if (wr64::kTargetCrc1 == 0 && wr64::kTargetCrc2 == 0) {
            std::printf(
                "Note: no reference CRC is pinned yet. If you trust this dump, copy the\n"
                "header crc above into kTargetCrc1/kTargetCrc2 in include/wr64/rom.h so\n"
                "every later build checks against it.\n");
        }
        return 0;
    }

    std::printf("\nThis dump does NOT match the pinned target:\n");
    for (const std::string& problem : problems) {
        std::printf("  - %s\n", problem.c_str());
    }
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string command = argv[1];

    if (command == "--version" || command == "-v") {
        print_version();
        return 0;
    }

    if (command == "--identify") {
        if (argc < 3) {
            std::fprintf(stderr, "error: --identify needs a path to a .z64 dump\n");
            return 1;
        }
        return identify(argv[2]);
    }

    print_usage(argv[0]);
    return 1;
}
