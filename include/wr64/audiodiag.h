#pragma once

// Audio diagnostics: what the game produces, what the device consumes, and how
// much silence SDL had to invent to make up the difference.
//
// **Why this exists.** The symptom is crackle, and crackle has two unrelated
// causes that sound identical: samples that are wrong when they are handed over,
// and samples that are right but arrive too late. Everything in this port that
// could produce the first -- the recompiled audio microcode, the mixer's command
// list, the channel swap, the volume scale -- sits upstream of SDL, and
// everything that could produce the second sits downstream of it. So the first
// job is to find out which side of `SDL_QueueAudio` the fault is on, and these
// two switches do exactly that.
//
// **`WR64_AUDIO_STATS=1`** reports every two seconds, on the same window as the
// frame-rate line. The number that matters is the last one: the device consumes
// samples in real time whether or not any are there, so
//
//     silence = elapsed x rate - (frames queued - change in queue depth)
//
// is the amount SDL had to zero-fill, measured rather than inferred. SDL's
// queued-audio drain asks for a whole device period every time, takes what the
// queue holds and fills the rest with silence without waiting, so a queue that
// runs shallower than one period crackles continuously and by an amount this
// figure states exactly.
//
// **`WR64_AUDIO_DUMP=1`** writes what is handed to `SDL_QueueAudio` -- after the
// channel swap and the volume scale, byte for byte -- to a WAV in the settings
// folder, one file per device open so the sample rate in its header is always
// right. If that file plays back clean, nothing upstream of SDL is at fault and
// the queue figures above are the whole story; if it crackles, the queue figures
// do not matter and the fault is in the microcode or the mix.
//
// Both are off unless the variable is set, and when off nothing here is called
// with anything to do.

#include <cstddef>
#include <cstdint>

namespace wr64::audiodiag {

// Whether the statistics are switched on. Checked once.
bool stats_enabled();

// Whether the WAV dump is switched on. Checked once.
bool dump_enabled();

// Called after the output device is opened, with what the port asked for and
// what the machine's default device actually runs at. The two differing means
// SDL is resampling, which is worth knowing and is invisible otherwise: the port
// opens with `SDL_AUDIO_ALLOW_ANY_CHANGE` unset, so the spec SDL hands back
// mirrors the request no matter what the hardware does.
void device_opened(uint32_t asked_rate, uint32_t asked_period_frames,
                   uint32_t device_rate, uint32_t device_period_frames);

// Called from queue_samples with the queue depth measured just before the
// samples go in, and the samples themselves, exactly as SDL will receive them.
void queued(uint32_t queue_frames, const int16_t* data, size_t sample_count);

// Called from get_frames_remaining with what is about to be reported to the
// game. This is the game's own view of the queue, and the moment it decides how
// much more audio to make.
void polled(uint32_t queue_frames);

}  // namespace wr64::audiodiag
