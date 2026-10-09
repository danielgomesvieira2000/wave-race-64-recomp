// include/wr64/courses.hpp
#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <array>
#include <unordered_map>

namespace wr64::courses {

// Course identifiers matching the N64 game's enum (see reference/wr64-decomp/include/course.h)
enum class CourseId : int {
    DOLPHIN_PARK = 0,
    SUNNY_BEACH = 1,
    SUNSET_BAY = 2,
    MARINE_FORTRESS = 3,
    DRAKE_LAKE = 4,
    PORT_BLUE = 5,
    TWILIGHT_CITY = 6,
    GLACIER_COAST = 7,
    SOUTHERN_ISLAND = 8,
    RIDER_SELECTION = 9,
    SUNSET_HARBOR = 10  // Custom course (Task 6 prototype)
};

// Wave level settings per difficulty
enum class Difficulty : int {
    NORMAL = 0,
    HARD = 1,
    EXPERT = 2
};

// Start position for a rider on the course
struct StartPosition {
    float x = 0.0f;
    float z = 0.0f;
    float heading = 0.0f;
};

// Buoy position for race markers (see D_801BB138 / D_801AEE20 in GAME-INTERNALS.md)
struct Buoy {
    float x = 0.0f;
    float z = 0.0f;
    int type = 0;  // 0 = small purple edge buoy, 1 = racing buoy
};

// Gate marker (arrow pointing to next racing buoy)
struct Gate {
    float x = 0.0f;
    float z = 0.0f;
};

// Water parameters per difficulty level
struct WaterParams {
    float wave_level = 0.0f;
    float visibility_range = 5000.0f;
};

// Course parameters loaded from JSON or builtin ROM tables
struct CourseParams {
    int id = 0;
    std::string name;
    std::string internal_name;
    float draw_distance = 6000.0f;  // Matches D_801C0C80 +0xA4 (5000 or 6000 in ROM)
    int buoy_limit = 32;            // Hard limit from segment 5 buffer (32 slots per view)
    float difficulty = 1.0f;
    
    // Geometry bounds from ROM course metadata
    struct GeometryBounds {
        float x_min = -5000.0f, x_max = 5000.0f;
        float z_min = -5000.0f, z_max = 5000.0f;
        float water_level = 0.0f;
    } bounds;
    
    // File references for assets
    std::vector<std::string> texture_files;
    std::vector<std::string> model_files;
    std::vector<std::string> audio_files;
    std::vector<std::string> water_profiles;
    
    // Start positions (up to 4 riders, matches D_800DA9B0)
    std::vector<StartPosition> start_positions;
    
    // Race data (built by func_80071E70 from per-course lists)
    std::vector<Buoy> buoys;
    std::vector<Gate> gates;
    
    // Wave settings per difficulty (from D_800EABEC / D_800EAC00 tables)
    std::array<WaterParams, 3> wave_params;
    
    // Race requirements (from D_800DA988 table)
    std::array<int, 3> laps = {3, 3, 3};
    std::array<int, 3> power_misses = {5, 5, 5};
};

// Course class representing a loaded course
class Course {
public:
    ~Course() = default;
    
    static std::unique_ptr<Course> load(int course_id);
    static std::unique_ptr<Course> load_from_file(const std::string& path);
    
    const CourseParams& params() const { return m_params; }
    bool is_loaded() const { return m_loaded; }
    
    // Data accessors
    const std::vector<Buoy>& get_buoys() const { return m_params.buoys; }
    const std::vector<Gate>& get_gates() const { return m_params.gates; }
    
    // Load wave parameters from ROM data
    void load_wave_level(Difficulty diff);
    
    // Factory: create from params (used by loaders/managers)
    static std::unique_ptr<Course> create(const CourseParams& params);
    
private:
    Course() = default;
    explicit Course(const CourseParams& params);
    
    CourseParams m_params;
    bool m_loaded = false;
};

// Course loader interface -- abstract base for format-specific loaders
class CourseLoader {
public:
    virtual ~CourseLoader() = default;
    virtual bool load(const std::string& path, CourseParams& out) = 0;
    virtual std::string name() const = 0;
};

// JSON-based course loader using nlohmann/json
class JsonCourseLoader : public CourseLoader {
public:
    bool load(const std::string& path, CourseParams& out) override;
    std::string name() const override { return "JSON Loader"; }
};

// Course manager singleton -- manages loaded courses and race settings
class CourseManager {
public:
    static CourseManager& instance();
    
    // Course management
    int current_course() const { return m_current_course; }
    void set_current_course(int id);
    
    std::vector<int> available_courses() const;
    std::unique_ptr<Course> get_course(int course_id);
    
    // Custom course registration (for mods)
    void register_custom_course(int course_id, std::unique_ptr<Course> course);
    
    // Progress tracking
    void save_course_completion(int course_id, int laps, float time);
    int get_best_time(int course_id) const;
    void reset_progress();
    
    // Wave level configuration
    void set_wave_level(int course_id, Difficulty diff, float level);
    float get_wave_level(int course_id, Difficulty diff) const;
    
    // Lap count configuration
    void set_laps(int course_id, Difficulty diff, int laps);
    int get_laps(int course_id, Difficulty diff) const;
    
private:
    CourseManager() = default;
    
    int m_current_course = 0;
    std::unordered_map<int, int> m_best_times;
    std::unordered_map<int, std::unique_ptr<Course>> m_courses;
    std::unordered_map<int, std::array<float, 3>> m_wave_levels;
    std::unordered_map<int, std::array<int, 3>> m_laps;
};

} // namespace wr64::courses