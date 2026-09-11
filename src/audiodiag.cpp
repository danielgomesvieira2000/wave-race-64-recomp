// See include/wr64/audiodiag.h for what this measures and why.

#include "wr64/audiodiag.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <librecomp/game.hpp>

namespace wr64::audiodiag {
namespace {

bool env_set(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != 0 && std::strcmp(value, "0") != 0;
}

// ------------------------------------------------------------ the window ----

using clock = std::chrono::steady_clock;

std::mutex g_lock;

uint32_t g_rate = 0;             // what the game believes it is feeding
uint32_t g_period_frames = 0;    // one device pull, in frames

clock::time_point g_window_start = clock::now();
uint64_t g_buffers = 0;          // calls to queue_samples in this window
uint64_t g_frames = 0;           // frames handed over in this window
uint32_t g_buffer_min = 0, g_buffer_max = 0;
uint32_t g_depth_min = 0, g_depth_max = 0;
uint64_t g_depth_total = 0, g_depth_samples = 0;
int32_t g_peak = 0;
bool g_have_depth = false;

// The depth immediately after a buffer went in, which is the one instant the
// accounting can be exact at: every frame counted in a window was queued between
// two of these, so the difference between them and the frames counted is what
// the device took. A depth sampled anywhere else -- when the game polls, say --
// would put the boundary in the middle of a buffer.
uint32_t g_depth_after_queue = 0;   // after the most recent buffer
uint32_t g_depth_at_start = 0;      // after the last buffer of the previous window

void reset_window(clock::time_point now) {
    g_window_start = now;
    g_buffers = 0;
    g_frames = 0;
    g_buffer_min = 0;
    g_buffer_max = 0;
    g_depth_min = 0;
    g_depth_max = 0;
    g_depth_total = 0;
    g_depth_samples = 0;
    g_depth_at_start = g_depth_after_queue;
    g_peak = 0;
    g_have_depth = false;
}

// Records a queue depth, from either call site. Both matter: the one taken just
// before queueing is the trough, and the one taken when the game asks is what
// the game decides against.
void note_depth(uint32_t frames) {
    if (!g_have_depth || frames < g_depth_min) g_depth_min = frames;
    if (!g_have_depth || frames > g_depth_max) g_depth_max = frames;
    g_have_depth = true;
    g_depth_total += frames;
    ++g_depth_samples;
}

double ms(uint64_t frames) {
    return g_rate == 0 ? 0.0 : (double(frames) * 1000.0 / double(g_rate));
}

// Two seconds' worth, printed as four lines.
//
// The third is the measurement this file exists for. The device consumes at the
// sample rate whether or not anything is queued, so what it wanted over the
// window is elapsed x rate; what it got is what was handed over, less whatever
// is still sitting in the queue. The difference is silence SDL inserted, and
// that silence is the crackle.
void report(clock::time_point now) {
    const double seconds = std::chrono::duration<double>(now - g_window_start).count();
    if (seconds <= 0.0 || g_buffers == 0) {
        return;
    }

    const double avg_buffer = double(g_frames) / double(g_buffers);
    const uint64_t avg_depth = g_depth_samples ? g_depth_total / g_depth_samples : 0;

    std::fprintf(stderr, "[wr64-audio] %.2f s at %u Hz: %.0f buffers/s of %.0f frames (%u-%u)\n",
                 seconds, g_rate, double(g_buffers) / seconds, avg_buffer,
                 g_buffer_min, g_buffer_max);
    std::fprintf(stderr, "[wr64-audio]   queue %u / %llu / %u frames (%.1f / %.1f / %.1f ms),"
                         " device period %u frames (%.1f ms)\n",
                 g_depth_min, static_cast<unsigned long long>(avg_depth), g_depth_max,
                 ms(g_depth_min), ms(avg_depth), ms(g_depth_max),
                 g_period_frames, ms(g_period_frames));

    const double wanted = seconds * double(g_rate);
    const double consumed =
        double(g_frames) - (double(g_depth_after_queue) - double(g_depth_at_start));
    const double silence = wanted - consumed;
    // The window's boundaries fall between buffers, so this arithmetic carries an
    // error of up to one buffer either way -- a few hundred frames, which is
    // nothing beside a starved queue and everything beside a healthy one. The
    // trough settles it: a queue that never fell below one device period cannot
    // have been zero-filled, whatever the subtraction says.
    if (g_have_depth && g_depth_min >= g_period_frames) {
        std::fprintf(stderr, "[wr64-audio]   produced %llu frames, the device wanted %.0f:"
                             " nothing zero-filled (the queue never fell below one period)\n",
                     static_cast<unsigned long long>(g_frames), wanted);
    }
    else if (silence > 0.0) {
        std::fprintf(stderr, "[wr64-audio]   produced %llu frames, the device wanted %.0f:"
                             " %.0f frames of silence inserted (%.1f%% of the window)\n",
                     static_cast<unsigned long long>(g_frames), wanted, silence,
                     100.0 * silence / wanted);
    }
    else {
        std::fprintf(stderr, "[wr64-audio]   produced %llu frames, the device wanted %.0f:"
                             " no silence inserted\n",
                     static_cast<unsigned long long>(g_frames), wanted);
    }
    std::fprintf(stderr, "[wr64-audio]   peak amplitude %d of 32767\n", g_peak);
    std::fflush(stderr);
}

// -------------------------------------------------------------- the dump ----
//
// A canonical 44-byte RIFF header followed by the samples, with the two length
// fields kept true after every buffer rather than written once at the end. There
// is no shutdown hook to close the file from, and the runs worth dumping are the
// ones that end badly.

std::FILE* g_wav = nullptr;
uint64_t g_wav_bytes = 0;
int g_wav_index = 0;
// The rate of the file that is open, which is not the rate the device is now
// running at: a device reopen sets the new rate first and then closes the old
// file, and stamping that file with the new rate makes it play back wrong.
uint32_t g_wav_rate = 0;

void put32(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
    p[2] = static_cast<unsigned char>(v >> 16);
    p[3] = static_cast<unsigned char>(v >> 24);
}

void put16(unsigned char* p, uint16_t v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
}

void write_wav_header(uint32_t rate, uint64_t data_bytes) {
    unsigned char header[44];
    std::memcpy(header + 0, "RIFF", 4);
    put32(header + 4, static_cast<uint32_t>(36 + data_bytes));
    std::memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16);                 // PCM chunk size
    put16(header + 20, 1);                  // PCM
    put16(header + 22, 2);                  // stereo
    put32(header + 24, rate);
    put32(header + 28, rate * 4);           // bytes per second
    put16(header + 32, 4);                  // block align
    put16(header + 34, 16);                 // bits per sample
    std::memcpy(header + 36, "data", 4);
    put32(header + 40, static_cast<uint32_t>(data_bytes));

    std::fseek(g_wav, 0, SEEK_SET);
    std::fwrite(header, 1, sizeof(header), g_wav);
    std::fseek(g_wav, 0, SEEK_END);
}

void open_wav(uint32_t rate) {
    if (g_wav != nullptr) {
        write_wav_header(g_wav_rate, g_wav_bytes);
        std::fclose(g_wav);
        g_wav = nullptr;
    }
    g_wav_bytes = 0;
    g_wav_rate = rate;

    char name[64];
    std::snprintf(name, sizeof(name), "wr64-audio-%d-%uHz.wav", ++g_wav_index, rate);
    const std::string path = (recomp::get_config_path() / name).string();
    g_wav = std::fopen(path.c_str(), "wb");
    if (g_wav == nullptr) {
        std::fprintf(stderr, "[wr64-audio] could not open %s for the dump\n", path.c_str());
        std::fflush(stderr);
        return;
    }
    write_wav_header(rate, 0);
    std::fprintf(stderr, "[wr64-audio] dumping what is handed to SDL to %s\n", path.c_str());
    std::fflush(stderr);
}

void write_wav(const int16_t* data, size_t sample_count) {
    if (g_wav == nullptr) {
        return;
    }
    const size_t bytes = sample_count * sizeof(int16_t);
    std::fwrite(data, 1, bytes, g_wav);
    g_wav_bytes += bytes;
    // Rewritten after every buffer rather than on a timer. There is no shutdown
    // hook to close this file from, and the runs worth dumping are the ones that
    // end badly, so the two length fields have to be true at every moment. It is
    // a 44-byte write sixty times a second on a file that is already open.
    write_wav_header(g_wav_rate, g_wav_bytes);
}

}  // namespace

bool stats_enabled() {
    static const bool on = env_set("WR64_AUDIO_STATS");
    return on;
}

bool dump_enabled() {
    static const bool on = env_set("WR64_AUDIO_DUMP");
    return on;
}

void device_opened(uint32_t asked_rate, uint32_t asked_period_frames,
                   uint32_t device_rate, uint32_t device_period_frames) {
    if (!stats_enabled() && !dump_enabled()) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_lock);

    if (stats_enabled()) {
        const double asked_ms =
            asked_rate ? double(asked_period_frames) * 1000.0 / double(asked_rate) : 0.0;
        if (device_rate == 0) {
            std::fprintf(stderr, "[wr64-audio] device: asked %u Hz, %u-frame buffer (%.1f ms);"
                                 " the machine's default device did not say what it runs at\n",
                         asked_rate, asked_period_frames, asked_ms);
        }
        else {
            // WASAPI reports a rate but no buffer size, so the period is printed
            // only when there is one.
            char period[48];
            period[0] = 0;
            if (device_period_frames != 0) {
                std::snprintf(period, sizeof(period), " with a %u-frame buffer",
                              device_period_frames);
            }
            std::fprintf(stderr, "[wr64-audio] device: asked %u Hz, %u-frame buffer (%.1f ms);"
                                 " the default device runs at %u Hz%s%s\n",
                         asked_rate, asked_period_frames, asked_ms,
                         device_rate, period,
                         device_rate == asked_rate ? "" : " -- SDL is resampling");
        }
        std::fflush(stderr);
    }

    // The queue went with the old device, so nothing measured against it carries
    // over.
    g_rate = asked_rate;
    g_period_frames = asked_period_frames;
    g_depth_after_queue = 0;
    reset_window(clock::now());

    if (dump_enabled()) {
        open_wav(asked_rate);
    }
}

void queued(uint32_t queue_frames, const int16_t* data, size_t sample_count) {
    if (!stats_enabled() && !dump_enabled()) {
        return;
    }
    const clock::time_point now = clock::now();
    std::lock_guard<std::mutex> guard(g_lock);

    if (dump_enabled()) {
        write_wav(data, sample_count);
    }
    if (!stats_enabled()) {
        return;
    }

    const uint32_t frames = static_cast<uint32_t>(sample_count / 2);
    note_depth(queue_frames);
    if (g_buffers == 0 || frames < g_buffer_min) g_buffer_min = frames;
    if (frames > g_buffer_max) g_buffer_max = frames;
    ++g_buffers;
    g_frames += frames;
    g_depth_after_queue = queue_frames + frames;

    // The amplitude separates a queue that is starving from a mixer that is
    // producing nothing: both leave the same silence on the speaker.
    for (size_t i = 0; i < sample_count; ++i) {
        const int32_t magnitude = data[i] < 0 ? -int32_t(data[i]) : int32_t(data[i]);
        if (magnitude > g_peak) g_peak = magnitude;
    }

    if (now - g_window_start >= std::chrono::seconds(2)) {
        report(now);
        reset_window(now);
    }
}

void polled(uint32_t queue_frames) {
    if (!stats_enabled()) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_lock);
    note_depth(queue_frames);
}

}  // namespace wr64::audiodiag
