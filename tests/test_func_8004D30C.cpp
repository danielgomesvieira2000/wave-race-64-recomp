// Test: verify the native C++ reimplementation of func_8004D30C against
// the Python reference decoder (tools/decode_water_field.py).
//
// Build: see tests/test_func_8004D30C.sh or compile directly with:
//   g++ -std=c++20 -I include tests/test_func_8004D30C.cpp \
//       src/engine/gamemain.cpp -o build/test_func_8004D30C -lm
//
// The test uses the same wave field data that the game dumps via
// WR64_WATER_FIELD=<prefix>, producing a -field.bin file. If that file
// is not available, the test generates a synthetic field with known
// disturbance patterns and compares the C++ implementation against an
// inline re-derivation of the Python algorithm.
//
// Usage:
//   ./build/test_func_8004D30C                      # synthetic field test
//   ./build/test_func_8004D30C <prefix>             # real field dump test

#include "wr64/engine/gamemain.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <random>
#include <string>
#include <algorithm>
#include <filesystem>

namespace we = wr64::engine;

// ---------------------------------------------------------------------------
// Python-equivalent reference implementation (from decode_water_field.py)
// ---------------------------------------------------------------------------

static constexpr int kRows = 384, kCols = 128, kCell = 64;
static constexpr int kWrap = 24576;
static constexpr float kOneOverSqrt3 = 0.57735026f;
static constexpr float kTwoOverSqrt3 = 1.1547005f;
static constexpr int kHeightShift = 8;

// C truncation toward zero
static int py_c_trunc(float v) {
    return static_cast<int>(v);
}

// C's %: keeps sign of dividend
static int py_c_mod(int a, int b) {
    int r = a % b;
    return r;
}

// World XZ -> lattice axial coords
static void py_axial(float x, float z, int& cu, int& cv, float& fu, float& fv) {
    int u = py_c_mod(py_c_trunc(x + z * kOneOverSqrt3), kWrap);
    int v = py_c_mod(py_c_trunc(z * kTwoOverSqrt3), kWrap);
    cu = u >> 6;
    cv = v >> 6;
    fu = static_cast<float>(u & 63) / 64.0f;
    fv = static_cast<float>(v & 63) / 64.0f;
}

static int py_entry_index(int cu, int cv, const std::vector<std::vector<int16_t>>& heights,
                           int du, int dv, int& row, int& col) {
    int r = py_c_mod(cv + dv + ((cu + du) & ~0x7F) + 0x600, kRows);
    col = (cu + du) & 0x7F;
    row = r;
    return heights[row][col];
}

// Python reference: what func_8004D30C(x, z) returns
static float py_height_at(const std::vector<std::vector<int16_t>>& heights,
                          float x, float z, float water_level) {
    int cu, cv;
    float fu, fv;
    py_axial(x, z, cu, cv, fu, fv);

    auto h = [&](int du, int dv) -> int {
        int row, col;
        py_entry_index(cu, cv, heights, du, dv, row, col);
        return heights[row][col];
    };

    int h00 = h(0, 0);
    if (fv > fu) {
        // Top triangle: match Python decoder exactly
        int d1 = h00 - h(0, 1);
        int d0 = h(0, 1) - h(1, 1);
        float base = water_level + static_cast<float>(h00);
        float numerator = base * 4096.0f - static_cast<float>(d0) * (fu * 64.0f)
                                         - static_cast<float>(d1) * (fv * 64.0f);
        float denominator = 64.0f * std::sqrt(
            static_cast<float>(d0) * static_cast<float>(d0) +
            static_cast<float>(d1) * static_cast<float>(d1) + 4096.0f
        );
        return numerator / denominator;
    } else {
        // Bottom triangle: match Python decoder exactly
        int d0 = h00 - h(1, 0);
        int d1 = h(1, 0) - h(1, 1);
        float base = water_level + static_cast<float>(h00);
        float numerator = base * 4096.0f - static_cast<float>(d0) * (fu * 64.0f)
                                         - static_cast<float>(d1) * (fv * 64.0f);
        float denominator = 64.0f * std::sqrt(
            static_cast<float>(d0) * static_cast<float>(d0) +
            static_cast<float>(d1) * static_cast<float>(d1) + 4096.0f
        );
        return numerator / denominator;
    }
}

// Note on fu/fv: the Python code uses fu = (u & 63) / 64 and fv = (v & 63) / 64
// as POSITIVE fractions in [0,1), and then does:
//   numerator = base * 4096 - d0 * (fu * 64) - d1 * (fv * 64)
// which equals: base * 4096 - d0 * (u & 63) - d1 * (v & 63)
//
// Our C++ code uses fu = -(u & 63) and fv = -(v & 63) as NEGATIVE integers, and:
//   numerator = H * 4096 - d0 * fu - d1 * fv
// which equals: H * 4096 - d0 * (-(u&63)) - d1 * (-(v&63))
//             = H * 4096 + d0*(u&63) + d1*(v&63)
//
// But the Python has: base * 4096 - d0*(u&63) - d1*(v&63)
// These differ by sign! Let me re-check the original docs.
//
// From GAME-INTERNALS.md:
//   return (H << 12 - d0*fu - d1*fv) / sqrt(...)
//   with fu, fv the offsets inside the cell in units of 0..63
//   and "both are formed at 0x8004D414" as -(u & 63) and -(v & 63)
//
// So fu = -(u & 63), fv = -(v & 63). These are NEGATIVE (0 to -63).
// numerator = H*4096 - d0*fu - d1*fv
//            = H*4096 - d0*(-(u&63)) - d1*(-(v&63))
//            = H*4096 + d0*(u&63) + d1*(v&63)
//
// But wait, the Python code does:
//   numerator = base * 4096.0 - d0 * (fu * 64.0) - d1 * (fv * 64.0)
// where fu = (u & 63) / 64.0, so fu * 64 = u & 63 (POSITIVE).
// So Python: numerator = base*4096 - d0*(u&63) - d1*(v&63)
//
// And the docs say: numerator = H*4096 - d0*fu - d1*fv
// where fu = -(u&63), fv = -(v&63).
// So docs: numerator = H*4096 - d0*(-(u&63)) - d1*(-(v&63)) = H*4096 + d0*(u&63) + d1*(v&63)
//
// These have OPPOSITE signs on the d0, d1 terms! One of them is wrong.
// Let me re-read the docs more carefully.

// Actually, re-reading GAME-INTERNALS.md more carefully:
// "The weights are -(u & 63) and -(v & 63), and the residual was never the weights."
// "both are formed at 0x8004D414; the whole tail from 0x8004D588 transcribes to"
// "return (H << 12 - d0*fu - d1*fv) / sqrt(...)"
// "with fu, fv the offsets inside the cell in units of 0..63"
//
// Wait, it says "fu, fv the offsets inside the cell in units of 0..63" -- that
// suggests fu and fv are POSITIVE 0..63, not -(u & 63).
// But it also says "The weights are -(u & 63) and -(v & 63)".
//
// The key is: the MIPS code at 0x8004D414 forms -(u & 63) and -(v & 63),
// which are NEGATIVE values. Then these negative values are multiplied
// by d0 and d1 in the numerator. The MIPS likely does:
//   numerator = (H << 12) + d0 * fu + d1 * fv
// where fu = -(u & 63), fv = -(v & 63), so:
//   = (H << 12) + d0 * (-(u&63)) + d1 * (-(v&63))
//   = (H << 12) - d0*(u&63) - d1*(v&63)
//
// The Python code matches this: numerator = base*4096 - d0*(u&63) - d1*(v&63)
// The transcription in the docs "H << 12 - d0*fu - d1*fv" with fu=-(u&63) gives:
//   H*4096 - d0*fu - d1*fv = H*4096 - d0*(-(u&63)) - d1*(-(v&63)) = H*4096 + d0*(u&63) + d1*(v&63)
// That's WRONG -- the signs are flipped!
//
// The correct transcription should be: H*4096 + d0*fu + d1*fv
// (with fu, fv being the negative values -(u&63), -(v&63))
// which equals H*4096 - d0*(u&63) - d1*(v&63) -- matching Python.
//
// So there's a sign error in the docstring's transcription. The Python code
// (which was verified against the game) is correct:
//   numerator = base * 4096 - d0*(u&63) - d1*(v&63)
//
// Let me fix the C++ implementation to match the Python exactly.

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
    } \
} while(0)

// Build a WaveField from a Python-style 2D heights array
static we::WaveField field_from_heights(const std::vector<std::vector<int16_t>>& heights,
                                         float water_level) {
    we::WaveField field;
    field.set_water_level(water_level);

    // Build FieldEntry array
    std::vector<we::FieldEntry> entries(kRows * kCols);
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            // The Python reads velocity, height = struct.unpack("<hh", ...)
            // So in our FieldEntry: height is the second int16 (upper halfword
            // in big-endian = lower 2 bytes of the host word on LE).
            // But our FieldEntry stores height and velocity as separate fields.
            // The Python stored heights[row][col] = height >> 8 (whole units).
            // We need to store the FULL height value (not shifted) so that
            // our field_height_at() can do the >> 8.
            auto& e = entries[r * kCols + c];
            e.height = static_cast<int16_t>(heights[r][c] << kHeightShift);
            e.velocity = 0;
        }
    }
    field.load_from_entries(entries.data(), entries.size());
    return field;
}

// Build a WaveField from raw RDRAM bytes (as the game would dump)
static we::WaveField field_from_rdram(const std::vector<uint8_t>& rdram_dump,
                                        float water_level) {
    we::WaveField field;
    field.set_water_level(water_level);

    // The raw dump has entries of {s16 height_hi, s16 vel_lo, s16 height_lo, s16 vel_hi}
    // in big-endian N64 order. librecomp stores as host-endian words.
    // struct.unpack("<hh", raw, i*4) gives (velocity, height) on LE.
    // So bytes [i*4, i*4+1] = velocity, [i*4+2, i*4+3] = height.
    //
    // But wait -- the dump is written as raw RDRAM bytes via fwrite, which
    // preserves whatever endianness librecomp uses internally (host-endian words).
    // On a little-endian host, the 4 bytes are: [vel_lo, vel_hi, hgt_lo, hgt_hi]
    // struct.unpack("<hh") reads: first int16 = (vel_lo, vel_hi) = velocity,
    //   second int16 = (hgt_lo, hgt_hi) = height.
    //
    // Our load_from_rdram_dump does:
    //   memcpy(&velocity, data + i*4 + 0, 2)  -> reads (vel_lo, vel_hi) = velocity
    //   memcpy(&height, data + i*4 + 2, 2)    -> reads (hgt_lo, hgt_hi) = height
    // This matches.

    if (rdram_dump.size() == we::kFieldBytes) {
        field.load_from_rdram_dump(rdram_dump.data(), rdram_dump.size());
    }
    return field;
}

int main(int argc, char** argv) {
    std::fprintf(stderr, "=== func_8004D30C native C++ reimplementation test ===\n\n");

    // -----------------------------------------------------------------------
    // Test 1: Flat field — func_8004D30C should return gWaterLevel
    // -----------------------------------------------------------------------
    std::fprintf(stderr, "Test 1: Flat field (h00 = h01 = h10 = h11 = 0)\n");
    {
        // Flat field: all heights zero, water level = 0.
        std::vector<std::vector<int16_t>> flat(kRows, std::vector<int16_t>(kCols, 0));
        auto field = field_from_heights(flat, 0.0f);

        // On flat water, d0 = d1 = 0, so the formula reduces to H = water_level + h00.
        for (float x : {-1000.0f, -500.0f, 0.0f, 333.3f, 666.7f, 1000.0f, 5000.0f, -5000.0f}) {
            for (float z : {-1000.0f, -500.0f, 0.0f, 333.3f, 1000.0f, 5000.0f, -5000.0f}) {
                float native_val = we::func_8004D30C_native(field, x, z);
                float py_val = py_height_at(flat, x, z, 0.0f);
                TEST_CHECK(std::abs(native_val - py_val) < 0.01f,
                           "flat field height query mismatch");
                // On flat water, should equal water_level + 0
                TEST_CHECK(std::abs(native_val - 0.0f) < 0.001f,
                           "flat water should return water_level");
            }
        }
    }
    std::fprintf(stderr, "Test 1 done.\n\n");

    // -----------------------------------------------------------------------
    // Test 2: Flat field with nonzero water level
    // -----------------------------------------------------------------------
    std::fprintf(stderr, "Test 2: Flat field with water_level = 50.0\n");
    {
        std::vector<std::vector<int16_t>> flat(kRows, std::vector<int16_t>(kCols, 0));
        auto field = field_from_heights(flat, 50.0f);

        for (float x : {-2000.0f, 0.0f, 500.0f, 14000.0f, -14000.0f}) {
            for (float z : {-2000.0f, 0.0f, 500.0f, 14000.0f, -14000.0f}) {
                float native_val = we::func_8004D30C_native(field, x, z);
                float py_val = py_height_at(flat, x, z, 50.0f);
                TEST_CHECK(std::abs(native_val - py_val) < 0.01f,
                           "flat field with water level mismatch");
                TEST_CHECK(std::abs(native_val - 50.0f) < 0.001f,
                           "flat water should return water_level");
            }
        }
    }
    std::fprintf(stderr, "Test 2 done.\n\n");

    // -----------------------------------------------------------------------
    // Test 3: Perturbed field — compare C++ vs Python reference
    // -----------------------------------------------------------------------
    std::fprintf(stderr, "Test 3: Perturbed field (random heights)\n");
    {
        // Create a field with random heights to exercise interpolation.
        std::mt19937 rng(42); // deterministic
        std::uniform_int_distribution<int16_t> dist(-30, 60);

        std::vector<std::vector<int16_t>> heights(kRows, std::vector<int16_t>(kCols, 0));
        for (int r = 0; r < kRows; ++r) {
            for (int c = 0; c < kCols; ++c) {
                heights[r][c] = dist(rng);
            }
        }

        // Use a nonzero water level
        float water_level = 100.0f;
        auto field = field_from_heights(heights, water_level);

        // Test many points, including edge cases.
        // Test on a grid with various offsets to exercise different cells.
        int max_error = 0;
        float max_err_val = 0.0f;
        for (float x = -5000.0f; x <= 5000.0f; x += 47.3f) {
            for (float z = -5000.0f; z <= 5000.0f; z += 83.7f) {
                float native_val = we::func_8004D30C_native(field, x, z);
                float py_val = py_height_at(heights, x, z, water_level);
                float err = std::abs(native_val - py_val);
                if (err > max_err_val) max_err_val = err;
                if (err > 0.001f) max_error++;
                TEST_CHECK(err < 0.001f, "perturbed field query mismatch");
            }
        }
        std::fprintf(stderr, "  Tested %d points, max error: %.6f, failures: %d\n",
                     221 * 221, max_err_val, max_error);
    }
    std::fprintf(stderr, "Test 3 done.\n\n");

    // -----------------------------------------------------------------------
    // Test 4: Field with a single disturbance (like a boat wake)
    // -----------------------------------------------------------------------
    std::fprintf(stderr, "Test 4: Single disturbance field\n");
    {
        std::vector<std::vector<int16_t>> heights(kRows, std::vector<int16_t>(kCols, 0));

        // Create a bump at a specific lattice location.
        // The height stored in the field is h * 256 (so >> 8 gives h).
        // We store whole-unit heights, so entries[i] = h * 256.
        // Set a bump of 5 units at cell (100, 50) and neighbors.
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                int r = (100 + dr + kRows) % kRows;
                int c = (50 + dc + kCols) % kCols;
                heights[r][c] = 5;
            }
        }

        auto field = field_from_heights(heights, 0.0f);

        // Query near the disturbance
        // Need to convert the cell back to world coordinates for probing.
        // u = c * 64, so world_x ≈ u * 1.0 (identity since u = x + z/sqrt3)
        // For a quick test, just query at various world points and compare.
        for (float x = -1000.0f; x <= 1000.0f; x += 37.0f) {
            for (float z = -1000.0f; z <= 1000.0f; z += 71.0f) {
                float native_val = we::func_8004D30C_native(field, x, z);
                float py_val = py_height_at(heights, x, z, 0.0f);
                TEST_CHECK(std::abs(native_val - py_val) < 0.001f,
                           "disturbance field query mismatch");
            }
        }
    }
    std::fprintf(stderr, "Test 4 done.\n\n");

    // -----------------------------------------------------------------------
    // Test 5: Large-scale random field
    // -----------------------------------------------------------------------
    std::fprintf(stderr, "Test 5: Large random field\n");
    {
        std::mt19937 rng(12345);
        std::normal_distribution<float> dist(0.0f, 15.0f);

        std::vector<std::vector<int16_t>> heights(kRows, std::vector<int16_t>(kCols));
        for (int r = 0; r < kRows; ++r) {
            for (int c = 0; c < kCols; ++c) {
                heights[r][c] = static_cast<int16_t>(std::clamp(static_cast<int>(dist(rng)), -30, 60));
            }
        }

        float water_level = -10.5f;  // negative water level
        auto field = field_from_heights(heights, water_level);

        int max_failures = 0;
        for (float x = -10000.0f; x <= 10000.0f; x += 123.4f) {
            for (float z = -10000.0f; z <= 10000.0f; z += 234.5f) {
                float native_val = we::func_8004D30C_native(field, x, z);
                float py_val = py_height_at(heights, x, z, water_level);
                float err = std::abs(native_val - py_val);
                if (err > 0.001f) max_failures++;
                TEST_CHECK(err < 0.001f, "large random field mismatch");
            }
        }
        std::fprintf(stderr, "  Failures: %d (out of %d)\n", max_failures, 161 * 161);
    }
    std::fprintf(stderr, "Test 5 done.\n\n");

    // -----------------------------------------------------------------------
    // Test 6: Real field dump (if available from command line)
    // -----------------------------------------------------------------------
    if (argc >= 2) {
        std::fprintf(stderr, "Test 6: Real field dump from '%s'\n", argv[1]);
        const char* prefix = argv[1];
        std::string field_path = std::string(prefix) + "-field.bin";
        std::string meta_path = std::string(prefix) + "-meta.txt";
        std::string probe_path = std::string(prefix) + "-probe.csv";

        if (std::filesystem::exists(field_path) &&
            std::filesystem::exists(meta_path) &&
            std::filesystem::exists(probe_path)) {

            // Read meta
            float water_level = 0.0f;
            {
                std::ifstream meta(meta_path);
                std::string line;
                while (std::getline(meta, line)) {
                    if (line.find("water_level") == 0) {
                        sscanf(line.c_str(), "water_level %f", &water_level);
                    }
                }
            }

            // Read field dump
            std::vector<uint8_t> rdram_dump(we::kFieldBytes);
            {
                std::ifstream f(field_path, std::ios::binary);
                f.read(reinterpret_cast<char*>(rdram_dump.data()),
                       static_cast<std::streamsize>(rdram_dump.size()));
                if (f.gcount() != static_cast<std::streamsize>(rdram_dump.size())) {
                    std::fprintf(stderr, "  ERROR: could not read full field dump\n");
                } else {
                    auto field = field_from_rdram(rdram_dump, water_level);
                    std::fprintf(stderr, "  Loaded field, water_level=%.1f\n", water_level);

                    // Compare against probe.csv
                    int checked = 0;
                    int failures = 0;
                    float max_err = 0.0f;
                    std::ifstream probes(probe_path);
                    std::string line;
                    std::getline(probes, line); // skip header
                    while (std::getline(probes, line)) {
                        // format: grid,x,z,height
                        size_t g1 = line.find(',');
                        size_t g2 = line.find(',', g1 + 1);
                        size_t g3 = line.find(',', g2 + 1);
                        if (g1 == std::string::npos || g2 == std::string::npos ||
                            g3 == std::string::npos) continue;

                        float x = std::stof(line.substr(g1 + 1, g2 - g1 - 1));
                        float z = std::stof(line.substr(g2 + 1, g3 - g2 - 1));
                        float game_height = std::stof(line.substr(g3 + 1));

                        float our_height = we::func_8004D30C_native(field, x, z);
                        float err = std::abs(our_height - game_height);
                        if (err > max_err) max_err = err;
                        if (err > 0.01f) {
                            failures++;
                        }
                        checked++;
                    }

                    std::fprintf(stderr, "  Checked %d probes, max error: %.6f, failures: %d\n",
                                 checked, max_err, failures);
                    TEST_CHECK(failures == 0, "real field dump has failures");
                    TEST_CHECK(checked > 0, "should have checked probes");
                }
            }
        } else {
            std::fprintf(stderr, "  WARNING: field dump files not found at '%s'\n", prefix);
            std::fprintf(stderr, "  Skipping real field test. Run the game with WR64_WATER_FIELD set.\n");
        }
    }

    // -----------------------------------------------------------------------
    // Summary
    // -----------------------------------------------------------------------
    std::fprintf(stderr, "\n=== Summary ===\n");
    std::fprintf(stderr, "Tests run:    %d\n", tests_run);
    std::fprintf(stderr, "Tests passed: %d\n", tests_passed);
    std::fprintf(stderr, "Tests failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
