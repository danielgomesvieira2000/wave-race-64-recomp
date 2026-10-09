// verify_course_loader.cpp - Standalone test for the JSON course loader
#include <iostream>
#include <cassert>
#include <filesystem>
#include <memory>
#include <cstdlib>
#include "nlohmann/json.hpp"
#include "wr64/courses.hpp"

using namespace wr64::courses;

// Resolve the course file path: check WR64_ASSET_DIR env var first (set by
// CMake during test execution), then fall back to relative path for
// standalone invocation from the repo root.
static std::string resolve_asset_path(const std::string& relative) {
    const char* env = std::getenv("WR64_ASSET_DIR");
    if (env != nullptr) {
        return std::string(env) + "/" + relative;
    }
    return relative;
}

int main() {
    // Test 1: Load Sunset Harbor from JSON
    std::cout << "=== Test 1: Load Sunset Harbor from JSON ===\n";
    std::string course_path = resolve_asset_path("courses/sunset_harbor.json");
    auto course = Course::load_from_file(course_path);
    assert(course != nullptr && "Failed to load Sunset Harbor course");
    assert(course->is_loaded() && "Course not marked as loaded");
    
    const auto& params = course->params();
    std::cout << "  name:            " << params.name << "\n";
    std::cout << "  internal_name:   " << params.internal_name << "\n";
    std::cout << "  id:              " << params.id << "\n";
    std::cout << "  draw_distance:   " << params.draw_distance << "\n";
    std::cout << "  buoy_limit:      " << params.buoy_limit << "\n";
    std::cout << "  difficulty:      " << params.difficulty << "\n";
    std::cout << "  bounds:          x[" << params.bounds.x_min << ", " << params.bounds.x_max 
              << "] z[" << params.bounds.z_min << ", " << params.bounds.z_max << "]\n";
    std::cout << "  start_positions: " << params.start_positions.size() << " entries\n";
    std::cout << "  buoys:           " << params.buoys.size() << " entries\n";
    std::cout << "  gates:           " << params.gates.size() << " entries\n";
    
    assert(params.id == 10 && "Course id should be 10");
    assert(params.name == "Sunset Harbor");
    assert(params.internal_name == "SUNSET_HARBOR");
    assert(params.draw_distance == 6000.0f);
    assert(params.buoy_limit == 32);
    assert(params.start_positions.size() == 4);
    assert(params.buoys.size() == 12);
    assert(params.gates.size() == 3);
    
    std::cout << "  PASSED\n\n";
    
    // Test 2: Verify start positions
    std::cout << "=== Test 2: Verify start positions ===\n";
    for (size_t i = 0; i < params.start_positions.size(); ++i) {
        const auto& sp = params.start_positions[i];
        std::cout << "  Rider " << i << ": x=" << sp.x << " z=" << sp.z 
                  << " heading=" << sp.heading << "\n";
    }
    assert(params.start_positions[0].x == 0.0f);
    assert(params.start_positions[0].z == -128.0f);
    assert(params.start_positions[1].x == -96.0f);
    std::cout << "  PASSED\n\n";
    
    // Test 3: Verify wave parameters
    std::cout << "=== Test 3: Verify wave parameters ===\n";
    std::cout << "  Normal:  wave=" << params.wave_params[0].wave_level 
              << " vis=" << params.wave_params[0].visibility_range << "\n";
    std::cout << "  Hard:    wave=" << params.wave_params[1].wave_level 
              << " vis=" << params.wave_params[1].visibility_range << "\n";
    std::cout << "  Expert:  wave=" << params.wave_params[2].wave_level 
              << " vis=" << params.wave_params[2].visibility_range << "\n";
    assert(params.wave_params[0].wave_level == 1.0f);
    assert(params.wave_params[1].wave_level == 1.5f);
    assert(params.wave_params[2].wave_level == 2.0f);
    std::cout << "  PASSED\n\n";
    
    // Test 4: Verify race settings
    std::cout << "=== Test 4: Verify race settings ===\n";
    std::cout << "  Laps:         normal=" << params.laps[0] 
              << " hard=" << params.laps[1] 
              << " expert=" << params.laps[2] << "\n";
    std::cout << "  Power misses: normal=" << params.power_misses[0] 
              << " hard=" << params.power_misses[1] 
              << " expert=" << params.power_misses[2] << "\n";
    assert(params.laps[0] == 3);
    assert(params.power_misses[0] == 5);
    assert(params.power_misses[2] == 5);
    std::cout << "  PASSED\n\n";
    
    // Test 5: CourseManager - register and retrieve custom course
    std::cout << "=== Test 5: CourseManager registration ===\n";
    CourseManager& mgr = CourseManager::instance();
    mgr.register_custom_course(10, std::move(course));
    
    auto retrieved = mgr.get_course(10);
    assert(retrieved != nullptr && "Failed to retrieve registered course");
    assert(retrieved->params().name == "Sunset Harbor");
    std::cout << "  Registered and retrieved: " << retrieved->params().name << "\n";
    
    mgr.set_current_course(10);
    std::cout << "  Current course: " << mgr.current_course() << "\n";
    assert(mgr.current_course() == 10);
    
    auto available = mgr.available_courses();
    std::cout << "  Available courses: " << available.size() << " (including builtins)\n";
    bool has_custom = false;
    for (int id : available) {
        if (id == 10) has_custom = true;
    }
    assert(has_custom && "Custom course not in available list");
    std::cout << "  PASSED\n\n";
    
    // Test 6: Wave level configuration
    std::cout << "=== Test 6: Wave level configuration ===\n";
    mgr.set_wave_level(10, Difficulty::NORMAL, 1.5f);
    mgr.set_wave_level(10, Difficulty::HARD, 2.0f);
    mgr.set_wave_level(10, Difficulty::EXPERT, 3.0f);
    
    assert(mgr.get_wave_level(10, Difficulty::NORMAL) == 1.5f);
    assert(mgr.get_wave_level(10, Difficulty::HARD) == 2.0f);
    assert(mgr.get_wave_level(10, Difficulty::EXPERT) == 3.0f);
    std::cout << "  Normal wave: " << mgr.get_wave_level(10, Difficulty::NORMAL) << "\n";
    std::cout << "  Hard wave:   " << mgr.get_wave_level(10, Difficulty::HARD) << "\n";
    std::cout << "  Expert wave: " << mgr.get_wave_level(10, Difficulty::EXPERT) << "\n";
    std::cout << "  PASSED\n\n";
    
    // Test 7: Progress tracking
    std::cout << "=== Test 7: Progress tracking ===\n";
    mgr.save_course_completion(10, 3, 52.45f);
    int best = mgr.get_best_time(10);
    std::cout << "  Best time for Sunset Harbor: " << best << " (frames/100ms units)\n";
    assert(best > 0);
    
    mgr.save_course_completion(10, 3, 48.30f);
    best = mgr.get_best_time(10);
    std::cout << "  Updated best time: " << best << "\n";
    assert(best == 48);
    std::cout << "  PASSED\n\n";
    
    // Test 8: Builtin course loading
    std::cout << "=== Test 8: Builtin course loading ===\n";
    auto builtin = Course::load(2); // SUNSET_BAY
    assert(builtin != nullptr);
    assert(builtin->params().name == "Sunset Bay");
    assert(builtin->params().draw_distance == 6000.0f);
    std::cout << "  Loaded builtin course: " << builtin->params().name << "\n";
    std::cout << "  Draw distance: " << builtin->params().draw_distance << "\n";
    std::cout << "  Wave level (normal): " << builtin->params().wave_params[0].wave_level << "\n";
    std::cout << "  PASSED\n\n";
    
    // Test 9: JSON file not found
    std::cout << "=== Test 9: Nonexistent file ===\n";
    auto nonexistent = Course::load_from_file("assets/courses/nonexistent.json");
    assert(nonexistent == nullptr);
    std::cout << "  Correctly returned nullptr for nonexistent file\n";
    std::cout << "  PASSED\n\n";
    
    std::cout << "=== ALL TESTS PASSED ===\n";
    return 0;
}