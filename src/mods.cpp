// Mod content types this port adds. See include/wr64/mods.h for what and why.

#include "wr64/mods.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <librecomp/mods.hpp>

// Added to RT64 by tools/patch_rt64_texturepacks.py. Records the list and lets
// RT64 apply it on its next frame, because a mod can be switched on while the
// game is running and loading a pack rebuilds the texture cache.
extern "C" void RT64_SetTexturePacks(const char* const* paths, int count);

namespace wr64::mods {
namespace {

// The texture-pack mods currently switched on, by mod id, with the file each was
// read from. Small by construction -- one entry per enabled pack.
std::mutex g_mutex;
std::vector<std::pair<std::string, std::string>> g_packs;

// Hands RT64 the enabled packs in mod order, so that a mod further down the list
// wins where two replace the same texture -- the order the Mods tab shows, and
// the order a player would expect from it.
void publish() {
    std::vector<std::pair<std::string, std::string>> packs;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        packs = g_packs;
    }

    std::sort(packs.begin(), packs.end(),
              [](const auto& a, const auto& b) {
                  return recomp::mods::get_mod_order_index(a.first) <
                         recomp::mods::get_mod_order_index(b.first);
              });

    std::vector<const char*> paths;
    paths.reserve(packs.size());
    for (const auto& pack : packs) {
        paths.push_back(pack.second.c_str());
    }

    RT64_SetTexturePacks(paths.empty() ? nullptr : paths.data(), static_cast<int>(paths.size()));
    std::fprintf(stderr, "[wr64] texture packs: %zu enabled\n", paths.size());
    std::fflush(stderr);
}

void on_pack_enabled(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    const std::string id = mod.manifest.mod_id;
    const std::string path = mod.manifest.mod_root_path.string();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = std::find_if(g_packs.begin(), g_packs.end(),
                                     [&id](const auto& pack) { return pack.first == id; });
        if (it != g_packs.end()) {
            it->second = path;
        }
        else {
            g_packs.emplace_back(id, path);
        }
    }
    std::fprintf(stderr, "[wr64] texture pack on: %s (%s)\n", id.c_str(), path.c_str());
    publish();
}

void on_pack_disabled(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    const std::string id = mod.manifest.mod_id;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_packs.erase(std::remove_if(g_packs.begin(), g_packs.end(),
                                     [&id](const auto& pack) { return pack.first == id; }),
                      g_packs.end());
    }
    std::fprintf(stderr, "[wr64] texture pack off: %s\n", id.c_str());
    publish();
}

void on_packs_reordered(recomp::mods::ModContext&) {
    publish();
}

}  // namespace

void register_content_types() {
    // The filename is the whole declaration: a mod carrying rt64.json is a
    // texture pack, and needs nothing in its manifest to say so. rt64.json is
    // RT64's own name for a replacement database, so a pack built with RT64's
    // texture dumper is already in the right shape -- add a manifest.json beside
    // it and zip it as .nrm.
    //
    // allow_runtime_toggle is what lets a pack be switched on and off without
    // restarting; RT64 rebuilds its texture cache when the list changes.
    recomp::mods::register_mod_content_type(recomp::mods::ModContentType{
        .content_filename = "rt64.json",
        .allow_runtime_toggle = true,
        .on_enabled = on_pack_enabled,
        .on_disabled = on_pack_disabled,
        .on_reordered = on_packs_reordered,
    });
    std::fprintf(stderr, "[wr64] mod content type registered: rt64.json (texture pack)\n");
    std::fflush(stderr);
}

}  // namespace wr64::mods
