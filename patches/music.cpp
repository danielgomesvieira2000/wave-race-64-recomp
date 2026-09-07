#include "recomp.h"
#include "wr64/music.h"

#include <array>
#include <cmath>
#include <cstring>

extern "C" void Audio_LoadSequence(uint8_t*, recomp_context*);
extern "C" void AudioSeq_SequencePlayerDisable(uint8_t*, recomp_context*);
extern "C" void Audio_SequencePlayerProcessSound(uint8_t*, recomp_context*);
extern "C" void AudioThread_CreateTask(uint8_t*, recomp_context*);

namespace wr64 {
namespace {
// USA Rev A sequence layout, verified against the recompiled instructions.
constexpr uint32_t kSequencePlayers = 0x8003FCC8;
constexpr uint32_t kSequencePlayerSize = 0x140;
constexpr unsigned kPlayerCount = 4;
constexpr uint8_t kEnabled = 0x80;
constexpr uint8_t kMuted = 0x20;
constexpr uint8_t kLoading = 0x18;
constexpr uint8_t kRecalculateVolume = 0x04;
constexpr uint8_t kMuteStopsScript = 0x80;
constexpr uint8_t kMuteStopsNotes = 0x40;
constexpr uint8_t kMuteSoftens = 0x20;

// These wrappers execute on the game's audio task thread. A sequence load
// disables/resets its own player internally; only the completed load is a
// restart, including when the requested sequence ID has not changed.
thread_local std::array<unsigned, kPlayerCount> loading{};
thread_local std::array<bool, kPlayerCount> nativeWasSuppressed{};
thread_local std::array<bool, kPlayerCount> active{};

int32_t address(unsigned player) {
    return static_cast<int32_t>(kSequencePlayers + player * kSequencePlayerSize);
}

unsigned player_index(uint32_t pointer) {
    const uint32_t offset = pointer - kSequencePlayers;
    return offset < kPlayerCount * kSequencePlayerSize && offset % kSequencePlayerSize == 0
        ? offset / kSequencePlayerSize : kPlayerCount;
}

float read_float(uint8_t* rdram, int32_t pointer, int offset) {
    const uint32_t bits = static_cast<uint32_t>(MEM_W(offset, pointer));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void stopped(unsigned player) {
    if (active[player]) music::sequence_stopped(player);
    active[player] = false;
    nativeWasSuppressed[player] = false;
}

void publish_states(uint8_t* rdram) {
    // Player zero owns engines, splashes, menu effects and announcer commands.
    for (unsigned player = 1; player < kPlayerCount; ++player) {
        const int32_t pointer = address(player);
        const uint8_t flags = MEM_BU(0, pointer);
        if (!(flags & kEnabled)) {
            stopped(player);
            continue;
        }
        const uint8_t behavior = MEM_BU(3, pointer);
        float gain = read_float(rdram, pointer, 0x18) * read_float(rdram, pointer, 0x28);
        if ((flags & kMuted) && (behavior & kMuteSoftens)) {
            gain *= read_float(rdram, pointer, 0x24);
        }
        if ((flags & kMuted) && (behavior & kMuteStopsNotes)) gain = 0.0f;
        if (!std::isfinite(gain) || gain < 0.0f) gain = 0.0f;
        // Muting only freezes playback when it freezes the native sequence
        // script. Async sequence/bank loading also precedes native playback.
        const bool paused = (flags & kLoading) || ((flags & kMuted) && (behavior & kMuteStopsScript));
        music::sequence_state(player, MEM_BU(4, pointer), gain, paused);
        active[player] = true;
    }
}
}

// 0x800B9F3C: a0 = player, a1 = sequence ID, a2 = asynchronous load flag.
void music_load_sequence_hook(uint8_t* rdram, recomp_context* ctx) {
    const unsigned player = static_cast<uint32_t>(ctx->r4);
    const uint32_t sequence = static_cast<uint32_t>(ctx->r5);
    if (player == 0 || player >= kPlayerCount) {
        Audio_LoadSequence(rdram, ctx);
        return;
    }
    const bool validSequence = sequence < MEM_HU(0, static_cast<int32_t>(0x80045514));
    {
        struct LoadingScope {
            unsigned& count;
            explicit LoadingScope(unsigned& value) : count(value) { ++count; }
            ~LoadingScope() { --count; }
        } scope(loading[player]);
        Audio_LoadSequence(rdram, ctx);
    }
    const int32_t pointer = address(player);
    const uint8_t flags = MEM_BU(0, pointer);
    if (validSequence && (flags & kEnabled) && MEM_BU(4, pointer) == sequence) {
        music::sequence_started(player, static_cast<uint8_t>(sequence));
        active[player] = true;
    } else if (!(flags & kEnabled)) {
        stopped(player);
    }
}

// 0x800BCEE0: a0 = SequencePlayer*. Includes natural endings and audio resets.
void music_disable_sequence_hook(uint8_t* rdram, recomp_context* ctx) {
    const unsigned player = player_index(static_cast<uint32_t>(ctx->r4));
    AudioSeq_SequencePlayerDisable(rdram, ctx);
    if (player > 0 && player < kPlayerCount && loading[player] == 0) stopped(player);
}

// 0x800BBFD4: a0 = SequencePlayer*. Keep its script and real fade state alive:
// the title sequence's channel 11 script IO drives original game callbacks.
void music_player_sound_hook(uint8_t* rdram, recomp_context* ctx) {
    const int32_t pointer = static_cast<int32_t>(ctx->r4);
    const unsigned player = player_index(static_cast<uint32_t>(pointer));
    if (player == 0 || player >= kPlayerCount) {
        Audio_SequencePlayerProcessSound(rdram, ctx);
        return;
    }
    const uint8_t flags = MEM_BU(0, pointer);
    const bool suppress = (flags & kEnabled) && music::replacement_available(MEM_BU(4, pointer));
    if (suppress || nativeWasSuppressed[player]) {
        MEM_B(0, pointer) = flags | kRecalculateVolume;
    }
    const int32_t originalScale = MEM_W(0x28, pointer);
    {
        struct RestoreScale {
            uint8_t* rdram;
            int32_t pointer;
            int32_t value;
            bool restore;
            ~RestoreScale() { if (restore) MEM_W(0x28, pointer) = value; }
        } restore{rdram, pointer, originalScale, suppress};
        if (suppress) MEM_W(0x28, pointer) = 0;
        Audio_SequencePlayerProcessSound(rdram, ctx);
    }
    nativeWasSuppressed[player] = suppress && (MEM_BU(0, pointer) & kEnabled);
}

// 0x800C4C40: no arguments. Snapshot even when no synthesis task was produced,
// so pauses, disabled players and reset periods cannot leave host music running.
void music_audio_task_hook(uint8_t* rdram, recomp_context* ctx) {
    AudioThread_CreateTask(rdram, ctx);
    publish_states(rdram);
}
}
