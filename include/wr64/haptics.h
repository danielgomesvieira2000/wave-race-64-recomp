#pragma once

#include <cstdint>
#include "wr64/haptics_mixer.h"

namespace wr64::haptics {
// Decoder and capture run on the guest game thread, after its native update.
Sample observe(const uint8_t* rdram);
void capture_frame(const uint8_t* rdram);
void reset_for_race();
void set_mode(Mode mode);
void set_strength(double percent);
void set_ambience(double percent);
void set_triggers(bool enabled);
// SDL handles are borrowed only during these main-thread calls.
void update_output(SDL_GameController* controller, bool allowed);
void shutdown(SDL_GameController* controller);
}
