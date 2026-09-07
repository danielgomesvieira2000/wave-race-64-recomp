#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace wr64::music {
void initialize(const std::filesystem::path& music_directory,
                const std::filesystem::path& bundled_directory = {});
void set_enabled(bool enabled);
void set_volume(double percent);
void set_replacement_volume(double percent);
void mix(int16_t* samples, size_t sample_count, uint32_t sample_rate);

// Only ready replacements suppress native music; missing tracks retain it.
bool replacement_available(uint8_t sequence);
void sequence_started(unsigned player, uint8_t sequence);
void sequence_stopped(unsigned player);
void sequence_state(unsigned player, uint8_t sequence, float gain, bool paused);
}
