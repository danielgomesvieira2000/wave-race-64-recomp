// Standalone output-level tests for the host music mixer. Run each scenario in
// a fresh process: missing, invalid, mix, bundled, local-override, invalid-local.
// Every scenario initializes its own temporary pack exactly once.
#include "wr64/music.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kRate = 48000;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct TemporaryPack {
    std::filesystem::path path;
    TemporaryPack() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                ("wr64-music-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path)) return;
        }
        require(false, "create temporary pack");
    }
    ~TemporaryPack() { std::filesystem::remove_all(path); }
};

void little_endian(std::ofstream& output, uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) output.put(char((value >> (8 * i)) & 255));
}

void write_wav(const std::filesystem::path& path, const std::vector<int16_t>& pcm,
               uint32_t rate = kRate) {
    std::ofstream output(path, std::ios::binary);
    const uint32_t data_bytes = uint32_t(pcm.size() * sizeof(int16_t));
    output.write("RIFF", 4); little_endian(output, 36 + data_bytes, 4);
    output.write("WAVEfmt ", 8); little_endian(output, 16, 4);
    little_endian(output, 1, 2); little_endian(output, 2, 2);
    little_endian(output, rate, 4); little_endian(output, rate * 4, 4);
    little_endian(output, 4, 2); little_endian(output, 16, 2);
    output.write("data", 4); little_endian(output, data_bytes, 4);
    for (int16_t sample : pcm) little_endian(output, uint16_t(sample), 2);
    require(output.good(), "write WAV fixture");
}

std::vector<int16_t> native_bed(size_t frames) {
    std::vector<int16_t> result(frames * 2);
    for (size_t frame = 0; frame < frames; ++frame) {
        result[frame * 2] = int16_t(137 + frame % 1100);
        result[frame * 2 + 1] = int16_t(-719 - frame % 700);
    }
    return result;
}

std::vector<int16_t> render(size_t frames, uint32_t rate = kRate) {
    std::vector<int16_t> pcm(frames * 2, 0);
    wr64::music::mix(pcm.data(), pcm.size(), rate);
    return pcm;
}

void reset() {
    wr64::music::set_enabled(true);
    wr64::music::set_volume(100);
    wr64::music::set_replacement_volume(100);
    for (unsigned player = 0; player < 4; ++player) wr64::music::sequence_stopped(player);
    render(1024); // Allow the documented 10 ms transition ramp to finish.
}

void start(uint8_t sequence, unsigned player = 1) {
    wr64::music::sequence_started(player, sequence);
    wr64::music::sequence_state(player, sequence, 1.0f, false);
}

void near(int16_t actual, int expected, const char* message, int tolerance = 1) {
    if (std::abs(int(actual) - expected) > tolerance) {
        std::fprintf(stderr, "%s: expected %d, got %d\n", message, expected, actual);
        require(false, message);
    }
}

void fallback_tests(const std::filesystem::path& path) {
    wr64::music::initialize(path);
    reset();
    require(!wr64::music::replacement_available(3), "absent/invalid main theme falls back");
    require(!wr64::music::replacement_available(6), "absent/invalid Dolphin Park falls back");
    for (uint8_t sequence : {9, 10, 11, 14})
        require(!wr64::music::replacement_available(sequence), "absent shared course track falls back");
    for (uint8_t sequence : {3, 6, 255}) {
        start(sequence);
        auto pcm = native_bed(2400);
        const auto original = pcm;
        wr64::music::mix(pcm.data(), pcm.size(), kRate);
        require(pcm == original, "unavailable replacements leave all native samples intact");
    }
}

void bundled_tests(const std::filesystem::path& path, const std::string& scenario) {
    const auto local = path / "local";
    const auto bundled = path / "bundled";
    std::filesystem::create_directory(local);
    std::filesystem::create_directory(bundled);
    std::vector<int16_t> theme(2400 * 2), dolphin(2400 * 2);
    for (size_t frame = 0; frame < 2400; ++frame) {
        theme[frame * 2] = 12000;
        theme[frame * 2 + 1] = -8000;
        dolphin[frame * 2] = int16_t(4000 + 2 * frame);
        dolphin[frame * 2 + 1] = int16_t(-6000 - 3 * frame);
    }
    write_wav(bundled / "main_theme.wav", theme);
    write_wav(bundled / "dolphin_park.wav", dolphin);
    std::ofstream(bundled / "loops.json") << R"({"dolphin_park":{
        "start_seconds":0.016666666666666666,
        "end_seconds":0.041666666666666664,
        "crossfade_seconds":0.004166666666666667,
        "gain_db":-6.020599913279624}})";

    if (scenario == "local-override") {
        for (size_t frame = 0; frame < 2400; ++frame) {
            theme[frame * 2] = 6000;
            theme[frame * 2 + 1] = -2000;
        }
        write_wav(local / "main_theme.wav", theme);
        // The Dolphin entry intentionally differs from the bundled settings:
        // missing local audio must also select metadata from the bundled pack.
        std::ofstream(local / "loops.json") << R"({
            "main_theme":{"start_seconds":0,"end_seconds":0.05,
                "crossfade_seconds":0,"gain_db":-6.020599913279624},
            "dolphin_park":{"start_seconds":0,"end_seconds":0.05,
                "crossfade_seconds":0,"gain_db":0}})";
    } else if (scenario == "invalid-local") {
        std::ofstream(local / "main_theme.wav") << "corrupt local replacement";
    }

    wr64::music::initialize(local, bundled);
    reset();
    start(3);
    if (scenario == "invalid-local") {
        require(!wr64::music::replacement_available(3),
                "invalid local recording falls back to native instead of silently using bundle");
        auto pcm = native_bed(1200);
        const auto original = pcm;
        wr64::music::mix(pcm.data(), pcm.size(), kRate);
        require(pcm == original, "invalid local recording preserves native music and effects");
    } else {
        require(wr64::music::replacement_available(3), "selected local or bundled main theme is ready");
        const auto pcm = render(700);
        const bool overridden = scenario == "local-override";
        near(pcm[600 * 2], overridden ? 3000 : 12000,
             "local recording and its gain take priority over bundled theme");
        near(pcm[600 * 2 + 1], overridden ? -1000 : -8000,
             "selected theme keeps its stereo channels");
    }

    reset(); start(6);
    require(wr64::music::replacement_available(6), "missing local cue uses bundled recording");
    const auto pcm = render(2100);
    near(pcm[600 * 2], 2600, "bundled recording uses its own gain metadata");
    near(pcm[600 * 2 + 1], -3900, "bundled gain preserves both stereo channels");
    near(pcm[2000 * 2], 3000, "bundled recording uses its own repeat and crossfade points");
    near(pcm[2000 * 2 + 1], -4500, "bundled repeat position is correct in the right channel");
}

void mix_tests(const std::filesystem::path& path) {
    // A low-amplitude ramp distinguishes channels and exposes sample positions.
    // The first 2,000 frames are played, with a 200-frame crossfade back to 800.
    std::vector<int16_t> theme(2400 * 2), dolphin(2400 * 2);
    for (size_t frame = 0; frame < 2400; ++frame) {
        theme[frame * 2] = int16_t(2000 + 2 * frame);
        theme[frame * 2 + 1] = int16_t(-3000 - 3 * frame);
        dolphin[frame * 2] = 30000;
        dolphin[frame * 2 + 1] = -30000;
    }
    write_wav(path / "main_theme.wav", theme);
    write_wav(path / "dolphin_park.wav", dolphin);
    write_wav(path / "seafoam_shoreline.wav", theme);
    std::ofstream(path / "loops.json") << R"({"main_theme":{
        "start_seconds":0.016666666666666666,
        "end_seconds":0.041666666666666664,
        "crossfade_seconds":0.004166666666666667}})";
    wr64::music::initialize(path);
    reset();
    require(wr64::music::replacement_available(3), "valid main theme available");
    require(wr64::music::replacement_available(6), "valid Dolphin Park available");
    for (uint8_t sequence : {9, 10, 11, 14})
        require(wr64::music::replacement_available(sequence), "shared course recording covers all four aliases");
    require(!wr64::music::replacement_available(5), "course-preview cue retains original music");
    require(!wr64::music::replacement_available(255), "unknown cue uses original");

    start(255);
    auto bed = native_bed(1000);
    const auto original = bed;
    wr64::music::mix(bed.data(), bed.size(), kRate);
    require(bed == original, "unmapped native music and effects remain unchanged");

    reset(); start(3);
    const auto first = render(800);
    near(first[600 * 2], 3200, "48 kHz left channel source position");
    near(first[600 * 2 + 1], -4800, "48 kHz right channel source position");
    require(std::abs(first[0]) < 20, "track starts through a short click-suppression ramp");
    render(333);
    start(3);
    require(render(800) == first, "a successful same-ID sequence reload restarts the recording");

    reset(); start(3);
    const auto resampled = render(410, 32000);
    near(resampled[400 * 2], 3200, "32 kHz playback advances 1.5 source frames per frame");
    near(resampled[401 * 2], 3203, "32 kHz left fractional sample interpolation");
    near(resampled[401 * 2 + 1], -4805, "32 kHz right fractional sample interpolation");

    reset(); start(3);
    const auto looped = render(5300);
    // The raw end/start jump is thousands of PCM units; a proper crossfade has
    // no jump greater than 20 here, including the second and third boundaries.
    for (size_t frame = 1790; frame < 5300; ++frame) {
        for (size_t channel = 0; channel < 2; ++channel) {
            require(std::abs(int(looped[frame * 2 + channel]) -
                             int(looped[(frame - 1) * 2 + channel])) <= 20,
                    "repeat crossfade stays continuous across multiple loop boundaries");
        }
    }
    near(looped[2000 * 2], 4000, "crossfade resumes after the already-blended loop head");
    near(looped[3000 * 2], 4000, "subsequent loop has the same timing");

    auto after_pause = [](size_t extra_paused_frames) {
        reset(); start(3); render(600);
        wr64::music::sequence_state(1, 3, 1.0f, true);
        render(600);
        auto paused = native_bed(extra_paused_frames);
        const auto unmodified = paused;
        wr64::music::mix(paused.data(), paused.size(), kRate);
        require(paused == unmodified, "paused replacement is silent after its ramp");
        wr64::music::sequence_state(1, 3, 1.0f, false);
        return render(600);
    };
    const auto immediate_resume = after_pause(1);
    const auto late_resume = after_pause(9600);
    require(immediate_resume == late_resume, "pause freezes playback position after the fade-out");

    reset(); start(3); render(600);
    wr64::music::sequence_state(1, 3, 0.25f, false);
    const auto faded = render(600);
    near(faded[550 * 2], 1075, "sequence fade gain controls replacement amplitude");
    wr64::music::sequence_stopped(1);
    render(600);
    auto stopped = native_bed(12000);
    const auto before_stop_mix = stopped;
    wr64::music::mix(stopped.data(), stopped.size(), kRate);
    require(stopped == before_stop_mix, "stopped sequence cannot leave music playing indefinitely");

    reset(); start(3); render(600);
    wr64::music::set_enabled(false);
    require(!wr64::music::replacement_available(3), "Original selection restores native BGM eligibility");
    render(600);
    bed = native_bed(1000);
    const auto disabled_original = bed;
    wr64::music::mix(bed.data(), bed.size(), kRate);
    require(bed == disabled_original, "Original selection leaves native audio intact after ramp");

    reset(); start(6); render(600);
    std::vector<int16_t> loud(200 * 2);
    for (size_t frame = 0; frame < 200; ++frame) {
        loud[frame * 2] = 12000;
        loud[frame * 2 + 1] = -12000;
    }
    wr64::music::mix(loud.data(), loud.size(), kRate);
    for (size_t frame = 0; frame < 200; ++frame) {
        require(loud[frame * 2] == 32767, "positive mix saturates without wrapping");
        require(loud[frame * 2 + 1] == -32768, "negative mix saturates without wrapping");
    }
    wr64::music::set_volume(0);
    auto muted = native_bed(600);
    wr64::music::mix(muted.data(), muted.size(), kRate);
    require(std::all_of(muted.begin(), muted.end(), [](int16_t value) { return value == 0; }),
            "zero main volume mutes native and replacement audio together");
}
}

int main(int argc, char** argv) {
    require(argc == 2, "usage: music_test missing|invalid|mix|bundled|local-override|invalid-local");
    TemporaryPack pack;
    const std::string scenario = argv[1];
    if (scenario == "missing") {
        fallback_tests(pack.path);
    } else if (scenario == "invalid") {
        std::ofstream(pack.path / "main_theme.wav") << "not a WAV recording";
        write_wav(pack.path / "dolphin_park.wav", {1000, -1000, 2000, -2000}, 44100);
        fallback_tests(pack.path);
    } else if (scenario == "bundled" || scenario == "local-override" || scenario == "invalid-local") {
        bundled_tests(pack.path, scenario);
    } else {
        require(scenario == "mix", "unknown test scenario");
        mix_tests(pack.path);
    }
    std::printf("Music mixer %s checks passed.\n", scenario.c_str());
}
