// Native C++ entry point for an independent Wave Race 64 port.
//
// This file provides a ROM-independent entry point that initializes the game
// using only native C++ code. It demonstrates that the port can run without
// the recompiled MIPS code or a ROM file.
//
// Build with: -DWR64_BUILD_NATIVE=ON

#include <SDL2/SDL.h>
#define SDL_MAIN_HANDLED
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wr64/engine/gamemain.hpp"
#include "wr64/courses.hpp"

using namespace wr64::engine;
using namespace wr64::courses;

// Simple game state for the native build
struct GameState {
    bool running = true;
    int frame_count = 0;
    WaveField wave_field;
};

// Initialize the game state
static void init_game(GameState& state) {
    // Initialize wave field with flat water (could load from file later)
    state.wave_field = WaveField();
    
    printf("[native] Game state initialized\n");
    printf("[native] Wave field: %ux%u entries\n", kFieldRows, kFieldCols);
}

// Process one game frame
static void update_frame(GameState& state) {
    // For now, just count frames
    state.frame_count++;
    
    // Could add physics simulation, water updates, etc.
    // The WaveField::water_height_at() function is ready for use
}

// Test the native water height function
static void test_water_heights(GameState& state) {
    printf("\n[native] Testing water height queries:\n");
    
    // Query a few sample points
    for (int i = 0; i < 5; ++i) {
        float x = static_cast<float>(i * 100);
        float z = static_cast<float>(i * 100);
        float height = water_height_at(state.wave_field, x, z);
        printf("  pos(%.0f, %.0f) -> height = %.6f\n", x, z, height);
    }
}

// Native main loop
static int native_main_loop() {
    printf("\n=== Wave Race 64 Native C++ Port ===\n\n");
    
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    
    // Create window
    SDL_Window* window = SDL_CreateWindow(
        "Wave Race 64 - Native C++ Port",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1024, 768,
        SDL_WINDOW_SHOWN
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    
    // Create renderer
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    
    // Initialize game state
    GameState state;
    init_game(state);
    
    // Test course loading
    printf("\n[native] Testing course loader:\n");
    auto course = Course::load_from_file("assets/courses/sunset_harbor.json");
    if (course) {
        printf("  Loaded: %s (id=%d)\n", 
               course->params().name.c_str(), 
               course->params().id);
    } else {
        printf("  Could not load course file (expected in standalone mode)\n");
    }
    
    // Test water heights on flat field
    test_water_heights(state);
    
    // Main loop
    bool running = true;
    Uint32 last_time = SDL_GetTicks();
    
    printf("\n[native] Entering main loop (close window to exit)\n");
    while (running) {
        // Handle events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }
        
        // Update
        update_frame(state);
        
        // Render (clear to blue - water color)
        SDL_SetRenderDrawColor(renderer, 64, 160, 255, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        
        // Frame timing
        Uint32 current_time = SDL_GetTicks();
        if (current_time - last_time < 16) {  // ~60 FPS
            SDL_Delay(16 - (current_time - last_time));
        }
        last_time = current_time;
    }
    
    // Cleanup
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    printf("\n[native] Game exited cleanly\n");
    return 0;
}

// Native entry point - called when WR64_BUILD_NATIVE=ON
// This allows building the port without any ROM file or recompiled MIPS code
#ifdef WR64_BUILD_NATIVE

// Windows entry point stub: the mingw CRT calls WinMain, which then calls main()
#ifdef _WIN32
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(0, nullptr);
}
#endif

int main(int argc, char* argv[]) {
    printf("=== Wave Race 64 C++ Native Port ===\n");
    printf("Build: WR64_BUILD_NATIVE\n\n");
    return native_main_loop();
}

#endif // WR64_BUILD_NATIVE