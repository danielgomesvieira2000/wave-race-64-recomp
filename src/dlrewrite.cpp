// The display-list rewriter. See include/wr64/dlrewrite.h for what it is for.
//
// What it does (phase 07, steps A1 to A3):
//
// 1. The drawn region. The scissor the game sets is read as it goes by, and
//    the presentation's crop follows it (wr64::display::set_content): with
//    two players the game draws into a taller region than with one.
//
// 2. 3D inside a menu's 2D layout. In a menu state, a perspective projection
//    drawn after the 2D layout has started -- the models on the watercraft
//    and rider select screens -- is given a viewport whose origin is the
//    centre of the frame. RT64 keeps a full-width projection "adjusted" to
//    the widened frame by default: a wider field of view for a perspective
//    projection, 2D kept at 4:3 in the middle for an orthographic one. A
//    viewport with an origin switches that off for its projection, so the
//    game's frustum is rendered into the centred 4:3 region, where the 2D is.
//    3D drawn before any 2D is a backdrop (the title, the course overview)
//    and stays as wide as the frame.
//
// 3. The 2D layer. Each 2D draw -- a vertex load, or a call to a static list,
//    under an orthographic projection -- is classified by where it sits on
//    the 320-wide screen: an element in the left third is anchored to the
//    left edge, one in the right third to the right edge, one that covers the
//    frame is stretched across it, and the rest stay in the middle. A tag
//    table in the settings folder (hud.json) overrides the class per texture
//    or per static list. Anchors are applied only when the HUD Placement
//    setting asks for them; stretching is applied always, since a sun glare
//    that stops at 4:3 is a defect in any mode. Stretching is for the 2D
//    layer alone, though: a full-frame rectangle drawn under a perspective
//    projection belongs to the 3D pass, which is already at the frame's
//    width, and stretching it magnifies the picture. See classify().
//
//    RT64 anchors 2D geometry by the origin carried on its viewport, and
//    stretches by a flag on its projection group, both per projection rather
//    than per triangle, so each change of class reissues the game's own
//    viewport or projection command after the new alignment or group. An
//    anchored element is also inset by how far the visible picture's edge
//    lies inside RT64's widened frame (wr64::display::anchor_inset), so that
//    RT64's edge means the window's edge. The 2D projection groups carry no
//    interpolation: the same projection is reissued several times per frame
//    with different flags, and RT64 must never blend one with another.
//
// 4. The sky and the water (phase 07, step B). The game rebuilds both from
//    vertices every frame, under an unchanging identity matrix, so RT64 pairs
//    that matrix perfectly, finds no motion, and holds them still between the
//    game's frames while the world moves. Each is given a matrix group asking
//    for the vertices and their texture coordinates to be interpolated as
//    well, and an explicit transform id so the pairing they depend on cannot
//    be lost to a tie. See the sky and water sections below.
//
// How the list is read. The game builds one top-level list per frame with
// every matrix load inline, and calls its model and HUD lists from it. This
// walks the top-level list, copying each command, following branches in
// place and leaving calls as calls: the called lists live where they live,
// and RT64 runs them from there. A called list is scanned, read-only, to
// learn its extent and texture, so it can be classified like an inline draw.
// Segment table writes, the projection matrix and a modelview stack are
// tracked so that matrices and vertices can be read from RDRAM; the
// perspective test is RT64's own, element [3][3] of the projection.
//
// Memory. RDRAM in the runtime stores 32-bit words natively, so a command is
// two host words at its physical offset, and RT64 reads it the same way. The
// 16-bit halves of a matrix or a vertex are the exception: they sit at their
// offset XOR 2, which is also how RT64 reads them. The scratch list lives at
// physical 0x700000, in the top of the 8 MB the runtime reports; the
// cartridge is a 4 MB one that never reads osMemSize, so nothing of the
// game's is there, and the audio task's private copy of its command list is
// above it at 0x7E0000.
//
// WR64_HUD_TRACE=1 prints every 2D draw of the first two drawing lists after
// each change of game state, with its identity, its extent and the class it
// was given; that is how a tag for hud.json is found. It also reports any
// element that is given one class in one frame and another in the next,
// which flickers between the two however defensible each classification is. WR64_HUD_OFF=1 leaves
// the 2D layer alone, WR64_NO_REWRITE=1 switches the rewriter off entirely,
// WR64_NO_SKY_INTERP=1 leaves the sky at the game's rate and
// WR64_NO_WATER_INTERP=1 leaves the water's waves at it. WR64_PAIRING=1, read
// in patches/framerate.cpp rather than here, reports how much of the frame
// RT64 is managing to interpolate at all.
//
// WR64_3D_TRACE names a file and writes the whole of a few race frames into
// it -- every matrix load, vertex load and call, with each block's hash and
// its clip-space extent -- which is how the sky was found: two consecutive
// frames diffed against each other say which geometry the game rebuilds
// rather than moves. WR64_3D_TRACE_FRAMES sets how many frames (two by
// default) and WR64_3D_TRACE_STATE takes a game state in hexadecimal for the
// screens that are not races, or "any" for whatever is on screen. WR64_3D_TRACE_EVERY spaces them out, in race
// frames: consecutive frames answer "what can the renderer pair between them",
// and spaced ones answer "is this object submitted at all from over there",
// which is the question behind a report of things popping in.
//
// WR64_LATTICE names a file and writes one line per frame for each of the two
// meshes the game rebuilds -- the water through segment 3, the sky through
// segment 6 -- saying how many of their vertex blocks moved in X or Z since
// the previous frame and how many only in height. It answers whether the mesh
// interpolation is paired on stands still under a moving camera, which pairing
// by index depends on. WR64_LATTICE_FRAMES bounds the run.

#include "wr64/dlrewrite.h"
#include "wr64/display.h"
#include "wr64/drawdistance.h"
#include "wr64/inspector.h"
#include "wr64/testdrive.h"
#include "wr64/water.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <librecomp/game.hpp>
#include <ultramodern/config.hpp>
#include "json/json.hpp"

#include "rt64_extended_gbi.h"

namespace {

constexpr uint32_t kScratch = 0x80700000u;
constexpr uint32_t kScratchSize = 0x40000u;      // 256 KB: 32768 commands
constexpr uint32_t kMaxCommands = kScratchSize / 8;

// Where a rewritten projection matrix is put, just past the list's own scratch
// and still inside the megabyte nothing of the game's occupies. A matrix is 64
// bytes; a frame loads a handful, and the cursor restarts every frame.
constexpr uint32_t kMatrixScratch = kScratch + kScratchSize;
constexpr uint32_t kMatrixScratchSize = 0x1000u;   // 64 matrices

// Fast3D opcodes this walk has to understand. Everything else is copied.
constexpr uint8_t kOpMtx = 0x01;
constexpr uint8_t kOpMoveMem = 0x03;
constexpr uint8_t kOpVtx = 0x04;
constexpr uint8_t kOpDisplayList = 0x06;
constexpr uint8_t kOpMoveWord = 0xBC;
constexpr uint8_t kOpPopMtx = 0xBD;
constexpr uint8_t kOpEndDisplayList = 0xB8;
constexpr uint8_t kOpSetScissor = 0xED;     // RDP, 10.2 fixed point
constexpr uint8_t kOpSetTextureImage = 0xFD;

// The three commands that draw a rectangle: a textured one, a textured one
// flipped, and a filled one. All three carry their corners in the same fields,
// and RT64 applies the same alignment and aspect state to all three.
//
// The filled one was missing here, and that is what left the pause screen's
// dim at 4:3 with a bright band down its left: the dim is a fill rectangle,
// so it was never classified and never stretched, however the test that
// decides what to stretch was written.
constexpr uint8_t kOpTexRect = 0xE4;
constexpr uint8_t kOpTexRectFlip = 0xE5;
constexpr uint8_t kOpFillRect = 0xF6;

bool is_rect(uint8_t op) {
    return op == kOpTexRect || op == kOpTexRectFlip || op == kOpFillRect;
}

constexpr uint32_t kMtxProjection = 0x01;
constexpr uint32_t kMtxLoad = 0x02;
constexpr uint32_t kMtxPush = 0x04;
constexpr uint8_t kMoveWordSegment = 0x06;
constexpr uint8_t kMoveMemViewport = 0x80;

// RT64 displaces a viewport by origin / 1024 of the framebuffer's width, in
// quarter pixels; these offsets undo it, so the game's viewport data is used
// as it is and only the anchor moves.
constexpr int kFramebufferWidth = 320;
int origin_cancel(uint32_t origin) {
    return -static_cast<int>((origin * kFramebufferWidth * 4) / G_EX_ORIGIN_RIGHT);
}

// A 2D draw covers the frame when its vertices reach this close to every edge
// of clip space; the margin only forgives rounding. Anchoring is by the
// element's centre: past a third of the way to an edge, it belongs to it.
constexpr float kFrameEdge = 0.97f;
constexpr float kAnchorThird = 1.0f / 3.0f;

// The states in which the game is racing: the attract demo, and the race
// itself, whichever mode it is in.
// The game's own vertical field of view, from its projection: m[1][1] is
// cot(fovy/2) and reads 2.4142, which is cot(22.5) to five figures. Every wider
// setting is expressed as a ratio against this, and the ratio is applied to
// every perspective pass of the frame so that the sky -- which has a frustum of
// its own -- keeps step with the world.
constexpr double kGameFovY = 45.0;

// Set from the settings menu, read once per display list.
std::atomic<float> g_fov_scale{ 1.0f };

// A multiplier read from the environment, for measuring one of these before it
// is worth a setting. Out-of-range or unparseable values are reported and
// ignored rather than silently taken.
float env_scale(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) return fallback;
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (end == value || parsed < 0.05f || parsed > 64.0f) {
        std::fprintf(stderr, "[wr64] %s=%s not usable (0.05 to 64); using %.2f\n",
                     name, value, fallback);
        return fallback;
    }
    std::fprintf(stderr, "[wr64] %s = %.3f\n", name, parsed);
    return parsed;
}

bool racing(uint32_t state) {
    return state == 0x07 || state == 0x28;
}

// Whether the frame is a race, read from the frame itself rather than from
// the game's state variable.
//
// The state variable was the wrong thing to key on: the game keeps a dozen
// states around a race -- 0x28 through 0x2D at least, by the decompilation's
// enumeration -- and a race frame in a state this port did not list was
// treated as a menu and squeezed to 4:3.
//
// The frame says it instead. A race draws its world first, under a
// perspective projection, and sets an inset scissor to do it: (8, 20)-(311,
// 219) with one player, (8, 12)-(311, 229) with two. Menus with a 3D backdrop
// -- the title, the main menu, the select screens -- do both of those as well,
// so neither is enough on its own. What separates them is where the 2D goes:
// a race switches to an orthographic projection to draw its HUD, while a menu
// lays its frames and labels out with the backdrop's perspective projection
// still loaded. Measured across the title, the main menu, the select screens
// and a race, every race HUD rectangle was drawn under an orthographic
// projection and every menu rectangle under a perspective one, with no
// overlap. The projection type is per draw, so it is tested there; these two
// are the frame-wide half of the same question.
struct RaceTest {
    bool first_projection_seen = false;
    bool world_first = false;
    bool inset_scissor = false;

    bool race() const { return world_first && inset_scissor; }
};

// What the rewriter does with a 2D element on a widened frame.
//
// The first four are about placement: keep it where it is, pin it to an edge, or
// widen it with the frame. Spill is about clipping instead, and is the only one
// that changes nothing about where the element is drawn or how big it is -- it
// lifts the game's 4:3 scissor so the element is allowed to continue past the
// edge of the old frame. The sun over the race is the case it exists for: the
// game draws it as a 48x48 square at x 0..48, running deliberately off the left
// of its own screen, and on a widened frame it met the 4:3 boundary and stopped
// dead instead of carrying on into the picture that is now there.
enum class Class { Auto, Left, Right, Stretch, Spill };

const char* class_name(Class c) {
    switch (c) {
        case Class::Left:    return "left";
        case Class::Right:   return "right";
        case Class::Stretch: return "stretch";
        case Class::Spill:   return "spill";
        default:             return "center";
    }
}

// The tag table: hud.json in the settings folder. Four lists of identities,
// "tex:0x01004A20" for a texture and "dl:0x0106F8A0" for a static list, as
// the trace prints them, under "left", "right", "center", "stretch" and
// "spill".
struct Tags {
    std::unordered_map<std::string, Class> by_identity;
    bool loaded = false;

    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // What the port knows before anyone edits anything. These are elements whose
    // class cannot be worked out from the frame alone, found with the trace and
    // written down here rather than left for every player to discover.
    //
    // tex:0x01005748 is the sun glare over the opening. It is a textured
    // rectangle covering the whole drawn region, and it is issued under the
    // world's own perspective projection, which is otherwise the signature of a
    // rectangle that belongs to the picture rather than lying over it -- the
    // intro's camera shots are composed that way, and stretching those magnifies
    // the picture. This one is a haze laid over the shot, so it has to reach the
    // frame's edges like any other overlay, and 4:3 glare over a widescreen
    // picture is exactly what it looked like.
    //
    // dl:0x0106F408 is the pair of red cubes that mark the selected entry on the
    // menus. The game draws that one list twice under an orthographic
    // projection, once on each side of the entry, at clip x -0.49 and +0.49 --
    // and the anchoring rule, which exists for the race HUD, reads a centre past
    // a third of the way out as belonging to that edge and pinned one cube to
    // each edge of the frame. In widescreen they slid off towards the corners
    // while the entry they belong to stayed in the middle.
    //
    // The rule was never meant to run here. It is guarded on the frame being a
    // race, and the main menu passes that test: it draws a 3D world first, under
    // a perspective projection, into the same inset scissor a race uses, which
    // is exactly what the test looks for. Rather than loosen a test that earns
    // its keep in a dozen states around a race, the cursor is named. Anything
    // else on these menus is 2D over a 4:3 layout and classifies correctly.
    //
    // hud.json still wins: a player who tags the same identity overrides this.
    // Identities are looked up lower-cased (see lookup), so hex digits above
    // nine must be written in lower case here or the entry is never found.
    void load_defaults() {
        by_identity["tex:0x01005748"] = Class::Stretch;
        by_identity["dl:0x0106f408"] = Class::Auto;

        // ---- promoted from hud.json by tools/promote_hud_tags.py ----
        // Tagged in the inspector and promoted here so a release carries them.
        // Everything between these two markers is rewritten by that script;
        // hand-written entries go above the first marker, with their reasons.
        by_identity["tex:0x01005748"] = Class::Stretch;
        by_identity["tex:0x0100fab0"] = Class::Spill;
        by_identity["tex:0x010331d0"] = Class::Right;
        by_identity["tex:0x01033cb8"] = Class::Auto;
        by_identity["tex:0x01033e98"] = Class::Auto;
        by_identity["tex:0x01034078"] = Class::Auto;
        by_identity["tex:0x01034258"] = Class::Auto;
        by_identity["tex:0x01034438"] = Class::Auto;
        by_identity["tex:0x01034618"] = Class::Auto;
        by_identity["tex:0x010347f8"] = Class::Auto;
        by_identity["tex:0x010349d8"] = Class::Auto;
        by_identity["tex:0x01034bb8"] = Class::Auto;
        by_identity["tex:0x01034d98"] = Class::Auto;
        by_identity["tex:0x01035338"] = Class::Auto;
        by_identity["tex:0x010358d8"] = Class::Auto;
        by_identity["tex:0x010369b8"] = Class::Auto;
        by_identity["tex:0x01036b98"] = Class::Auto;
        by_identity["tex:0x0103d8d8"] = Class::Right;
        by_identity["tex:0x0103ddd8"] = Class::Right;
        by_identity["tex:0x0103e2d8"] = Class::Right;
        by_identity["tex:0x0103e7d8"] = Class::Right;
        by_identity["tex:0x0103ecd8"] = Class::Right;
        by_identity["tex:0x0103f1d8"] = Class::Right;
        by_identity["tex:0x0103f6d8"] = Class::Right;
        by_identity["tex:0x0103fbd8"] = Class::Right;
        by_identity["tex:0x010400d8"] = Class::Right;
        by_identity["tex:0x010405d8"] = Class::Right;
        by_identity["tex:0x01044f60"] = Class::Auto;
        by_identity["tex:0x01046910"] = Class::Spill;
        by_identity["tex:0x010515a8"] = Class::Auto;
        by_identity["tex:0x08004400"] = Class::Auto;
        by_identity["tex:0x08004c00"] = Class::Spill;
        by_identity["tex:0x08024008"] = Class::Auto;
        by_identity["tex:0x08024e10"] = Class::Auto;
        by_identity["tex:0x08025c88"] = Class::Stretch;
        by_identity["tex:0x080267f0"] = Class::Auto;
        by_identity["tex:0x08027748"] = Class::Stretch;
        by_identity["tex:0x08028470"] = Class::Spill;
        by_identity["tex:0x0802a7f0"] = Class::Spill;
        by_identity["tex:0x0802c0f0"] = Class::Spill;
        // ---- end promoted ----
    }

    void load() {
        loaded = true;
        load_defaults();
        const std::filesystem::path path = recomp::get_config_path() / "hud.json";
        std::ifstream in(path);
        if (!in) {
            // Leave a template behind so the format does not have to be guessed.
            std::ofstream out(path);
            if (out) {
                out << "{\n"
                       "    \"help\": \"Run with WR64_HUD_TRACE=1 to list 2D draws with their identity"
                       " (tex:... or dl:...) and the class they were given; put an identity in one"
                       " of the lists below to override it.\",\n"
                       "    \"left\": [],\n"
                       "    \"right\": [],\n"
                       "    \"center\": [],\n"
                       "    \"stretch\": []\n"
                       "}\n";
            }
            return;
        }
        nlohmann::json doc;
        try {
            in >> doc;
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[wr64] hud.json could not be read: %s\n", e.what());
            return;
        }
        const std::pair<const char*, Class> lists[] = {
            { "left", Class::Left }, { "right", Class::Right },
            { "center", Class::Auto }, { "stretch", Class::Stretch },
            { "spill", Class::Spill },
        };
        int count = 0;
        for (const auto& [key, cls] : lists) {
            if (!doc.contains(key) || !doc[key].is_array()) continue;
            for (const auto& item : doc[key]) {
                if (item.is_string()) {
                    by_identity[lower(item.get<std::string>())] = cls;
                    ++count;
                }
            }
        }
        if (count > 0) {
            std::fprintf(stderr, "[wr64] hud.json: %d tagged elements\n", count);
        }
    }

    bool lookup(const std::string& identity, Class& out) const {
        const auto it = by_identity.find(lower(identity));
        if (it == by_identity.end()) return false;
        out = it->second;
        return true;
    }
};

struct Mat4 {
    float m[4][4];

    static Mat4 identity() {
        Mat4 r{};
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
        return r;
    }

    // Row-vector convention, as the N64's matrices are: v' = v * M.
    Mat4 operator*(const Mat4& o) const {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }
};

// The clip-space extent of a set of vertices.
struct Extent {
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    bool empty() const { return max_x < min_x; }
    // The draw's horizontal extent in the game's own screen pixels.
    float left_px() const { return (min_x + 1.0f) * (kFramebufferWidth / 2.0f); }
    float right_px() const { return (max_x + 1.0f) * (kFramebufferWidth / 2.0f); }
    float center_x() const { return (min_x + max_x) / 2.0f; }
    void add(const Extent& o) {
        min_x = std::min(min_x, o.min_x); max_x = std::max(max_x, o.max_x);
        min_y = std::min(min_y, o.min_y); max_y = std::max(max_y, o.max_y);
    }
};

struct Walker {
    uint8_t* rdram;
    uint32_t segments[16] = {};
    uint32_t* out;
    uint32_t written = 0;       // commands
    bool overflow = false;
    uint32_t state = 0;
    bool menu = true;
    bool anchors = false;       // HUD Placement asks for edges
    bool hud = true;            // the 2D layer is handled at all
    bool noemit = false;        // classify and trace, but change nothing (WR64_HUD_NOEMIT)
    float inset = 0.0f;         // wr64::display::anchor_inset()
    const Tags* tags = nullptr;
    std::string* trace = nullptr;

    // The game's most recent viewport and projection commands, reissued after
    // a change of alignment or group so that the change takes effect.
    bool have_viewport = false;
    uint32_t viewport_w0 = 0, viewport_w1 = 0;
    bool have_projection = false;
    uint32_t projection_w0 = 0, projection_w1 = 0;
    bool have_scissor_cmd = false;
    uint32_t scissor_w0 = 0, scissor_w1 = 0;
    bool scissor_widened = false;

    // The matrices in force, as floats, and the texture last set.
    Mat4 projection = Mat4::identity();         // everything on the projection stack
    Mat4 projection_only = Mat4::identity();    // the projection without the camera
    bool projection_is_perspective = false;

    // Screen-space perspective sections released to the widened frame this
    // list, for the per-frame report, and the switch that turns the treatment
    // off. The switch is here for the same reason the other bisect switches are:
    // whether a change is an improvement is a question only a side-by-side
    // answers.
    uint32_t released_sections = 0;
    bool no_curtain = false;

    // Whether this walk has a screen-space section open. Deliberately separate
    // from `cls`: the 2D classifier assigns Class::Stretch to full-frame
    // rectangles on these same frames, and keying off that class would tangle
    // the two.
    bool curtain_open = false;

    // Whether the origin was on when the section opened, so it can be put back
    // exactly as it was: the screens on either side of the wipe still want it.
    bool curtain_took_origin_off = false;
    Mat4 modelview[16];
    int modelview_depth = 0;
    uint32_t texture = 0;

    // Section state: A1's centred perspective sections, and the 2D class in
    // force.
    // Draw distance and field of view, as multipliers on the world's frustum.
    // One is the game's own.
    float far_scale = 1.0f;
    float fov_scale = 1.0f;
    uint32_t matrix_cursor = 0;

    bool centred = false;
    bool centred_by_viewport = false;   // ... and on_viewport_load is what decided it
    bool seen_ortho = false;
    RaceTest race_test;
    Class cls = Class::Auto;
    uint32_t centred_sections = 0;
    uint32_t class_changes = 0;
    uint32_t draws_2d = 0;      // how many 2D elements the list drew

    // The union of the scissors the game set for its drawing this frame. A
    // scissor covering the whole framebuffer is a clear, not drawing, and is
    // left out; what remains is the region the game draws into.
    int scissor_left = 0x7FFF, scissor_top = 0x7FFF, scissor_right = -1, scissor_bottom = -1;

    Walker() {
        modelview[0] = Mat4::identity();
    }

    // ---- memory ---------------------------------------------------------

    uint32_t physical(uint32_t segmented) const {
        const uint32_t base = segments[(segmented >> 24) & 0xF];
        return (base + (segmented & 0x00FFFFFFu)) & 0x00FFFFF8u;
    }

    const uint32_t* words(uint32_t physical_addr) const {
        return reinterpret_cast<const uint32_t*>(rdram + (physical_addr & 0x00FFFFFFu));
    }

    uint16_t half(uint32_t physical_addr) const {
        return *reinterpret_cast<const uint16_t*>(rdram + ((physical_addr & 0x00FFFFFFu) ^ 2));
    }

    int16_t signed_half(uint32_t physical_addr) const {
        return static_cast<int16_t>(half(physical_addr));
    }

    void write_half(uint32_t physical_addr, uint16_t value) const {
        *reinterpret_cast<uint16_t*>(rdram + ((physical_addr & 0x00FFFFFFu) ^ 2)) = value;
    }

    // The inverse of read_matrix: sixteen integer halves then sixteen fraction
    // halves, row-major, s15.16 throughout.
    void write_matrix(uint32_t physical_addr, const Mat4& m) const {
        for (int i = 0; i < 16; ++i) {
            const int32_t fixed =
                static_cast<int32_t>(std::lround(double(m.m[i / 4][i % 4]) * 65536.0));
            write_half(physical_addr + 2 * i, uint16_t(uint32_t(fixed) >> 16));
            write_half(physical_addr + 32 + 2 * i, uint16_t(uint32_t(fixed) & 0xFFFF));
        }
    }

    // A fixed-point Mtx: sixteen integer halves then sixteen fraction halves,
    // row-major; the same conversion RT64 makes.
    Mat4 read_matrix(uint32_t physical_addr) const {
        Mat4 r{};
        for (int i = 0; i < 16; ++i) {
            const uint32_t full = (uint32_t(half(physical_addr + 2 * i)) << 16) |
                                  half(physical_addr + 32 + 2 * i);
            r.m[i / 4][i % 4] = static_cast<float>(static_cast<int32_t>(full)) / 65536.0f;
        }
        return r;
    }

    // Screen-space geometry laid out under a perspective projection, told apart
    // from an actual 3D view by the aspect ratio the game built the projection
    // with. `guPerspective` divides the horizontal term by the aspect, so a view
    // meant for a 4:3 screen has [0][0] = [1][1] / 1.333. These are equal, which
    // means an aspect of exactly one -- nobody frames a camera that way, and it
    // is what you get when the matrix exists to put flat geometry on the screen
    // rather than to look at a world.
    //
    // The transition curtain is the case this was written for: eight bands of
    // 3200 x 320 units at z = 0, under [0][0] = [1][1] = 3.370880, with an
    // identity view. 3200 is 320 x 10 -- exactly the width of the game's 4:3
    // screen, which is why it leaves bars on a wider one and why widening the
    // scissor alone would not have helped: there is no more curtain to reveal.
    static bool square_aspect(const Mat4& p) {
        const float x = std::fabs(p.m[0][0]);
        const float y = std::fabs(p.m[1][1]);
        if (x < 1e-6f || y < 1e-6f) return false;
        return std::fabs(x - y) < 1e-3f * y;
    }

    // Whether what is in force right now is screen-space geometry: built at an
    // aspect of one, and owning the whole frame. Both halves, whenever asked.
    bool screen_space_section() const {
        return !no_curtain && have_projection && projection_is_perspective &&
               square_aspect(projection_only) && have_viewport &&
               viewport_is_full_frame(viewport_w1);
    }

    static bool perspective(const Mat4& p) {
        return p.m[3][3] == 0.0f && (p.m[1][1] > 1e-6f || p.m[1][1] < -1e-6f);
    }

    Extent extent(uint32_t vtx_physical, uint32_t count, const Mat4& mv) const {
        const Mat4 mvp = mv * projection;
        Extent e;
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            const uint32_t v = vtx_physical + i * 16;
            const float x = static_cast<float>(signed_half(v + 0));
            const float y = static_cast<float>(signed_half(v + 2));
            const float z = static_cast<float>(signed_half(v + 4));
            const float cx = x * mvp.m[0][0] + y * mvp.m[1][0] + z * mvp.m[2][0] + mvp.m[3][0];
            const float cy = x * mvp.m[0][1] + y * mvp.m[1][1] + z * mvp.m[2][1] + mvp.m[3][1];
            const float cw = x * mvp.m[0][3] + y * mvp.m[1][3] + z * mvp.m[2][3] + mvp.m[3][3];
            if (cw == 0.0f) continue;
            e.min_x = std::min(e.min_x, cx / cw);
            e.max_x = std::max(e.max_x, cx / cw);
            e.min_y = std::min(e.min_y, cy / cw);
            e.max_y = std::max(e.max_y, cy / cw);
        }
        return e;
    }

    // ---- output ---------------------------------------------------------

    void emit(uint32_t w0, uint32_t w1) {
        if (written >= kMaxCommands) {
            overflow = true;
            return;
        }
        out[written * 2] = w0;
        out[written * 2 + 1] = w1;
        ++written;
    }

    GfxCommand* reserve(uint32_t count) {
        if (written + count > kMaxCommands) {
            overflow = true;
            return nullptr;
        }
        GfxCommand* cmd = reinterpret_cast<GfxCommand*>(out + written * 2);
        written += count;
        return cmd;
    }

    // The origin RT64 is to apply to viewports from here on, on its own. Used
    // where the game's own viewport command is about to follow anyway.
    void viewport_align(uint32_t origin, int offset_x) {
        if (GfxCommand* cmd = reserve(2)) {
            gEXSetViewportAlign(cmd, origin, offset_x, 0);
        }
    }

    void align_viewport(uint32_t origin, int offset_x) {
        viewport_align(origin, offset_x);
        if (have_viewport) {
            emit(viewport_w0, viewport_w1);
        }
    }

    // Whether the viewport the game is loading is the whole frame.
    //
    // Vp carries vscale and vtrans as signed halves in 2.2 fixed point, so the
    // full frame reads 640 and 640: a half-width of 160 about x = 160. The
    // tolerance is four, which is one pixel.
    bool viewport_is_full_frame(uint32_t segmented) const {
        const uint32_t addr = physical(segmented);
        const int full = kFramebufferWidth * 4 / 2;
        return std::abs(int(signed_half(addr)) - full) <= 4 &&
               std::abs(int(signed_half(addr + 8)) - full) <= 4;
    }

    // A viewport the game has moved off the centre of the screen is how it
    // places a 3D object inside a 2D layout, and RT64 cannot see that.
    //
    // The race results screen gives each of its four craft a full-size frustum
    // and moves the viewport's centre to that craft's row: scale 160x120,
    // translate 86,88. RT64 decides whether to render a pass across the widened
    // frame by asking whether it covers the frame's width, and it measures a
    // viewport through its clip ratios -- 3 here, so a 320-wide viewport is
    // measured 1920 wide and covers the frame however far it has been moved. So
    // it answers yes, and the craft land at 86/320 of the widened frame instead
    // of 86/320 of the 4:3 box: they slide outwards by the widening factor while
    // the rows they belong to stay put.
    //
    // An origin on the viewport switches that off, and it is the same origin the
    // menus' model boxes already get. The rule at the projection load decides it
    // from whether the 2D layout has started, which it reads as "an orthographic
    // projection has been loaded" -- and this screen lays its 2D out under a
    // perspective one, so it never fired here. The viewport says it directly
    // instead: it is the only thing in the list that states where the game meant
    // the object to be.
    // The world's frustum, rewritten: a copy of the game's own matrix with its
    // far plane pushed out, its field of view widened, or both. Returns the
    // address to load instead, or 0 to leave the game's alone.
    //
    // This game builds its projection the way gluPerspective does, so
    //
    //     m[2][2] = (f + n) / (n - f)        m[3][2] = 2 f n / (n - f)
    //
    // and the two planes come back out as near = m32 / (m22 - 1) and
    // far = m32 / (m22 + 1). Measured against the projection the menus place
    // objects with, those read exactly 16 and 4096, which is the check that the
    // form is right rather than merely plausible.
    //
    // Field of view is the other two: m[0][0] and m[1][1] are cot(fovy/2), with
    // m[0][0] additionally divided by the aspect ratio, so dividing both by the
    // same number widens the view without changing its shape. RT64's own
    // widescreen adjustment happens after this and is unaffected.
    uint32_t rewrite_projection(const Mat4& loaded) {
        if (far_scale == 1.0f && fov_scale == 1.0f) return 0;
        Mat4 m = loaded;
        if (far_scale != 1.0f) {
            const float m22 = loaded.m[2][2];
            const float m32 = loaded.m[3][2];
            if (std::fabs(m22 - 1.0f) < 1e-6f || std::fabs(m22 + 1.0f) < 1e-6f) return 0;
            const float near_plane = m32 / (m22 - 1.0f);
            const float far_plane = (m32 / (m22 + 1.0f)) * far_scale;
            // Reported once, because whether the far plane is anywhere near the
            // geometry is the whole question and a screenshot cannot answer it.
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr, "[wr64] frustum: near %.1f far %.1f -> far %.1f"
                                     " (m00 %.3f m11 %.3f)\n",
                             near_plane, m32 / (m22 + 1.0f), far_plane,
                             loaded.m[0][0], loaded.m[1][1]);
                std::fflush(stderr);
            }
            const float d = near_plane - far_plane;
            if (std::fabs(d) < 1e-6f) return 0;
            m.m[2][2] = (far_plane + near_plane) / d;
            m.m[3][2] = 2.0f * far_plane * near_plane / d;
        }
        if (fov_scale != 1.0f) {
            m.m[0][0] /= fov_scale;
            m.m[1][1] /= fov_scale;
        }
        if (matrix_cursor + 64 > kMatrixScratchSize) return 0;
        const uint32_t address = kMatrixScratch + matrix_cursor;
        matrix_cursor += 64;
        write_matrix(address, m);
        return address;
    }

    void on_viewport_load(uint32_t segmented) {
        if (!menu || noemit) return;
        const bool full_frame = viewport_is_full_frame(segmented);

        if (!full_frame && !centred) {
            viewport_align(G_EX_ORIGIN_CENTER, origin_cancel(G_EX_ORIGIN_CENTER));
            centred = true;
            centred_by_viewport = true;
            ++centred_sections;
        }
        else if (full_frame && centred_by_viewport) {
            viewport_align(G_EX_ORIGIN_NONE, 0);
            centred = false;
            centred_by_viewport = false;
        }

    }

    // Whether a called list loads a viewport of its own that covers the whole
    // frame, centred.
    //
    // The rewriter emits a call and lets RT64 walk into it, so a viewport the
    // called list loads is invisible at the call site: what is in force there is
    // whatever the previous group left behind. This game draws its transition
    // curtain from such a list and loads its own centred, frame-wide viewport
    // inside it -- which is why the debugger shows that viewport on the curtain's
    // draw while the rewriter, asked at the call, sees the stale off-centre one
    // belonging to the screen underneath, and rejects it. Every test made at the
    // call site was reading state that is, for this element, systematically one
    // group out of date.
    //
    // So this looks. It walks the called list for a viewport load and answers
    // whether that viewport covers the frame, following nested calls and
    // branches the same way the rest of the walker does.
    bool list_loads_full_frame_viewport(uint32_t addr, int depth) {
        if (depth > 4) return false;
        for (uint32_t steps = 0; steps < 8192; ++steps) {
            const uint32_t* c = words(addr);
            if (c == nullptr) return false;
            const uint32_t w0 = c[0];
            const uint32_t w1 = c[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            addr += 8;

            if (op == kOpEndDisplayList) return false;
            if (op == kOpMoveMem && ((w0 >> 16) & 0xFF) == kMoveMemViewport) {
                return viewport_is_full_frame(w1);
            }
            if (op == kOpDisplayList) {
                const bool branch = ((w0 >> 16) & 0xFF) != 0;
                if (branch) {
                    addr = physical(w1);
                    continue;
                }
                if (list_loads_full_frame_viewport(physical(w1), depth + 1)) return true;
            }
        }
        return false;
    }

    // Opens or closes a screen-space section.
    //
    // Asked at each vertex load rather than at a projection or viewport load,
    // and that is the whole of what took four attempts to get right. The two
    // facts that define such a section -- a projection built at an aspect of one
    // and a viewport that still covers the whole frame -- arrive as separate
    // commands, in either order, and the order differs between the two phases of
    // the wipe. Deciding at either load therefore asks the question while half
    // the answer is the previous group's. At a vertex load both are final for
    // the geometry about to be drawn, and there is nothing left to reason about.
    //
    // A 2D class in force means the classifier is placing this, and it is left
    // alone.
    void curtain(bool want) {
        if (want == curtain_open) return;
        if (want && cls != Class::Auto) return;
        curtain_open = want;

        // RT64 already renders a pass that covers the frame across the widened
        // frame -- it measures a viewport through its clip ratios, so a 320-wide
        // one measures 1920 and covers it however far it has been moved. What
        // stops it is the origin this port puts on a menu's viewport to keep 3D
        // objects in the boxes the layout gives them. That is right for a craft
        // in a box and wrong for a curtain meant to cover the screen, so the
        // origin comes off for the length of the call and goes back after.
        //
        // Through viewport_align rather than align_viewport: the latter also
        // re-emits the last viewport the rewriter saw, which here belongs to the
        // screen underneath, and its translate of 59,74 against the curtain's
        // 160,120 moved the curtain down the screen by the difference.
        if (want) {
            curtain_took_origin_off = centred;
            if (centred) {
                viewport_align(G_EX_ORIGIN_NONE, 0);
                centred = false;
            }
            ++released_sections;
        }
        else if (curtain_took_origin_off) {
            if (!centred) {
                viewport_align(G_EX_ORIGIN_CENTER, origin_cancel(G_EX_ORIGIN_CENTER));
                centred = true;
            }
            curtain_took_origin_off = false;
        }
    }

    // A projection group for a 2D section: the aspect mode given, and no
    // interpolation of anything.
    void projection_group(uint32_t aspect) {
        if (GfxCommand* cmd = reserve(2)) {
            gEXMatrixGroup(cmd, G_EX_ID_AUTO, G_EX_INTERPOLATE_SIMPLE, G_EX_NOPUSH, 1,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                           G_EX_COMPONENT_SKIP, G_EX_ORDER_AUTO, G_EX_EDIT_NONE, aspect,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
        }
    }

    void projection_aspect(uint32_t aspect) {
        projection_group(aspect);
        if (have_projection) {
            emit(projection_w0, projection_w1);
        }
    }

    // ---- 2D classes -----------------------------------------------------

    // Leaves the class in force, restoring RT64's defaults for what follows.
    // `before_projection_load` is set when the game's own projection load
    // comes next, in which case the group alone is enough for it.
    void leave_class(bool before_projection_load) {
        switch (cls) {
            case Class::Stretch:
                if (before_projection_load) projection_group(G_EX_ASPECT_AUTO);
                else projection_aspect(G_EX_ASPECT_AUTO);
                break;
            case Class::Left:
            case Class::Right:
                align_viewport(G_EX_ORIGIN_NONE, 0);
                break;
            default:
                break;
        }
        cls = Class::Auto;
    }

    void set_class(Class next) {
        if (next == cls) return;
        leave_class(false);
        const int inset_q = static_cast<int>(inset * 4.0f);
        switch (next) {
            case Class::Stretch:
                projection_aspect(G_EX_ASPECT_STRETCH);
                break;
            case Class::Left:
                align_viewport(G_EX_ORIGIN_LEFT, origin_cancel(G_EX_ORIGIN_LEFT) + inset_q);
                break;
            case Class::Right:
                align_viewport(G_EX_ORIGIN_RIGHT, origin_cancel(G_EX_ORIGIN_RIGHT) - inset_q);
                break;
            default:
                break;
        }
        cls = next;
        ++class_changes;
    }

    // ---- the sky --------------------------------------------------------
    //
    // The sky is the one thing in a race frame that RT64 cannot smooth on its
    // own, and the reason is worth stating: it does not move.
    //
    // Wave Race 64 puts the camera on the projection stack and draws the
    // world under an identity model matrix, so the world is smooth because
    // the camera's matrix is interpolated. The sky is drawn the same way --
    // three seven-vertex bands, the haze, the horizon and the clouds, under
    // that same identity matrix -- but it is not part of the world: the game
    // rebuilds its vertices every frame into a scratch buffer reached through
    // segment 6, following the camera and scrolling the clouds. So the
    // matrix RT64 interpolates is identical from one frame to the next, RT64
    // pairs it perfectly, computes no motion, and holds the sky still for
    // every frame it generates in between. The sky then jumps once per game
    // frame while everything around it moves smoothly, which is the shimmer
    // along the horizon at 60 frames per second and worse above it.
    //
    // The fix is to tell RT64 to interpolate the vertices themselves, which
    // it will do -- it takes the per-vertex difference from the previous
    // frame and carries it as a velocity -- but not by default: a transform
    // group's vertex and texture-coordinate components are G_EX_COMPONENT_SKIP
    // unless something asks for them, because most geometry that changes its
    // vertices between frames is geometry that has been replaced rather than
    // moved. The sky is the opposite case: the same seven points, in the same
    // order, a little further along.
    //
    // The section is opened at the frame's first perspective projection, and
    // closed at the first world matrix the top-level list loads for itself --
    // the sky's own two matrix loads are inside the game's static lists, so
    // the top level's first one is the course, and it must be created under
    // RT64's defaults. Segment 6 having a base at all is what says the sky is
    // about to be drawn. If any of that does not hold, the section simply
    // never opens and RT64 behaves as it did.
    //
    // WR64_NO_SKY_INTERP=1 switches it off, for comparison.

    static constexpr uint32_t kSkyId = 0x57A00001u;
    static constexpr uint32_t kWaterId = 0x57A00002u;

    enum class SkySection { Before, Open, Done };
    SkySection sky = SkySection::Before;
    bool sky_interp = true;

    // The group both the sky and the water are drawn under, and the one that
    // puts RT64's defaults back afterwards. Only the vertex and
    // texture-coordinate components differ from those defaults; every other
    // field is passed as RT64 would have set it, so that closing a section
    // restores the renderer's own behaviour exactly.
    //
    // G_EX_COMPONENT_INTERPOLATE rather than G_EX_COMPONENT_AUTO for the
    // texture coordinates: automatic means "only when the positions did not
    // change", which is the pure texture scroll of a waterfall, and both of
    // these move their positions.
    //
    // Each section is given an explicit transform id rather than G_EX_ID_AUTO,
    // with linear ordering, and that is not decoration. Both the sky and the
    // water are drawn under the game's identity matrix, and identical matrices
    // are exactly what ties a matcher that pairs by position: RT64 was
    // measured leaving them unpaired in some frames. A transform that finds no
    // pair never reaches matchTransform, and it is matchTransform that
    // computes the per-vertex velocities both sections depend on, so a tie lost
    // meant the sky quietly falling back to the game's rate for a frame. An
    // explicit id is matched first and by identity, before any of that
    // (GameFrame::match), and several transforms sharing one id pair up in the
    // order they were submitted, which is what the sky's two want.
    //
    // The values only have to be distinct from each other and from RT64's
    // G_EX_ID_IGNORE (0) and G_EX_ID_AUTO (~0); nothing else in this port
    // gives a transform an id.
    void vertex_interp_group(uint32_t id) {
        const bool on = id != G_EX_ID_AUTO;
        const uint32_t v = on ? G_EX_COMPONENT_INTERPOLATE : G_EX_COMPONENT_SKIP;
        const uint32_t order = on ? G_EX_ORDER_LINEAR : G_EX_ORDER_AUTO;
        if (GfxCommand* cmd = reserve(2)) {
            gEXMatrixGroup(cmd, id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_NOPUSH, 0,
                           G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO,
                           G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, v,
                           G_EX_COMPONENT_AUTO, order, G_EX_EDIT_NONE,
                           G_EX_ASPECT_AUTO, v, G_EX_COMPONENT_AUTO);
        }
    }

    // Called for every matrix command the top-level list carries, after the
    // walk has taken account of it and before the command itself is written
    // out. A projection multiply -- the camera, which follows the projection
    // load -- is neither end of the section and is passed over.
    void sky_matrix(uint32_t params) {
        if (!sky_interp) return;
        if (params & kMtxProjection) {
            if ((params & kMtxLoad) && sky == SkySection::Before &&
                projection_is_perspective && segments[6] != 0) {
                vertex_interp_group(kSkyId);
                sky = SkySection::Open;
            }
            return;
        }
        sky_close();
    }

    void sky_close() {
        if (sky == SkySection::Open) {
            vertex_interp_group(G_EX_ID_AUTO);
            sky = SkySection::Done;
        }
    }

    // ---- the water ------------------------------------------------------
    //
    // The waves are the sky's problem again, in the place where it is most
    // worth solving. The water surface is a lattice of rows marching away
    // from the camera, fifty vertex blocks reached through segment 3 under
    // the same identity matrix the sky uses, and the game recomputes it every
    // frame: measured across four consecutive race frames, forty-one of the
    // fifty blocks carry different vertex data each time while the course and
    // the models (segments 1, 8 and 13) are byte-for-byte identical. So the
    // waves moved at the game's twenty or thirty frames a second under a
    // camera gliding at sixty.
    //
    // RT64 takes the per-vertex difference by index, guarded only by the
    // block's vertex count being unchanged, so a mesh whose vertices mean
    // something different from one frame to the next translates rather than
    // animates. This was believed to be safe here: each block's vertex count
    // is the same every frame (13, 14, 15, then 16 for the rest), and across
    // the four consecutive frames that were measured every block's first
    // vertex was bit-for-bit identical.
    //
    // Those four frames were taken with the camera at rest, which is the one
    // condition under which a mesh built around the camera looks fixed.
    // Measured with flush_lattice below over 2,172 race frames: the lattice
    // moves in 74% of them, and in 100% of those every one of the fifty blocks
    // moves by the identical step.
    //
    // So the correspondence is not what is wrong. The lattice is carried
    // whole -- index i is the same slot, and it carries the same detail: a
    // 60-unit carry changes about as many heights as standing still does
    // (15.1 against 12.2 of 50), so the wave pattern travels with it rather
    // than being resampled from a world-fixed field.
    //
    // What is wrong is the positions. The step is a multiple of 32 world units
    // -- 64, 32, 96, 128 -- while the camera moves a few units a frame: these
    // are the camera's coordinates rounded, not where the surface is.
    // Interpolating between two of them surges the whole surface 64 units
    // across the frames in between, in the 31% of race frames that carry. The
    // heights and texture coordinates are worth interpolating and the
    // positions are not, which means pairing against the previous surface
    // sampled at each current world XZ -- a change inside the renderer, not
    // here. The group below is left as it is, and WR64_NO_WATER_INTERP=1
    // switches it off for the comparison.
    //
    // The sky is the opposite case and the group there is sound: three bands
    // moving independently (all three share a step in 5% of frames), each a
    // coherent object whose index i keeps its meaning.
    //
    // The whole surface is drawn from one of the game's static lists, so the
    // group can be put around the call rather than threaded through the list:
    // RT64 does not create a world transform when a matrix is loaded but when
    // the first vertex after it is, so a group set before the call is the one
    // the water's transform is created under, and a group restoring the
    // defaults after the call leaves everything else as it was.
    //
    // WR64_NO_WATER_INTERP=1 switches it off, for comparison.

    bool water_interp = true;

    // The segment a called list draws its first vertices from, or 0 for a
    // list that draws none. The game's drawing lists are static and are
    // called by the same segmented address every frame, so each is scanned
    // once and answered from the table afterwards.
    //
    // The answer holds only for the segment table that produced it. A
    // segmented address says which segment a list lives in, not where that
    // segment is: the game remaps them between courses and between the
    // screens that are not races, so the same address can name a different
    // list later in the run. The physical address the current table resolves
    // it to is therefore part of the key, and a remap simply misses the cache
    // and rescans rather than answering from a list that is no longer there.
    uint32_t first_vertex_segment(uint32_t segmented) {
        static std::unordered_map<uint64_t, uint32_t> cache;
        const uint64_t key = (uint64_t(segmented) << 32) | physical(segmented);
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        const uint32_t found = scan_first_vertex_segment(physical(segmented), 0);
        cache.emplace(key, found);
        return found;
    }

    uint32_t scan_first_vertex_segment(uint32_t physical_addr, int depth) const {
        uint32_t cursor = physical_addr;
        for (uint32_t steps = 0; steps < 512; ++steps) {
            const uint32_t* c = words(cursor);
            const uint32_t w0 = c[0];
            const uint32_t w1 = c[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            cursor += 8;
            if (op == kOpEndDisplayList) return 0;
            if (op == kOpVtx) return (w1 >> 24) & 0xF;
            if (op == kOpDisplayList) {
                const bool branch = ((w0 >> 16) & 0xFF) != 0;
                if (branch) { cursor = physical(w1); continue; }
                if (depth < 3) {
                    const uint32_t found = scan_first_vertex_segment(physical(w1), depth + 1);
                    if (found != 0) return found;
                }
            }
        }
        return 0;
    }

    // Whether a call about to be written out draws the water, and so should
    // be wrapped. Segment 3 is where the game builds the frame's own
    // geometry; in a race frame the whole of it is the water surface.
    static constexpr uint32_t kWaterSegment = 3;

    bool draws_water(uint32_t segmented_list) {
        return projection_is_perspective &&
               first_vertex_segment(segmented_list) == kWaterSegment;
    }

    // Which of the segment-3 lists the modern water renderer may take over.
    //
    // draws_water() above answers "is this the water surface" well enough to
    // hang an interpolation group off, because anything else in segment 3 that
    // it catches is harmless to interpolate. Handing a list to the water
    // renderer is a stronger claim, and a wrong one would shade craft spray or
    // some other segment-3 geometry as though it were the sea. These four are
    // the lists USA Rev A's Draw_WaterEffects selects for the full grid, the
    // opening and rider-select scene, and the two split-screen views.
    bool known_water_material(uint32_t segmented_list) const {
        return segmented_list == 0x010082F0 || segmented_list == 0x0100B590 ||
               segmented_list == 0x0100D258 || segmented_list == 0x0100E680;
    }

    // The frame's water material, as an extended GBI command in the display
    // list itself. RT64 has no callback into the port here: rt64_gbi_extended
    // reads this command and attaches the payload to the draw that follows, so
    // the material has to travel in-band, immediately before the call that
    // draws the surface, and be cleared immediately after it.
    //
    // An identity of zero means the player has Water set to Original, and the
    // cleared command is what tells RT64 to draw the surface exactly as the
    // game asked. That is why this is emitted at all in that case rather than
    // skipped: leaving a stale material attached would be worse than sending
    // an empty one.
    void water_material(bool enabled) {
        const uint32_t op = (RT64_EXTENDED_OPCODE << 24) | G_EX_WATER_MATERIAL_V1;
        const auto m = wr64::water::material(0);
        if (!enabled || m.identity.x == 0) {
            emit(op, 0);
            return;
        }
        if (GfxCommand* commands = reserve(1 + sizeof(m) / 8)) {
            auto* data = reinterpret_cast<uint32_t*>(commands);
            data[0] = op;
            data[1] = 2;  // payload v2: v1 stopped before the effect preferences
            std::memcpy(data + 2, &m, sizeof(m));
        }
    }

    // ---- diagnostic: does index i mean the same point twice? ------------
    //
    // Both interpolated meshes rest on it, and the evidence for both was four
    // consecutive frames in which every block's first vertex was bit-for-bit
    // identical. Four frames is an eighth of a second, and a mesh the game
    // builds around the camera looks exactly like a fixed one until the camera
    // moves far enough -- which is what the water turned out to be.
    //
    // So measure it over a run, from the game rather than from the screen.
    // WR64_LATTICE names a file. Every vertex block a frame loads through the
    // water's segment (3) or the sky's (6) is recorded with its first vertex,
    // and each frame is compared with the last: how many blocks moved in X or
    // Z, how many moved only in height, and the largest step in each. Drive in
    // a straight line and read the xz column. Zero while moving is a fixed
    // lattice, which per-index pairing needs; anything else is the mesh
    // following the camera, and what the renderer interpolates then is the
    // mesh sliding rather than its surface moving.
    //
    // WR64_LATTICE_FRAMES bounds the run (600 frames by default).
    // WR64_WATER_LATTICE is still accepted as the name this started under.

    struct LatticeBlock {
        uint8_t segment;
        int16_t x, y, z;
        uint32_t count;
    };

    static constexpr uint32_t kSkySegment = 6;

    // Every vertex block the frame loads through a watched segment, in the
    // order the frame loads them. Filled during the walk, compared once at the
    // end of it: the sky is three blocks in one list, the water is fifty in
    // another, and a two-player frame draws two waters.
    std::vector<LatticeBlock> lattice_blocks;
    bool lattice_trace = false;

    // One vertex load, wherever it was found: inline in the frame's list or
    // inside a list it calls.
    void note_vertex(uint32_t w0, uint32_t w1) {
        const uint32_t segment = (w1 >> 24) & 0xF;
        if (segment != kWaterSegment && segment != kSkySegment) return;
        if (lattice_blocks.size() >= 512) return;
        const uint32_t addr = physical(w1);
        lattice_blocks.push_back({ static_cast<uint8_t>(segment), signed_half(addr),
                                   signed_half(addr + 2), signed_half(addr + 4),
                                   (w0 >> 9) & 0x7Fu });
    }

    void collect_lattice(uint32_t physical_addr, int depth) {
        uint32_t cursor = physical_addr;
        for (uint32_t steps = 0; steps < 4096 && lattice_blocks.size() < 512; ++steps) {
            const uint32_t* c = words(cursor);
            const uint32_t w0 = c[0];
            const uint32_t w1 = c[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            cursor += 8;
            if (op == kOpEndDisplayList) return;
            if (op == kOpVtx) {
                note_vertex(w0, w1);
                continue;
            }
            if (op == kOpDisplayList) {
                const bool branch = ((w0 >> 16) & 0xFF) != 0;
                if (branch) { cursor = physical(w1); continue; }
                if (depth < 3) collect_lattice(physical(w1), depth + 1);
            }
        }
    }

    // One line per watched segment per frame, at the end of the walk.
    void flush_lattice(uint32_t state) {
        static const char* path = [] {
            const char* p = std::getenv("WR64_LATTICE");
            return p != nullptr ? p : std::getenv("WR64_WATER_LATTICE");
        }();
        if (path == nullptr) return;
        static const char* frames_env = [] {
            const char* p = std::getenv("WR64_LATTICE_FRAMES");
            return p != nullptr ? p : std::getenv("WR64_WATER_LATTICE_FRAMES");
        }();
        static const int wanted = frames_env != nullptr ? std::atoi(frames_env) : 600;
        static int written = 0;
        static std::FILE* f = nullptr;
        static std::vector<LatticeBlock> previous[16];
        if (written >= wanted) return;
        if (lattice_blocks.empty()) return;
        if (f == nullptr) {
            f = std::fopen(path, "w");
            if (f == nullptr) {
                written = wanted;
                std::fprintf(stderr, "[wr64] lattice trace: cannot write %s\n", path);
                std::fflush(stderr);
                return;
            }
            std::fprintf(f, "# One line per watched segment per frame: 3 is the water, 6 the sky.\n"
                            "# blocks: vertex blocks loaded through it. xz: blocks whose first vertex\n"
                            "# moved in X or Z since that segment's previous frame -- zero is a mesh\n"
                            "# standing still, which is what per-index interpolation needs. y: blocks\n"
                            "# whose height moved. dx/dz: the largest step in each, in world units.\n"
                            "# same: blocks that moved by the same step as the first -- all of them\n"
                            "# is a mesh being carried, which keeps index i meaning what it meant;\n"
                            "# fewer is a lattice re-assigning its slots, which does not.\n"
                            "# v0: the first block's first vertex.\n"
                            "# frame seg state blocks xz y dx dz same v0\n");
            std::fprintf(stderr, "[wr64] lattice trace: watching, into %s\n", path);
            std::fflush(stderr);
        }

        ++written;
        for (const uint32_t segment : { kWaterSegment, kSkySegment }) {
            std::vector<LatticeBlock> current;
            for (const LatticeBlock& b : lattice_blocks) {
                if (b.segment == segment) current.push_back(b);
            }
            if (current.empty()) continue;
            std::vector<LatticeBlock>& last = previous[segment];
            if (last.size() == current.size()) {
                int moved_xz = 0, moved_y = 0, max_dx = 0, max_dz = 0, counts_changed = 0;
                // Whether every block moved by the same step. A mesh the game
                // translates keeps index i meaning what it meant -- the whole
                // thing has been carried, and interpolating index to index
                // smooths the carry. A lattice that recentres re-assigns its
                // slots instead, and then the blocks move by different amounts.
                // This is the column that separates the two.
                const int dx0 = current[0].x - last[0].x;
                const int dz0 = current[0].z - last[0].z;
                int same_step = 0;
                for (size_t i = 0; i < current.size(); ++i) {
                    const int dx = current[i].x - last[i].x;
                    const int dz = current[i].z - last[i].z;
                    if (dx != 0 || dz != 0) ++moved_xz;
                    if (dx == dx0 && dz == dz0) ++same_step;
                    if (current[i].y != last[i].y) ++moved_y;
                    if (current[i].count != last[i].count) ++counts_changed;
                    max_dx = std::max(max_dx, std::abs(dx));
                    max_dz = std::max(max_dz, std::abs(dz));
                }
                std::fprintf(f, "%5d %3u 0x%02X %6zu %4d %4d %5d %5d %5d (%d,%d,%d)%s\n",
                             written, segment, state, current.size(), moved_xz, moved_y,
                             max_dx, max_dz, same_step, current[0].x, current[0].y, current[0].z,
                             counts_changed != 0 ? "  vertex counts changed" : "");
            }
            else {
                std::fprintf(f, "%5d %3u 0x%02X %6zu    -    -     -     -     - (%d,%d,%d)"
                                "  block count changed from %zu\n",
                             written, segment, state, current.size(),
                             current[0].x, current[0].y, current[0].z, last.size());
            }
            last = std::move(current);
        }
        std::fflush(f);
        if (written >= wanted) {
            std::fclose(f);
            f = nullptr;
            std::fprintf(stderr, "[wr64] lattice trace: %d frames written to %s\n", written, path);
            std::fflush(stderr);
        }
    }

    // ---- rectangles -----------------------------------------------------
    //
    // The race HUD is drawn as texture rectangles, which RT64 anchors through
    // a rect-alignment state that applies to every plain rectangle that
    // follows: no command has to be reissued, and nothing is interpolated.
    // The offsets undo RT64's own displacement for the origin, as for
    // viewports, and add the inset.

    Class rect_cls = Class::Auto;

    // A run: consecutive rectangles on one row, close together, which are one
    // element -- the digits of a time, the markers of a row -- and must be
    // classified as one, or a number is torn at the boundary of a zone. The
    // rectangles and the texture-setup commands between them are held back
    // until the run ends, then classified and written out together.
    std::vector<std::pair<uint32_t, uint32_t>> pending;
    std::vector<uint32_t> pending_textures;
    Extent pending_extent;
    bool pending_active = false;
    // Whether the run just classified took its class from the tag table, and so
    // has to hand the rect state back when it is done. See flush_run.
    bool tagged_run = false;

    static bool run_command(uint8_t op) {
        switch (op) {
            case kOpTexRect: case kOpTexRectFlip: case kOpFillRect:
            case 0xFD: case 0xF5: case 0xF2: case 0xF3: // texture image, tile, tile size, load block
            case 0xF4: case 0xE6: case 0xE7: case 0xE8: // load tile, syncs
            case 0xE9: case 0xFA: case 0xFB: case 0xFC: // sync, colours, combine
            case 0xB3: case 0xB2: case 0xB4:            // RDP halves
            case 0xB9: case 0xBA:                       // other modes
            case 0xF7:                                  // fill colour
                return true;
            default:
                return false;
        }
    }

    static bool chains(const Extent& run, const Extent& r) {
        // Same row (the vertical extents overlap) and no more than 12 pixels apart.
        const float gap = 12.0f / 160.0f;
        const bool overlap_y = r.min_y <= run.max_y && r.max_y >= run.min_y;
        const bool near_x = r.min_x <= run.max_x + gap && r.max_x >= run.min_x - gap;
        return overlap_y && near_x;
    }

    void push_rect(uint32_t w0, uint32_t w1) {
        const Extent r = rect_extent(w0, w1);
        if (pending_active && !chains(pending_extent, r)) {
            flush_run();
        }
        if (!pending_active) {
            pending_active = true;
            pending_extent = r;
            pending_textures.clear();
            pending_textures.push_back(texture);
        }
        else {
            pending_extent.add(r);
            if (std::find(pending_textures.begin(), pending_textures.end(), texture) == pending_textures.end()) {
                pending_textures.push_back(texture);
            }
        }
        pending.emplace_back(w0, w1);
    }

    void flush_run() {
        const Class before = rect_cls;
        if (pending_active) {
            classify_rect(pending_extent, 0, pending_textures);
        }
        for (const auto& [w0, w1] : pending) {
            emit(w0, w1);
        }
        // A tagged element gets the state put back immediately after it, rather
        // than whenever the next classified run happens to come along.
        //
        // The rect states -- alignment and aspect -- apply to every rectangle
        // that follows, and the runs collected here are only the rectangles the
        // *top level* issues. A called list's rectangles are emitted by the
        // renderer running that list, and never pass through this classifier, so
        // they inherit whatever state was left set. That is harmless while the
        // state only ever changes for elements the top level draws in sequence,
        // and it is not harmless for a tag: tagging the opening's sun glare as
        // stretched left every rectangle drawn after it stretched too, including
        // the Wave Race logo, which is drawn from a called list and never
        // reclassified.
        if (tagged_run && rect_cls != before) {
            set_rect_class(before);
        }
        tagged_run = false;
        pending.clear();
        pending_active = false;
    }

    // The game's scissor is the 4:3 frame, and RT64 keeps it there for the
    // 2D pass, so an anchored rectangle would be cut off at the frame's old
    // edge. While rectangles are anchored, the scissor's left edge is anchored
    // to the frame's left edge and its right edge to the right edge, and the
    // game's own scissor command is reissued so that it takes effect.
    void widen_scissor(bool widen) {
        if (widen == scissor_widened || !have_scissor_cmd) return;
        scissor_widened = widen;
        if (widen) {
            // A scissor spanning the whole widened frame, with the game's own
            // vertical bounds and mode: its left edge anchored to the frame's
            // left edge and its right edge to the right, so an anchored
            // element is not cut off at the 4:3 boundary.
            //
            // This is a scissor command, not an alignment. The first attempt
            // used gEXSetScissorAlign, which is persistent state that applies
            // to every plain scissor the game sets afterwards -- including the
            // ones its 3D pass sets on the next frame. That is what put black
            // bars around races and stopped the pause screen drawing.
            // The coordinates are given relative to the origin each edge is
            // anchored to, because RT64 displaces them by the origin when it
            // stores the scissor: the left edge by nothing, the right edge by
            // the framebuffer's width. Passing the game's own numbers less
            // that displacement leaves the stored rectangle exactly as the
            // game set it, so every test RT64 makes against it -- including
            // whether the 3D pass covers the frame, and so whether it is
            // widened at all -- sees no change. Only the origins differ, and
            // those are what spread the scissor across the widened frame when
            // it is drawn. Giving the widened rectangle directly instead cost
            // the 3D pass its widescreen and put black bars beside the race.
            const uint8_t mode = static_cast<uint8_t>((scissor_w1 >> 24) & 3);
            const int ulx = static_cast<int>((scissor_w0 >> 12) & 0xFFF) >> 2;
            const int uly = static_cast<int>(scissor_w0 & 0xFFF) >> 2;
            const int lrx = static_cast<int>((scissor_w1 >> 12) & 0xFFF) >> 2;
            const int lry = static_cast<int>(scissor_w1 & 0xFFF) >> 2;
            if (GfxCommand* cmd = reserve(2)) {
                gEXSetScissor(cmd, mode, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT,
                              ulx, uly, lrx - kFramebufferWidth, lry);
            }
        }
        else {
            // Restored by the game's own command, which carries no origins.
            emit(scissor_w0, scissor_w1);
        }
    }

    void set_rect_class(Class next, const Extent& e = Extent{}) {
        if (next == rect_cls) return;
        (void)e;
        // Spill is here for the scissor and nothing else: it leaves the
        // element's alignment and aspect at their defaults below, and only stops
        // the game's 4:3 scissor cutting it off.
        widen_scissor(next == Class::Left || next == Class::Right ||
                      next == Class::Spill);
        if (rect_cls == Class::Stretch) {
            if (GfxCommand* cmd = reserve(1)) gEXSetRectAspect(cmd, G_EX_ASPECT_AUTO);
        }
        const int inset_q = static_cast<int>(inset * 4.0f);
        switch (next) {
            case Class::Left:
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_LEFT, inset_q, 0, inset_q, 0);
                }
                break;
            case Class::Right: {
                const int off = origin_cancel(G_EX_ORIGIN_RIGHT) - inset_q;
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_RIGHT, G_EX_ORIGIN_RIGHT, off, 0, off, 0);
                }
                break;
            }
            case Class::Stretch:
                // Widen the element by however much the frame was widened,
                // about its centre. Pinning its own edges to the frame's
                // edges instead was tried and reverted: it made no difference
                // to the MAX POWER banner, whose last letter is clipped by
                // the game's own rectangle rather than by the frame, and it
                // undid the select screens' backgrounds, which this gets
                // right.
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
                }
                if (GfxCommand* cmd = reserve(1)) gEXSetRectAspect(cmd, G_EX_ASPECT_STRETCH);
                break;
            default:
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
                }
                break;
        }
        rect_cls = next;
        ++class_changes;
    }

    // A rectangle's extent in the same clip units as a vertex draw's, so the
    // one classifier serves both. Coordinates arrive in 10.2 fixed point.
    static Extent rect_extent(uint32_t w0, uint32_t w1) {
        const float ulx = float((w1 >> 12) & 0xFFF) / 4.0f;
        const float uly = float(w1 & 0xFFF) / 4.0f;
        const float lrx = float((w0 >> 12) & 0xFFF) / 4.0f;
        const float lry = float(w0 & 0xFFF) / 4.0f;
        Extent e;
        e.min_x = ulx / 160.0f - 1.0f;
        e.max_x = lrx / 160.0f - 1.0f;
        e.min_y = 1.0f - lry / 120.0f;
        e.max_y = 1.0f - uly / 120.0f;
        return e;
    }

    void classify_rect(const Extent& e, uint32_t dl_segmented, const std::vector<uint32_t>& textures) {
        ++draws_2d;
        const std::string tex_id = hex_identity("tex", textures.empty() ? texture : textures.front());
        const std::string dl_id = dl_segmented != 0 ? hex_identity("dl", dl_segmented) : std::string{};
        Class next = classify(e, tex_id, dl_id);
        Class ignored;
        tagged_run = tags != nullptr &&
                     (tags->lookup(tex_id, ignored) ||
                      (!dl_id.empty() && tags->lookup(dl_id, ignored)));
        // A tag on any texture of the run applies to the run.
        for (uint32_t t : textures) {
            Class tagged;
            if (tags != nullptr && tags->lookup(hex_identity("tex", t), tagged)) {
                next = (tagged == Class::Stretch || anchors) ? tagged : Class::Auto;
                tagged_run = true;
                break;
            }
        }
        if (trace != nullptr) {
            char line[200];
            std::snprintf(line, sizeof(line),
                          "[hud] state 0x%02X rect %s %s px x[%.0f..%.0f] y[%.0f..%.0f] under %s -> %s" "\n",
                          state, tex_id.c_str(), dl_id.empty() ? "dl:-" : dl_id.c_str(),
                          (e.min_x + 1.0f) * 160.0f, (e.max_x + 1.0f) * 160.0f,
                          (1.0f - e.max_y) * 120.0f, (1.0f - e.min_y) * 120.0f,
                          projection_is_perspective ? "persp" : "ortho", class_name(next));
            trace->append(line);
        }
        // Every texture of the run, not just the run's identity: a run is a
        // set of rectangles that were classified together, and which
        // rectangles chain into one run changes as the elements move, so an
        // element can be stretched one frame and centred the next without the
        // run it was in ever being reported as having changed class.
        if (textures.empty()) {
            report_flip(tex_id, next);
        }
        else {
            for (uint32_t t : textures) report_flip(hex_identity("tex", t), next);
        }
        report_flip(dl_id, next);
        wr64::inspector::note_element(tex_id.c_str(), dl_id.c_str(),
                                      (e.min_x + 1.0f) * 160.0f, (e.max_x + 1.0f) * 160.0f,
                                      (1.0f - e.max_y) * 120.0f, (1.0f - e.min_y) * 120.0f,
                                      projection_is_perspective, static_cast<int>(next), true);
        if (!noemit) set_rect_class(next, e);
    }

    // Whether a draw spans the frame from side to side, and so should be
    // stretched across the widened one.
    //
    // The test is on width alone, against the width of the region the game
    // draws into, and asks for nine tenths of it rather than the whole thing.
    //
    // Neither looseness is arbitrary. Height is out because the pause screen's
    // dim is drawn as horizontal strips, each a slice of the height. And the
    // overlays do not sit where a full-screen overlay might be expected to: the
    // dim spans columns 32 to 336 of a 320-wide screen -- offset right, and
    // over the edge at the far end -- while the tint over a race spans 9 to
    // 310. Demanding that a draw reach both edges of the drawn region rejected
    // the dim, which was measured off a capture of the pause screen: it left
    // an undimmed band 267 window pixels wide down the left, exactly where an
    // unstretched rectangle starting at column 32 lands. Nine tenths of the
    // width takes both overlays and still refuses the widest HUD element,
    // which reaches five sixths.
    bool covers_width(const Extent& e) const {
        const float left = has_scissor() ? float(scissor_left) : 0.0f;
        const float right = has_scissor() ? float(scissor_right) : float(kFramebufferWidth);
        return (e.right_px() - e.left_px()) >= 0.9f * (right - left);
    }

    Class classify(const Extent& e, const std::string& identity, const std::string& identity2) {
        // The inspector's overrides come first, so that a class chosen in the
        // panel takes effect on the next frame and can be taken away again
        // without a restart. hud.json and the built-in table are below it.
        int chosen = 0;
        if (wr64::inspector::override_class(identity.c_str(), &chosen) ||
            (!identity2.empty() && wr64::inspector::override_class(identity2.c_str(), &chosen))) {
            switch (chosen) {
                case wr64::inspector::kLeft:    return anchors ? Class::Left : Class::Auto;
                case wr64::inspector::kRight:   return anchors ? Class::Right : Class::Auto;
                case wr64::inspector::kStretch: return Class::Stretch;
                default:                        return Class::Auto;
            }
        }
        Class tagged;
        if (tags != nullptr && (tags->lookup(identity, tagged) || tags->lookup(identity2, tagged))) {
            if (tagged == Class::Stretch || anchors) return tagged;
            return Class::Auto;
        }
        if (e.empty()) return Class::Auto;
        // Stretching is for the 2D layer's own full-screen elements, and those
        // are drawn under an orthographic projection: the tint over a race,
        // the pause screen's dim, the fades between screens, the menus'
        // backgrounds. Measured across the title, the main menu, the options
        // and name-entry screens and a race, every one of them is orthographic.
        //
        // A rectangle that covers the frame under a perspective projection is
        // a different thing -- it belongs to the 3D pass, which RT64 has
        // already drawn at the frame's full width -- and stretching it scales
        // the picture instead of widening an overlay. The intro is where that
        // shows: the game composes it from full-frame rectangles under the
        // world's own projection, present in some of its camera shots and not
        // others, so the picture jumped between its proper width and a
        // magnified one from shot to shot. Measured on the Wave Race logo in
        // the corner, which the stretch dragged towards the frame's edge: 420
        // window pixels wide in a shot without one, 524 in a shot with, at the
        // same height.
        if (covers_width(e) && !projection_is_perspective) return Class::Stretch;
        // Anchoring is for the race HUD, which is drawn under an orthographic
        // projection in a frame that drew its world first. A menu's layout is
        // 4:3 by design and stays so whatever the setting says.
        if (!anchors || projection_is_perspective || !race_test.race()) return Class::Auto;
        // A HUD element sits inside the frame. Anything reaching beyond it is
        // a world object drawn through an orthographic projection, and moving
        // it with an edge would tear it from its neighbours.
        if (e.min_x < -1.0f || e.max_x > 1.0f || e.min_y < -1.0f || e.max_y > 1.0f) return Class::Auto;
        const float c = e.center_x();
        if (c < -kAnchorThird) return Class::Left;
        if (c > kAnchorThird) return Class::Right;
        return Class::Auto;
    }

    // A 2D element that is given one class in one frame and another in the
    // next flickers between them, and that is a defect however defensible
    // each classification is on its own. Under WR64_HUD_TRACE it is reported
    // as it happens, with both classes, so the element can be named and
    // pinned in hud.json.
    void report_flip(const std::string& identity, Class next) {
        if (trace == nullptr || identity.empty()) return;
        static std::unordered_map<std::string, Class> last;
        const auto it = last.find(identity);
        if (it == last.end()) {
            last.emplace(identity, next);
            return;
        }
        if (it->second == next) return;
        std::fprintf(stderr, "[hud] flip: state 0x%02X %s %s -> %s\n", state, identity.c_str(),
                     class_name(it->second), class_name(next));
        std::fflush(stderr);
        it->second = next;
    }

    static std::string hex_identity(const char* kind, uint32_t address) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s:0x%08X", kind, address);
        return buf;
    }

    void classify_draw(const Extent& e, uint32_t dl_segmented) {
        const std::string tex_id = hex_identity("tex", texture);
        const std::string dl_id = dl_segmented != 0 ? hex_identity("dl", dl_segmented) : std::string{};
        const Class next = classify(e, tex_id, dl_id);
        if (trace != nullptr) {
            char line[200];
            std::snprintf(line, sizeof(line),
                          "[hud] state 0x%02X %s %s clip x[%.2f..%.2f] y[%.2f..%.2f] -> %s\n",
                          state, tex_id.c_str(), dl_id.empty() ? "dl:-" : dl_id.c_str(),
                          e.empty() ? 0.0f : e.min_x, e.empty() ? 0.0f : e.max_x,
                          e.empty() ? 0.0f : e.min_y, e.empty() ? 0.0f : e.max_y,
                          class_name(next));
            trace->append(line);
        }
        wr64::inspector::note_element(tex_id.c_str(), dl_id.c_str(),
                                      e.empty() ? 0.0f : (e.min_x + 1.0f) * 160.0f,
                                      e.empty() ? 0.0f : (e.max_x + 1.0f) * 160.0f,
                                      e.empty() ? 0.0f : (1.0f - e.max_y) * 120.0f,
                                      e.empty() ? 0.0f : (1.0f - e.min_y) * 120.0f,
                                      projection_is_perspective, static_cast<int>(next), false);
        report_flip(dl_id.empty() ? tex_id : dl_id, next);
        if (!noemit) set_class(next);
    }

    // A read-only scan of a called list for its extent and the texture it
    // sets, so a HUD element drawn from a static list can be classified. The
    // modelview stack is copied, since the list may push and pop its own.
    Extent scan(uint32_t physical_addr, int depth, Mat4 mv[16], int& mv_depth, Extent& rects) {
        Extent e;
        uint32_t cursor = physical_addr;
        for (uint32_t steps = 0; steps < 4096; ++steps) {
            const uint32_t* c = words(cursor);
            const uint32_t w0 = c[0];
            const uint32_t w1 = c[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            cursor += 8;
            if (op == kOpEndDisplayList) break;
            if (op == kOpDisplayList) {
                const bool branch = ((w0 >> 16) & 0xFF) != 0;
                if (branch) { cursor = physical(w1); continue; }
                if (depth < 4) e.add(scan(physical(w1), depth + 1, mv, mv_depth, rects));
                continue;
            }
            if (op == kOpSetTextureImage) { texture = w1; continue; }
            if (is_rect(op)) { rects.add(rect_extent(w0, w1)); continue; }
            if (op == kOpPopMtx) { if (mv_depth > 0) --mv_depth; continue; }
            if (op == kOpMtx) {
                const uint32_t params = (w0 >> 16) & 0xFF;
                if (params & kMtxProjection) continue;    // rare in a called list; ignored
                const Mat4 loaded = read_matrix(physical(w1));
                if ((params & kMtxPush) && mv_depth < 15) { mv[mv_depth + 1] = mv[mv_depth]; ++mv_depth; }
                mv[mv_depth] = (params & kMtxLoad) ? loaded : loaded * mv[mv_depth];
                continue;
            }
            if (op == kOpVtx) {
                const uint32_t count = (w0 >> 9) & 0x7F;
                e.add(extent(physical(w1), count, mv[mv_depth]));
            }
        }
        return e;
    }

    // ---- state tracking -------------------------------------------------

    // Whether a matrix is affine: no perspective terms in its last column.
    static bool affine(const Mat4& m) {
        return m.m[0][3] == 0.0f && m.m[1][3] == 0.0f && m.m[2][3] == 0.0f && m.m[3][3] == 1.0f;
    }

    void on_matrix(uint32_t w0, uint32_t w1) {
        const uint32_t params = (w0 >> 16) & 0xFF;
        const Mat4 loaded = read_matrix(physical(w1));
        if (params & kMtxProjection) {
            // As RT64 keeps it: the product of everything on the projection
            // stack is what transforms vertices, but an affine matrix
            // multiplied onto the stack is the camera, not the projection, and
            // the projection's type is judged from the projection alone. The
            // game loads its perspective projection and then multiplies the
            // camera onto it; the product has a non-zero [3][3] and would read
            // as orthographic.
            if (params & kMtxLoad) {
                projection = loaded;
                projection_only = loaded;
                have_projection = true;
                projection_w0 = w0;
                projection_w1 = w1;
            }
            else {
                projection = loaded * projection;
                if (!affine(loaded)) {
                    projection_only = loaded * projection_only;
                }
            }
            projection_is_perspective = perspective(projection_only);
            if (!race_test.first_projection_seen) {
                race_test.first_projection_seen = true;
                race_test.world_first = projection_is_perspective;
            }
            return;
        }
        if ((params & kMtxPush) && modelview_depth < 15) {
            modelview[modelview_depth + 1] = modelview[modelview_depth];
            ++modelview_depth;
        }
        modelview[modelview_depth] = (params & kMtxLoad) ? loaded : loaded * modelview[modelview_depth];
    }

    void on_scissor(uint32_t w0, uint32_t w1) {
        const int ulx = static_cast<int>((w0 >> 12) & 0xFFF) >> 2;
        const int uly = static_cast<int>(w0 & 0xFFF) >> 2;
        const int lrx = static_cast<int>((w1 >> 12) & 0xFFF) >> 2;
        const int lry = static_cast<int>(w1 & 0xFFF) >> 2;
        if (ulx == 0 && uly == 0 && lrx >= 319 && lry >= 239) return;
        race_test.inset_scissor = true;
        scissor_left = std::min(scissor_left, ulx);
        scissor_top = std::min(scissor_top, uly);
        scissor_right = std::max(scissor_right, lrx);
        scissor_bottom = std::max(scissor_bottom, lry);
    }

    bool has_scissor() const {
        return scissor_right > scissor_left && scissor_bottom > scissor_top;
    }
};


// ---- the 3D trace ------------------------------------------------------
//
// Diagnosis only, and read-only: WR64_3D_TRACE names a file, and the first
// few race frames' drawing is written to it in full -- every matrix load,
// vertex load, call and triangle batch, with each vertex block's hash and
// its clip-space extent. Two consecutive frames diffed against each other
// say which geometry the game rebuilds from scratch every frame, which is
// the geometry RT64 has nothing to interpolate between unless it is told to
// interpolate the vertices themselves.
struct Tracer {
    uint8_t* rdram = nullptr;
    std::FILE* f = nullptr;
    uint32_t segments[16] = {};
    Mat4 projection = Mat4::identity();
    Mat4 modelview[16];
    int modelview_depth = 0;
    uint32_t texture = 0;
    uint32_t vtx_loads = 0;

    Tracer() { modelview[0] = Mat4::identity(); }

    uint32_t physical(uint32_t segmented) const {
        return (segments[(segmented >> 24) & 0xF] + (segmented & 0x00FFFFFFu)) & 0x00FFFFF8u;
    }
    const uint32_t* words(uint32_t addr) const {
        return reinterpret_cast<const uint32_t*>(rdram + (addr & 0x00FFFFFFu));
    }
    uint16_t half(uint32_t addr) const {
        return *reinterpret_cast<const uint16_t*>(rdram + ((addr & 0x00FFFFFFu) ^ 2));
    }
    int16_t signed_half(uint32_t addr) const { return static_cast<int16_t>(half(addr)); }

    Mat4 read_matrix(uint32_t addr) const {
        Mat4 r{};
        for (int i = 0; i < 16; ++i) {
            const uint32_t full = (uint32_t(half(addr + 2 * i)) << 16) | half(addr + 32 + 2 * i);
            r.m[i / 4][i % 4] = static_cast<float>(static_cast<int32_t>(full)) / 65536.0f;
        }
        return r;
    }

    // FNV-1a over a block of RDRAM, so a frame-to-frame diff says at a glance
    // whether the game rewrote it.
    uint32_t hash(uint32_t addr, uint32_t bytes) const {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < bytes; ++i) {
            h = (h ^ rdram[((addr + i) & 0x00FFFFFFu) ^ 3]) * 16777619u;
        }
        return h;
    }

    void walk(uint32_t cursor, int depth) {
        for (uint32_t steps = 0; steps < 8192; ++steps) {
            const uint32_t* c = words(cursor);
            const uint32_t w0 = c[0];
            const uint32_t w1 = c[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            cursor += 8;

            switch (op) {
                case kOpEndDisplayList:
                    std::fprintf(f, "%*send\n", depth * 2, "");
                    return;
                case kOpDisplayList: {
                    const bool branch = ((w0 >> 16) & 0xFF) != 0;
                    std::fprintf(f, "%*s%s 0x%08X (phys 0x%06X)\n", depth * 2, "",
                                 branch ? "branch" : "call", w1, physical(w1));
                    if (branch) { cursor = physical(w1); continue; }
                    if (depth < 6) {
                        const int saved = modelview_depth;
                        walk(physical(w1), depth + 1);
                        modelview_depth = saved;
                    }
                    continue;
                }
                case kOpMoveWord:
                    if ((w0 & 0xFF) == kMoveWordSegment) {
                        segments[(w0 >> 10) & 0xF] = w1 & 0x00FFFFFFu;
                        std::fprintf(f, "%*sseg %u = 0x%06X\n", depth * 2, "",
                                     (w0 >> 10) & 0xF, w1 & 0x00FFFFFFu);
                    }
                    continue;
                case kOpPopMtx:
                    if (modelview_depth > 0) --modelview_depth;
                    std::fprintf(f, "%*spop -> depth %d\n", depth * 2, "", modelview_depth);
                    continue;
                case kOpMtx: {
                    const uint32_t params = (w0 >> 16) & 0xFF;
                    const uint32_t addr = physical(w1);
                    const Mat4 loaded = read_matrix(addr);
                    if (params & kMtxProjection) {
                        projection = (params & kMtxLoad) ? loaded : loaded * projection;
                        std::fprintf(f, "%*sproj %s 0x%08X (phys 0x%06X) hash %08X"
                                        " [1][1]=%.4f [3][3]=%.4f t=(%.1f,%.1f,%.1f)\n",
                                     depth * 2, "", (params & kMtxLoad) ? "load" : "mul", w1, addr,
                                     hash(addr, 64), loaded.m[1][1], loaded.m[3][3],
                                     loaded.m[3][0], loaded.m[3][1], loaded.m[3][2]);
                        continue;
                    }
                    if ((params & kMtxPush) && modelview_depth < 15) {
                        modelview[modelview_depth + 1] = modelview[modelview_depth];
                        ++modelview_depth;
                    }
                    modelview[modelview_depth] =
                        (params & kMtxLoad) ? loaded : loaded * modelview[modelview_depth];
                    std::fprintf(f, "%*smtx %s%s 0x%08X (phys 0x%06X) hash %08X t=(%.1f,%.1f,%.1f)"
                                    " depth %d\n",
                                 depth * 2, "", (params & kMtxLoad) ? "load" : "mul",
                                 (params & kMtxPush) ? "+push" : "", w1, addr, hash(addr, 64),
                                 loaded.m[3][0], loaded.m[3][1], loaded.m[3][2], modelview_depth);
                    continue;
                }
                case kOpSetTextureImage:
                    texture = w1;
                    std::fprintf(f, "%*stimg 0x%08X\n", depth * 2, "", w1);
                    continue;
                case kOpVtx: {
                    const uint32_t count = (w0 >> 9) & 0x7F;
                    const uint32_t addr = physical(w1);
                    const Mat4 mvp = modelview[modelview_depth] * projection;
                    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
                    for (uint32_t i = 0; i < count && i < 64; ++i) {
                        const uint32_t v = addr + i * 16;
                        const float x = signed_half(v + 0);
                        const float y = signed_half(v + 2);
                        const float z = signed_half(v + 4);
                        const float cx = x * mvp.m[0][0] + y * mvp.m[1][0] + z * mvp.m[2][0] + mvp.m[3][0];
                        const float cy = x * mvp.m[0][1] + y * mvp.m[1][1] + z * mvp.m[2][1] + mvp.m[3][1];
                        const float cw = x * mvp.m[0][3] + y * mvp.m[1][3] + z * mvp.m[2][3] + mvp.m[3][3];
                        if (cw <= 0.0f) continue;
                        min_x = std::min(min_x, cx / cw); max_x = std::max(max_x, cx / cw);
                        min_y = std::min(min_y, cy / cw); max_y = std::max(max_y, cy / cw);
                    }
                    std::fprintf(f, "%*svtx #%u 0x%08X (phys 0x%06X) n=%u hash %08X tex 0x%08X"
                                    " v0=(%d,%d,%d) clip x[%.2f..%.2f] y[%.2f..%.2f]\n",
                                 depth * 2, "", vtx_loads++, w1, addr, count, hash(addr, count * 16),
                                 texture, signed_half(addr), signed_half(addr + 2),
                                 signed_half(addr + 4),
                                 max_x < min_x ? 0.0f : min_x, max_x < min_x ? 0.0f : max_x,
                                 min_y > max_y ? 0.0f : min_y, min_y > max_y ? 0.0f : max_y);
                    continue;
                }
                default:
                    continue;
            }
        }
        std::fprintf(f, "%*s(walk cut off)\n", depth * 2, "");
    }
};

// Writes one race frame's drawing to the file named by WR64_3D_TRACE, for
// the first WR64_3D_TRACE_FRAMES frames (two by default), and reports when
// it is done.
void trace_3d(uint8_t* rdram, uint32_t list_vaddr, uint32_t state) {
    static const char* path = std::getenv("WR64_3D_TRACE");
    if (path == nullptr) return;
    // Race frames by default; WR64_3D_TRACE_STATE names another game state,
    // in hexadecimal, for the screens that are not races.
    static const char* state_env = std::getenv("WR64_3D_TRACE_STATE");
    // "any" traces whatever the game is showing, which is how to reach a mode
    // whose state number is not known yet -- stunt mode, a ceremony, a screen
    // nobody has enumerated. A hexadecimal number picks one state; the default
    // is the race states.
    static const bool any_state = state_env != nullptr && std::strcmp(state_env, "any") == 0;
    static const long wanted_state =
        (state_env != nullptr && !any_state) ? std::strtol(state_env, nullptr, 16) : -1;
    if (!any_state && (wanted_state >= 0 ? (state != uint32_t(wanted_state)) : !racing(state))) {
        return;
    }

    static const char* frames_env = std::getenv("WR64_3D_TRACE_FRAMES");
    static const int wanted = frames_env != nullptr ? std::atoi(frames_env) : 2;
    // How far apart the traced frames are, in race frames. One is consecutive,
    // which is what a frame-to-frame diff of the geometry wants. A larger
    // number spreads them over the run, which is what a question about
    // distance wants -- whether an object is submitted at all from far away.
    static const char* every_env = std::getenv("WR64_3D_TRACE_EVERY");
    static const int every = every_env != nullptr ? std::max(1, std::atoi(every_env)) : 1;
    static int seen = 0;
    static int written = 0;
    static std::FILE* f = nullptr;
    if (written >= wanted) return;
    if ((seen++ % every) != 0) return;
    if (f == nullptr) {
        f = std::fopen(path, "w");
        if (f == nullptr) {
            written = wanted;
            std::fprintf(stderr, "[wr64] 3D trace: cannot write %s\n", path);
            return;
        }
    }

    ++written;
    std::fprintf(f, "==== frame %d of %d (race frame %d, state 0x%02X, list 0x%08X) ====\n",
                 written, wanted, seen - 1, state, list_vaddr);
    Tracer t{};
    t.rdram = rdram;
    t.f = f;
    t.walk(list_vaddr & 0x00FFFFFFu, 0);
    std::fflush(f);
    if (written >= wanted) {
        std::fclose(f);
        f = nullptr;
        std::fprintf(stderr, "[wr64] 3D trace: %d frames written to %s\n", written, path);
        std::fflush(stderr);
    }
}

}  // namespace

namespace wr64::dlrewrite {

uint32_t rewrite(uint8_t* rdram, uint32_t list_vaddr) {
    static const bool disabled = std::getenv("WR64_NO_REWRITE") != nullptr;
    static const bool hud_off = std::getenv("WR64_HUD_OFF") != nullptr;
    // The transition curtain, and anything else laid out in screen space under a
    // perspective projection: stretched to the frame unless this says otherwise.
    static const bool no_curtain = std::getenv("WR64_NO_CURTAIN") != nullptr;
    static const bool tracing = std::getenv("WR64_HUD_TRACE") != nullptr;
    static Tags tags;
    static uint32_t traced_state = 0xFFFFFFFFu;
    static uint32_t traced_lists = 0;
    static std::string trace_text;

    if (disabled) {
        return 0;
    }
    if (!tags.loaded) {
        tags.load();
    }

    const uint32_t state = wr64::current_game_state();
    const auto& config = ultramodern::renderer::get_graphics_config();



    // Claim this frame's water snapshot before anything is emitted. The game
    // thread published it at task submission keyed by this display list, and
    // water_material() below reads whatever this call selected. A list with no
    // matching snapshot leaves the material empty, which is the same as
    // Original: unknown tasks keep the game's own water.
    wr64::water::begin_frame(list_vaddr);

    wr64::inspector::begin_frame(state);
    trace_3d(rdram, list_vaddr, state);

    Walker w{};
    w.rdram = rdram;
    w.state = state;
    w.menu = !racing(state);
    w.no_curtain = no_curtain;
    w.hud = !hud_off;
    static const bool noemit = std::getenv("WR64_HUD_NOEMIT") != nullptr;
    w.noemit = noemit;
    // The menus' layouts are 4:3 by design and stay so whatever the setting
    // says; the setting is about the race HUD. Anchoring the race HUD is
    // whether a frame is a race is read from the frame (see RaceTest), not
    // from this state number. WR64_HUD_NO_ANCHORS switches anchoring off.
    static const float far_scale = env_scale("WR64_FAR", 1.0f);
    // WR64_FOV overrides the setting, for measuring against it.
    static const float fov_override = env_scale("WR64_FOV", 0.0f);
    w.far_scale = far_scale;
    w.fov_scale = fov_override != 0.0f ? fov_override
                                       : g_fov_scale.load(std::memory_order_relaxed);
    static const bool anchors_disabled = std::getenv("WR64_HUD_NO_ANCHORS") != nullptr;
    static const bool sky_interp_off = std::getenv("WR64_NO_SKY_INTERP") != nullptr;
    w.sky_interp = !sky_interp_off;
    static const bool water_interp_off = std::getenv("WR64_NO_WATER_INTERP") != nullptr;
    w.water_interp = !water_interp_off;
    static const bool lattice_trace = std::getenv("WR64_LATTICE") != nullptr ||
                                      std::getenv("WR64_WATER_LATTICE") != nullptr;
    w.lattice_trace = lattice_trace;
    w.anchors = !anchors_disabled &&
                config.hr_option != ultramodern::renderer::HUDRatioMode::Original;
    w.inset = wr64::display::anchor_inset();
    w.tags = &tags;
    w.out = reinterpret_cast<uint32_t*>(rdram + (kScratch & 0x00FFFFFFu));
    if (tracing) {
        if (state != traced_state) {
            traced_state = state;
            traced_lists = 0;
        }
        // Every list is traced into the buffer; the buffer is printed only for
        // the first few of a state and, after that, whenever the list draws a
        // different number of 2D elements than the one before it. Screens that
        // appear without changing the game's state -- the pause menu over a
        // race is the one that matters -- are otherwise never seen.
        trace_text.clear();
        w.trace = &trace_text;
    }

    // RT64 forgets the extended GBI at the end of every list, so every list
    // that uses it has to enable it first.
    if (GfxCommand* cmd = w.reserve(1)) {
        gEXEnable(cmd);
    }

    uint32_t cursor = list_vaddr & 0x00FFFFFFu;
    for (uint32_t steps = 0; steps < kMaxCommands && !w.overflow; ++steps) {
        const uint32_t* c = w.words(cursor);
        const uint32_t w0 = c[0];
        const uint32_t w1 = c[1];
        const uint8_t op = static_cast<uint8_t>(w0 >> 24);
        cursor += 8;

        // Rectangles are held back with the commands between them until their
        // run ends (see push_rect); anything else ends the run.
        if (w.hud && is_rect(op)) {
            w.push_rect(w0, w1);
            continue;
        }
        if (w.pending_active && Walker::run_command(op)) {
            if (op == kOpSetTextureImage) w.texture = w1;
            w.pending.emplace_back(w0, w1);
            continue;
        }
        w.flush_run();

        if (op == kOpEndDisplayList) {
            // Nothing this walk turned on may outlive the list. RT64 forgets
            // its extended state at the end of one, but the scissor is plain
            // RDP state and has to be put back by hand.
            w.set_rect_class(Class::Auto);
            w.leave_class(false);
            w.curtain(false);
            w.sky_close();
            w.emit(w0, w1);
            break;
        }

        if (op == kOpDisplayList) {
            const bool branch = ((w0 >> 16) & 0xFF) != 0;
            if (branch) {
                cursor = w.physical(w1);
                continue;
            }
            // A call under an orthographic projection is a 2D element drawn
            // from a static list: classify it by what the list draws.
            if (w.hud && w.have_projection && !w.projection_is_perspective) {
                Mat4 mv[16];
                std::memcpy(mv, w.modelview, sizeof(mv));
                int depth = w.modelview_depth;
                Extent rects;
                const Extent e = w.scan(w.physical(w1), 0, mv, depth, rects);
                if (!e.empty()) w.classify_draw(e, w1);
                if (!rects.empty()) w.classify_rect(rects, w1, {});
            }
            // Screen-space geometry drawn from a static list: a projection
            // built at an aspect of one, and a viewport inside the called list
            // that covers the whole frame. The group wraps the call, for the
            // same reason the water's does below -- what it applies to is inside.
            const bool curtain =
                w.menu && w.have_projection && w.projection_is_perspective &&
                Walker::square_aspect(w.projection_only) &&
                w.list_loads_full_frame_viewport(w.physical(w1), 0);
            if (curtain) w.curtain(true);

            // The water surface is drawn from one of the game's static lists;
            // the group goes around the call, since the transform inside it is
            // created at its first vertex rather than at its matrix load.
            if (w.lattice_trace) w.collect_lattice(w.physical(w1), 0);
            const bool water = w.draws_water(w1);
            const bool modern_water = water && w.known_water_material(w1);
            if (water && w.water_interp) w.vertex_interp_group(Walker::kWaterId);
            if (modern_water) w.water_material(true);
            w.emit(w0, w1);
            if (modern_water) w.water_material(false);
            if (water && w.water_interp) w.vertex_interp_group(G_EX_ID_AUTO);
            if (curtain) w.curtain(false);
            continue;
        }

        if (op == kOpMoveWord && (w0 & 0xFF) == kMoveWordSegment) {
            w.segments[(w0 >> 10) & 0xF] = w1 & 0x00FFFFFFu;
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpMoveMem && ((w0 >> 16) & 0xFF) == kMoveMemViewport) {
            w.have_viewport = true;
            w.viewport_w0 = w0;
            w.viewport_w1 = w1;
            if (w.trace != nullptr) {
                const uint32_t vp = w.physical(w1);
                char line[176];
                std::snprintf(line, sizeof(line),
                              "[hud] state 0x%02X viewport load 0x%08X half-width %.1f centre %.1f%s (class %s)\n",
                              state, w1, w.signed_half(vp) / 4.0f, w.signed_half(vp + 8) / 4.0f,
                              w.viewport_is_full_frame(w1) ? "" : " OFF-CENTRE", class_name(w.cls));
                w.trace->append(line);
            }
            w.on_viewport_load(w1);
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpSetScissor) {
            w.on_scissor(w0, w1);
            w.have_scissor_cmd = true;
            w.scissor_w0 = w0;
            w.scissor_w1 = w1;
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpSetTextureImage) {
            w.texture = w1;
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpPopMtx) {
            if (w.modelview_depth > 0) --w.modelview_depth;
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpMtx) {
            const uint32_t params = (w0 >> 16) & 0xFF;
            const bool projection_load = (params & kMtxProjection) && (params & kMtxLoad);

            // A 2D class must not outlive the projection it was set for.
            if (projection_load && w.cls != Class::Auto) {
                w.leave_class(true);
            }
            if (projection_load && w.rect_cls != Class::Auto) {
                w.set_rect_class(Class::Auto);
            }

            w.on_matrix(w0, w1);
            if (projection_load && w.trace != nullptr) {
                // Both halves of the screen-space test, side by side. A section
                // needs a square aspect and the whole frame, and the two arrive
                // as separate commands: without seeing which half is missing,
                // "it did not fire" is indistinguishable from "it fired and did
                // nothing", which cost three rounds of guessing.
                char line[256];
                std::snprintf(line, sizeof(line), "[hud] state 0x%02X projection load %s"
                              " [0][0]=%.3f [1][1]=%.3f [3][3]=%.3f square %s, viewport %s%s"
                              " -> screen-space %s (class %s)\n",
                              state, w.projection_is_perspective ? "perspective" : "orthographic",
                              w.projection_only.m[0][0], w.projection_only.m[1][1],
                              w.projection.m[3][3],
                              Walker::square_aspect(w.projection_only) ? "YES" : "no",
                              w.have_viewport ? "seen" : "NOT YET SEEN",
                              w.have_viewport
                                  ? (w.viewport_is_full_frame(w.viewport_w1) ? " full-frame"
                                                                             : " SUB-VIEWPORT")
                                  : "",
                              w.screen_space_section() ? "YES" : "no", class_name(w.cls));
                w.trace->append(line);
            }

            if (projection_load && w.menu) {
                const bool persp = w.projection_is_perspective;

                if (persp && !w.centred && w.seen_ortho) {
                    w.align_viewport(G_EX_ORIGIN_CENTER, origin_cancel(G_EX_ORIGIN_CENTER));
                    w.centred = true;
                    ++w.centred_sections;
                }
                else if (!persp && w.centred) {
                    w.align_viewport(G_EX_ORIGIN_NONE, 0);
                    w.centred = false;
                    w.centred_by_viewport = false;
                }
            }
            if (projection_load && !w.projection_is_perspective) {
                w.seen_ortho = true;
            }
            w.sky_matrix(params);

            // Draw distance and field of view, applied to the world's own
            // frustum. Only in a race frame, and only to a perspective
            // projection: a menu lays its 2D out under a perspective projection
            // too (see GAME-INTERNALS), and widening that would spread the
            // layout rather than the view.
            if (projection_load && w.projection_is_perspective && w.race_test.race()) {
                const uint32_t rewritten = w.rewrite_projection(w.projection_only);
                if (rewritten != 0) {
                    w.projection_w1 = rewritten;
                    w.emit(w0, rewritten);
                    continue;
                }
            }
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpVtx && w.hud && w.have_projection && !w.projection_is_perspective) {
            const uint32_t count = (w0 >> 9) & 0x7F;
            const Extent e = w.extent(w.physical(w1), count, w.modelview[w.modelview_depth]);
            w.classify_draw(e, 0);
            w.emit(w0, w1);
            continue;
        }

        // A rebuilt mesh's vertices can be loaded by the frame's own list as
        // well as by one it calls; the trace wants both.
        if (op == kOpVtx && w.lattice_trace) w.note_vertex(w0, w1);

        w.emit(w0, w1);
    }

    if (w.overflow) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "[wr64] a display list did not fit the rewriter's scratch space;"
                                 " it was passed through unchanged\n");
            std::fflush(stderr);
        }
        return 0;
    }

    if (w.lattice_trace) w.flush_lattice(state);

    wr64::inspector::end_frame();

    // The drawn region follows the game's scissor, once it has held for a few
    // frames: a transition frame can draw into less than the whole region, and
    // the crop must not twitch with it.
    if (w.has_scissor()) {
        static int seen_left = -1, seen_top = -1, seen_right = -1, seen_bottom = -1;
        static int seen_for = 0;
        if (w.scissor_left == seen_left && w.scissor_top == seen_top &&
            w.scissor_right == seen_right && w.scissor_bottom == seen_bottom) {
            if (++seen_for == 3) {
                wr64::display::set_content(seen_left, seen_top, seen_right, seen_bottom);
            }
        }
        else {
            seen_left = w.scissor_left;
            seen_top = w.scissor_top;
            seen_right = w.scissor_right;
            seen_bottom = w.scissor_bottom;
            seen_for = 1;
        }
    }

    static uint32_t last_draws = 0xFFFFFFFFu;
    const bool composition_changed = w.draws_2d != last_draws;
    last_draws = w.draws_2d;
    if (w.trace != nullptr && w.written >= 30 && (traced_lists < 6 || composition_changed)) {
        ++traced_lists;
        std::fprintf(stderr, "[hud] ---- state 0x%02X list %u: %u commands, %u class changes, inset %.1f,"
                             " anchors %s, race %s (world-first %d, scissor %d,%d-%d,%d) ----\n%s",
                     state, traced_lists, w.written, w.class_changes, w.inset,
                     w.anchors ? "on" : "off", w.race_test.race() ? "yes" : "no",
                     w.race_test.world_first ? 1 : 0, w.scissor_left, w.scissor_top,
                     w.scissor_right, w.scissor_bottom,
                     trace_text.c_str());
        std::fflush(stderr);
    }

    // The sky section is reported the first time it is found and the first
    // time a race frame goes without it, so a course that draws its sky some
    // other way says so in the log rather than quietly staying at the game's
    // rate.
    static bool reported_sky = false, reported_no_sky = false;
    if (w.sky != Walker::SkySection::Before && !reported_sky) {
        reported_sky = true;
        std::fprintf(stderr, "[wr64] the sky's vertices are interpolated (state 0x%02X)\n", state);
        std::fflush(stderr);
    }
    else if (w.sky == Walker::SkySection::Before && w.sky_interp && w.race_test.race() &&
             !reported_no_sky) {
        reported_no_sky = true;
        std::fprintf(stderr, "[wr64] no sky section found in a race frame (state 0x%02X);"
                             " the sky stays at the game's rate\n", state);
        std::fflush(stderr);
    }

    // Whether a screen-space section was stretched, reported the first time it
    // happens in each state. The per-list report below fires on the first list
    // and the six-hundredth, which never coincides with a transition, so without
    // this the log could not say whether the treatment reached the wipe at all --
    // and "did it fire" is the whole question while this is being tuned.
    {
        static uint32_t reported_state = 0xFFFFFFFFu;
        if (w.released_sections > 0 && state != reported_state) {
            reported_state = state;
            std::fprintf(stderr, "[wr64] %u screen-space section(s) released to the widened frame"
                                 " (state 0x%02X)\n", w.released_sections, state);
            std::fflush(stderr);
        }
    }

    static uint32_t lists = 0;
    if (++lists == 1 || lists == 600) {
        std::fprintf(stderr, "[wr64] display list rewritten: %u commands, %u centred sections,"
                             " %u released to the frame, %u class changes (state 0x%02X)\n",
                     w.written, w.centred_sections, w.released_sections,
                     w.class_changes, state);
        std::fflush(stderr);
    }
    return kScratch;
}

// The setting, converted to the ratio the rewriter applies.
//
// m[1][1] is cot(fovy/2), so the ratio between the game's frustum and the one
// asked for is tan(target/2) / tan(45/2). Forty-five degrees gives exactly one
// and the projection is then left alone entirely, which is what keeps the
// default free of any rewriting at all.
void set_field_of_view(double degrees) {
    const double clamped = std::min(std::max(degrees, 30.0), 130.0);
    const double ratio = std::tan(clamped * 0.5 * 3.14159265358979323846 / 180.0) /
                         std::tan(kGameFovY * 0.5 * 3.14159265358979323846 / 180.0);
    g_fov_scale.store(static_cast<float>(ratio), std::memory_order_relaxed);
    std::fprintf(stderr, "[wr64] field of view: %.0f degrees (x%.3f)\n", clamped, ratio);
    std::fflush(stderr);
}

}  // namespace wr64::dlrewrite
