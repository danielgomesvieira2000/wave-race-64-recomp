// Inspect generated texture packs using the exact DDS parser vendored by RT64.
// clang++ -std=c++17 -Ilib/RT64/src/contrib/ddspp tools/tests/ddspp_probe.cpp \
//   -o build/texture-qa/ddspp-probe
#include "ddspp.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    for (int arg = 1; arg < argc; ++arg) {
        std::ifstream stream(argv[arg], std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)), {});
        if (bytes.size() < ddspp::MAX_HEADER_SIZE) return 2;
        ddspp::Descriptor desc{};
        if (ddspp::decode_header(bytes.data(), desc) != ddspp::Success) return 3;
        if (desc.format != ddspp::R8G8B8A8_UNORM || desc.type != ddspp::Texture2D ||
            desc.arraySize != 1 || desc.compressed || desc.srgb || desc.depth != 1) return 4;
        unsigned expectedMips = 1;
        for (unsigned n = std::max(desc.width, desc.height); n > 1; n >>= 1) ++expectedMips;
        if (desc.numMips != expectedMips) return 5;
        std::cout << "{\"width\":" << desc.width << ",\"height\":" << desc.height
                  << ",\"format\":" << unsigned(desc.format) << ",\"header_bytes\":" << desc.headerSize
                  << ",\"mip_count\":" << desc.numMips << ",\"mips\":[";
        unsigned end = 0;
        for (unsigned mip = 0; mip < desc.numMips; ++mip) {
            const unsigned w = std::max(desc.width >> mip, 1u);
            const unsigned h = std::max(desc.height >> mip, 1u);
            const unsigned offset = ddspp::get_offset(desc, mip, 0);
            const unsigned pitch = ddspp::get_row_pitch(desc, mip);
            if (offset != end || pitch != w * 4) return 6;
            end = offset + pitch * h;
            if (desc.headerSize + end > bytes.size()) return 7;
            if (mip) std::cout << ',';
            std::cout << "{\"width\":" << w << ",\"height\":" << h << ",\"offset\":"
                      << offset << ",\"row_pitch\":" << pitch << ",\"bytes\":" << pitch * h << '}';
        }
        if (desc.headerSize + end != bytes.size()) return 8;
        std::cout << "]}" << std::endl;
    }
    return 0;
}
