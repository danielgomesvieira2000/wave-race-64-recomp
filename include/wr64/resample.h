#pragma once

// Sample-rate conversion, done in the port rather than left to SDL.
//
// **Why this exists.** The game asks for 32000 Hz in the menus and 26900 in a
// race; almost no sound card runs at either, so something has to resample. SDL
// will, and does it badly -- not because its filter is poor but because it loses
// continuity at every block boundary, and it is fed one device period at a time.
// Driving `SDL2.dll` directly with a sine, where a sine in must be a sine out
// and everything else in the result is the converter's:
//
//     26900 -> 48000, 8 kHz tone      SINAD
//       fed in 448-frame blocks       14.4 dB
//       fed in one single call        43.4 dB
//
// Twenty-nine decibels between the same converter's two extremes. Sweeping the
// block size shows what it is: at 32000 -> 48000 the figure rises 2.7 dB per
// doubling of the block (27.1, 29.9, 32.6, 35.1), which is the signature of a
// fixed amount of damage at each boundary, repeated at the block rate. SDL's
// `SDL_AUDIO_RESAMPLING_MODE` hint changes nothing -- fast, medium and best all
// measure 14.4.
//
// The block size is the device period, and the device period is also what
// decides whether the queue can be kept ahead of the sound card (see
// kQueueHeadroomMs in src/callbacks.cpp). Leaving the conversion to SDL therefore
// sets the crackle against the dirt: short periods for one, long periods for the
// other, and 23 dB at the best setting available.
//
// So the port converts instead, once, with state that persists across buffers --
// there are no block boundaries at all -- and opens the device at whatever rate
// the hardware actually runs, so SDL has nothing left to resample.
//
// **What it is.** A windowed-sinc interpolator: 64 taps, Kaiser window at beta
// 8.6, cutoff at 0.455 of the source rate, evaluated at arbitrary fractional
// positions from a table of 256 points per source sample with linear
// interpolation between them, and normalised by the sum of its own weights so the
// gain does not ripple with phase. When downsampling the cutoff and the kernel's
// width scale with the ratio, which is what keeps it from aliasing.
//
// Roughly ten million multiply-adds a second at 48000 Hz stereo, on a thread
// that was already copying every sample twice. The delay it adds is half the
// kernel, about 1.2 ms.
//
// `WR64_AUDIO_NO_RESAMPLE=1` hands the job back to SDL, for comparing the two.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wr64::resample {

// Prepares a conversion from one rate to another and discards any state from the
// previous one. `src == dst` is a pass-through and costs nothing.
void configure(uint32_t src_rate, uint32_t dst_rate);

// Converts one buffer of interleaved 16-bit stereo. `sample_count` counts
// individual samples, not frames, matching queue_samples. `out` is replaced with
// the result, which is a different length and, because the conversion carries
// state, not a whole number of anything in particular.
void process(const int16_t* in, size_t sample_count, std::vector<int16_t>& out);

// What a count of destination frames is worth in source frames. The SDL queue
// holds converted audio, and the game has to be told a depth in its own rate.
uint64_t to_source_frames(uint64_t dst_frames);

}  // namespace wr64::resample
