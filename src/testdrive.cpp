// Phase 05 verification: scripted input and a game-state transcript.
//
// See include/wr64/testdrive.h for why this exists. In short: the phase gate is
// that menus work and a race runs, and from outside the process a port stuck on
// the title screen is indistinguishable from one that is racing.

#include "wr64/testdrive.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <SDL.h>
#include <ultramodern/ultramodern.hpp>

namespace {

// ------------------------------------------------------------ input script ---

// N64 controller button bits, as libultra defines them. Duplicated from
// callbacks.cpp deliberately: this file is a test harness and should not make
// the input path depend on it.
struct NamedButton { const char* name; uint16_t bit; };
constexpr NamedButton kButtons[] = {
    { "A",      0x8000 }, { "B",      0x4000 }, { "Z",      0x2000 },
    { "START",  0x1000 }, { "DUP",    0x0800 }, { "DDOWN",  0x0400 },
    { "DLEFT",  0x0200 }, { "DRIGHT", 0x0100 }, { "L",      0x0020 },
    { "R",      0x0010 }, { "CUP",    0x0008 }, { "CDOWN",  0x0004 },
    { "CLEFT",  0x0002 }, { "CRIGHT", 0x0001 },
};

struct ScriptEntry {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    uint16_t buttons = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    std::string note;
    bool announced = false;
};

std::vector<ScriptEntry> g_script;
bool g_script_loaded = false;
Uint64 g_script_start_ticks = 0;
uint8_t* g_rdram = nullptr;
bool g_script_uses_ticks = false;
bool g_script_exclusive = false;
uint32_t g_script_first_tick = 0;
bool g_script_tick_started = false;

uint32_t game_tick() {
    uint32_t tick = 0;
    if (g_rdram) std::memcpy(&tick, g_rdram + 0x151960, sizeof(tick));
    return tick;
}

double now_seconds() {
    if (g_script_start_ticks == 0) {
        g_script_start_ticks = SDL_GetTicks64();
    }
    return static_cast<double>(SDL_GetTicks64() - g_script_start_ticks) / 1000.0;
}

bool parse_buttons(const std::string& field, uint16_t* out) {
    *out = 0;
    if (field == "-" || field.empty()) {
        return true;
    }
    std::stringstream parts{ field };
    std::string name;
    while (std::getline(parts, name, ',')) {
        const NamedButton* found = nullptr;
        for (const NamedButton& candidate : kButtons) {
            if (name == candidate.name) {
                found = &candidate;
                break;
            }
        }
        if (found == nullptr) {
            std::fprintf(stderr, "[wr64] input script: unknown button '%s'\n", name.c_str());
            return false;
        }
        *out |= found->bit;
    }
    return true;
}

}  // namespace

namespace wr64 {

// Script format, one entry per line, '#' starts a comment:
//
//     <start> <end> <buttons> [stick_x stick_y] [# note]
//
// Times are seconds since the window opened; buttons are comma-separated names
// or '-' for none; the stick is in the N64's own +/-80 range. Entries overlap
// freely and are OR'd together, which is what makes both a tap (start 4.0, end
// 4.2) and a hold (accelerate for thirty seconds while steering) expressible in
// the same file without two syntaxes.
bool load_input_script() {
    const char* path = std::getenv("WR64_INPUT_SCRIPT");
    if (path == nullptr) {
        return false;
    }

    std::ifstream file{ path };
    if (!file) {
        std::fprintf(stderr, "[wr64] input script: cannot open %s\n", path);
        return false;
    }

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        const size_t comment = line.find('#');
        std::string note;
        if (comment != std::string::npos) {
            note = line.substr(comment + 1);
            line = line.substr(0, comment);
        }
        std::stringstream fields{ line };
        if (line.find("@ticks") != std::string::npos) {
            g_script_uses_ticks = true;
            g_script_exclusive = true;
            continue;
        }
        if (line.find("@exclusive") != std::string::npos) { g_script_exclusive = true; continue; }
        ScriptEntry entry;
        std::string button_field;
        if (!(fields >> entry.start_seconds >> entry.end_seconds >> button_field)) {
            continue;  // blank or comment-only line
        }
        if (!parse_buttons(button_field, &entry.buttons)) {
            std::fprintf(stderr, "[wr64] input script: %s:%d\n", path, line_number);
            return false;
        }
        fields >> entry.stick_x >> entry.stick_y;  // optional

        // Trim the note so the log reads cleanly.
        const size_t first = note.find_first_not_of(" \t");
        if (first != std::string::npos) {
            entry.note = note.substr(first);
        }
        g_script.push_back(entry);
    }

    std::sort(g_script.begin(), g_script.end(),
              [](const ScriptEntry& a, const ScriptEntry& b) {
                  return a.start_seconds < b.start_seconds;
              });

    g_script_loaded = !g_script.empty();
    std::fprintf(stderr, "[wr64] input script: %zu entries from %s (%s)\n", g_script.size(), path,
        g_script_uses_ticks ? "game updates" : "wall seconds");
    std::fflush(stderr);
    return g_script_loaded;
}

bool input_script_active() {
    return g_script_loaded;
}

bool input_script_exclusive() { return g_script_loaded && g_script_exclusive; }

void trace_input(int controller, uint16_t buttons, float x, float y) {
    static FILE *file=[]() {
        const char *name=std::getenv("WR64_INPUT_TRACE");
        FILE *f=name ? std::fopen(name,"w") : nullptr;
        if (f) std::fprintf(f,"tick,controller,buttons,x,y\n");
        return f;
    }();
    if (file) {
        std::fprintf(file,"%u,%d,%u,%.6f,%.6f\n",game_tick(),controller,buttons,x,y);
        std::fflush(file);
    }
}

void input_script_state(uint16_t* buttons, float* stick_x, float* stick_y) {
    *buttons = 0;
    *stick_x = 0.0f;
    *stick_y = 0.0f;
    if (!g_script_loaded) {
        return;
    }

    if (!g_script_tick_started) {
        g_script_first_tick = game_tick();
        g_script_tick_started = true;
    }
    const double t = g_script_uses_ticks ? double(game_tick() - g_script_first_tick) : now_seconds();
    for (ScriptEntry& entry : g_script) {
        if (t < entry.start_seconds || t >= entry.end_seconds) {
            continue;
        }
        if (!entry.announced) {
            entry.announced = true;
            std::fprintf(stderr, "[wr64] %s=%6.2f input: %s\n", g_script_uses_ticks ? "tick" : "t", t,
                         entry.note.empty() ? "(no note)" : entry.note.c_str());
            std::fflush(stderr);
        }
        *buttons |= entry.buttons;
        // Last writer wins for the stick; entries are sorted by start time, so
        // a later overlapping entry is the more specific instruction.
        if (entry.stick_x != 0.0f) { *stick_x = entry.stick_x; }
        if (entry.stick_y != 0.0f) { *stick_y = entry.stick_y; }
    }
}

// ------------------------------------------------------- game state watch ---

namespace {

// From the decomp's GameState enum. Only the named values are listed; anything
// else is reported as a bare number, which is still useful -- a transition to
// an unnamed state is a fact, not a gap.
const char* state_name(uint32_t state) {
    switch (state) {
        case 0x02: return "TITLE_SCREEN";
        case 0x03: return "MAIN_MENU";
        case 0x05: return "BOOT_UP";
        case 0x07: return "DEMO";
        case 0x0A: return "RIDER_SELECT";
        case 0x14: return "COURSE_SELECT";
        case 0x1E: return "COURSE_OVERVIEW";
        case 0x28: return "TIME_TRIAL";
        case 0x32: return "TIME_TRIALS_RESULTS";
        case 0x34: return "RACE_RESULTS";
        case 0x38: return "STUNT_MODE_RESULTS";
        case 0x3C: return "OPTIONS_MENU";
        case 0x3E: return "OPTIONS_CHANGE_NAMES";
        case 0x40: return "OPTIONS_SAVE_AND_LOAD";
        case 0x42: return "OPTIONS_VIEW_RECORDS";
        case 0x44: return "OPTIONS_CHANGE_CONDITIONS";
        case 0x46: return "OPTIONS_ERASE_COURSE_RECORDS";
        case 0x48: return "OPTIONS_AUDIO";
        case 0x66: return "CEREMONY";
        default:   return nullptr;
    }
}

const char* mode_name(uint32_t mode) {
    switch (mode) {
        case 0:  return "TIME_TRIALS";
        case 1:  return "2P_VS";
        case 4:  return "CHAMPIONSHIP";
        case 11: return "STUNT";
        default: return nullptr;
    }
}

// Addresses from the ELF: gGameState is a byte-sized enum in the main segment,
// gGameModes and gGameModeState live in bss. Read through the same swizzle the
// recompiled code uses -- 32-bit words are stored natively, so a word read is a
// plain load at the physical offset.
constexpr uint32_t kGameState     = 0x800DAB24;
constexpr uint32_t kGameModes     = 0x801CE620;
constexpr uint32_t kGameModeState = 0x801CE650;

uint32_t read_word(uint32_t vaddr) {
    return *reinterpret_cast<const uint32_t*>(g_rdram + (vaddr & 0x00FFFFFFu));
}

}  // namespace

void set_rdram_base(uint8_t* rdram) {
    g_rdram = rdram;
}

uint32_t current_game_state() {
    return g_rdram != nullptr ? read_word(kGameState) : 0;
}

void poll_game_state() {
    if (g_rdram == nullptr) {
        return;
    }
    static const uint32_t stopTick=[]() {
        const char *v=std::getenv("WR64_TEST_STOP_TICK");
        if (!v) return 0u;
        char *end=nullptr; const unsigned long tick=std::strtoul(v,&end,10);
        return end!=v && *end=='\0' && tick<=UINT32_MAX ? uint32_t(tick) : 0u;
    }();
    static bool stopped=false;
    if (stopTick && !stopped && game_tick()>stopTick) {
        stopped=true;
        std::fprintf(stderr,"[wr64] replay reached stop tick %u\n",stopTick);
        ultramodern::quit();
    }

    static uint32_t last_state = 0xFFFFFFFFu;
    static uint32_t last_mode = 0xFFFFFFFFu;

    const uint32_t state = read_word(kGameState);
    const uint32_t mode = read_word(kGameModes);
    if (state == last_state && mode == last_mode) {
        return;
    }
    last_state = state;
    last_mode = mode;

    const char* state_text = state_name(state);
    const char* mode_text = mode_name(mode);
    char state_buf[16];
    char mode_buf[16];
    if (state_text == nullptr) {
        std::snprintf(state_buf, sizeof(state_buf), "0x%02X", state);
        state_text = state_buf;
    }
    if (mode_text == nullptr) {
        std::snprintf(mode_buf, sizeof(mode_buf), "0x%X", mode);
        mode_text = mode_buf;
    }

    std::fprintf(stderr, "[wr64] t=%6.2f tick=%u state: %s (mode %s, phase %u)\n",
                 now_seconds(), game_tick(), state_text, mode_text, read_word(kGameModeState));
    std::fflush(stderr);
}

}  // namespace wr64
