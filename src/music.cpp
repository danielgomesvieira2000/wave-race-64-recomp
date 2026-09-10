// The Sound tab's Music Volume.
//
// Main Volume can be applied to the finished buffer on its way to the sound
// card, because everything in it should get quieter together. Music cannot: by
// the time samples reach the port, the audio microcode has already mixed the
// music and the effects into one stereo stream, and nothing downstream can tell
// them apart again. So this reaches into the game's own audio engine instead.
//
// Wave Race 64 uses the sequence-player engine that most of this era's Nintendo
// games use -- the one whose decompilations call it `gSequencePlayers`, with
// per-player fade volumes and sixteen channels each. Each player recomputes
//
//     appliedFadeVolume = fadeVolume * fadeVolumeScale
//
// whenever its `recalculateVolume` bit is set, and `fadeVolumeScale` exists
// precisely so that something outside the sequence can duck it. Writing a scale
// there and asking for the recompute is what the game's own audio commands do;
// this does the same thing once a frame, from the same thread the game's own
// audio commands run on.
//
// Music and effects are separated by which player they are on: the game's music
// sequences are the ones this port's records list from 3 upwards, and the
// effects live on a player below that. WR64_MUSIC_TRACE prints every player's
// state so that this can be checked rather than assumed.

#include "wr64/music.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wr64::music {
namespace {

// USA Rev A. gSequencePlayers is four players of 0x140 bytes, from the
// decompilation's own linker symbols: the array is 0x500 bytes and the symbol
// after it is gSequenceChannels.
constexpr uint32_t kSequencePlayers = 0x8003FCC8;
constexpr uint32_t kPlayerStride = 0x140;
constexpr uint32_t kPlayerCount = 4;

// Offsets inside a player, by the struct's own field order.
constexpr uint32_t kFlags            = 0x00;  // byte: bitfield, see below
constexpr uint32_t kSeqId            = 0x04;  // byte
constexpr uint32_t kFadeVolume       = 0x18;  // float
constexpr uint32_t kVolume           = 0x20;  // float
constexpr uint32_t kFadeVolumeScale  = 0x28;  // float
constexpr uint32_t kAppliedFadeVolume = 0x2C; // float

// The bitfield at the top of the struct. MIPS packs bitfields from the most
// significant bit down, so `enabled` is the top bit and `recalculateVolume` is
// the sixth.
constexpr uint8_t kEnabledBit = 0x80;
constexpr uint8_t kRecalculateBit = 0x04;

// The lowest sequence id that is music. The game's sequences from 3 up are the
// title theme, the menus and the courses; the effects sit below it.
constexpr int kFirstMusicSequence = 3;

uint8_t read_byte(const uint8_t* ram, uint32_t address) {
    return ram[(address & 0x7FFFFFu) ^ 3u];
}

void write_byte(uint8_t* ram, uint32_t address, uint8_t value) {
    ram[(address & 0x7FFFFFu) ^ 3u] = value;
}

float read_float(const uint8_t* ram, uint32_t address) {
    float v = 0.0f;
    std::memcpy(&v, ram + (address & 0x7FFFFFu), sizeof(v));
    return std::isfinite(v) ? v : 0.0f;
}

void write_float(uint8_t* ram, uint32_t address, float value) {
    std::memcpy(ram + (address & 0x7FFFFFu), &value, sizeof(value));
}

std::atomic<int> g_volume{ 100 };

// What the game asked for, and what was last written over it, per player. See
// the comment in apply(): the two together are what lets the slider scale the
// game's own value instead of replacing it.
float g_base[kPlayerCount] = { 1.0f, 1.0f, 1.0f, 1.0f };
float g_written[kPlayerCount] = { -1.0f, -1.0f, -1.0f, -1.0f };

}  // namespace

void set_volume(double percent) {
    const int clamped = static_cast<int>(std::lround(std::clamp(percent, 0.0, 100.0)));
    const int previous = g_volume.exchange(clamped, std::memory_order_relaxed);
    if (clamped != previous) {
        std::fprintf(stderr, "[wr64] music volume: %d%%\n", clamped);
        std::fflush(stderr);
    }
}

void apply(uint8_t* rdram) {
    static const bool disabled = std::getenv("WR64_NO_MUSIC_VOLUME") != nullptr;
    if (disabled || rdram == nullptr) return;

    static const char* trace_path = std::getenv("WR64_MUSIC_TRACE");
    static std::FILE* trace = nullptr;
    static int traced = 0;
    if (trace_path != nullptr && trace == nullptr && traced == 0) {
        trace = std::fopen(trace_path, "w");
        traced = trace == nullptr ? -1 : 0;
        if (trace != nullptr) {
            std::fprintf(trace, "player,enabled,seqId,fadeVolume,volume,"
                                "fadeVolumeScale,appliedFadeVolume\n");
        }
    }

    const float scale = static_cast<float>(g_volume.load(std::memory_order_relaxed)) / 100.0f;

    for (uint32_t i = 0; i < kPlayerCount; ++i) {
        const uint32_t player = kSequencePlayers + i * kPlayerStride;
        const uint8_t flags = read_byte(rdram, player + kFlags);
        const uint8_t seq_id = read_byte(rdram, player + kSeqId);
        const bool enabled = (flags & kEnabledBit) != 0;

        if (trace != nullptr && traced < 40000) {
            ++traced;
            std::fprintf(trace, "%u,%d,%u,%.3f,%.3f,%.3f,%.3f\n", i, enabled ? 1 : 0, seq_id,
                         read_float(rdram, player + kFadeVolume),
                         read_float(rdram, player + kVolume),
                         read_float(rdram, player + kFadeVolumeScale),
                         read_float(rdram, player + kAppliedFadeVolume));
            std::fflush(trace);
        }

        if (!enabled || seq_id < kFirstMusicSequence) {
            continue;   // silent, or the effects player
        }

        // The game uses this field itself -- it was measured ducking the title
        // music to 0.55 -- so the setting has to compose with the game's value
        // rather than replace it, and it cannot do that by multiplying in place
        // without compounding a little more every frame.
        //
        // So each player's own value is shadowed. Anything there that this did
        // not write is the game's, and becomes the new base; what gets written
        // is that base scaled by the slider. A duck to 0.55 with the slider at
        // 40% is 0.22, and putting the slider back to 100% restores 0.55 rather
        // than 1.0.
        const float current = read_float(rdram, player + kFadeVolumeScale);
        if (current != g_written[i]) {
            g_base[i] = current;
        }
        const float desired = g_base[i] * scale;
        if (current != desired) {
            write_float(rdram, player + kFadeVolumeScale, desired);
            write_byte(rdram, player + kFlags, static_cast<uint8_t>(flags | kRecalculateBit));
            g_written[i] = desired;
        }
    }
}

}  // namespace wr64::music
