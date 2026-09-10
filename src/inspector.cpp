// The HUD inspector. See include/wr64/inspector.h for what it is for.

#include "wr64/inspector.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include <librecomp/game.hpp>

#include "json/json.hpp"

// Set by this file, called by RT64 once per frame with an ImGui frame open.
// tools/patch_rt64_inspector.py adds the call.
extern "C" void (*RT64_PortInspectorHook)();

namespace wr64::inspector {
namespace {

struct Element {
    std::string identity;
    std::string second_identity;
    float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
    bool perspective = false;
    bool is_rect = false;
    int given_class = kAuto;
};

struct Frame {
    uint32_t game_state = 0;
    uint64_t number = 0;
    std::vector<Element> elements;
};

bool g_enabled = false;

// The frame the classifier is filling, and the last one it finished. The panel
// only ever reads the finished one.
std::mutex g_mutex;
Frame g_building;
Frame g_published;
uint64_t g_frames = 0;

// What the panel has overridden, by identity. Read by the classifier on every
// element of every frame, so it is kept small and looked up under the same lock
// as the frames -- the whole table is a handful of entries by construction.
std::unordered_map<std::string, int> g_overrides;

// A frame is only interesting while the game is on the screen it was captured
// from, so the panel can hold one still while the game runs on.
bool g_hold = false;
Frame g_held;

const char* class_name(int cls) {
    switch (cls) {
        case kLeft:    return "left";
        case kRight:   return "right";
        case kStretch: return "stretch";
        default:       return "center";
    }
}

std::filesystem::path tag_path() {
    return recomp::get_config_path() / "hud.json";
}

// Writes the overrides into hud.json, keeping any entries already there that
// the panel has not touched. The classifier reads that file at startup, so a
// saved override survives a restart -- and an identity worth keeping can then be
// moved into the port's built-in table.
void save_overrides(std::string& status) {
    nlohmann::json doc;
    const std::filesystem::path path = tag_path();
    {
        std::ifstream in(path);
        if (in) {
            try {
                in >> doc;
            }
            catch (const std::exception&) {
                doc = nlohmann::json::object();
            }
        }
    }
    if (!doc.is_object()) doc = nlohmann::json::object();

    const char* lists[] = { "center", "left", "right", "stretch" };
    for (const char* list : lists) {
        if (!doc.contains(list) || !doc[list].is_array()) doc[list] = nlohmann::json::array();
    }

    std::unordered_map<std::string, int> overrides;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        overrides = g_overrides;
    }

    for (const auto& [identity, cls] : overrides) {
        // An identity belongs to one list; drop it from the others first so that
        // changing your mind in the panel does not leave both answers behind.
        for (const char* list : lists) {
            auto& array = doc[list];
            for (auto it = array.begin(); it != array.end();) {
                it = (it->is_string() && it->get<std::string>() == identity) ? array.erase(it)
                                                                            : it + 1;
            }
        }
        doc[class_name(cls)].push_back(identity);
    }

    std::ofstream out(path);
    if (!out) {
        status = "could not write " + path.string();
        return;
    }
    out << doc.dump(4) << "\n";
    status = "saved " + std::to_string(overrides.size()) + " override(s) to hud.json";
}

// Where an element lands on the screen, given the class it is being drawn with.
//
// This is the rewriter's geometry read forwards. The game draws in 320x240; RT64
// squeezes that into a 4:3 box in the middle of the window unless an element
// carries an extended origin, and an origin pins one of its edges to the
// corresponding edge of the widened frame instead. Stretch spreads it across the
// whole frame. Close enough to point at an element with, which is what it is for
// -- it is not what the renderer computes, and a scissor can still cut it short.
ImVec2 screen_x(float min_x, float max_x, int cls, const ImVec2& origin, const ImVec2& size) {
    const float box = size.y * 4.0f / 3.0f;   // the 4:3 frame, at this height
    float left = origin.x + (size.x - box) * 0.5f;
    float scale = box / 320.0f;
    switch (cls) {
        case kLeft:    left = origin.x; break;
        case kRight:   left = origin.x + size.x - box; break;
        case kStretch: left = origin.x; scale = size.x / 320.0f; break;
        default:       break;
    }
    return ImVec2(left + min_x * scale, left + max_x * scale);
}

void outline(const Element& e, int cls, ImU32 colour) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 xs = screen_x(e.min_x, e.max_x, cls, vp->Pos, vp->Size);
    const float y_scale = vp->Size.y / 240.0f;
    const float y0 = vp->Pos.y + e.min_y * y_scale;
    const float y1 = vp->Pos.y + e.max_y * y_scale;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRect(ImVec2(xs.x, y0), ImVec2(xs.y, y1), colour, 0.0f, 0, 2.5f);
}

void draw_panel() {
    Frame frame;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        frame = g_hold ? g_held : g_published;
    }

    ImGui::SetNextWindowSize(ImVec2(700.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Wave Race HUD")) {
        ImGui::End();
        return;
    }

    ImGui::Text("state 0x%02X   frame %llu   %zu elements", frame.game_state,
                static_cast<unsigned long long>(frame.number), frame.elements.size());
    ImGui::TextDisabled("RT64's own pause (Debugger tab) freezes the game; Hold keeps this");
    ImGui::TextDisabled("list on one frame while the game runs on.");

    bool hold = g_hold;
    if (ImGui::Checkbox("Hold this frame", &hold)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (hold) g_held = g_published;
        g_hold = hold;
    }

    static std::string status;
    ImGui::SameLine();
    if (ImGui::Button("Save to hud.json")) {
        save_overrides(status);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear overrides")) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_overrides.clear();
        status = "overrides cleared";
    }
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());

    // A busy frame is a few dozen elements and the one being looked for is
    // usually already known by part of its identity.
    static char filter[64] = "";
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("filter", "part of an identity", filter, sizeof(filter));

    ImGui::Separator();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("elements", 6, flags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("identity", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, 86.0f);
        ImGui::TableSetupColumn("y", ImGuiTableColumnFlags_WidthFixed, 86.0f);
        ImGui::TableSetupColumn("proj", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn("class", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        static int selected_row = -1;
        int hovered_row = -1;
        if (selected_row >= static_cast<int>(frame.elements.size())) selected_row = -1;

        for (size_t i = 0; i < frame.elements.size(); ++i) {
            const Element& e = frame.elements[i];
            if (filter[0] != ' ' &&
                e.identity.find(filter) == std::string::npos &&
                e.second_identity.find(filter) == std::string::npos) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            // The class this element is actually being drawn with, which is what
            // the outline has to use and what the dropdown has to show.
            int current = e.given_class;
            bool overridden = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto it = g_overrides.find(e.identity);
                if (it != g_overrides.end()) {
                    current = it->second;
                    overridden = true;
                }
            }

            ImGui::TableNextColumn();
            char label[16];
            std::snprintf(label, sizeof(label), "%zu", i);
            const bool is_selected = selected_row == static_cast<int>(i);
            if (ImGui::Selectable(label, is_selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowItemOverlap)) {
                selected_row = is_selected ? -1 : static_cast<int>(i);
            }
            if (ImGui::IsItemHovered()) hovered_row = static_cast<int>(i);

            ImGui::TableNextColumn();
            if (e.second_identity.empty()) {
                ImGui::TextUnformatted(e.identity.c_str());
            }
            else {
                ImGui::Text("%s  %s", e.identity.c_str(), e.second_identity.c_str());
            }

            ImGui::TableNextColumn();
            ImGui::Text("%.0f..%.0f", e.min_x, e.max_x);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f..%.0f", e.min_y, e.max_y);
            ImGui::TableNextColumn();
            ImGui::Text("%s %s", e.perspective ? "persp" : "ortho", e.is_rect ? "rect" : "tris");

            // The class, as a dropdown that starts on whatever the element was
            // given. Choosing another overrides this identity from the next
            // frame; "as classified" takes the override away again.
            ImGui::TableNextColumn();
            const char* items[] = { "center", "left", "right", "stretch" };
            int selected = overridden ? current + 1 : 0;
            const char* preview = overridden ? items[current] : class_name(e.given_class);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##class", preview)) {
                if (ImGui::Selectable("as classified", selected == 0)) {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_overrides.erase(e.identity);
                }
                for (int c = 0; c < 4; ++c) {
                    if (ImGui::Selectable(items[c], selected == c + 1)) {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_overrides[e.identity] = c;
                    }
                }
                ImGui::EndCombo();
            }
            if (overridden) {
                ImGui::SameLine();
                ImGui::TextDisabled("*");
            }

            // Hovering a row draws its box over the game; a click keeps the box
            // up while the dropdown is being used.
            if (hovered_row == static_cast<int>(i)) {
                outline(e, current, IM_COL32(255, 230, 60, 255));
            }
            else if (is_selected) {
                outline(e, current, IM_COL32(80, 200, 255, 220));
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace

bool enabled() {
    return g_enabled;
}

void init() {
    g_enabled = std::getenv("WR64_INSPECTOR") != nullptr;
    if (g_enabled) {
        std::fprintf(stderr, "[wr64] HUD inspector on; RT64's developer UI is forced on with it\n");
        std::fflush(stderr);
    }
}

void install() {
    if (!g_enabled) return;
    RT64_PortInspectorHook = draw_panel;
}

void begin_frame(uint32_t game_state) {
    if (!g_enabled) return;
    g_building.game_state = game_state;
    g_building.number = ++g_frames;
    g_building.elements.clear();
}

void note_element(const char* identity, const char* second_identity,
                  float min_x, float max_x, float min_y, float max_y,
                  bool perspective, int given_class, bool is_rect) {
    if (!g_enabled) return;
    // A frame of a busy menu is a few dozen elements; the cap is only so that a
    // pathological list cannot grow without bound behind the panel's back.
    if (g_building.elements.size() >= 512) return;
    Element e;
    e.identity = identity != nullptr ? identity : "";
    e.second_identity = second_identity != nullptr ? second_identity : "";
    e.min_x = min_x;
    e.max_x = max_x;
    e.min_y = min_y;
    e.max_y = max_y;
    e.perspective = perspective;
    e.given_class = given_class;
    e.is_rect = is_rect;
    g_building.elements.push_back(std::move(e));
}

void end_frame() {
    if (!g_enabled) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_published = g_building;
}

bool override_class(const char* identity, int* out_class) {
    if (!g_enabled || identity == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_overrides.empty()) return false;
    const auto it = g_overrides.find(identity);
    if (it == g_overrides.end()) return false;
    *out_class = it->second;
    return true;
}

}  // namespace wr64::inspector
