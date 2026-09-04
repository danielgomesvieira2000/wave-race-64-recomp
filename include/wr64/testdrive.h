#pragma once

#include <cstdint>

// Phase 05 verification: drive the game without a person holding the pad, and
// report where it got to.
//
// The phase gate is "menus work and a race runs", and neither is observable
// from outside the process: a port that hangs on the title screen and one that
// is quietly racing look identical from a terminal, and both look like a window
// that is up. Two things fix that.
//
// A script of timed inputs replaces the pad, so a run is repeatable and can be
// checked into the repository next to the thing it tests. Pressing keys at a
// window by hand is neither.
//
// A watcher on gGameState turns the run into a transcript. The game keeps its
// current screen in one byte at a known address and the decomp names the
// values, so "title screen -> main menu -> rider select -> course select ->
// racing" is a fact the log can state rather than something to infer from
// pixels.

namespace wr64 {

// Loads a script of timed inputs from the path in WR64_INPUT_SCRIPT, if set.
// Returns false and explains itself on stderr if the file cannot be used, in
// which case the pad and keyboard still work as usual.
bool load_input_script();

// True once a script is loaded and still has entries left to apply.
bool input_script_active();

// The buttons and stick the script asks for at the current moment. Merged with
// the pad and keyboard rather than replacing them, so a run can be nudged by
// hand while it plays.
void input_script_state(uint16_t* buttons, float* stick_x, float* stick_y);

// Reports every change of gGameState, by name. Called once per frame from the
// main loop; needs the RDRAM base, which only the runtime hooks have.
void set_rdram_base(uint8_t* rdram);
void poll_game_state();

// The game's state variable as it is right now, or 0 before RDRAM is known.
// The display-list rewriter reads it to tell a race from a menu.
uint32_t current_game_state();

}  // namespace wr64
