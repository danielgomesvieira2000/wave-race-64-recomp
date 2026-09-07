#include "wr64/textures.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#include <unknwn.h>
#include <objidl.h>
#endif

#include "hle/rt64_application.h"
#include "hle/rt64_state.h"
#include "hle/rt64_workload_queue.h"
#include "render/rt64_texture_cache.h"

namespace wr64::textures {
namespace {
std::atomic<bool> requested_enabled{true};

struct Overrides {
    std::filesystem::path dump;
    std::filesystem::path pack;
    Overrides() {
        if (const char* value = std::getenv("WR64_TEXTURE_DUMP"); value && *value) dump = value;
        if (const char* value = std::getenv("WR64_TEXTURE_PACK"); value && *value) pack = value;
    }
};

const Overrides& overrides() {
    static const Overrides value;
    return value;
}

size_t resolved_count(const RT64::TextureCache& cache) {
    size_t count = 0;
    for (const auto& paths : cache.textureMap.replacementMap.fileSystemResolvedPaths) count += paths.size();
    return count;
}
}

void set_enabled(bool enabled) {
    requested_enabled.store(enabled, std::memory_order_relaxed);
}

void apply(RT64::Application& app) {
    if (!app.textureCache) return;
    auto& cache = *app.textureCache;
    const bool enabled = !overrides().pack.empty() || requested_enabled.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(cache.textureMapMutex);
        if (cache.textureMap.replacementMapEnabled == enabled) return;
    }
    // A workload snapshots descriptors before resolving individual GPU tiles.
    // Keep those reads on one mode. Never wait here: a busy workload can itself
    // be waiting for a present event that this task thread still needs to send.
    std::unique_lock<std::mutex> workload_lock;
    if (app.workloadQueue) {
        workload_lock = std::unique_lock(app.workloadQueue->threadMutex, std::try_to_lock);
        if (!workload_lock.owns_lock()) return;
    }
    // Match the worker's lock order: workload thread, then texture map.
    std::lock_guard lock(cache.textureMapMutex);
    if (cache.textureMap.replacementMapEnabled != enabled) {
        cache.textureMap.replacementMapEnabled = enabled;
        std::fprintf(stderr, "[textures] %s textures selected; %zu replacement mappings available, %zu cached replacement textures%s\n",
            enabled ? "HD" : "original", resolved_count(cache), cache.textureMap.replacementMap.loadedTextureMap.size(),
            overrides().pack.empty() ? "" : " (process override)");
    }
}

void setup(RT64::Application& app, const std::filesystem::path& settings_directory) {
    const auto& options = overrides();
    if (!app.state || !app.textureCache) return;

    // Configure dumping before the first display list so menus and boot assets
    // are included. Original TMEM, palette data and tile metadata stay intact.
    if (!options.dump.empty()) {
        std::error_code error;
        std::filesystem::create_directories(options.dump, error);
        if (!error && std::filesystem::is_directory(options.dump, error)) {
            app.state->dumpingTexturesDirectory = options.dump;
            app.state->textureManager.dumpedSet.clear();
            std::fprintf(stderr, "[textures] dumping original TMEM and tile metadata to %s\n", options.dump.string().c_str());
        } else {
            std::fprintf(stderr, "[textures] cannot create dump directory %s: %s\n",
                options.dump.string().c_str(), error ? error.message().c_str() : "not a directory");
        }
    }

    // main() changes to the executable directory before initializing the
    // frontend. Match the other bundled assets on each release platform, and
    // also support an unpackaged macOS executable with adjacent assets.
    std::filesystem::path bundled_pack = "assets/textures/nano-banana-2";
#if defined(__APPLE__)
    const std::filesystem::path resources_pack = "../Resources/assets/textures/nano-banana-2";
    std::error_code resources_error;
    if (std::filesystem::exists(resources_pack, resources_error)) bundled_pack = resources_pack;
#endif
    const auto pack = resolve_pack_path(settings_directory, bundled_pack, options.pack);
    std::error_code error;
    const bool exists = std::filesystem::exists(pack, error);
    if (exists && !error) {
        try {
            if (app.textureCache->loadReplacementDirectory(RT64::ReplacementDirectory(pack))) {
                std::lock_guard lock(app.textureCache->textureMapMutex);
                const size_t count = resolved_count(*app.textureCache);
                std::fprintf(stderr, "[textures] loaded pack %s: %zu replacement mappings%s\n",
                    pack.string().c_str(), count, count ? "" : " (no matching PNG/DDS files resolved)");
            } else {
                std::fprintf(stderr, "[textures] failed to load pack %s; original textures remain available\n", pack.string().c_str());
            }
        } catch (const std::exception& exception) {
            std::fprintf(stderr, "[textures] failed to load pack %s: %s\n", pack.string().c_str(), exception.what());
        }
    } else if (error || !options.pack.empty()) {
        std::fprintf(stderr, "[textures] cannot load pack %s: %s\n", pack.string().c_str(),
            error ? error.message().c_str() : "path does not exist");
    } else {
        std::fprintf(stderr, "[textures] no HD pack at %s; original textures available\n", pack.string().c_str());
    }
    apply(app);
}
}
