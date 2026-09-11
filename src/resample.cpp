// See include/wr64/resample.h for what this is and the measurements behind it.

#include "wr64/resample.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
std::vector<float> g_pending;    // source frames still needed, interleaved
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
    // Eight guard points past the end, all zero, so the inner loop can step off
    // the tail of the kernel by a sample or two without a bounds check. The
    // alternative is a compare and a branch on every tap.
    const int points = static_cast<int>(std::ceil(g_half * kTableResolution)) + 8;
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

int16_t clamp_to_sample(float value) {
    if (value > 32767.0f) return 32767;
    if (value < -32768.0f) return -32768;
    return static_cast<int16_t>(value < 0.0f ? value - 0.5f : value + 0.5f);
}

// Whether to report what the conversion costs. The same switch as the rest of
// the audio statistics, read here rather than called for: this file has no other
// dependency in the project and is worth keeping that way, since it is the part
// most likely to be lifted into another port.
bool stats_wanted() {
    static const bool on = [] {
        const char* value = std::getenv("WR64_AUDIO_STATS");
        return value != nullptr && value[0] != 0 && std::strcmp(value, "0") != 0;
    }();
    return on;
}

// What the conversion costs, reported with the rest of the audio statistics.
// It runs on the thread that has to keep pace with the game, on machines that
// are not always keeping pace as it is, so the figure is worth having in front of
// anyone changing the tap count.
std::chrono::steady_clock::duration g_spent{};
uint64_t g_produced = 0;
std::chrono::steady_clock::time_point g_reported = std::chrono::steady_clock::now();

void note_cost(std::chrono::steady_clock::duration spent, uint64_t frames,
               std::chrono::steady_clock::time_point now) {
    g_spent += spent;
    g_produced += frames;
    if (now - g_reported < std::chrono::seconds(4)) {
        return;
    }
    const double window = std::chrono::duration<double>(now - g_reported).count();
    const double used = std::chrono::duration<double>(g_spent).count();
    std::fprintf(stderr, "[wr64-audio] conversion: %.2f%% of one core"
                         " (%.0f frames a second)\n",
                 100.0 * used / window, double(g_produced) / window);
    std::fflush(stderr);
    g_reported = now;
    g_spent = {};
    g_produced = 0;
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
    g_pending.assign(lead * 2, 0.0f);
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

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    g_pending.reserve(g_pending.size() + sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
        g_pending.push_back(static_cast<float>(in[i]));
    }
    const size_t frames = g_pending.size() / 2;

    out.clear();
    out.reserve(static_cast<size_t>(double(sample_count) / g_step) + 4);

    const float* const history = g_pending.data();
    const float* const table = g_table.data();

    // An output can be produced while the kernel's right-hand side is covered by
    // frames that have actually arrived. Everything else waits for the next
    // buffer, which is what makes this continuous across calls.
    const double limit = double(frames) - g_half - 1.0;
    while (g_pos < limit) {
        // The distance from the output's position to each tap changes by exactly
        // one source sample per tap, so the table index changes by exactly
        // kTableResolution -- an integer add -- and the fractional position
        // inside a table cell is the same for every tap on a side. That is the
        // whole trick: no absolute value, no multiply and no conversion inside
        // the loop, where a first attempt at this had all three and cost seven
        // per cent of a core.
        const long centre = static_cast<long>(g_pos);
        const double fraction = g_pos - double(centre);
        const long first = static_cast<long>(std::ceil(g_pos - g_half));
        const long last = static_cast<long>(g_pos + g_half);

        float left = 0.0f, right = 0.0f, weight_sum = 0.0f;

        // Taps at and before the output's position, walking away from it.
        {
            const double exact = fraction * kTableResolution;
            size_t index = static_cast<size_t>(exact);
            const float within = static_cast<float>(exact - double(index));
            for (long i = centre; i >= first; --i, index += kTableResolution) {
                const float a = table[index];
                const float weight = a + within * (table[index + 1] - a);
                const float* const frame = history + static_cast<size_t>(i) * 2;
                left += weight * frame[0];
                right += weight * frame[1];
                weight_sum += weight;
            }
        }
        // And the taps after it.
        {
            const double exact = (1.0 - fraction) * kTableResolution;
            size_t index = static_cast<size_t>(exact);
            const float within = static_cast<float>(exact - double(index));
            for (long i = centre + 1; i <= last; ++i, index += kTableResolution) {
                const float a = table[index];
                const float weight = a + within * (table[index + 1] - a);
                const float* const frame = history + static_cast<size_t>(i) * 2;
                left += weight * frame[0];
                right += weight * frame[1];
                weight_sum += weight;
            }
        }

        // Normalising by the weights the kernel actually landed on flattens the
        // gain, which otherwise ripples by a fraction of a decibel with the
        // fractional position.
        if (weight_sum > 1e-9f) {
            const float scale = 1.0f / weight_sum;
            left *= scale;
            right *= scale;
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

    if (stats_wanted()) {
        const std::chrono::steady_clock::time_point finished = std::chrono::steady_clock::now();
        note_cost(finished - started, out.size() / 2, finished);
    }
}

uint64_t to_source_frames(uint64_t dst_frames) {
    if (g_passthrough || g_dst == 0) {
        return dst_frames;
    }
    return dst_frames * g_src / g_dst;
}

}  // namespace wr64::resample
