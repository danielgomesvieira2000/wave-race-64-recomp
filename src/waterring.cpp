// See include/wr64/waterring.h for what this is and why it samples where it does.

#include "wr64/waterring_sample.h"
#include "wr64/water.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

// The game's water height at a world XZ. Recompiled; declaring it is all it
// takes to call it.
extern "C" void func_8004D30C(uint8_t* rdram, recomp_context* ctx);

namespace wr64::waterring {
namespace {

// Where the ring stops: the same integer the course's own scenery cull reads, so
// the sea reaches as far as the things standing in it.
//
// It reads the *game's* value, not the Draw Distance setting's. Both touch the
// same field, but this runs on the game thread at task submission and
// drawdistance::apply runs on the graphics thread when the list is rewritten, so
// what is in the field here is whatever the game last put there: a run with the
// setting at 12,000 measured a ring of 5,000. Making the water follow the
// setting would mean reading the setting rather than the field, and it is not
// obvious that it should -- the sea reaching further than the course is drawn
// buys nothing, and the far bands are the expensive ones.
constexpr uint32_t kCoursePointer = 0x001C0C80;
constexpr uint32_t kCullField = 0xA4;

// gCameraPerspective, and the index of the one in use. The same addresses the
// draw distance and the render-distance census read; see drawdistance.cpp.
constexpr uint32_t kCameraIndex = 0x00223930;
constexpr uint32_t kCameraBase = 0x00227C80;
constexpr uint32_t kCameraStride = 0x10C;
constexpr uint32_t kCameraX = 0x4C;
constexpr uint32_t kCameraZ = 0x54;

// The reach of the game's own patch, from GAME-INTERNALS.md. The ring starts
// just inside it so the two overlap rather than leaving a seam: the patch is
// drawn second and covers the overlap.
constexpr float kInnerRadius = 860.0f;

// Never less than this much ring, and never more. The lower bound keeps a course
// with a small cull from producing a degenerate ring; the upper is the far
// plane, past which nothing is drawn at all.
constexpr float kMinOuter = 2000.0f;
constexpr float kMaxOuter = 16192.0f;

std::mutex g_lock;
struct Pending {
    uint32_t list;
    Ring ring;
};
std::deque<Pending> g_pending;

uint32_t read_word(const uint8_t* rdram, uint32_t offset) {
    uint32_t value;
    std::memcpy(&value, rdram + (offset & 0x007FFFFFu), sizeof value);
    return value;
}

float read_float(const uint8_t* rdram, uint32_t offset) {
    float value;
    std::memcpy(&value, rdram + (offset & 0x007FFFFFu), sizeof value);
    return value;
}

// The game's own answer for the height at (x, z).
//
// The context is *copied* rather than borrowed, and f_odd repointed after the
// copy -- both for the reasons src/waterfield.cpp sets out at length: a borrowed
// context would let the callee write into the live frame, and f_odd points into
// the context it belongs to, so a struct copy leaves it aimed at the original.
float height_at(uint8_t* rdram, const recomp_context* ctx, float x, float z) {
    recomp_context probe = *ctx;
    probe.f_odd = probe.mips3_float_mode ? &probe.f1.u32l : &probe.f0.u32h;
    probe.f12.fl = x;
    probe.f14.fl = z;
    func_8004D30C(rdram, &probe);
    return probe.f0.fl;
}

int16_t clamp_short(float value) {
    if (!std::isfinite(value)) return 0;
    if (value > 32000.0f) return 32000;
    if (value < -32000.0f) return -32000;
    return static_cast<int16_t>(value);
}

}  // namespace

bool enabled() {
    static const bool off = [] {
        const char* value = std::getenv("WR64_NO_WATER_RING");
        return value != nullptr && value[0] != 0 && std::strcmp(value, "0") != 0;
    }();
    return !off;
}

void publish(uint8_t* rdram, recomp_context* ctx, uint32_t display_list) {
    if (!enabled()) return;
    // Only with the modern renderer on. At Original the ring would be drawn with
    // the game's own flat shading against a horizon the game painted, which is
    // a worse picture than the one the game ships.
    if (water::quality() == water::Quality::Original) return;

    const uint32_t course = read_word(rdram, kCoursePointer) & 0x00FFFFFFu;
    if (course == 0 || course + kCullField + 4 > 0x00800000u) return;
    const int32_t cull = static_cast<int32_t>(read_word(rdram, course + kCullField));

    const uint32_t index = read_word(rdram, kCameraIndex) & 3;
    const uint32_t camera = kCameraBase + index * kCameraStride;
    const float cx = read_float(rdram, camera + kCameraX);
    const float cz = read_float(rdram, camera + kCameraZ);
    if (!std::isfinite(cx) || !std::isfinite(cz)) return;

    float outer = static_cast<float>(cull);
    outer = std::clamp(outer, kMinOuter, kMaxOuter);
    if (outer <= kInnerRadius * 1.2f) return;

    Ring ring{};
    ring.inner = kInnerRadius;
    ring.outer = outer;

    // Radii grow geometrically, so each band covers more ground than the one
    // inside it for the same number of vertices. That is where the cost belongs:
    // a band at 5,000 units is a few pixels tall on the screen.
    const float ratio = std::pow(outer / kInnerRadius, 1.0f / float(kCircles - 1));
    float radius = kInnerRadius;
    for (int c = 0; c < kCircles; ++c) {
        for (int s = 0; s < kSectors; ++s) {
            const float angle = (6.2831853f * float(s)) / float(kSectors);
            const float x = cx + radius * std::cos(angle);
            const float z = cz + radius * std::sin(angle);
            Vertex& v = ring.vertices[c * kSectors + s];
            v.x = clamp_short(x);
            v.z = clamp_short(z);
            v.y = clamp_short(height_at(rdram, ctx, x, z));
        }
        radius *= ratio;
    }

    std::lock_guard<std::mutex> guard(g_lock);
    g_pending.push_back({ display_list & 0x7FFFFFu, ring });
    // The same bound water.cpp keeps: a snapshot nobody claimed is a frame the
    // renderer never drew, and holding more than a handful means holding stale
    // ones.
    while (g_pending.size() > 16) g_pending.pop_front();
}

const Ring* claim(uint32_t display_list) {
    if (!enabled()) return nullptr;
    static Ring held;
    const uint32_t key = display_list & 0x7FFFFFu;
    std::lock_guard<std::mutex> guard(g_lock);
    const auto match = std::find_if(g_pending.begin(), g_pending.end(),
                                    [&](const Pending& p) { return p.list == key; });
    if (match == g_pending.end()) return nullptr;
    held = match->ring;
    g_pending.erase(g_pending.begin(), match + 1);
    return &held;
}

}  // namespace wr64::waterring
