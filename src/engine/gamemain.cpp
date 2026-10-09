// Native C++ reimplementation of selected Wave Race 64 functions.
//
// This file implements func_8004D30C (the water height query) in C++,
// replacing the call into the recompiled MIPS-to-C code. The algorithm is
// transcribed from GAME-INTERNALS.md §4 ("The wave field: where the water
// actually comes from") and verified against tools/decode_water_field.py,
// which itself was validated against the game's own query over 19,042 probes
// (median error 0.0002, 100% within tolerance).
//
// The function signature matches how the port calls the recompiled function:
//   float func_8004D30C(float x, float z)
// It reads the wave field from RDRAM-backed data and returns the plane
// constant divided by its normal length (NOT a geometric height; see the
// header for why and how to convert).

#include "wr64/engine/gamemain.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace wr64::engine {

// ---------------------------------------------------------------------------
// WaveField
// ---------------------------------------------------------------------------

WaveField::WaveField() {
    // Zero-initialize the field (flat calm water).
    std::memset(field_, 0, sizeof(field_));
}

bool WaveField::load_from_rdram_dump(const uint8_t* data, size_t size) {
    if (data == nullptr || size != kFieldBytes) {
        return false;
    }
    // RDRAM is stored as host-endian 32-bit words. Each 4-byte entry holds
    // {s16 velocity (lower halfword), s16 height (upper halfword)} when
    // viewed as a host word, because the N64's big-endian layout is swapped
    // to host order. We want height in the upper halfword.
    //
    // The Python decoder does: struct.unpack_from("<hh", raw, i*4) which gives
    // (velocity, height) on little-endian. So height is the second halfword.
    // In a host-endian 32-bit word at byte offset i*4:
    //   bytes [i*4, i*4+1] = height (upper halfword of the big-endian pair)
    //   bytes [i*4+2, i*4+3] = velocity (lower halfword)
    // When read as little-endian 32-bit: lower 16 bits = height, upper 16 = velocity
    // But we read with memcpy into int16_t pairs via the <hh> format, so:
    //   first s16 (lower 2 bytes) = velocity, second s16 (upper 2 bytes) = height
    //
    // Actually, let's follow the Python exactly. struct.unpack_from("<hh", ...)
    // on little-endian reads two consecutive int16s: first from bytes [0,1], second from [2,3].
    // In RDRAM big-endian: bytes [0,1] = height, bytes [2,3] = velocity.
    // But librecomp stores RDRAM as host-endian words, so the 4 bytes are:
    //   [0,1] = velocity, [2,3] = height  (swapped within each word)
    // Wait -- the Python reads "<hh" from the raw dump. The dump is written
    // via fwrite of rdram+offset, which is host-endian words. On little-endian
    // host: bytes [0,1] = lower halfword = height (from the game's upper halfword),
    // bytes [2,3] = upper halfword = velocity.
    // So struct.unpack_from("<hh") gives (height, velocity)? No:
    //
    // Let's be precise. The game's 32-bit word at address A is:
    //   big-endian bytes: [height_hi, height_lo, vel_hi, vel_lo]
    // librecomp stores it as host-endian, so on little-endian host:
    //   bytes in memory: [vel_lo, vel_hi, height_lo, height_hi]
    //   -- i.e., the 32-bit word value is (vel << 16 | height) on LE
    //
    // struct.unpack_from("<hh", raw, i*4) reads:
    //   first  s16 from bytes [i*4+0, i*4+1] = vel_lo, vel_hi = velocity
    //   second s16 from bytes [i*4+2, i*4+3] = height_lo, height_hi = height
    // So: velocity = unpack[0], height = unpack[1]
    //
    // That matches the Python code: velocity, height = struct.unpack_from("<hh", ...)
    //
    // So in our FieldEntry, we store height and velocity as read:
    for (uint32_t i = 0; i < kFieldRows * kFieldCols; ++i) {
        int16_t velocity;
        int16_t height;
        std::memcpy(&velocity, data + i * 4 + 0, sizeof(int16_t));
        std::memcpy(&height, data + i * 4 + 2, sizeof(int16_t));
        field_[i].height = height;
        field_[i].velocity = velocity;
    }
    return true;
}

void WaveField::load_from_entries(const FieldEntry* entries, size_t count) {
    const size_t expected = kFieldRows * kFieldCols;
    for (size_t i = 0; i < count && i < expected; ++i) {
        field_[i] = entries[i];
    }
}

const FieldEntry& WaveField::entry(uint32_t row, uint32_t col) const {
    assert(row < kFieldRows && col < kFieldCols);
    return field_[row * kFieldCols + col];
}

// ---------------------------------------------------------------------------
// Lattice arithmetic
// ---------------------------------------------------------------------------

/// C's `%` operator: result has the sign of the dividend (truncated toward zero).
/// Python's `%` floors instead, so we need to emulate C/MIPS behavior.
int32_t c_mod(int32_t a, int32_t b) {
    if (b == 0) return 0; // should not happen
    int32_t r = a % b;
    // C99: a % b has the sign of a. If a is negative and r != 0, adjust.
    // Actually C90/C99: (a/b)*b + a%b == a, and a%b has sign of a.
    // So just use the built-in %, which already does this in C++.
    return r;
}

/// Convert world (x, z) to lattice axial coordinates.
///
/// u = (s32)(x + z * 0.57735026f) % 24576
/// v = (s32)(    z * 1.1547005f) % 24576
///
/// cu = u >> 6, cv = v >> 6  (arithmetic right shifts)
/// fu = -(u & 63), fv = -(v & 63)  (these are the interpolation weights, negated)
///
/// The sign of u and v matters: c_trunc truncates toward zero (like MIPS),
/// and C's % preserves the sign of the dividend.
LatticePos world_to_lattice(float x, float z) {
    const int32_t u = c_mod(c_trunc(x + z * kOneOverSqrt3), kWrap);
    const int32_t v = c_mod(c_trunc(z * kTwoOverSqrt3), kWrap);

    const int32_t cu = u >> 6;  // arithmetic shift on signed int
    const int32_t cv = v >> 6;

    // Fractional parts within the cell: (u & 63) and (v & 63).
    // These are 0..63 (unsigned, since u and v are already made non-negative
    // by c_mod with positive b).
    // The interpolation weights are -(u & 63) and -(v & 63), as formed
    // at 0x8004D414 in the original.
    const int32_t fu_raw = u & 63;
    const int32_t fv_raw = v & 63;

    return {
        cu, cv,
        static_cast<float>(fu_raw) / 64.0f,  // [0, 1)
        static_cast<float>(fv_raw) / 64.0f,  // [0, 1)
        fu_raw,  // raw 0..63, used as -(fu_raw) in fixed-point math
        fv_raw,  // raw 0..63, used as -(fv_raw) in fixed-point math
    };
}

/// Look up the height at lattice cell (cu, cv), applying the row-folding.
///
/// row = (cv + (cu & ~0x7F) + 0x600) % 384
/// col = cu & 0x7F
/// height = s16(field[row * 128 + col]) >> 8
int16_t field_height_at(const WaveField& field, int32_t cu, int32_t cv) {
    // The field is 128 columns wide in u. cu & ~0x7F folds every 128 columns
    // of u into the row index.
    int32_t row = c_mod(cv + (cu & ~0x7F) + kRowBias, kFieldRows);
    int32_t col = cu & 0x7F;

    // Clamp row/col to valid range (should always be in range after modulo).
    if (row < 0) row += kFieldRows;
    if (col < 0) col += kFieldCols;
    row %= kFieldRows;
    col %= kFieldCols;

    const FieldEntry& e = field.entry(static_cast<uint32_t>(row),
                                        static_cast<uint32_t>(col));
    // Height stored at 1/256 world units; readers take s16 >> 8 (arithmetic shift).
    return static_cast<int16_t>(e.height >> 8);
}

// ---------------------------------------------------------------------------
// func_8004D30C
// ---------------------------------------------------------------------------

/// The native reimplementation of func_8004D30C(x, z).
///
/// Returns the plane constant divided by the normal's length, NOT a geometric
/// height. See the header documentation for the full algorithm and the
/// important note about the division.
float func_8004D30C_native(const WaveField& field, float x, float z) {
    const LatticePos pos = world_to_lattice(x, z);

    const int32_t cu = pos.cu;
    const int32_t cv = pos.cv;

    // The interpolation weights from the MIPS code at 0x8004D414 are
    // -(u & 63) and -(v & 63), which are NEGATIVE values in [-63, 0].
    // The branch compares frac v > frac u, i.e., (v & 63) > (u & 63).
    const int32_t fu = -(pos.u_raw);  // -(u & 63), in range [-63, 0]
    const int32_t fv = -(pos.v_raw);  // -(v & 63), in range [-63, 0]

    const int16_t h00 = field_height_at(field, cu,     cv);
    const int16_t h10 = field_height_at(field, cu + 1, cv);
    const int16_t h01 = field_height_at(field, cu,     cv + 1);
    const int16_t h11 = field_height_at(field, cu + 1, cv + 1);

    int16_t d0, d1;

    // The rhombus is split along frac_u == frac_v.
    // Branch at 0x8004D41C: compares -(v & 63) against -(u & 63),
    // which is equivalent to: (v & 63) > (u & 63), i.e., frac_v > frac_u.
    if (pos.v_raw > pos.u_raw) {
        // Top triangle: (cu, cv), (cu, cv+1), (cu+1, cv+1)
        // Match Python decoder exactly:
        //   d1 = h00 - h01
        //   d0 = h01 - h11
        d1 = static_cast<int16_t>(h00 - h01);
        d0 = static_cast<int16_t>(h01 - h11);
    } else {
        // Bottom triangle: (cu, cv), (cu+1, cv), (cu+1, cv+1)
        // Match Python decoder exactly:
        //   d0 = h00 - h10
        //   d1 = h10 - h11
        d0 = static_cast<int16_t>(h00 - h10);
        d1 = static_cast<int16_t>(h10 - h11);
    }

    // Fixed-point computation at 0x8004D588-0x8004D600:
    //   (gWaterLevel + h00) << 12  +  d0 * fu  +  d1 * fv
    //   -----------------------------------------------
    //   sqrt((d0^2 + d1^2 + 0x1000) << 12)
    //
    // 0x1000 = 4096 = 64^2. The denominator is 64 * sqrt(d0^2 + d1^2 + 64^2),
    // which is the length of the normal of a plane rising d0 and d1 over a
    // 64-unit cell.
    //
    // fu and fv are the MIPS-era negative weights -(u & 63) and -(v & 63).
    // The numerator is H*4096 + d0*fu + d1*fv, which expands (since fu, fv
    // are negative) to H*4096 - d0*(u&63) - d1*(v&63). This matches the
    // verified Python decoder in tools/decode_water_field.py exactly.

    const float H = field.water_level() + static_cast<float>(h00);

    // Compute in float (equivalent to the fixed-point math, but with full
    // precision; the result is a float anyway).
    // numerator = H * 4096 + d0 * fu + d1 * fv
    //           = H * 4096 - d0 * (u & 63) - d1 * (v & 63)
    const float numerator = (H * 4096.0f)
                          + static_cast<float>(d0) * static_cast<float>(fu)
                          + static_cast<float>(d1) * static_cast<float>(fv);
    const float denominator = 64.0f * std::sqrt(
        static_cast<float>(d0) * static_cast<float>(d0) +
        static_cast<float>(d1) * static_cast<float>(d1) + 4096.0f
    );

    return numerator / denominator;
}

/// The true geometric water height at (x, z).
///
/// func_8004D30C returns the plane constant over its normal's length.
/// A caller that wants the actual height multiplies back by sqrt(d0^2 + d1^2 + 1),
/// where d0 and d1 are the height differences over the cell size.
///
/// To get d0 and d1, we re-derive them here (same logic as func_8004D30C_native).
float water_height_at(const WaveField& field, float x, float z) {
    const LatticePos pos = world_to_lattice(x, z);

    const int32_t cu = pos.cu;
    const int32_t cv = pos.cv;

    const int16_t h00 = field_height_at(field, cu,     cv);
    const int16_t h10 = field_height_at(field, cu + 1, cv);
    const int16_t h01 = field_height_at(field, cu,     cv + 1);
    const int16_t h11 = field_height_at(field, cu + 1, cv + 1);

    int16_t d0, d1;

    if (pos.v_raw > pos.u_raw) {
        // Top triangle: match Python decoder assignments
        //   d1 = h00 - h01, d0 = h01 - h11
        d1 = static_cast<int16_t>(h00 - h01);
        d0 = static_cast<int16_t>(h01 - h11);
    } else {
        // Bottom triangle
        //   d0 = h00 - h10, d1 = h10 - h11
        d0 = static_cast<int16_t>(h00 - h10);
        d1 = static_cast<int16_t>(h10 - h11);
    }

    const float raw = func_8004D30C_native(field, x, z);
    const float correction = std::sqrt(
        static_cast<float>(d0) * static_cast<float>(d0) +
        static_cast<float>(d1) * static_cast<float>(d1) + 1.0f
    );
    // D0 and D1 are differences over one cell (64 units). The correction
    // factor sqrt(d0^2 + d1^2 + 1) uses the same d0, d1 because the field
    // stores heights as whole units (>>8), and the differences d0, d1 are
    // already in those same whole-unit terms.
    return raw * correction;
}

} // namespace wr64::engine
