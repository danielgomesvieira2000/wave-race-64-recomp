#pragma once
#include <cstdint>
#include <string>
#include "shared/rt64_water_params.h"

namespace wr64::water {
enum class Quality : uint32_t { Original, Modern, High, Ultra };
enum class Style : uint32_t { Modern, Classic, Aqua };
enum class RippleDetail : uint32_t { Soft, Normal, Strong };
// Publish on the original game's task submission thread; consume by list identity.
void publish_frame(const uint8_t *rdram, uint32_t display_list);
void reset_for_race();
void begin_frame(uint32_t display_list);
interop::WaterMaterial material(uint32_t view);
void toggle();
void cycle_debug();
Quality quality();
void set_quality(Quality value);
void set_style(Style value);
void set_aqua_brightness(float percent);
void set_aqua_tint(float percent);
// Modern and Aqua; the setting's id is still aqua_clarity, see src/frontend.cpp.
void set_clarity(float percent);
void set_ripple_detail(RippleDetail value);
void set_spray_enabled(bool enabled);

// The sun, per course, for the F1 editor (src/watersun.cpp). A direction toward
// the sun in the course's world axes, Y up, and the highlight's strength -- the
// profile's `sun_direction`. An override replaces the profile's value from the
// next frame; overrides are saved to water_sun.json in the settings folder, read
// at startup, and copied into assets/water/profiles.json by
// tools/promote_water_sun.py. Any thread.
struct Sun { float x, y, z, strength; };
constexpr uint32_t kCourses = 10;
const char* course_name(uint32_t course);
uint32_t current_course();                  // >= kCourses before a course is drawn
bool profiles_loaded();
Sun profile_sun(uint32_t course);           // assets/water/profiles.json's
bool sun_override(uint32_t course, Sun* out);
void set_sun_override(uint32_t course, const Sun& sun);
void clear_sun_override(uint32_t course);
bool camera_heading(float* x, float* z);    // the game camera's horizontal heading, normalised
bool save_sun_overrides(std::string& status);
void draw_sun_editor();                     // inside RT64's F1 menu, called by the inspector
}
