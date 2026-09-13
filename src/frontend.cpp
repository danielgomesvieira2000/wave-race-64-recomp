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
#include "wr64/water.h"
#include "wr64/inspector.h"
#include "wr64/drawdistance.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
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
//
// It also sits between ultramodern and RecompFrontend's renderer so that every
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
    // RT64 already binds F1 to its developer UI, and the port's HUD inspector
    // draws inside it. Every path to that UI is gated on RT64's developer mode,
    // though: the key handler, the event filter RT64 installs for itself, and
    // State::inspect() at the other end. So it is on unconditionally here.
    //
    // A debug menu that only exists in a build made for it is a debug menu
    // nobody has when they need it -- the person looking at a misplaced menu
    // element is running the game they downloaded. Nothing is drawn until F1 is
    // pressed: RT64 creates its inspector on the keystroke and State::inspect()
    // returns immediately while there is none, so the cost of leaving this on
    // is a null check per frame.
    //
    // The frontend's own developer-mode setting is left to mean whatever else
    // it means; it no longer decides whether the debug menu can be opened.
    (void)developer_mode;
    wr64::inspector::install();
    return std::make_unique<RewritingContext>(
        rdram, recompui::renderer::create_render_context(
                   rdram, window_handle, presentation_mode(), true));
}

// Which frame RT64 puts on screen, and when.
//
// Console shows what the N64's video interface would have shown: the buffer
// the game finished two frames ago, because this game triple-buffers. That is
// the faithful choice, and it also switches the Framerate setting off. RT64
// only generates frames between two game frames when the buffer it just drew
// is the one being presented, which under Console never happens for a game
// that buffers at all -- so the menu's Display and Manual options changed
// nothing, and the game was shown at its own rate: 20 frames per second in races
// and menus.
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
    options->add_mods_option("Mods");
    // Closing the window works, but a menu the pad can reach should not need a
    // mouse to leave. add_exit_option calls ultramodern::quit(), which unwinds
    // the game thread and the renderer in order rather than tearing the process
    // down, so a race in progress saves its records on the way out.
    options->add_exit_option("Quit");
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

    // The port's own options in tabs with an Apply button take effect on Apply,
    // and on Load at startup -- never on Temporary, which librecomp reports on
    // every click and every step of a slider. Graphics and Water both confirm.
    // (General and Sound are created by recompui without confirmation: they have
    // no Apply button, and a change there applies as it is made.)
    auto on_choice = [](std::function<void(uint32_t)> apply) {
        return [apply](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant,
                       recomp::config::OptionChangeContext context) {
            if (context == recomp::config::OptionChangeContext::Temporary) {
                return;
            }
            if (const uint32_t* choice = std::get_if<uint32_t>(&value)) {
                apply(*choice);
            }
        };
    };
    auto on_number = [](std::function<void(double)> apply) {
        return [apply](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant,
                       recomp::config::OptionChangeContext context) {
            if (context == recomp::config::OptionChangeContext::Temporary) {
                return;
            }
            if (const double* number = std::get_if<double>(&value)) {
                apply(*number);
            }
        };
    };
    auto on_percent = [on_number](std::function<void(float)> apply) {
        return on_number([apply](double percent) { apply(static_cast<float>(percent)); });
    };

    // Field of view, on the world's own frustum. The game draws at 45 degrees
    // vertically; this widens it without changing the shape of anything, since
    // the horizontal half is derived from the vertical one and the aspect ratio.
    // The sky has a frustum of its own and is widened by the same ratio, so it
    // keeps step.
    //
    // The draw distance sits beside it and is a different thing entirely: the
    // game's far plane is already at 16,192 against a course that needs a few
    // thousand, so pushing *that* out reveals nothing. What limits the view is
    // the game's own culling, one integer per course and view, which the setting
    // below raises. See include/wr64/drawdistance.h.
    auto& graphics = recompui::config::create_graphics_tab();
    graphics.add_number_option(
        "fov", "Field of View",
        "How much of the world is in view, vertically, in degrees. "
        "<recomp-color primary>45</recomp-color> is what the game draws at; higher shows more "
        "without stretching anything. The HUD and the menus are unaffected.",
        45.0, 110.0, 5.0, 0, false, 45.0);
    // Draw distance. Not the far plane -- that is already twenty times further
    // out than anything the game draws -- but the game's own culling. It was
    // built as a list of per-kind limits that would grow as each was found; a
    // census of every display-list call, run again with one field doubled,
    // showed there is only the one, and that it governs nearly all of a course's
    // static geometry. See include/wr64/drawdistance.h.
    graphics.add_enum_option(
        "object_draw_distance", "Draw Distance",
        "How far away the course is drawn.<br /><br />"
        "<recomp-color primary>Original</recomp-color>: the game's own, where distant scenery "
        "and buoys pop in.<br /><br />"
        "<recomp-color primary>Extended</recomp-color>: the course out to the horizon, every "
        "buoy in view, and the sea beyond the game's own patch of water. Costs some frame rate.",
        std::vector<recomp::config::ConfigOptionEnumOption>{
            { 0u, "Original", "Original" },
            { 1u, "Extended", "Extended" },
        },
        1u);
    // Settings saved before there were two steps. Far, Very far and Maximum all
    // come back as Extended, so a player who raised it keeps it raised; anything
    // else, the multiplier names this setting carried before those, comes back
    // as Original rather than misread. Without this, librecomp resolves an id it
    // does not know to the default silently.
    graphics.on_json_parse_option(
        "object_draw_distance",
        [](const nlohmann::json& saved) -> recomp::config::ConfigValueVariant {
            if (saved.is_string()) {
                const std::string id = saved.get<std::string>();
                if (id == "Extended" || id == "Maximum" || id == "VeryFar" || id == "Far") {
                    return 1u;
                }
            }
            return 0u;
        });
    // A distance, and there is only one worth having above the game's: the far
    // plane, past which nothing can be drawn. The furthest object measured on any
    // course sat at 13,175. See include/wr64/drawdistance.h.
    graphics.add_option_change_callback("object_draw_distance", on_choice([](uint32_t choice) {
        wr64::drawdistance::set_reach(choice == 1u ? wr64::drawdistance::kFarPlane : 0);
    }));

    graphics.add_option_change_callback("fov", on_number([](double degrees) {
        wr64::dlrewrite::set_field_of_view(degrees);
    }));

    // Every water setting, in one tab, immediately after Graphics. The master
    // switch used to sit in the Graphics tab with the rest here, which meant
    // the Water tab did nothing until you found an option in another tab.
    //
    // The ordering rule this has to respect: create_config_tab appends to a
    // vector of tabs and returns a reference into it, so creating a tab can
    // reallocate that vector and leave every reference an earlier
    // create_*_tab returned dangling. Configure each tab *completely* before
    // creating the next one, and never touch an earlier reference again.
    // Getting that wrong crashed the launcher inside the graphics tab's own
    // fov callback -- an option this change did not touch -- with a stack
    // that named the callback rather than the tab that had moved.
    //
    // Everything below the first option does nothing while it is Original,
    // and is greyed out then.
    auto& water = recompui::config::create_config_tab("Water", "water", true);

    // Laid out as one decision and then refinements of it. Water quality is a
    // three-step ladder -- Original, Enhanced, Best -- and each option below it
    // says what it shapes. An option that does nothing under the current
    // choices is greyed out when the quality is the reason (it comes back with
    // a higher step, so it is worth seeing) and hidden when the style is (it
    // belongs to another look). librecomp keys each kind of dependency by the
    // dependent option alone, so an option can have one of each and no more.
    //
    // The enum ids are the lower-case ones the fork this renderer came from
    // used, and they are deliberately not the display names -- which is what
    // let the display names change here without touching anyone's settings.
    // They are what gets written into water.json, and an id that matches
    // nothing does not fail loudly, it quietly resolves to some other entry.
    //

    // Original by default: the modern renderer costs frame time, and a machine
    // without the headroom -- or without a GPU, where it is ten times slower --
    // should not have to find the setting first. Deep is the style it opens with
    // when raised. With a saved
    // water.json its Load callback sets the renderer before the first frame; on a
    // first run there is no file and no callback, so the defaults in
    // src/water.cpp apply -- keep the two the same.
    water.add_enum_option(
        "water_quality", "Water Quality",
        "<recomp-color primary>Original</recomp-color>: the game's own water.<br /><br />"
        "<recomp-color primary>Enhanced</recomp-color>: sun and sky lighting, colour that "
        "deepens with the water, refraction, and wakes that linger.<br /><br />"
        "<recomp-color primary>Best</recomp-color>: Enhanced plus reflections and spray. The "
        "most demanding.<br /><br />"
        "The waves, and how the craft handles on them, are the same at every setting.",
        std::vector<recomp::config::ConfigOptionEnumOption>{
            { 0u, "original", "Original" },
            { 1u, "modern", "Enhanced" },
            { 2u, "high", "Best" },
        },
        0u);
    water.add_option_change_callback("water_quality", on_choice([](uint32_t choice) {
        wr64::water::set_quality(static_cast<wr64::water::Quality>(choice));
    }));

    water.add_enum_option(
        "water_style", "Water Style",
        "The colours of Enhanced and Best water.<br /><br />"
        "<recomp-color primary>Classic</recomp-color>: the game's own colours and "
        "transparency, with the new effects on top.<br /><br />"
        "<recomp-color primary>Deep</recomp-color>: a richer, darker sea.<br /><br />"
        "<recomp-color primary>Aqua</recomp-color>: a lighter teal with clearer shallows.",
        // Listed least to most departure from the cartridge. recompui builds the
        // control by walking this vector and maps the chosen position back
        // through options[i].value, so the values stay bound to their meanings
        // however the list is arranged. They must keep matching
        // wr64::water::Style, which is why they are not renumbered, and the
        // dependencies below name values rather than positions. "Deep" is the
        // style the code and the docs call Modern; its id stays "modern".
        std::vector<recomp::config::ConfigOptionEnumOption>{
            { 1u, "classic", "Classic" },
            { 0u, "modern", "Deep" },
            { 2u, "aqua", "Aqua" },
        },
        0u);
    water.add_option_change_callback("water_style", on_choice([](uint32_t choice) {
        wr64::water::set_style(static_cast<wr64::water::Style>(choice));
    }));

    // Clarity keeps the id aqua_clarity from when it was Aqua-only, so a value
    // saved then is still read. 50% is each style's own look; the default is
    // clearer than that.
    water.add_percent_number_option(
        "aqua_clarity", "Clarity",
        "How clear the water is. 50% is the style's own look. Lower is murkier; higher shows "
        "the sea floor and fish below, fully see-through at 100%. In two-player races only "
        "lower values have an effect.",
        85.0);
    water.add_option_change_callback("aqua_clarity", on_percent([](float percent) {
        wr64::water::set_clarity(percent);
    }));

    water.add_percent_number_option(
        "aqua_brightness", "Aqua Brightness",
        "Darkens or lightens the Aqua water. 50% is the style's own look. Reflections and "
        "foam keep their own brightness.",
        80.0);
    water.add_option_change_callback("aqua_brightness", on_percent([](float percent) {
        wr64::water::set_aqua_brightness(percent);
    }));

    water.add_percent_number_option(
        "aqua_tint", "Aqua Tint",
        "Shifts the Aqua water from deep blue at 0% to green turquoise at 100%. 50% is the "
        "usual look.",
        50.0);
    water.add_option_change_callback("aqua_tint", on_percent([](float percent) {
        wr64::water::set_aqua_tint(percent);
    }));

    // Fine surface detail, separate from the cartridge's waves. The game's own
    // wave simulation is untouched by any of this: handling does not change.
    water.add_enum_option(
        "water_ripples", "Surface Ripples",
        "Fine detail on the surface of Enhanced and Best water, added on top of the game's "
        "waves rather than replacing them. <recomp-color primary>Normal</recomp-color> is the "
        "usual look.",
        std::vector<recomp::config::ConfigOptionEnumOption>{
            { 0u, "soft", "Soft" },
            { 1u, "normal", "Normal" },
            { 2u, "strong", "Strong" },
        },
        1u);
    water.add_option_change_callback("water_ripples", on_choice([](uint32_t choice) {
        wr64::water::set_ripple_detail(static_cast<wr64::water::RippleDetail>(choice));
    }));

    water.add_enum_option(
        "water_spray", "Spray",
        "Fine airborne spray around each craft, part of Best. It is added on top of the "
        "game's own splashes; turning it off keeps the splashes, the foam and the wakes.",
        std::vector<recomp::config::ConfigOptionEnumOption>{
            { 0u, "off", "Off" },
            { 1u, "on", "On" },
        },
        1u);
    water.add_option_change_callback("water_spray", on_choice([](uint32_t choice) {
        wr64::water::set_spray_enabled(choice != 0u);
    }));

    // Greyed out by quality: everything is Enhanced's or Best's.
    for (const char* id : { "water_style", "aqua_clarity", "aqua_brightness", "aqua_tint", "water_ripples" }) {
        water.add_option_disable_dependency(id, "water_quality", 0u);
    }
    water.add_option_disable_dependency("water_spray", "water_quality", 0u, 1u);
    // Hidden by style: values, not positions -- Deep is 0, Classic 1, Aqua 2.
    water.add_option_hidden_dependency("aqua_clarity", "water_style", 1u);
    water.add_option_hidden_dependency("aqua_brightness", "water_style", 0u, 1u);
    water.add_option_hidden_dependency("aqua_tint", "water_style", 0u, 1u);

    // Main Volume did nothing until this. recompui defines the slider and reads
    // it back, and nothing upstream ever applies it -- the port is expected to,
    // and this one was not. The Sound tab has no Apply button (recompui creates
    // it without confirmation), so the callback hears Load when the saved setting
    // is read at startup and Permanent on every step of the slider, which is what
    // makes the volume follow the handle as it moves.
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

    sound.add_bool_option(
        "mute_unfocused", "Mute When Not In Focus",
        "Silences the game, and stops controller rumble, while another window has focus.",
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

    // Mods. The runtime half of this has been running since the port first
    // started: recomp::start calls initialize_mods() and scan_mods() on its own,
    // main.cpp gives librecomp this game's mod id, and the mods and mod_config
    // folders have existed in the settings directory all along. What was missing
    // was any way to see what is in them -- so a mod could be installed and
    // never appear, never be enabled and never be reported broken.
    //
    // The tab lists what was found, with each mod's description, author, version
    // and its own options; the launcher entry below opens it without starting the
    // game first.
    recompui::config::create_mods_tab();

    // No add_game_input calls: recompinput already knows the N64 controller,
    // and this game has no inputs beyond it. Ports with extra actions -- an
    // ocarina, a quick-save -- declare them here so they appear in the
    // remapping list.

    // Whether this is a first run, asked before finalize(): loading a missing
    // settings file writes one with the defaults, so afterwards it always exists.
    const bool first_run = !std::filesystem::exists(config_directory() / "graphics.json");

    // Loads the player's saved settings from disk. Must come after every tab.
    recompui::config::finalize();

    // Play fullscreen at the display's own resolution and aspect ratio.
    //
    // Two thirds of that are already the library's defaults: resolution is Auto,
    // which renders at the window's true pixel size rather than upscaling
    // 320x240, and aspect ratio is Expand, which widens the frustum to whatever
    // shape the window is. Only the window mode defaults to Windowed.
    //
    // Only when the player had no saved graphics settings, which is what makes it
    // a first-run default rather than an override: choose Windowed in the menu and
    // that choice is saved and respected from then on. It is written after
    // finalize() -- the option map does not exist until the file has loaded, and
    // writing into it faults.
    //
    // The Graphics tab confirms its changes, so set_option_value only stages the
    // value; save_config() applies it, which is what the Apply button does, and
    // writes it. Without that the menu showed Fullscreen as an unapplied change
    // and nothing was saved. ultramodern's copy, which the renderer reads, is set
    // as well, so the first window opens fullscreen either way.
    //
    // HUD Placement is left at the library's default; the display-list rewriter
    // anchors the race HUD to the frame's edges whenever it is not Original.
    if (first_run) {
        auto& graphics_config = recompui::config::get_graphics_config();
        graphics_config.set_option_value(
            recompui::config::graphics::options::wm_option,
            static_cast<uint32_t>(ultramodern::renderer::WindowMode::Fullscreen));
        graphics_config.save_config();

        ultramodern::renderer::GraphicsConfig gfx = ultramodern::renderer::get_graphics_config();
        gfx.wm_option = ultramodern::renderer::WindowMode::Fullscreen;
        ultramodern::renderer::set_graphics_config(gfx);

        std::fprintf(stderr, "[wr64] no saved graphics settings; defaulting to fullscreen at the display\'s size\n");
    }

    // Test hook: WR64_TEST_INSPECTOR=25 opens RT64's F1 menu -- the HUD inspector
    // and the water sun editor -- 25 seconds after startup, by posting the F1 key
    // RT64's event filter listens for, so a capture can show those windows.
    if (const char* spec = std::getenv("WR64_TEST_INSPECTOR")) {
        const double delay = std::atof(spec) > 0.0 ? std::atof(spec) : 20.0;
        std::thread([delay]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(delay));
            SDL_Event event{};
            event.type = SDL_KEYDOWN;
            event.key.state = SDL_PRESSED;
            event.key.keysym.scancode = SDL_SCANCODE_F1;
            event.key.keysym.sym = SDLK_F1;
            SDL_PushEvent(&event);
            std::fprintf(stderr, "[wr64] test: pressed F1\n");
            std::fflush(stderr);
        }).detach();
    }

    // Test hook: WR64_TEST_OPEN_SETTINGS=water@25 opens the settings menu on the
    // Water tab 25 seconds after startup, so a capture can show a tab as a
    // player sees it without anyone pressing Escape. Opening it changes nothing
    // that is saved.
    if (const char* spec = std::getenv("WR64_TEST_OPEN_SETTINGS")) {
        const std::string text = spec;
        const size_t at = text.find('@');
        const std::string tab = text.substr(0, at);
        const double delay = at == std::string::npos ? 20.0 : std::atof(text.c_str() + at + 1);
        std::thread([tab, delay]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(delay));
            recompui::ContextId context = recompui::config::get_config_context_id();
            context.open();
            recompui::config::set_tab(tab);
            context.close();
            recompui::config::open();
            std::fprintf(stderr, "[wr64] test: opened the settings menu on the %s tab\n", tab.c_str());
            std::fflush(stderr);
        }).detach();
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
