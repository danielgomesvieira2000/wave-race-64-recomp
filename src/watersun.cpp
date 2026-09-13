// The sun editor: a window in the F1 menu for the modern water renderer's sun, per
// course. See docs/WATER.md, "Tuning the sun".
//
// The water renderer lights the surface from one direction a course, read from
// assets/water/profiles.json -- set by hand, not by the skybox, so the glint can
// sit well away from the sun painted in the sky. This window changes it on the
// course being played and shows the result on the next frame. Save writes the
// changes to water_sun.json in the settings folder, which the game reads at
// startup; tools/promote_water_sun.py copies them into profiles.json so a release
// carries them.
//
// Direction is edited as a bearing and a height rather than as x, y and z. A
// bearing is what lining the glint up with the sun actually needs, and "Aim at
// camera heading" gets it from the game: face the sun in the sky and press it.

#include "wr64/water.h"

#include <cmath>
#include <string>

#include <imgui.h>

namespace wr64::water {
namespace {

constexpr float kRadians = 3.14159265358979f / 180.0f;

struct Angles { float bearing, height, strength; };

// Bearing: 0 degrees looks down +Z, 90 down +X. Height: 0 is the horizon.
Angles to_angles(const Sun& s) {
    const float flat = std::sqrt(s.x * s.x + s.z * s.z);
    return { std::atan2(s.x, s.z) / kRadians, std::atan2(s.y, flat) / kRadians, s.strength };
}

Sun to_sun(const Angles& a) {
    const float h = a.height * kRadians, b = a.bearing * kRadians;
    return { std::cos(h) * std::sin(b), std::sin(h), std::cos(h) * std::cos(b), a.strength };
}

}  // namespace

void draw_sun_editor() {
    ImGui::SetNextWindowSize(ImVec2(520.0f, 300.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Wave Race water: sun")) {
        ImGui::End();
        return;
    }

    const uint32_t course = current_course();
    if (!profiles_loaded() || course >= kCourses) {
        ImGui::TextDisabled("No course on screen yet.");
        ImGui::End();
        return;
    }

    const Sun profile = profile_sun(course);
    Sun sun = profile;
    const bool overridden = sun_override(course, &sun);

    ImGui::Text("%s (course %u)%s", course_name(course), course, overridden ? "   * changed" : "");
    ImGui::TextDisabled("Needs Water Quality Enhanced or Best. Changes show on the next frame.");
    ImGui::Separator();

    Angles angles = to_angles(sun);
    bool changed = false;
    changed |= ImGui::SliderFloat("bearing", &angles.bearing, -180.0f, 180.0f, "%.1f deg");
    changed |= ImGui::SliderFloat("height", &angles.height, 0.0f, 90.0f, "%.1f deg");
    changed |= ImGui::SliderFloat("strength", &angles.strength, 0.0f, 4.0f, "%.2f");
    if (changed) set_sun_override(course, to_sun(angles));

    float hx = 0.0f, hz = 0.0f;
    const bool have_heading = camera_heading(&hx, &hz);
    if (!have_heading) ImGui::BeginDisabled();
    if (ImGui::Button("Aim at camera heading")) {
        angles.bearing = std::atan2(hx, hz) / kRadians;
        set_sun_override(course, to_sun(angles));
    }
    if (!have_heading) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Face the sun in the sky, then press: the bearing becomes the camera's.");
    }
    ImGui::SameLine();
    if (!overridden) ImGui::BeginDisabled();
    if (ImGui::Button("Revert to profile")) clear_sun_override(course);
    if (!overridden) ImGui::EndDisabled();

    static std::string status;
    if (ImGui::Button("Save to water_sun.json")) save_sun_overrides(status);
    if (!status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status.c_str());
    }

    ImGui::Separator();
    const Sun shown = overridden ? sun : profile;
    ImGui::TextDisabled("in use   [%.3f, %.3f, %.3f, %.2f]", shown.x, shown.y, shown.z, shown.strength);
    ImGui::TextDisabled("profile  [%.3f, %.3f, %.3f, %.2f]", profile.x, profile.y, profile.z, profile.strength);
    ImGui::TextDisabled("Saved changes load at startup. To ship them:");
    ImGui::TextDisabled("  python tools/promote_water_sun.py");

    ImGui::End();
}

}  // namespace wr64::water
