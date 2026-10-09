// src/courses/course_loader.cpp
#include "wr64/courses.hpp"

#include <fstream>
#include <iostream>
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace wr64::courses {

// ---------------------------------------------------------------------------
// Static data mirrors the N64 ROM's hardcoded course tables (see reference
// decompilation at reference/wr64-decomp/src/game/code_4C750.c).
//
// D_800DAB10  -- default draw distances per course (5000 for courses 0-1,
//                6000 for course 2)
// D_800EABEC  -- buoy limits (6000 for course 2 per GAME-INTERNALS.md)
// D_800DA988  -- { difficulty_selector, power_misses, lap_count } triples
// ---------------------------------------------------------------------------

static const struct BuiltinCourseInfo {
    int id;
    const char* name;
    const char* internal_name;
    float draw_distance;
    int buoy_limit;
    Difficulty difficulty;
    std::array<WaterParams, 3> wave_params;
    std::array<int, 3> laps;
    std::array<int, 3> power_misses;
} kBuiltinCourses[] = {
    // id, name, internal_name, draw_dist, buoy_limit, difficulty,
    // {wave_levels}, {laps}, {power_misses}
    { 0, "Dolphin Park",      "DOLPHIN_PARK",       5000.0f, 32, Difficulty::NORMAL,
      { WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f} },
      {3, 3, 3}, {5, 5, 5} },
    { 1, "Sunny Beach",       "SUNNY_BEACH",        5000.0f, 32, Difficulty::NORMAL,
      { WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f} },
      {3, 3, 3}, {5, 5, 5} },
    { 2, "Sunset Bay",        "SUNSET_BAY",         6000.0f, 32, Difficulty::NORMAL,
      { WaterParams{1.0f, 6000.0f}, WaterParams{1.0f, 6000.0f}, WaterParams{1.0f, 6000.0f} },
      {3, 3, 3}, {5, 5, 5} },
    { 3, "Marine Fortress",   "MARINE_FORTRESS",    5000.0f, 32, Difficulty::NORMAL,
      { WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f} },
      {3, 3, 3}, {5, 5, 5} },
    { 4, "Drake Lake",        "DRAKE_LAKE",         5000.0f, 32, Difficulty::NORMAL,
      { WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f} },
      {3, 3, 3}, {5, 5, 5} },
    { 5, "Port Blue",         "PORT_BLUE",          5000.0f, 32, Difficulty::NORMAL,
      { WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f} },
      {3, 3, 3}, {5, 5, 5} },
    { 6, "Twilight City",     "TWILIGHT_CITY",      5000.0f, 32, Difficulty::NORMAL,
      { WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f} },
      {3, 3, 3}, {5, 5, 5} },
    { 7, "Glacier Coast",     "GLACIER_COAST",      5000.0f, 32, Difficulty::NORMAL,
      { WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f} },
      {3, 3, 3}, {5, 5, 5} },
    { 8, "Southern Island",   "SOUTHERN_ISLAND",    5000.0f, 32, Difficulty::NORMAL,
      { WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f}, WaterParams{1.0f, 5000.0f} },
      {3, 3, 3}, {5, 5, 5} },
};

// ---------------------------------------------------------------------------
// Course implementation
// ---------------------------------------------------------------------------

Course::Course(const CourseParams& params) : m_params(params), m_loaded(true) {}

std::unique_ptr<Course> Course::create(const CourseParams& params) {
    return std::unique_ptr<Course>(new Course(params));
}

std::unique_ptr<Course> Course::load_from_file(const std::string& path) {
    JsonCourseLoader loader;
    CourseParams params;
    if (!loader.load(path, params)) {
        return nullptr;
    }
    return create(params);
}

std::unique_ptr<Course> Course::load(int course_id) {
    // First check if this is a registered custom course
    CourseManager& mgr = CourseManager::instance();
    auto course = mgr.get_course(course_id);
    if (course) {
        return course;
    }
    
    // Fall back to builtin course data from the ROM tables
    for (const auto& info : kBuiltinCourses) {
        if (info.id == course_id) {
            CourseParams params;
            params.id = info.id;
            params.name = info.name;
            params.internal_name = info.internal_name;
            params.draw_distance = info.draw_distance;
            params.buoy_limit = info.buoy_limit;
            params.difficulty = static_cast<float>(
                (info.difficulty == Difficulty::EXPERT) ? 1.5f :
                (info.difficulty == Difficulty::HARD) ? 1.25f : 1.0f);
            params.wave_params = info.wave_params;
            params.laps = info.laps;
            params.power_misses = info.power_misses;
            return create(params);
        }
    }
    
    return nullptr;
}

void Course::load_wave_level(Difficulty diff) {
    // Wave level is already populated from the JSON or builtin course data.
    // This method is reserved for dynamic overrides from ROM data.
    int idx = static_cast<int>(diff);
    if (idx >= 0 && idx < 3) {
        // No-op: wave level already loaded
    }
}

// ---------------------------------------------------------------------------
// JSON course loader -- parses custom course JSON files
// ---------------------------------------------------------------------------

static StartPosition parse_start_position(const json& j) {
    StartPosition pos;
    if (j.contains("x")) pos.x = j["x"].get<float>();
    if (j.contains("z")) pos.z = j["z"].get<float>();
    if (j.contains("heading")) pos.heading = j["heading"].get<float>();
    return pos;
}

static Buoy parse_buoy(const json& j) {
    Buoy b;
    if (j.contains("x")) b.x = j["x"].get<float>();
    if (j.contains("z")) b.z = j["z"].get<float>();
    if (j.contains("type")) b.type = j["type"].get<int>();
    return b;
}

static Gate parse_gate(const json& j) {
    Gate g;
    if (j.contains("x")) g.x = j["x"].get<float>();
    if (j.contains("z")) g.z = j["z"].get<float>();
    return g;
}

static WaterParams parse_water_params(const json& j) {
    WaterParams w;
    if (j.contains("wave_level")) w.wave_level = j["wave_level"].get<float>();
    if (j.contains("visibility_range")) w.visibility_range = j["visibility_range"].get<float>();
    return w;
}

static CourseParams::GeometryBounds parse_geometry_bounds(const json& j) {
    CourseParams::GeometryBounds bounds;
    if (j.contains("x_min")) bounds.x_min = j["x_min"].get<float>();
    if (j.contains("x_max")) bounds.x_max = j["x_max"].get<float>();
    if (j.contains("z_min")) bounds.z_min = j["z_min"].get<float>();
    if (j.contains("z_max")) bounds.z_max = j["z_max"].get<float>();
    if (j.contains("water_level")) bounds.water_level = j["water_level"].get<float>();
    return bounds;
}

bool JsonCourseLoader::load(const std::string& path, CourseParams& out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "CourseLoader: Failed to open " << path << "\n";
        return false;
    }
    
    json j;
    try {
        file >> j;
    } catch (const json::parse_error& e) {
        std::cerr << "CourseLoader: Parse error in " << path << ": " << e.what() << "\n";
        return false;
    }
    
    // Required fields
    if (j.contains("id")) out.id = j["id"].get<int>();
    if (j.contains("name")) out.name = j["name"].get<std::string>();
    if (j.contains("internal_name")) out.internal_name = j["internal_name"].get<std::string>();
    
    // Optional scalar fields
    if (j.contains("draw_distance")) out.draw_distance = j["draw_distance"].get<float>();
    if (j.contains("buoy_limit")) out.buoy_limit = j["buoy_limit"].get<int>();
    if (j.contains("difficulty")) out.difficulty = j["difficulty"].get<float>();
    
    // Geometry bounds
    if (j.contains("bounds")) {
        out.bounds = parse_geometry_bounds(j["bounds"]);
    }
    
    // File references
    if (j.contains("texture_files")) {
        for (const auto& f : j["texture_files"]) {
            out.texture_files.push_back(f.get<std::string>());
        }
    }
    if (j.contains("model_files")) {
        for (const auto& f : j["model_files"]) {
            out.model_files.push_back(f.get<std::string>());
        }
    }
    if (j.contains("audio_files")) {
        for (const auto& f : j["audio_files"]) {
            out.audio_files.push_back(f.get<std::string>());
        }
    }
    if (j.contains("water_profiles")) {
        for (const auto& f : j["water_profiles"]) {
            out.water_profiles.push_back(f.get<std::string>());
        }
    }
    
    // Start positions
    if (j.contains("start_positions")) {
        for (const auto& sp : j["start_positions"]) {
            out.start_positions.push_back(parse_start_position(sp));
        }
    }
    
    // Buoys
    if (j.contains("buoys")) {
        for (const auto& b : j["buoys"]) {
            out.buoys.push_back(parse_buoy(b));
        }
    }
    
    // Gates
    if (j.contains("gates")) {
        for (const auto& g : j["gates"]) {
            out.gates.push_back(parse_gate(g));
        }
    }
    
    // Wave parameters per difficulty
    if (j.contains("wave_params")) {
        const auto& wp = j["wave_params"];
        if (wp.contains("normal")) out.wave_params[0] = parse_water_params(wp["normal"]);
        if (wp.contains("hard")) out.wave_params[1] = parse_water_params(wp["hard"]);
        if (wp.contains("expert")) out.wave_params[2] = parse_water_params(wp["expert"]);
    }
    
    // Race settings
    if (j.contains("laps")) {
        const auto& laps = j["laps"];
        if (laps.contains("normal")) out.laps[0] = laps["normal"].get<int>();
        if (laps.contains("hard")) out.laps[1] = laps["hard"].get<int>();
        if (laps.contains("expert")) out.laps[2] = laps["expert"].get<int>();
    }
    if (j.contains("power_misses")) {
        const auto& pm = j["power_misses"];
        if (pm.contains("normal")) out.power_misses[0] = pm["normal"].get<int>();
        if (pm.contains("hard")) out.power_misses[1] = pm["hard"].get<int>();
        if (pm.contains("expert")) out.power_misses[2] = pm["expert"].get<int>();
    }
    
    return true;
}

// ---------------------------------------------------------------------------
// CourseManager singleton
// ---------------------------------------------------------------------------

CourseManager& CourseManager::instance() {
    static CourseManager mgr;
    return mgr;
}

void CourseManager::set_current_course(int id) {
    m_current_course = id;
}

std::vector<int> CourseManager::available_courses() const {
    std::vector<int> ids;
    for (const auto& [id, _] : m_courses) {
        ids.push_back(id);
    }
    // Also list builtin courses (0-8)
    for (int i = 0; i <= 8; ++i) {
        if (m_courses.find(i) == m_courses.end()) {
            ids.push_back(i);
        }
    }
    return ids;
}

std::unique_ptr<Course> CourseManager::get_course(int course_id) {
    auto it = m_courses.find(course_id);
    if (it != m_courses.end()) {
        CourseParams params = it->second->params();
        return Course::create(params);
    }
    return nullptr;
}

void CourseManager::register_custom_course(int course_id, std::unique_ptr<Course> course) {
    m_courses[course_id] = std::move(course);
}

void CourseManager::save_course_completion(int course_id, int laps, float time) {
    int best = get_best_time(course_id);
    if (best == 0 || static_cast<int>(time) < best) {
        m_best_times[course_id] = static_cast<int>(time);
    }
    (void)laps; // TODO: persist lap completion
}

int CourseManager::get_best_time(int course_id) const {
    auto it = m_best_times.find(course_id);
    if (it != m_best_times.end()) {
        return it->second;
    }
    return 0;
}

void CourseManager::reset_progress() {
    m_best_times.clear();
}

void CourseManager::set_wave_level(int course_id, Difficulty diff, float level) {
    m_wave_levels[course_id][static_cast<int>(diff)] = level;
}

float CourseManager::get_wave_level(int course_id, Difficulty diff) const {
    auto it = m_wave_levels.find(course_id);
    if (it != m_wave_levels.end()) {
        return it->second[static_cast<int>(diff)];
    }
    return 0.0f;
}

void CourseManager::set_laps(int course_id, Difficulty diff, int laps) {
    m_laps[course_id][static_cast<int>(diff)] = laps;
}

int CourseManager::get_laps(int course_id, Difficulty diff) const {
    auto it = m_laps.find(course_id);
    if (it != m_laps.end()) {
        return it->second[static_cast<int>(diff)];
    }
    // Default lap counts from D_800DA988
    return (course_id >= 0 && course_id <= 8) ? 3 : 3;
}

} // namespace wr64::courses