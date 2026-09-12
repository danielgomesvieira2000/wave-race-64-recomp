// See include/wr64/drawdistance.h for what this is and how it was measured.

#include "wr64/drawdistance.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wr64::drawdistance {
namespace {

// Where the number lives.
//
// The struct is not indexed by course. The game leaves a pointer to the current
// course's environment at 0x801C0C80 and its own code dereferences exactly that,
// so following the same pointer needs no stride and cannot be wrong about one.
// The pointer reads 0x801CB058 on every course -- the game *copies* into a
// single struct rather than indexing an array -- which is why nothing here may
// use the pointer's value to notice that the course changed.
constexpr uint32_t kStructPointer = 0x001C0C80;   // RDRAM offset of the pointer
constexpr uint32_t kReachField    = 0xA4;         // the integer, inside the struct
constexpr uint32_t kCourseNumber  = 0x000D8170;   // the course the game is on

constexpr uint32_t kNoCourse = 0xFFFFFFFFu;

// Expanded RDRAM, which is what librecomp allocates and what this game runs in.
constexpr uint32_t kRdramBytes = 0x00800000u;

// What the setting asks for, in world units. Zero is the game's own.
std::atomic<int32_t> g_reach{ 0 };

// What the game itself last put in the field, and what was written over it.
//
// Recapturing the game's own value is the part that has to be right, and the
// first version of this got it wrong in a way that only shows on some courses.
// It recaptured whenever the field held something other than what it had
// written -- which is true when the game reloads the struct, but is also *false*
// when a new course's own value happens to equal what was written for the last
// one. With the values the game actually uses that collides: 2500 doubled is
// 5000, which is another course's own limit, so moving from a 2500 course to a
// 5000 course left the port believing it had already done its work and the
// second course silently ran at the game's own distance.
//
// The course number is the real identity and the game already keeps it, so this
// keys on that, and treats an unexpected value as a second, independent reason
// to recapture -- the game also rewrites the struct part-way through a course.
int32_t  g_original = 0;
int32_t  g_written = 0;
uint32_t g_course = kNoCourse;
bool     g_holding = false;       // true while the game's own value is overwritten

// After the course number changes, nothing here writes for this many frames.
// The course number and the struct's contents do not change together -- the
// number moves first and the environment is filled in behind it -- so writing
// during that window destroys the only evidence of what the game's own value
// is: read the field back and it holds what this wrote. Sixty race frames is
// two or three seconds of loading and countdown, long enough for the fill and
// invisible to a player. The same wait is why the census's own struct dump is
// trustworthy; see docs/RENDER-DISTANCE-CENSUS.md.
constexpr int kSettleFrames = 60;

int g_settling = 0;

// Every frame, what was found and what was written over it.
//
// "It flickers" has two completely different causes and they are invisible from
// outside: a limit that changes between frames, so the same object is kept on
// one and dropped on the next, or a limit that is perfectly stable while
// something else decides what gets drawn. It is the second -- measured from the
// census captures, a site inside the limit blinks off for a single frame in
// 0.03% of its chances at the game's own distance and 0.01% at twice it -- so
// this exists to keep that answerable rather than to be argued about.
bool trace_wanted() {
    static const bool on = [] {
        const char* value = std::getenv("WR64_DRAW_DISTANCE_TRACE");
        return value != nullptr && value[0] != 0 && std::strcmp(value, "0") != 0;
    }();
    return on;
}

int32_t* reach_field(uint8_t* rdram, uint32_t address) {
    return reinterpret_cast<int32_t*>(rdram + address + kReachField);
}

uint32_t read_word(const uint8_t* rdram, uint32_t offset) {
    uint32_t value;
    std::memcpy(&value, rdram + (offset & 0x007FFFFFu), sizeof value);
    return value;
}

}  // namespace

void set_reach(int32_t world_units) {
    const int32_t clamped = world_units <= 0 ? 0
                          : (world_units > kFarPlane ? kFarPlane : world_units);
    g_reach.store(clamped, std::memory_order_relaxed);
    if (clamped == 0) {
        std::fprintf(stderr, "[wr64] draw distance: the game's own\n");
    }
    else {
        std::fprintf(stderr, "[wr64] draw distance: %d units\n", clamped);
    }
    std::fflush(stderr);
}

int32_t reach() {
    return g_reach.load(std::memory_order_relaxed);
}

void apply(uint8_t* rdram) {
    // This writes through a pointer read out of the game's own memory, so it is
    // checked before it is followed. A null one is the ordinary case -- before a
    // course is loaded there is nothing to point at -- and anything outside
    // RDRAM would be a corrupt read that must not become a corrupt write.
    const uint32_t address = read_word(rdram, kStructPointer) & 0x00FFFFFFu;
    if (address == 0 || address + kReachField + sizeof(int32_t) > kRdramBytes) return;
    int32_t* value = reach_field(rdram, address);

    const int32_t wanted = g_reach.load(std::memory_order_relaxed);
    if (wanted == 0) {
        // Put the game's own number back once, then leave it alone. At the
        // default nothing here writes to memory at all.
        if (g_holding) {
            *value = g_original;
            g_holding = false;
            g_course = kNoCourse;
        }
        return;
    }

    const uint32_t course = read_word(rdram, kCourseNumber);
    if (course != g_course) {
        g_course = course;
        g_settling = kSettleFrames;
        g_holding = false;
    }
    if (g_settling > 0) {
        --g_settling;
        return;                   // the field is the game's to fill; leave it
    }

    // Whatever is in the field now is the game's own, unless it is exactly what
    // was written into it. Both halves are needed: the value catches the game
    // rewriting the struct part-way through a course, and the settle above
    // catches a new course whose own value happens to equal what was written
    // for the last one -- 2500 doubled is 5000, which is another course's own
    // limit, and that collision is what the first version of this got wrong.
    const int32_t found = *value;
    if (!g_holding || found != g_written) {
        g_original = found;
    }
    if (g_original <= 0) return;

    // A floor and a ceiling. Never below what the game asked for, so the setting
    // cannot take away a marker a player needs; never above the far plane, past
    // which the geometry is clipped and the submission is pure cost.
    int32_t target = wanted < g_original ? g_original : wanted;
    if (target > kFarPlane) target = kFarPlane;

    // Logged when the answer changes, not every time this recomputes it. The
    // frontend hands the setting over twice at startup -- the default, then what
    // was saved -- and every round trip through Original drops the hold and
    // takes the game's value again, so one decision prints four times unless the
    // last answer is remembered.
    static uint32_t said_course = kNoCourse;
    static int32_t said_original = 0;
    static int32_t said_target = 0;
    if (course != said_course || g_original != said_original || target != said_target) {
        said_course = course;
        said_original = g_original;
        said_target = target;
        std::fprintf(stderr, "[wr64] draw distance: course %u, %d -> %d\n",
                     course, g_original, target);
        std::fflush(stderr);
    }
    g_written = target;
    g_holding = true;
    *value = target;

    if (trace_wanted()) {
        static uint64_t frame = 0;
        static int32_t last_found = -1;
        static int32_t last_written = -1;
        ++frame;
        // Printed when anything moves, and once every hundred frames regardless,
        // so a stable limit is visible as a stable limit rather than as silence.
        if (found != last_found || g_written != last_written || frame % 100 == 0) {
            std::fprintf(stderr, "[wr64-dd] frame %llu: course %u, struct 0x%08X,"
                                 " found %d, game's own %d, wrote %d%s\n",
                         static_cast<unsigned long long>(frame), course, address,
                         found, g_original, g_written,
                         found != last_found ? "   <- the game changed it" : "");
            std::fflush(stderr);
            last_found = found;
            last_written = g_written;
        }
    }
}

}  // namespace wr64::drawdistance
