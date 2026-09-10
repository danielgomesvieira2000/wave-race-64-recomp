#pragma once

#include <cstddef>

#include <ultramodern/ultramodern.hpp>
#include <ultramodern/error_handling.hpp>
#include <ultramodern/events.hpp>
#include <ultramodern/input.hpp>
#include <ultramodern/renderer_context.hpp>
#include <ultramodern/threads.hpp>
#include <librecomp/rsp.hpp>

namespace wr64 {

// The platform I/O ultramodern deliberately does not own. Implemented on SDL2
// in src/callbacks.cpp; audio and RSP are placeholders, documented there.
ultramodern::input::callbacks_t          input_callbacks();
ultramodern::audio_callbacks_t           audio_callbacks();
recomp::rsp::callbacks_t                 rsp_callbacks();
ultramodern::gfx_callbacks_t             gfx_callbacks();
ultramodern::events::callbacks_t         events_callbacks();
ultramodern::error_handling::callbacks_t error_handling_callbacks();
ultramodern::threads::callbacks_t        threads_callbacks();
ultramodern::renderer::callbacks_t       renderer_callbacks();

// The Sound tab's Main Volume, 0-100, applied to every buffer on its way to the
// sound card. Nothing upstream consumes the setting: recompui defines the
// slider and leaves applying it to the port.
void set_audio_volume(double percent);

// The Sound tab's "Mute when the window is not in focus". On by default.
void set_mute_when_unfocused(bool mute);

void shutdown_platform();

// Defined in src/overlays.cpp, which owns the generated section tables.
void register_overlays();
// Must run from the game's on_init hook: init_overlays() clears the map.
void register_runtime_functions();
size_t overlay_section_count();
size_t code_section_count();

}  // namespace wr64
