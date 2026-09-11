// See include/wr64/resample.h for what this is and the measurements behind it.

#include "wr64/resample.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wr64::resample {
namespace {

// Half the kernel, in source samples, at a ratio of one. Sixty-four taps in
// total: a Kaiser window at this length puts the stopband about 85 dB down, which
// is past the point where 16-bit output is the limit.
constexpr int kHalfTaps = 32;

// Points of the kernel stored per source sample. Consecutive entries are linearly
// interpolated between, and at this spacing that interpolation is far below the
// noise floor of the output.
constexpr int kTableResolution = 256;

// Where the filter turns over, as a fraction of the source rate. Not 0.5: a
// 64-tap Kaiser has a transition band of real width, and centring the cutoff at
// Nyquist would put half of that band past it, where it folds back. This keeps
// the whole transition inside and costs a gentle roll-off over the top tenth of
// the band -- above about 12 kHz at 26900 Hz, where this game has very little.
constexpr double kCutoff = 0.455;

constexpr double kKaiserBeta = 8.6;

// M_PI is not standard C++ and the MSVC headers hide it without a define.
constexpr double kPi = 3.14159265358979323846;

std::vector<float> g_table;      // the windowed sinc, at kTableResolution per sample
std::vector<int16_t> g_pending;  // source frames still needed, interleaved
double g_pos = 0.0;              // where the next output sits, in g_pending frames
double g_step = 1.0;             // source frames per destination frame
double g_half = kHalfTaps;       // half the kernel, in source frames
bool g_passthrough = true;
uint32_t g_src = 0;
uint32_t g_dst = 0;

// Modified Bessel function of the first kind, order zero, by its series. It
// converges quickly for the arguments a Kaiser window needs, and this runs once
// per rate change.
double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-16) {
            break;
        }
    }
    return sum;
}

void build_table(double cutoff_per_source_sample) {
    const int points = static_cast<int>(std::ceil(g_half * kTableResolution)) + 2;
    g_table.assign(static_cast<size_t>(points), 0.0f);

    const double denominator = bessel_i0(kKaiserBeta);
    for (int k = 0; k < points; ++k) {
        const double x = double(k) / kTableResolution;   // source samples from the centre
        if (x > g_half) {
            g_table[static_cast<size_t>(k)] = 0.0f;
            continue;
        }
        // sin(2 pi fc x) / (pi x), which is the ideal low-pass, times a Kaiser
        // window over the kernel's own width.
        const double angle = 2.0 * kPi * cutoff_per_source_sample * x;
        const double sinc = (k == 0) ? (2.0 * cutoff_per_source_sample)
                                     : (std::sin(angle) / (kPi * x));
        const double t = x / g_half;
        const double window = bessel_i0(kKaiserBeta * std::sqrt(1.0 - t * t)) / denominator;
        g_table[static_cast<size_t>(k)] = static_cast<float>(sinc * window);
    }
}

int16_t clamp_to_sample(double value) {
    if (value > 32767.0) return 32767;
    if (value < -32768.0) return -32768;
    return static_cast<int16_t>(value < 0.0 ? value - 0.5 : value + 0.5);
}

}  // namespace

void configure(uint32_t src_rate, uint32_t dst_rate) {
    g_src = src_rate;
    g_dst = dst_rate;
    g_pending.clear();
    g_pos = 0.0;

    if (src_rate == 0 || dst_rate == 0 || src_rate == dst_rate) {
        g_passthrough = true;
        std::fprintf(stderr, "[wr64] no resampling needed (%u Hz both sides)\n", src_rate);
        std::fflush(stderr);
        return;
    }
    g_passthrough = false;

    g_step = double(src_rate) / double(dst_rate);

    // Downsampling has to move the cutoff down with the ratio, and the kernel
    // then has to cover proportionally more source samples to keep the same
    // shape. Upsampling leaves both alone: the source is already band-limited to
    // its own Nyquist.
    const double ratio = dst_rate < src_rate ? (double(dst_rate) / double(src_rate)) : 1.0;
    g_half = double(kHalfTaps) / ratio;
    build_table(kCutoff * ratio);

    // Start with silence to the left of the first output, so the kernel always
    // has its full context and the conversion begins in a defined state rather
    // than with whatever the first buffer's edge implies.
    const size_t lead = static_cast<size_t>(std::ceil(g_half)) + 1;
    g_pending.assign(lead * 2, int16_t{ 0 });
    g_pos = double(lead);

    std::fprintf(stderr, "[wr64] resampling %u -> %u Hz in the port"
                         " (%d taps, %.1f source samples of kernel)\n",
                 src_rate, dst_rate, 2 * kHalfTaps, 2.0 * g_half);
    std::fflush(stderr);
}

void process(const int16_t* in, size_t sample_count, std::vector<int16_t>& out) {
    if (g_passthrough) {
        out.assign(in, in + sample_count);
        return;
    }

    g_pending.insert(g_pending.end(), in, in + sample_count);
    const size_t frames = g_pending.size() / 2;

    out.clear();
    out.reserve(static_cast<size_t>(double(sample_count) / g_step) + 4);

    // An output can be produced while the kernel's right-hand side is covered by
    // frames that have actually arrived. Everything else waits for the next
    // buffer, which is what makes this continuous across calls.
    const double limit = double(frames) - g_half - 1.0;
    while (g_pos < limit) {
        const long first = static_cast<long>(std::ceil(g_pos - g_half));
        const long last = static_cast<long>(std::floor(g_pos + g_half));

        double left = 0.0, right = 0.0, weight_sum = 0.0;
        for (long i = first; i <= last; ++i) {
            if (i < 0 || static_cast<size_t>(i) >= frames) {
                continue;
            }
            const double distance = std::fabs(g_pos - double(i)) * kTableResolution;
            const size_t index = static_cast<size_t>(distance);
            if (index + 1 >= g_table.size()) {
                continue;
            }
            const double fraction = distance - double(index);
            const double weight = g_table[index] + (g_table[index + 1] - g_table[index]) * fraction;
            left += weight * g_pending[static_cast<size_t>(i) * 2 + 0];
            right += weight * g_pending[static_cast<size_t>(i) * 2 + 1];
            weight_sum += weight;
        }
        // Normalising by the weights the kernel actually landed on flattens the
        // gain, which otherwise ripples by a fraction of a decibel with the
        // fractional position.
        if (weight_sum > 1e-9) {
            left /= weight_sum;
            right /= weight_sum;
        }
        out.push_back(clamp_to_sample(left));
        out.push_back(clamp_to_sample(right));
        g_pos += g_step;
    }

    // Drop what the kernel can no longer reach back to.
    const long spent = static_cast<long>(std::floor(g_pos - g_half)) - 1;
    if (spent > 0) {
        g_pending.erase(g_pending.begin(), g_pending.begin() + spent * 2);
        g_pos -= double(spent);
    }
}

uint64_t to_source_frames(uint64_t dst_frames) {
    if (g_passthrough || g_dst == 0) {
        return dst_frames;
    }
    return dst_frames * g_src / g_dst;
}

}  // namespace wr64::resample
