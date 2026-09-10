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

// The game's own frame counter, so a line here lines up with a line of the
// feedback trace and an event can be matched to the channel that voiced it.
constexpr uint32_t kTick = 0x80151960;

// A player's sixteen channel pointers, and the fields inside a channel that say
// what it is doing. Offsets are the struct's own field order; `enabled` is the
// top bit of the first byte, as in the player.
constexpr uint32_t kChannels = 0x30;
constexpr uint32_t kChannelBankId = 0x06;      // byte
constexpr uint32_t kChannelInstrument = 0x1C;  // s16
constexpr uint32_t kChannelVolumeScale = 0x20; // float
constexpr uint32_t kChannelVolume = 0x24;      // float
constexpr uint32_t kChannelApplied = 0x2C;     // float
constexpr uint32_t kChannelCount = 16;

int32_t read_word(const uint8_t* ram, uint32_t address) {
    int32_t v = 0;
    std::memcpy(&v, ram + (address & 0x7FFFFFu), sizeof(v));
    return v;
}

int16_t read_half(const uint8_t* ram, uint32_t address) {
    int16_t v = 0;
    std::memcpy(&v, ram + ((address & 0x7FFFFFu) ^ 2u), sizeof(v));
    return v;
}

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
std::atomic<int> g_announcer{ 100 };

// The announcer's channels on the effects player, and the channel field that
// says a channel's volume needs recomputing.
//
// Nothing names them; they were found by lining channel activity up against the
// events in the feedback trace. Channel 14 begins three frames after the
// countdown starts and runs for 144 -- the count and the "GO!" -- and it speaks
// again on the course screens and after a retirement, while every other channel
// of that player either runs continuously (the engine, the water) or fires in
// bursts at splashes and collisions. Channel 13 accompanies it on the longer
// lines, starting and ending on exactly the same frames.
constexpr uint32_t kAnnouncerChannels[] = { 13, 14 };
constexpr uint32_t kChannelChanges = 0x01;     // byte: freqScale 0x80, volume 0x40, pan 0x20
constexpr uint8_t kChannelVolumeChanged = 0x40;

float g_channel_base[16] = {};
float g_channel_written[16] = {};
bool g_channel_seen[16] = {};

// What the game asked for, and what was last written over it, per player. See
// the comment in apply(): the two together are what lets the slider scale the
// game's own value instead of replacing it.
float g_base[kPlayerCount] = { 1.0f, 1.0f, 1.0f, 1.0f };
float g_written[kPlayerCount] = { -1.0f, -1.0f, -1.0f, -1.0f };

}  // namespace

void set_announcer_volume(double percent) {
    const int clamped = static_cast<int>(std::lround(std::clamp(percent, 0.0, 100.0)));
    const int previous = g_announcer.exchange(clamped, std::memory_order_relaxed);
    if (clamped != previous) {
        std::fprintf(stderr, "[wr64] announcer volume: %d%%\n", clamped);
        std::fflush(stderr);
    }
}

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
            std::fprintf(trace, "tick,player,enabled,seqId,fadeVolume,volume,"
                                "fadeVolumeScale,appliedFadeVolume,channels\n");
        }
    }

    const float scale = static_cast<float>(g_volume.load(std::memory_order_relaxed)) / 100.0f;

    for (uint32_t i = 0; i < kPlayerCount; ++i) {
        const uint32_t player = kSequencePlayers + i * kPlayerStride;
        const uint8_t flags = read_byte(rdram, player + kFlags);
        const uint8_t seq_id = read_byte(rdram, player + kSeqId);
        const bool enabled = (flags & kEnabledBit) != 0;

        if (trace != nullptr && traced < 200000) {
            ++traced;
            const uint32_t tick = static_cast<uint32_t>(read_word(rdram, kTick));
            std::fprintf(trace, "%u,%u,%d,%u,%.3f,%.3f,%.3f,%.3f", tick, i, enabled ? 1 : 0, seq_id,
                         read_float(rdram, player + kFadeVolume),
                         read_float(rdram, player + kVolume),
                         read_float(rdram, player + kFadeVolumeScale),
                         read_float(rdram, player + kAppliedFadeVolume));
            // Every channel that is playing, with the instrument it is playing.
            // This is how the announcer gets found: it is not a player of its
            // own -- players two and three are never enabled -- so it is
            // channels of the effects player, and the way to tell which is to
            // line their arrivals up against the feedback trace's events.
            if (enabled) {
                for (uint32_t c = 0; c < kChannelCount; ++c) {
                    const uint32_t channel =
                        static_cast<uint32_t>(read_word(rdram, player + kChannels + c * 4));
                    if (channel == 0) continue;
                    const uint8_t channel_flags = read_byte(rdram, channel + kFlags);
                    if ((channel_flags & kEnabledBit) == 0) continue;
                    const float applied = read_float(rdram, channel + kChannelApplied);
                    if (applied <= 0.0f) continue;
                    std::fprintf(trace, " ch%u:bank%u:inst%d:%.2f", c,
                                 read_byte(rdram, channel + kChannelBankId),
                                 read_half(rdram, channel + kChannelInstrument), applied);
                }
            }
            std::fprintf(trace, "\n");
            std::fflush(trace);
        }

        // The announcer lives on the effects player, so that player is not
        // skipped: its voice channels are scaled while everything else on it --
        // the engine, the water, the collisions -- is left alone.
        if (enabled && seq_id < kFirstMusicSequence) {
            const float announcer =
                static_cast<float>(g_announcer.load(std::memory_order_relaxed)) / 100.0f;
            for (const uint32_t c : kAnnouncerChannels) {
                const uint32_t channel =
                    static_cast<uint32_t>(read_word(rdram, player + kChannels + c * 4));
                if (channel == 0) continue;
                const uint8_t channel_flags = read_byte(rdram, channel + kFlags);
                if ((channel_flags & kEnabledBit) == 0) {
                    g_channel_seen[c] = false;   // it will be set up again when it speaks
                    continue;
                }
                // The same shadowing as the music below: the sequence writes
                // this field itself, so anything here that this did not write is
                // the game's own and becomes the base.
                const float current = read_float(rdram, channel + kChannelVolumeScale);
                if (!g_channel_seen[c] || current != g_channel_written[c]) {
                    g_channel_base[c] = current;
                    g_channel_seen[c] = true;
                }
                const float wanted = g_channel_base[c] * announcer;
                if (current != wanted) {
                    write_float(rdram, channel + kChannelVolumeScale, wanted);
                    // Tell the channel its volume needs recomputing, the way the
                    // sequence's own volume command does.
                    const uint8_t changes = read_byte(rdram, channel + kChannelChanges);
                    write_byte(rdram, channel + kChannelChanges,
                               static_cast<uint8_t>(changes | kChannelVolumeChanged));
                    g_channel_written[c] = wanted;
                }
            }
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
