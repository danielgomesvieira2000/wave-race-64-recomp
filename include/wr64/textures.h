#pragma once

#include <filesystem>
#include <system_error>

namespace RT64 { struct Application; }

namespace wr64::textures {
// An explicitly selected pack (including an invalid one) wins, so a broken
// override is reported instead of silently displaying unrelated artwork.
// Installed packs retain the same directory/ZIP support as RT64.
inline std::filesystem::path resolve_pack_path(
        const std::filesystem::path& settings_directory,
        const std::filesystem::path& bundled_directory,
        const std::filesystem::path& explicit_pack = {}) {
    if (!explicit_pack.empty()) return explicit_pack;
    const auto installed = settings_directory / "textures" / "nano-banana-2";
    std::error_code error;
    if (std::filesystem::exists(installed, error) || error) return installed;
    return bundled_directory;
}

// UI callbacks only publish a preference. RT64 is updated on its task thread.
void set_enabled(bool enabled);
void setup(RT64::Application& app, const std::filesystem::path& settings_directory);
void apply(RT64::Application& app);
}
