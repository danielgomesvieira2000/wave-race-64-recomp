#pragma once

// The Sound tab's Music Volume, applied inside the game's audio engine.
//
// See src/music.cpp for why it cannot be applied to the finished buffer the way
// Main Volume is: the microcode has already mixed music and effects together by
// then, so the music is scaled where the game still knows which is which.

#include <cstdint>

namespace wr64::music {

// UI thread, when the slider moves. 0-100.
void set_volume(double percent);

// The Sound tab's Announcer Volume, applied to the voice channels of the
// effects player. 0-100.
void set_announcer_volume(double percent);

// Game thread, once per frame, from the frame's osViSwapBuffer.
void apply(uint8_t* rdram);

}  // namespace wr64::music
