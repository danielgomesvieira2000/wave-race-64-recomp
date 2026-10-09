# Task 3: Projeto de Camada de Abstração (API C++ para modding)

## Objetivo
Definir interfaces C++ para substituir chamadas de sistema do N64 e permitir modding facilitado.

## Arquitetura Proposta

### Componentes Principais

```
wr64::engine/          # Game engine core
wr64::assets/          # Asset management
wr64::physics/         # Physics simulation
wr64::graphics/        # Graphics abstraction (RT64 -> custom)
wr64::audio/           # Audio system (RSP microcode -> custom)
wr64::network/         # Multiplayer networking
wr64::modding/         # Mod loading system
wr64::courses/         # Course management
```

---

## API Proposta

### 1. GameEngine - Core do jogo

```cpp
// include/wr64/engine.hpp
#pragma once
#include <memory>
#include <functional>

namespace wr64 {

class GameEngine {
public:
    static GameEngine& instance();
    
    // Lifecycle
    void initialize();
    void shutdown();
    bool is_running() const;
    
    // Main loop
    void update(float delta_time);
    void render();
    
    // Game state
    enum class State {
        BOOT, TITLE, MAIN_MENU, COURSE_SELECT,
        RACE_PREPARE, RACE, PAUSE, RESULTS, OPTIONS
    };
    
    State get_state() const;
    void set_state(State new_state);
    
    // Frame timing
    float get_frame_time() const;  // Game frame time (20Hz)
    float get_real_time() const;  // Real time for interpolation
    
private:
    GameEngine() = default;
    ~GameEngine();
    GameEngine(const GameEngine&) = delete;
    GameEngine& operator=(const GameEngine&) = delete;
};

// Frame callback for integrations
using FrameCallback = std::function<void(float delta_time)>;
void GameEngine::set_frame_callback(FrameCallback callback);

} // namespace wr64
```

### 2. AssetManager - Gerenciamento de assets

```cpp
// include/wr64/assets.hpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace wr64 {

enum class TextureFormat {
    CI4, CI8, RGBA16, RGBA32, IA4, IA8, IA16
};

struct TextureInfo {
    uint32_t address;        // VRAM address
    TextureFormat format;
    int width, height;
    bool is_tlut;
};

struct ModelInfo {
    uint32_t vlist_address;  // Geometry data
    std::vector<uint32_t> mat_addresses;  // Matrix slots
    int vertex_count;
    int triangle_count;
};

class AssetManager {
public:
    static AssetManager& instance();
    
    // Texture loading
    std::shared_ptr<TextureInfo> load_texture(const std::string& name);
    bool has_texture(const std::string& name) const;
    
    // Model loading
    std::shared_ptr<ModelInfo> load_model(const std::string& name);
    
    // Course data
    struct CourseData {
        std::string name;
        std::string filename;  // .z64 or custom course file
        std::vector<std::string> textures;
        std::vector<std::string> models;
        float draw_distance;
        struct StartPos { float x, z, heading; } start_positions[8];
    };
    
    std::shared_ptr<CourseData> load_course(int course_id);
    
    // Custom course registration (for mods)
    void register_custom_course(int course_id, const CourseData& course);
    
    // Asset hot reload for development
    void enable_hot_reload(bool enable);
    
private:
    AssetManager() = default;
    ~AssetManager();
    
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wr64
```

### 3. PhysicsEngine - Física do jogo

```cpp
// include/wr64/physics.hpp
#pragma once
#include <cmath>

namespace wr64 {

class PhysicsEngine {
public:
    static PhysicsEngine& instance();
    
    // Craft simulation
    struct CraftState {
        float x, z, y;           // Position
        float vx, vz, vy;        // Velocity
        float heading;           // Facing (radians)
        float speed;             // Speed (HUD display * 1.8)
        float throttle;          // 0.0 - 1.0
        float weight_on_bow;     // Front/rear weight distribution
    };
    
    // Water physics
    struct WaterSurface {
        float height_at(float x, float z) const;
        float slope_at(float x, float z) const;
        float velocity_field(float x, float z) const;  // Wave velocity
    };
    
    // Buoyancy calculation (replaces func_8004D30C)
    float calculate_buoyancy(const CraftState& craft, const WaterSurface& water);
    
    // Update craft physics
    void update_craft(CraftState& craft, float delta_time, const WaterSurface& water);
    
    // Wave rider interaction
    struct RiderState {
        float x, z, y;
        float vx, vz;
        int animation_state;  // 0x17 = crash
    };
    
    void update_rider(RiderState& rider, const CraftState& craft, float delta_time);
    
private:
    PhysicsEngine() = default;
};

} // namespace wr64
```

### 4. CourseManager - Gerenciamento de pistas

```cpp
// include/wr64/courses.hpp
#pragma once
#include <vector>
#include <memory>
#include <string>

namespace wr64 {

class Course {
public:
    struct CourseParams {
        int id;
        std::string name;
        std::string internal_name;
        float draw_distance = 6000.0f;  // Units
        int buoy_limit = 32;            // Per view
        float difficulty = 1.0f;        
        
        // Course-specific data
        std::vector<std::string> texture_files;
        std::vector<std::string> model_files;
        std::vector<std::string> audio_files;
        
        // Course geometry metadata (extracted from game)
        struct GeometryBounds {
            float x_min, x_max, z_min, z_max;
            float water_level;
        } bounds;
    };
    
    static std::unique_ptr<Course> load(int course_id);
    static std::unique_ptr<Course> load_from_file(const std::string& path);
    
    const CourseParams& params() const { return m_params; }
    
    // Course data accessors
    const std::vector<uint16_t>& buoy_positions() const;
    const std::vector<uint16_t>& gate_markers() const;
    
    // For custom courses/mods
    void add_buoys(const std::vector<CourseParams::GeometryBounds>& positions);
    
private:
    CourseParams m_params;
    Course(const CourseParams& params);
};

class CourseManager {
public:
    static CourseManager& instance();
    
    int current_course() const;
    void set_current_course(int id);
    
    std::vector<int> available_courses() const;
    
    // Save/load course progress
    void save_course_completion(int course_id, int laps, float time);
    int get_best_time(int course_id) const;
    
private:
    CourseManager() = default;
    int m_current_course = 0;
    std::unordered_map<int, int> m_best_times;
};

} // namespace wr64
```

### 5. NetworkManager - Multiplayer

```cpp
// include/wr64/network.hpp
#pragma once
#include <string>
#include <functional>

namespace wr64 {

class NetworkManager {
public:
    static NetworkManager& instance();
    
    // Session management
    enum class HostMode { LOCAL, LAN, ONLINE };
    
    struct SessionInfo {
        std::string name;
        std::string host;
        int port = 14000;
        int player_count = 1;
        int max_players = 4;
        bool is_host = false;
    };
    
    // Create/Join
    bool create_session(const SessionInfo& info);
    bool join_session(const SessionInfo& info);
    void close_session();
    
    // State synchronization
    struct PlayerState {
        int slot;              // Player slot (0-3)
        float x, z, y;         // Position
        float heading;         // Facing
        float speed;
        int lap;
        int buoys_passed;
        bool is_racing;        // In race vs menu
    };
    
    // Input broadcasting
    struct InputState {
        int slot;
        uint8_t buttons;       // Bitfield of button states
        float analog_x, analog_y;  // -1.0 to 1.0
    };
    
    void broadcast_input(const InputState& input);
    void set_input_handler(std::function<void(const InputState&)> handler);
    
    // Receive remote player states
    PlayerState get_remote_state(int slot) const;
    
    // Events
    std::function<void()> on_session_started;
    std::function<void()> on_session_ended;
    std::function<void(int slot, const PlayerState&)> on_player_joined;
    
private:
    NetworkManager() = default;
};

} // namespace wr64
```

### 6. ModLoader - Sistema de mods

```cpp
// include/wr64/modding.hpp
#pragma once
#include <string>
#include <vector>
#include <unordered_map>

namespace wr64 {

class Mod {
public:
    struct Metadata {
        std::string name;
        std::string version;
        std::string author;
        std::string description;
        std::vector<std::string> dependencies;
    };
    
    Metadata metadata;
    std::string path;  // Full path to mod directory
    
    // Registration callbacks
    std::function<void()> on_load;
    std::function<void()> on_unload;
};

class ModLoader {
public:
    static ModLoader& instance();
    
    // Mod discovery
    std::vector<Mod> scan_mod_directories();
    
    // Mod loading
    bool load_mod(const std::string& mod_path);
    void unload_mod(int mod_id);
    
    // Mod events
    std::function<void(int mod_id, const Mod& mod)> on_mod_loaded;
    std::function<void(int mod_id)> on_mod_unloaded;
    
    // Course override (for new courses)
    void register_course_override(int original_id, int new_id);
    
    // Texture pack support
    void load_texture_pack(const std::string& pack_path);
    
    // Event hooks
    using CourseLoadHook = std::function<void(int course_id)>;
    using BeforeRaceHook = std::function<void(int course_id, int num_players)>;
    using AfterRaceHook = std::function<void(int course_id, int winner_slot)>;
    
    void register_course_load_hook(CourseLoadHook hook);
    void register_before_race_hook(BeforeRaceHook hook);
    void register_after_race_hook(AfterRaceHook hook);
    
private:
    ModLoader() = default;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wr64
```

---

## Integração com Código Existente

### Mapeando funções do N64 para APIs C++

| Função N64 | API C++ Equivalente | Arquivo |
|------------|---------------------|---------|
| `func_8004D30C` (water height) | `PhysicsEngine::WaterSurface::height_at()` | `physics.hpp` |
| `func_8006E674` (buoys culling) | `PhysicsEngine::detect_visible_buoys()` | `physics.hpp` |
| `func_80092CF0` (renderer dispatch) | `GameEngine::render()` + RT64 abstraction | `engine.hpp`, `graphics.hpp` |
| `func_800922E4` (per-frame update) | `GameEngine::update()` | `engine.hpp` |
| Overlay loading | `AssetManager::load_course()` | `assets.hpp` |

### Estratégia de migração gradual

1. **Fase 1**: Mantém código recompilado, adiciona wraps em C++ para novas funcionalidades
2. **Fase 2**: Substitui funções críticas uma por uma usando hooks
3. **Fase 3**: Reimplementa sistema de assets em C++ puro
4. **Fase 4**: Reimplementa física e UI em C++

---

## Próximos Passos

1. Criar implementação stub de `GameEngine`
2. Integrar `AssetManager` com sistema existente de texturas
3. Implementar `PhysicsEngine` com referência a GAME-INTERNALS.md
4. Criar testes de unidade para cada componente
5. Documentar API para community modding