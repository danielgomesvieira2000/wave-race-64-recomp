// Native C++ game state manager - independent port
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <array>

#include "wr64/engine/gamemain.hpp"

namespace wr64::engine {

// Game state enum
enum class GameState {
    BOOT,
    TITLE_SCREEN,
    MAIN_MENU,
    COURSE_SELECT,
    RACE_LOADING,
    RACING,
    RESULTS,
    OPTIONS,
    PAUSED
};

// Player configuration
struct PlayerConfig {
    int slot = 0;
    bool human = true;
    int ai_difficulty = 1;  // 0=easy, 1=normal, 2=hard
};

// Racer state (simplified from N64 internals)
struct RacerState {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float heading = 0.0f;
    float speed = 0.0f;
    float roll = 0.0f;
    int lap = 0;
    int buoys_passed = 0;
    int power_misses = 0;
    bool finished = false;
    bool retired = false;
};

// Native game state manager - replaces the recompiled Main_Thread
class GameEngine {
public:
    GameEngine();
    ~GameEngine() = default;
    
    // Lifecycle
    bool initialize();
    void shutdown();
    void update(float dt);
    
    // State management
    GameState state() const { return m_state; }
    void transition_to(GameState new_state);
    
    // Rendering
    void render();
    
    // Water physics
    float query_water_height(float x, float z) const;
    void update_water_field(float dt);
    
    // Player management
    RacerState& player(int slot);
    int num_players() const { return static_cast<int>(m_players.size()); }
    
    // Game flow
    void start_race();
    void reset_race();
    
private:
    GameState m_state = GameState::BOOT;
    WaveField m_wave_field;
    std::array<RacerState, 4> m_players;
    int m_active_players = 1;
    float m_game_time = 0.0f;
    int m_current_course = 0;
};

} // namespace wr64::engine