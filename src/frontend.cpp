// Phase 06: wire RecompFrontend's launcher, ROM picker and config menu in.
//
// See include/wr64/frontend.h for what this is and why it is optional.
//
// The division of labour is worth stating, because almost none of it is ours.
// RecompFrontend owns the launcher, the native file dialog, the ROM validation
// error messages, the settings tabs, the controller remapping and the profiles
// that persist between sessions. What a port supplies is three things: which
// game this is, what the menu entries should say, and a stylesheet.

#include "wr64/frontend.h"

#include <cstdio>
#include <filesystem>
#include <vector>

#include <recompui/recompui.h>
#include <recompui/config.h>
#include <recompui/program_config.h>
#include <recompui/renderer.h>

#include <librecomp/config.hpp>
#include <ultramodern/config.hpp>

#include "wr64/rom.h"

// The two globals recompui expects the port to define. It declares them extern
// in its own translation units and links against whatever the port provides:
//
//     extern std::vector<recomp::GameEntry> supported_games;   // ui_launcher.cpp
//     extern SDL_Window* window;                               // ui_state.cpp
//
// They are in the global namespace because that is where the library looks for
// them. `supported_games` is what the launcher falls back to when a port does
// not register its own init callback; it is populated anyway so the two
// descriptions of this game cannot drift apart.
std::vector<recomp::GameEntry> supported_games;
SDL_Window* window = nullptr;

namespace {

// Where recompui keeps its settings: beside the executable, which is where
// general.json and the rest appear.
std::filesystem::path config_directory() {
    char* base = SDL_GetBasePath();
    std::filesystem::path path = base != nullptr ? std::filesystem::path{ base }
                                                 : std::filesystem::current_path();
    if (base != nullptr) {
        SDL_free(base);
    }
    return path;
}

// Adapts RecompFrontend's renderer to the callback ultramodern asks for.
//
// The two differ by one argument: ultramodern's callback does not carry a
// presentation mode, and RecompFrontend's context wants one. It comes from the
// graphics config, which is where the setting the player chose in the menu
// lives, so reading it here is what makes that setting take effect.
std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle,
        bool developer_mode) {
    return recompui::renderer::create_render_context(
        rdram, window_handle,
        // Console: present exactly as the hardware did. The other modes trade
        // faithfulness for latency, which is a choice worth exposing later but
        // not one to make silently on the player's behalf.
        ultramodern::renderer::PresentationMode::Console,
        developer_mode);
}

// Builds the launcher's menu. Called once, when the launcher is created.
//
// add_start_game_or_load_rom_option is the whole first-run flow: with no valid
// dump it reads "Load ROM" and opens a file dialog, validates what comes back
// against the hash this project is pinned to, and reports which way it failed;
// once a dump is accepted it becomes "Start Game" and calls into librecomp.
// Nothing about that is written here, which is the point of the library.
void build_launcher(recompui::LauncherMenu* menu) {
    recompui::GameOptionsMenu* options = menu->init_game_options_menu(
        std::u8string{ wr64::kGameId },
        wr64::kModGameId,
        wr64::kDisplayName,
        // No thumbnail: any artwork for this game would be taken from the
        // cartridge, and this project does not ship anything derived from a
        // dump. The launcher lays out fine without one.
        {});

    options->add_start_game_or_load_rom_option("Load ROM", "Start Game");
    options->add_setup_controls_option("Controls");
    options->add_settings_option("Settings");
}

}  // namespace

namespace wr64::frontend {

void init() {
    // The program's own identity, as distinct from the game's. recompui shows
    // the name in the launcher and uses the id to decide where settings and
    // controller profiles are stored, so both must be set before anything is
    // built -- the launcher throws from its constructor otherwise.
    recompui::programconfig::set_program_name("Wave Race 64: Recompiled");
    recompui::programconfig::set_program_id(u8"WaveRace64Recomp");

    // The family name is the one inside the font, not the filename: this file
    // is LatoLatin-Regular.ttf and declares itself "LatoLatin". Getting it wrong
    // is silent -- the UI lays out and draws with every element in place and no
    // text in any of them. The stylesheet has to name the same family.
    //
    // Lato is the face RmlUi vendors for its own samples, under the SIL Open
    // Font License; the build copies it next to the executable rather than
    // committing a second copy of a binary this repository already has.
    recompui::register_primary_font("LatoLatin-Regular.ttf", "LatoLatin");

    recompui::register_launcher_init_callback(build_launcher);

    // The prefab tabs. Wave Race predates the Rumble Pak and has no gyro or
    // mouse control, so the general tab keeps only what applies.
    recompui::config::GeneralTabOptions general{};
    general.has_rumble_strength = false;
    general.has_gyro_sensitivity = false;
    general.has_mouse_sensitivity = false;

    recompui::config::create_general_tab(general);
    recompui::config::create_graphics_tab();
    recompui::config::create_sound_tab();
    recompui::config::create_controls_tab();

    // No add_game_input calls: recompinput already knows the N64 controller,
    // and this game has no inputs beyond it. Ports with extra actions -- an
    // ocarina, a quick-save -- declare them here so they appear in the
    // remapping list.

    // Loads the player's saved settings from disk. Must come after every tab.
    recompui::config::finalize();

    // Play fullscreen at the display's own resolution and aspect ratio.
    //
    // Two thirds of that are already the library's defaults: resolution is Auto,
    // which renders at the window's true pixel size rather than upscaling
    // 320x240, and aspect ratio is Expand, which widens the frustum to whatever
    // shape the window is. Only the window mode defaults to Windowed.
    //
    // This runs after finalize() and only when the player has no saved graphics
    // settings, which is what makes it a first-run default rather than an
    // override: choose Windowed in the menu and that choice is written to
    // graphics.json and respected from then on. Setting it before finalize()
    // does not work -- the option map does not exist until it has loaded, and
    // writing into it faults.
    //
    // Both copies are set. recompui owns the value the menu shows, ultramodern
    // owns the one the renderer reads, and the graphics tab syncs the two on
    // change; setting only one leaves the menu and the window disagreeing.
    if (!std::filesystem::exists(config_directory() / "graphics.json")) {
        recompui::config::get_graphics_config().set_option_value(
            recompui::config::graphics::options::wm_option,
            static_cast<uint32_t>(ultramodern::renderer::WindowMode::Fullscreen));

        // The HUD stays where the cartridge put it, in the middle 4:3 of the
        // screen. The library defaults it to Expand, which spreads the readouts
        // to the edges of a widened frame -- and in this game that pushes the
        // speed readout off the right-hand side entirely. Keeping it Original is
        // both correct-looking and the setting that does not lose information.
        // The frustum is still widened; this is only about where the 2D overlay
        // sits inside it.
        recompui::config::get_graphics_config().set_option_value(
            recompui::config::graphics::options::hr_option,
            static_cast<uint32_t>(ultramodern::renderer::HUDRatioMode::Original));

        ultramodern::renderer::GraphicsConfig gfx = ultramodern::renderer::get_graphics_config();
        gfx.wm_option = ultramodern::renderer::WindowMode::Fullscreen;
        gfx.hr_option = ultramodern::renderer::HUDRatioMode::Original;
        ultramodern::renderer::set_graphics_config(gfx);

        std::fprintf(stderr, "[wr64] no saved graphics settings; defaulting to fullscreen at the display\'s size\n");
    }

    std::fprintf(stderr, "[wr64] frontend ready: launcher, ROM picker and config menu\n");
    std::fflush(stderr);
}

void publish_game(const recomp::GameEntry& game) {
    supported_games.clear();
    supported_games.push_back(game);
}

void publish_window(SDL_Window* sdl_window) {
    ::window = sdl_window;
}

ultramodern::renderer::callbacks_t renderer_callbacks() {
    ultramodern::renderer::callbacks_t callbacks{};
    callbacks.create_render_context = create_render_context;
    return callbacks;
}

bool handle_event(const SDL_Event& event) {
    recompui::queue_event(event);
    return recompui::is_context_capturing_input();
}

bool capturing_input() {
    return recompui::is_context_capturing_input();
}

}  // namespace wr64::frontend
