#pragma once

// Native C++ reimplementation of selected Wave Race 64 functions that were
// originally in the recompiled code. This header declares the C++ equivalents
// that replace calls into func_<hex> symbols.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace wr64::engine {

// --- Wave field constants (from GAME-INTERNALS.md §4, "The wave field") ---
//
// D_80162420: 384 rows x 128 columns of {s16 height, s16 velocity} = 192 KiB.
// Height stored at 1/256 world units (func_8004F3D4 writes arg2 * 256).
// Readers take s16 >> 8, so the stored unit is 1/256 but the *used* unit is
// floored to a whole world unit, discarding the fraction.
//
// The lattice is triangular/oblique with 64-unit cells, wrapping at 24576
// (384 * 64) on both axial coordinates.
constexpr uint32_t kFieldAddress    = 0x80162420;
constexpr uint32_t kFieldRows       = 384;
constexpr uint32_t kFieldCols       = 128;
constexpr uint32_t kCellSize        = 64;
constexpr uint32_t kFieldBytes      = kFieldRows * kFieldCols * 4; // 192 KiB

// gWaterLevel at 0x80192458, immediately after the field array.
constexpr uint32_t kWaterLevelAddress = 0x80192458;
constexpr uint32_t kWrap              = kFieldRows * kCellSize; // 24576

// Lattice axial-to-world constants.
constexpr float kOneOverSqrt3 = 0.57735026f;   // 1/sqrt(3)
constexpr float kTwoOverSqrt3 = 1.1547005f;    // 2/sqrt(3)
constexpr int32_t kRowBias     = 0x600;        // 4 * 384, keeps row sum positive

/// A single field entry: height (1/256 world units, read as s16 >> 8) and
/// signed velocity (1/256 world unit per tick).
struct alignas(4) FieldEntry {
    int16_t height;   // offset 0
    int16_t velocity; // offset 2
};

/// A snapshot of the wave field and its water level, decoupled from RDRAM.
/// This lets the C++ implementation be tested independently of the recompiled
/// game state. The field is stored as RDRAM-native 32-bit words (big-endian
/// halfwords swapped), matching what the recompiled code sees.
class WaveField {
public:
    WaveField();
    ~WaveField() = default;

    /// Load the field from a raw RDRAM dump (192 KiB, host-endian words).
    /// The dump has height in the upper halfword and velocity in the lower,
    /// because RDRAM is stored as host-endian 32-bit words.
    bool load_from_rdram_dump(const uint8_t* data, size_t size);

    /// Load the field from a flat array of entries.
    void load_from_entries(const FieldEntry* entries, size_t count);

    /// Set the water level (mean sea level the field's offsets add to).
    void set_water_level(float level) { water_level_ = level; }
    float water_level() const { return water_level_; }

    /// The number of rows/cols (384 x 128).
    static constexpr uint32_t rows()    { return kFieldRows; }
    static constexpr uint32_t cols()    { return kFieldCols; }

    /// Direct access to a field entry for testing/inspection.
    const FieldEntry& entry(uint32_t row, uint32_t col) const;

private:
    FieldEntry field_[kFieldRows * kFieldCols];
    float water_level_ = 0.0f;
};

/// --- func_8004D30C: the game's own water height query ---
///
/// Given world (x, z) coordinates, returns the water "height" at that point.
///
/// IMPORTANT: despite the name, this function does NOT return a geometric
/// height. It returns the plane constant (d) divided by the plane normal's
/// length. On flat water (d0=d1=0) this reduces to exactly (gWaterLevel + h00),
/// which is why it looks like a height there. On tilted water it is lower by
/// a factor of sqrt(d0^2 + d1^2 + 1), where d0 and d1 are the height
/// differences across one 64-unit cell.
///
/// To get the true geometric height, multiply the result by
/// sqrt(d0^2 + d1^2 + 1). (See GAME-INTERNALS.md §4, "The indexing".)
///
/// The algorithm:
///   1. Convert (x, z) to oblique lattice coordinates (u, v):
///        v = (s32)(z * 1.1547005f) % 24576
///        u = (s32)(z * 0.57735026f + x) % 24576
///   2. Index the field:
///        cu = u >> 6,  cv = v >> 6   (arithmetic shifts)
///        row = (cv + (cu & ~0x7F) + 0x600) % 384
///        col = cu & 0x7F
///      height = field[row * 128 + col].height  (s16 >> 8)
///   3. Interpolate within the cell using the fractional parts:
///      The rhombus cell is split by the diagonal frac_u == frac_v.
///      The branch is: if (frac_v > frac_u) use the top triangle,
///      else the bottom triangle.
///   4. Compute two height differences (d0, d1) between adjacent corners
///      in 12-bit fixed point (heights are whole units, differences are
///      shifted left by 12 = 0x1000).
///   5. Return:
///        (H << 12 - d0*fu - d1*fv) / sqrt((d0^2 + d1^2 + 0x1000) << 12)
///      where H = gWaterLevel + h00, and fu, fv are -(u & 63), -(v & 63).

/// C-style modulus that truncates toward zero (matching MIPS/MIPS3 C `%` on int).
/// This matters because negative world coordinates must wrap correctly.
int32_t c_mod(int32_t a, int32_t b);

/// C-style float-to-int truncation (toward zero, not toward -inf).
inline int32_t c_trunc(float v) {
    return static_cast<int32_t>(v);
}

/// Convert world (x, z) to lattice axial coordinates (cu, cv) and fractional
/// offsets within the cell (fu, fv in [0, 1)).
/// Returns (cu, cv, fu, fv) where cu, cv are cell indices and fu, fv are
/// the fractional position inside the cell (0..1).
struct LatticePos {
    int32_t cu;
    int32_t cv;
    float fu;  // fractional part of u, in [0, 1) = (u & 63) / 64.0
    float fv;  // fractional part of v, in [0, 1) = (v & 63) / 64.0
    int32_t u_raw;  // raw (u & 63), in [0, 63] — used as negation weight in fixed-point math
    int32_t v_raw;  // raw (v & 63), in [0, 63] — used as negation weight in fixed-point math
};

LatticePos world_to_lattice(float x, float z);

/// Look up the height at lattice cell (cu, cv), with optional neighbor offsets.
/// Returns the height as a whole world unit (s16 >> 8).
int16_t field_height_at(const WaveField& field, int32_t cu, int32_t cv);

/// func_8004D30C(x, z) — native C++ reimplementation.
/// Reads the water field and returns the plane constant over its normal length.
/// See the function doc comment above for the exact semantics.
float func_8004D30C_native(const WaveField& field, float x, float z);

/// The true geometric water height at (x, z). This multiplies the raw query
/// result by sqrt(d0^2 + d1^2 + 1) to recover the actual surface height.
float water_height_at(const WaveField& field, float x, float z);

} // namespace wr64::engine
