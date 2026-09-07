#include "wr64/music.h"

#include <SDL.h>
#include <json/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace wr64::music {
namespace {
constexpr size_t kPlayers = 4;
constexpr uint32_t kRate = 48000;
struct Track {
    uint8_t sequence;
    const char* name;
    std::vector<int16_t> pcm;
    double start = 0, end = 0, crossfade = 0;
    float gain = 1;
    Track(uint8_t id, const char* filename) : sequence(id), name(filename) {}
};
std::array<Track, 9> tracks{{
    {3, "main_theme"}, {4, "options"}, {6, "dolphin_park"},
    {7, "sunny_beach"}, {8, "sunset_bay"}, {12, "twilight_city"},
    {13, "glacier_coast"}, {15, "victory"}, {9, "seafoam_shoreline"}
}};
struct Control {
    uint64_t generation = 0;
    uint8_t sequence = 0;
    bool playing = false, paused = false;
    float gain = 0;
};
struct Voice {
    uint64_t generation = 0;
    const Track* track = nullptr;
    double position = 0;
    float gain = 0;
};
std::array<Control, kPlayers> controls{};
std::array<Voice, kPlayers> voices{};
std::mutex control_mutex;
std::atomic<bool> enabled{true};
std::atomic<float> main_volume{1.0f}, replacement_volume{0.65f};

const Track* find_track(uint8_t sequence) {
    // One optional recording fills the courses without a matching remix.
    // Share its decoded samples rather than loading four identical copies.
    if (sequence == 10 || sequence == 11 || sequence == 14) sequence = 9;
    for (const auto& track : tracks) {
        if (track.sequence == sequence && !track.pcm.empty()) return &track;
    }
    return nullptr;
}

nlohmann::json read_loop_config(const std::filesystem::path& directory) {
    if (directory.empty()) return nlohmann::json::object();
    try {
        std::ifstream file(directory / "loops.json");
        if (file) {
            auto config = nlohmann::json::parse(file);
            if (!config.is_object()) throw std::runtime_error("expected a track settings object");
            return config;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[music] ignoring invalid %s: %s\n",
                     (directory / "loops.json").string().c_str(), error.what());
    }
    return nlohmann::json::object();
}

float sample(const Track& track, double position, size_t channel) {
    const size_t frame = std::min(size_t(position), track.pcm.size() / 2 - 1);
    const size_t next = std::min(frame + 1, track.pcm.size() / 2 - 1);
    const float fraction = float(position - std::floor(position));
    return track.pcm[frame * 2 + channel] * (1 - fraction) + track.pcm[next * 2 + channel] * fraction;
}

float loop_sample(const Track& track, double position, size_t channel) {
    float value = sample(track, position, channel);
    if (track.crossfade > 0 && position >= track.end - track.crossfade) {
        const double offset = position - (track.end - track.crossfade);
        const float blend = float(offset / track.crossfade);
        value = value * (1 - blend) + sample(track, track.start + offset, channel) * blend;
    }
    return value;
}

// Opt-in QA capture: four PCM16 channels, native L/R followed by final L/R.
// File work occurs only with WR64_AUDIO_CAPTURE set. Rate changes close the
// previous WAV and open a new segment, without changing audio queue timing.
class Capture {
    FILE* file = nullptr;
    uint32_t rate = 0, bytes = 0, segment = 0;
    std::string prefix;
    void header() {
        const uint32_t words[] = {36 + bytes, 16, 0x00040001, rate, rate * 8, 0x00100008, bytes};
        std::fseek(file, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, file); std::fwrite(&words[0], 4, 1, file);
        std::fwrite("WAVEfmt ", 1, 8, file); std::fwrite(&words[1], 4, 5, file);
        std::fwrite("data", 1, 4, file); std::fwrite(&words[6], 4, 1, file);
    }
    void close() {
        if (file) { header(); std::fclose(file); file = nullptr; }
    }
public:
    Capture() { if (const char* path = std::getenv("WR64_AUDIO_CAPTURE")) prefix = path; }
    ~Capture() { close(); }
    bool active(uint32_t frequency) {
        if (prefix.empty()) return false;
        if (frequency != rate) {
            close(); rate = frequency; bytes = 0;
            const auto path = prefix + "-" + std::to_string(segment++) + ".wav";
            file = std::fopen(path.c_str(), "wb");
            if (file) { header(); std::fprintf(stderr, "[music] audio capture: %s (%u Hz, native L/R + mixed L/R)\n", path.c_str(), rate); }
        }
        return file != nullptr;
    }
    void write(const std::vector<int16_t>& samples) {
        if (!file || bytes > 0xF0000000u) return;
        bytes += uint32_t(std::fwrite(samples.data(), sizeof(int16_t), samples.size(), file) * sizeof(int16_t));
    }
};
}

void initialize(const std::filesystem::path& directory, const std::filesystem::path& bundled_directory) {
    // Called once before the game/audio threads start; decoded data is immutable
    // thereafter. A bad or absent file never silences its native sequence.
    const auto local_config = read_loop_config(directory);
    const auto bundled_config = read_loop_config(bundled_directory);
    for (auto& track : tracks) {
        const auto filename = std::string(track.name) + ".wav";
        auto path = directory / filename;
        std::error_code error;
        const bool local_exists = std::filesystem::exists(path, error);
        const bool use_bundle = !local_exists && !error && !bundled_directory.empty();
        if (use_bundle) path = bundled_directory / filename;
        const auto& config = use_bundle ? bundled_config : local_config;
        const auto bytes = std::filesystem::file_size(path, error);
        if (error) continue;
        if (bytes > 256 * 1024 * 1024) {
            std::fprintf(stderr, "[music] %s exceeds 256 MiB; using original\n", track.name); continue;
        }
        SDL_AudioSpec spec{};
        Uint8* data = nullptr;
        Uint32 length = 0;
        if (!SDL_LoadWAV(path.string().c_str(), &spec, &data, &length)) {
            std::fprintf(stderr, "[music] cannot load %s: %s; using original\n", track.name, SDL_GetError()); continue;
        }
        if (spec.freq != kRate || spec.channels != 2 || spec.format != AUDIO_S16SYS || length < 4 || length % 4) {
            std::fprintf(stderr, "[music] %s must be stereo 48 kHz PCM16 WAV; using original\n", track.name);
            SDL_FreeWAV(data); continue;
        }
        track.pcm.resize(length / sizeof(int16_t));
        std::memcpy(track.pcm.data(), data, length);
        SDL_FreeWAV(data);
        track.start = 0;
        track.end = track.pcm.size() / 2;
        track.crossfade = std::min(double(kRate), track.end / 4);
        try {
            if (config.contains(track.name)) {
                const auto& loop = config.at(track.name);
                const double start = loop.at("start_seconds").get<double>() * kRate;
                const double end = loop.at("end_seconds").get<double>() * kRate;
                const double fade = loop.at("crossfade_seconds").get<double>() * kRate;
                if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(fade) ||
                    start < 0 || end > track.end || end - start < 2 || fade < 0 || fade > (end - start) / 2)
                    throw std::runtime_error("loop points are outside the recording");
                const double gain_db = loop.value("gain_db", 0.0);
                if (!std::isfinite(gain_db)) throw std::runtime_error("gain must be finite");
                track.start = start; track.end = end; track.crossfade = fade;
                track.gain = float(std::pow(10.0, std::clamp(gain_db, -30.0, 0.0) / 20.0));
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[music] %s: %s; looping the whole recording\n", track.name, error.what());
        }
        std::fprintf(stderr, "[music] loaded %s (%s): %.3f seconds; repeat %.3f..%.3f, crossfade %.3f seconds\n",
            track.name, use_bundle ? "bundled" : "local", track.pcm.size() / (2.0 * kRate),
            track.start / kRate, track.end / kRate, track.crossfade / kRate);
    }
}

bool replacement_available(uint8_t sequence) {
    return enabled.load(std::memory_order_relaxed) && find_track(sequence);
}
void set_enabled(bool value) { enabled.store(value, std::memory_order_relaxed); }
void set_volume(double percent) { main_volume.store(float(std::clamp(percent, 0.0, 100.0) / 100.0), std::memory_order_relaxed); }
void set_replacement_volume(double percent) { replacement_volume.store(float(std::clamp(percent, 0.0, 100.0) / 100.0), std::memory_order_relaxed); }

void sequence_started(unsigned player, uint8_t sequence) {
    if (player >= kPlayers) return;
    std::lock_guard lock(control_mutex);
    auto& control = controls[player];
    ++control.generation;
    control.sequence = sequence; control.playing = true; control.paused = false; control.gain = 0;
    std::fprintf(stderr, "[music] player %u started sequence %u (%s)\n", player, sequence,
        replacement_available(sequence) ? "replacement" : "original");
}
void sequence_stopped(unsigned player) {
    if (player >= kPlayers) return;
    std::lock_guard lock(control_mutex);
    auto& control = controls[player];
    if (control.playing) std::fprintf(stderr, "[music] player %u stopped sequence %u\n", player, control.sequence);
    control.playing = false;
}
void sequence_state(unsigned player, uint8_t sequence, float gain, bool paused) {
    if (player >= kPlayers) return;
    std::lock_guard lock(control_mutex);
    auto& control = controls[player];
    // Also catches sequences enabled by asynchronous DMA completion.
    if (!control.playing || control.sequence != sequence) {
        ++control.generation; control.sequence = sequence; control.playing = true;
    }
    control.paused = paused;
    control.gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.0f) : 0;
}

void mix(int16_t* samples, size_t sample_count, uint32_t sample_rate) {
    if (!sample_rate || sample_count < 2) return;
    std::array<Control, kPlayers> snapshot;
    { std::lock_guard lock(control_mutex); snapshot = controls; }
    const bool use_replacements = enabled.load(std::memory_order_relaxed);
    const float master = main_volume.load(std::memory_order_relaxed);
    const float music = replacement_volume.load(std::memory_order_relaxed);
    const double step = double(kRate) / sample_rate;
    // Ten-millisecond ramps suppress clicks at cue changes and mute/unmute.
    const float ramp = std::min(1.0f, 1.0f / (0.010f * sample_rate));
    static Capture capture;
    static std::vector<int16_t> evidence;
    const bool capturing = capture.active(sample_rate);
    if (capturing) evidence.resize((sample_count / 2) * 4);
    for (size_t p = 0; p < kPlayers; ++p) {
        auto& voice = voices[p];
        if (voice.generation != snapshot[p].generation) {
            voice.generation = snapshot[p].generation;
            voice.track = find_track(snapshot[p].sequence);
            voice.position = 0; voice.gain = 0;
        }
    }
    for (size_t frame = 0; frame < sample_count / 2; ++frame) {
        float left = 0, right = 0;
        for (size_t p = 0; p < kPlayers; ++p) {
            auto& voice = voices[p];
            const auto& control = snapshot[p];
            if (!voice.track) continue;
            const bool audible = use_replacements && control.playing && !control.paused;
            const float target = audible ? control.gain * music * voice.track->gain : 0;
            voice.gain += std::clamp(target - voice.gain, -ramp, ramp);
            if (voice.gain > 0) {
                left += loop_sample(*voice.track, voice.position, 0) * voice.gain;
                right += loop_sample(*voice.track, voice.position, 1) * voice.gain;
            }
            // Original/custom switching preserves the replacement's position.
            // A genuine new load of the same sequence resets its generation.
            if ((control.playing && !control.paused) || voice.gain > 0) {
                voice.position += step;
                if (voice.position >= voice.track->end) {
                    const double restart = voice.track->start + voice.track->crossfade;
                    voice.position = restart + std::fmod(voice.position - voice.track->end, voice.track->end - restart);
                }
            }
        }
        if (capturing) {
            evidence[frame * 4] = samples[frame * 2];
            evidence[frame * 4 + 1] = samples[frame * 2 + 1];
        }
        // Saturate in a wider accumulator; loud effects must never wrap around.
        samples[frame * 2] = int16_t(std::clamp(std::lround((samples[frame * 2] + left) * master), -32768l, 32767l));
        samples[frame * 2 + 1] = int16_t(std::clamp(std::lround((samples[frame * 2 + 1] + right) * master), -32768l, 32767l));
        if (capturing) {
            evidence[frame * 4 + 2] = samples[frame * 2];
            evidence[frame * 4 + 3] = samples[frame * 2 + 1];
        }
    }
    if (capturing) capture.write(evidence);
}
}
