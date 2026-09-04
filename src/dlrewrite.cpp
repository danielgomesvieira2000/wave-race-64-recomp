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
//    that stops at 4:3 is a defect in any mode.
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
// was given; that is how a tag for hud.json is found. WR64_HUD_OFF=1 leaves
// the 2D layer alone, WR64_NO_REWRITE=1 switches the rewriter off entirely.

#include "wr64/dlrewrite.h"
#include "wr64/display.h"
#include "wr64/testdrive.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

enum class Class { Auto, Left, Right, Stretch };

const char* class_name(Class c) {
    switch (c) {
        case Class::Left:    return "left";
        case Class::Right:   return "right";
        case Class::Stretch: return "stretch";
        default:             return "center";
    }
}

// The tag table: hud.json in the settings folder. Four lists of identities,
// "tex:0x01004A20" for a texture and "dl:0x0106F8A0" for a static list, as
// the trace prints them, under "left", "right", "center" and "stretch".
struct Tags {
    std::unordered_map<std::string, Class> by_identity;
    bool loaded = false;

    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    void load() {
        loaded = true;
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
    Mat4 modelview[16];
    int modelview_depth = 0;
    uint32_t texture = 0;

    // Section state: A1's centred perspective sections, and the 2D class in
    // force.
    bool centred = false;
    bool seen_ortho = false;
    RaceTest race_test;
    Class cls = Class::Auto;
    uint32_t centred_sections = 0;
    uint32_t class_changes = 0;

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

    void align_viewport(uint32_t origin, int offset_x) {
        if (GfxCommand* cmd = reserve(2)) {
            gEXSetViewportAlign(cmd, origin, offset_x, 0);
        }
        if (have_viewport) {
            emit(viewport_w0, viewport_w1);
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

    static bool run_command(uint8_t op) {
        switch (op) {
            case 0xE4: case 0xE5:                       // rectangles
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
        if (pending_active) {
            classify_rect(pending_extent, 0, pending_textures);
        }
        for (const auto& [w0, w1] : pending) {
            emit(w0, w1);
        }
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

    void set_rect_class(Class next) {
        if (next == rect_cls) return;
        widen_scissor(next == Class::Left || next == Class::Right);
        const int inset_q = static_cast<int>(inset * 4.0f);
        if (rect_cls == Class::Stretch) {
            if (GfxCommand* cmd = reserve(1)) gEXSetRectAspect(cmd, G_EX_ASPECT_AUTO);
        }
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
        const std::string tex_id = hex_identity("tex", textures.empty() ? texture : textures.front());
        const std::string dl_id = dl_segmented != 0 ? hex_identity("dl", dl_segmented) : std::string{};
        Class next = classify(e, tex_id, dl_id);
        // A tag on any texture of the run applies to the run.
        for (uint32_t t : textures) {
            Class tagged;
            if (tags != nullptr && tags->lookup(hex_identity("tex", t), tagged)) {
                next = (tagged == Class::Stretch || anchors) ? tagged : Class::Auto;
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
        if (!noemit) set_rect_class(next);
    }

    // Whether a draw spans the frame from side to side, and so should be
    // stretched across the widened one.
    //
    // Measured against the region the game draws into, not the framebuffer.
    // This game's full-screen overlays -- the tint over the world, the dim the
    // pause screen lays over it, a fade -- span its own drawn region, 8 to
    // 311, because that is what its scissor allows; against the framebuffer's
    // 0 to 320 they fall eight pixels short at each end and were left at 4:3,
    // which showed as a brighter band of untinted picture down both edges.
    // Height is not part of the test: the dim is drawn as horizontal strips,
    // each spanning the width and a slice of the height.
    bool covers_width(const Extent& e) const {
        const float left = has_scissor() ? float(scissor_left) : 0.0f;
        const float right = has_scissor() ? float(scissor_right) : float(kFramebufferWidth);
        return e.left_px() <= left + 1.0f && e.right_px() >= right - 1.0f;
    }

    Class classify(const Extent& e, const std::string& identity, const std::string& identity2) {
        Class tagged;
        if (tags != nullptr && (tags->lookup(identity, tagged) || tags->lookup(identity2, tagged))) {
            if (tagged == Class::Stretch || anchors) return tagged;
            return Class::Auto;
        }
        if (e.empty()) return Class::Auto;
        if (covers_width(e)) return Class::Stretch;
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
            if (op == 0xE4 || op == 0xE5) { rects.add(rect_extent(w0, w1)); continue; }
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

}  // namespace

namespace wr64::dlrewrite {

uint32_t rewrite(uint8_t* rdram, uint32_t list_vaddr) {
    static const bool disabled = std::getenv("WR64_NO_REWRITE") != nullptr;
    static const bool hud_off = std::getenv("WR64_HUD_OFF") != nullptr;
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

    Walker w{};
    w.rdram = rdram;
    w.state = state;
    w.menu = !racing(state);
    w.hud = !hud_off;
    static const bool noemit = std::getenv("WR64_HUD_NOEMIT") != nullptr;
    w.noemit = noemit;
    // The menus' layouts are 4:3 by design and stay so whatever the setting
    // says; the setting is about the race HUD. Anchoring the race HUD is
    // whether a frame is a race is read from the frame (see RaceTest), not
    // from this state number. WR64_HUD_NO_ANCHORS switches anchoring off.
    static const bool anchors_disabled = std::getenv("WR64_HUD_NO_ANCHORS") != nullptr;
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
        if (traced_lists < 6) {
            trace_text.clear();
            w.trace = &trace_text;
        }
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
        if (w.hud && (op == 0xE4 || op == 0xE5)) {
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
            w.emit(w0, w1);
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
                char line[96];
                std::snprintf(line, sizeof(line), "[hud] state 0x%02X viewport load 0x%08X (class %s)\n",
                              state, w1, class_name(w.cls));
                w.trace->append(line);
            }
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
                char line[160];
                std::snprintf(line, sizeof(line), "[hud] state 0x%02X projection load %s [1][1]=%.3f [3][3]=%.3f"
                              " (class %s, viewport %s)\n",
                              state, w.projection_is_perspective ? "perspective" : "orthographic",
                              w.projection.m[1][1], w.projection.m[3][3], class_name(w.cls),
                              w.have_viewport ? "seen" : "not yet seen");
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
                }
            }
            if (projection_load && !w.projection_is_perspective) {
                w.seen_ortho = true;
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

    if (w.trace != nullptr && w.written >= 30) {
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

    static uint32_t lists = 0;
    if (++lists == 1 || lists == 600) {
        std::fprintf(stderr, "[wr64] display list rewritten: %u commands, %u centred sections,"
                             " %u class changes (state 0x%02X)\n",
                     w.written, w.centred_sections, w.class_changes, state);
        std::fflush(stderr);
    }
    return kScratch;
}

}  // namespace wr64::dlrewrite
