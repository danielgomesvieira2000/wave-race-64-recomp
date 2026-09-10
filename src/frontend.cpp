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
#include "wr64/callbacks.h"
#include "wr64/music.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vector>

#include <recompui/recompui.h>
#include <recompui/config.h>
#include <recompui/program_config.h>
#include <recompui/renderer.h>
#include <recompinput/players.h>
#include <recompinput/input_mapping.h>

#include <librecomp/config.hpp>
#include <librecomp/game.hpp>
#include <ultramodern/config.hpp>

#include "wr64/display.h"
#include "wr64/dlrewrite.h"
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

// Where recompui keeps its settings: the directory main.cpp registered with
// librecomp, which is what recompui's own load and save use. This used to look
// beside the executable, while librecomp -- never told otherwise -- wrote the
// files into whatever directory the game was started from. The two agreed only
// when the game was launched from its own directory, and disagreed silently
// otherwise: settings saved in the menu came back as defaults on the next run.
std::filesystem::path config_directory() {
    return recomp::get_config_path();
}

ultramodern::renderer::PresentationMode presentation_mode();

// Adapts RecompFrontend's renderer to the callback ultramodern asks for.
//
// The two differ by one argument: ultramodern's callback does not carry a
// presentation mode, and RecompFrontend's context wants one. See
// presentation_mode() below for what it decides.
// Sits between ultramodern and RecompFrontend's renderer so that every
// display list can be rewritten before RT64 sees it (see wr64/dlrewrite.h).
// Everything else is forwarded untouched.
class RewritingContext final : public ultramodern::renderer::RendererContext {
public:
    RewritingContext(uint8_t* rdram,
                     std::unique_ptr<ultramodern::renderer::RendererContext> inner)
        : rdram_(rdram), inner_(std::move(inner)) {
        setup_result = inner_->get_setup_result();
        chosen_api = inner_->get_chosen_api();
    }

    bool valid() override { return inner_->valid(); }
    ultramodern::renderer::SetupResult get_setup_result() const override {
        return inner_->get_setup_result();
    }
    ultramodern::renderer::GraphicsApi get_chosen_api() const override {
        return inner_->get_chosen_api();
    }
    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        return inner_->update_config(old_config, new_config);
    }
    void enable_instant_present() override { inner_->enable_instant_present(); }
    void send_dummy_workload(uint32_t fb_address) override { inner_->send_dummy_workload(fb_address); }
    void update_screen() override { inner_->update_screen(); }
    void shutdown() override { inner_->shutdown(); }
    uint32_t get_display_framerate() const override { return inner_->get_display_framerate(); }
    float get_resolution_scale() const override { return inner_->get_resolution_scale(); }

    void send_dl(const OSTask* task) override {
        const uint32_t rewritten =
            wr64::dlrewrite::rewrite(rdram_, static_cast<uint32_t>(task->t.data_ptr));
        if (rewritten == 0) {
            inner_->send_dl(task);
            return;
        }
        OSTask copy = *task;
        copy.t.data_ptr = rewritten;
        inner_->send_dl(&copy);
    }

private:
    uint8_t* rdram_;
    std::unique_ptr<ultramodern::renderer::RendererContext> inner_;
};

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle,
        bool developer_mode) {
    return std::make_unique<RewritingContext>(
        rdram, recompui::renderer::create_render_context(
                   rdram, window_handle, presentation_mode(), developer_mode));
}

// Which frame RT64 puts on screen, and when.
//
// Console shows what the N64's video interface would have shown: the buffer
// the game finished two frames ago, because this game triple-buffers. That is
// the faithful choice, and it also switches the Framerate setting off. RT64
// only generates frames between two game frames when the buffer it just drew
// is the one being presented, which under Console never happens for a game
// that buffers at all -- so the menu's Display and Manual options changed
// nothing, and the game was shown at its own rate: 20 frames per second in the
// menus and in Time Trial, 30 in the championship.
//
// PresentEarly shows each frame as soon as it is drawn, which is what the other
// recompiled ports do. It takes two frames of latency off, and it is what lets
// RT64 interpolate. SkipBuffering is the middle ground -- it presents the
// buffer the game meant to show, but as soon as the game has finished it --
// and also allows interpolation. WR64_PRESENT_MODE picks one by name for
// comparing them; it is a testing knob, not a setting.
ultramodern::renderer::PresentationMode presentation_mode() {
    using Mode = ultramodern::renderer::PresentationMode;
    const char* env = std::getenv("WR64_PRESENT_MODE");
    if (env != nullptr) {
        const std::string_view value{ env };
        if (value == "console") return Mode::Console;
        if (value == "skip")    return Mode::SkipBuffering;
        if (value == "early")   return Mode::PresentEarly;
        std::fprintf(stderr, "[wr64] WR64_PRESENT_MODE=%s not recognised (console, skip, early); using early\n", env);
    }
    return Mode::PresentEarly;
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
        // No thumbnail: any artwork for the game itself would be taken from
        // the cartridge, and this project does not ship anything derived from
        // a dump. The launcher lays out fine without one.
        {});

    // The launcher's own background, distinct from the game thumbnail above:
    // this is original artwork supplied with the project (assets/icons/Logo.svg),
    // not derived from the cartridge, so it carries none of that restriction.
    // remove_default_title() takes down the plain-text program name the
    // library shows in its place -- the two would otherwise overlap.
    menu->set_launcher_background_svg("icons/Logo.svg");
    menu->remove_default_title();

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

    // The keyboard layout this port has always documented, declared as the
    // frontend's defaults so that the keys in docs/BUILDING.md are the keys a
    // fresh profile is bound to. RecompFrontend's own defaults are a different
    // scheme -- WASD and space -- and once input started going through its
    // profiles, that scheme silently replaced this one.
    //
    // Defaults apply to a profile the first time it is created; a keyboard
    // profile already saved keeps whatever it holds until it is reset in the
    // controls tab.
    {
        using recompinput::GameInput;
        using recompinput::InputField;
        const struct { GameInput input; SDL_Scancode key; } keys[] = {
            { GameInput::X_AXIS_NEG,  SDL_SCANCODE_LEFT },
            { GameInput::X_AXIS_POS,  SDL_SCANCODE_RIGHT },
            { GameInput::Y_AXIS_POS,  SDL_SCANCODE_UP },
            { GameInput::Y_AXIS_NEG,  SDL_SCANCODE_DOWN },
            { GameInput::A,           SDL_SCANCODE_X },
            { GameInput::B,           SDL_SCANCODE_C },
            { GameInput::Z,           SDL_SCANCODE_Z },
            { GameInput::START,       SDL_SCANCODE_RETURN },
            { GameInput::L,           SDL_SCANCODE_A },
            { GameInput::R,           SDL_SCANCODE_S },
            // The C buttons work the camera, which this game uses constantly,
            // so they stay under the right hand while the left drives.
            { GameInput::C_UP,        SDL_SCANCODE_I },
            { GameInput::C_DOWN,      SDL_SCANCODE_K },
            { GameInput::C_LEFT,      SDL_SCANCODE_J },
            { GameInput::C_RIGHT,     SDL_SCANCODE_L },
            { GameInput::DPAD_UP,     SDL_SCANCODE_T },
            { GameInput::DPAD_DOWN,   SDL_SCANCODE_G },
            { GameInput::DPAD_LEFT,   SDL_SCANCODE_F },
            { GameInput::DPAD_RIGHT,  SDL_SCANCODE_H },
        };
        for (const auto& binding : keys) {
            recompinput::set_default_mapping_for_keyboard(
                binding.input, { InputField::keyboard(binding.key) });
        }
    }

    recompui::register_launcher_init_callback(build_launcher);

    // Wave Race 64 is a two-player game, so the controls tab offers two player
    // slots rather than the frontend's default four. Which pad is which is not
    // a choice anyone should have to make here: the port assigns them in the
    // order they are connected (see refresh_players in src/callbacks.cpp), and
    // the modal in the controls tab is left for anyone who wants to override it.
    recompinput::players::set_player_count_range(1, 2);

    // The prefab tabs. Wave Race has no gyro or mouse control, so the general
    // tab keeps only what applies.
    //
    // Rumble strength is on, and it is the only control the feedback has: the
    // slider is 0-100, recompinput scales the motor by it, and zero is off.
    // The game itself never asks for rumble -- it predates the Rumble Pak, and
    // `Motor` appears nowhere in its code -- so what the slider governs is the
    // feedback this port works out for itself from the race (src/haptics.cpp).
    // Without the option the whole rumble path in recompinput is skipped, so
    // this line is also what turns the feature on at all.
    recompui::config::GeneralTabOptions general{};
    general.has_rumble_strength = true;
    general.has_gyro_sensitivity = false;
    general.has_mouse_sensitivity = false;

    recompui::config::create_general_tab(general);
    recompui::config::create_graphics_tab();
    // Main Volume did nothing until this. recompui defines the slider and reads
    // it back, and nothing upstream ever applies it -- the port is expected to,
    // and this one was not. The callback covers all three ways it changes:
    // Load, when the saved setting is read at startup; Temporary, while the
    // slider is being dragged, which is what makes it audible as you move it;
    // and Permanent, on Apply.
    auto& sound = recompui::config::create_sound_tab();
    sound.add_option_change_callback(
        recompui::config::sound::options::main_volume,
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant,
           recomp::config::OptionChangeContext) {
            if (const double* percent = std::get_if<double>(&value)) {
                wr64::set_audio_volume(*percent);
            }
        });

    // Music separately from everything else. Main Volume is applied to the
    // finished buffer; this one cannot be, because music and effects are already
    // mixed together by then, so it is applied inside the game's own sequence
    // players instead (src/music.cpp).
    sound.add_percent_number_option(
        "music_volume", "Music Volume",
        "Controls the volume of the game's music, without changing the effects.",
        100.0);
    sound.add_option_change_callback(
        "music_volume",
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant,
           recomp::config::OptionChangeContext) {
            if (const double* percent = std::get_if<double>(&value)) {
                wr64::music::set_volume(*percent);
            }
        });

    sound.add_percent_number_option(
        "announcer_volume", "Announcer Volume",
        "Controls the volume of the announcer's voice, without changing the "
        "other effects.",
        100.0);
    sound.add_option_change_callback(
        "announcer_volume",
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant,
           recomp::config::OptionChangeContext) {
            if (const double* percent = std::get_if<double>(&value)) {
                wr64::music::set_announcer_volume(*percent);
            }
        });

    sound.add_bool_option(
        "mute_unfocused", "Mute When Not In Focus",
        "Silences the game while another window has focus. Feedback stops with it.",
        true);
    sound.add_option_change_callback(
        "mute_unfocused",
        [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant,
           recomp::config::OptionChangeContext) {
            if (const bool* mute = std::get_if<bool>(&value)) {
                wr64::set_mute_when_unfocused(*mute);
            }
        });
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
    //
    // HUD Placement is left at the library's default. It only moves 2D content
    // that names an edge through RT64's extended GBI, and this game predates
    // that; its HUD is kept at 4:3 in the middle of the frame regardless.
    if (!std::filesystem::exists(config_directory() / "graphics.json")) {
        recompui::config::get_graphics_config().set_option_value(
            recompui::config::graphics::options::wm_option,
            static_cast<uint32_t>(ultramodern::renderer::WindowMode::Fullscreen));

        ultramodern::renderer::GraphicsConfig gfx = ultramodern::renderer::get_graphics_config();
        gfx.wm_option = ultramodern::renderer::WindowMode::Fullscreen;
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
    wr64::display::set_window(sdl_window);
}

ultramodern::renderer::callbacks_t renderer_callbacks() {
    ultramodern::renderer::callbacks_t callbacks{};
    callbacks.create_render_context = create_render_context;
    return callbacks;
}

bool capturing_input() {
    return recompui::is_context_capturing_input();
}

}  // namespace wr64::frontend
