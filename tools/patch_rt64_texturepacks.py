"""Let the port hand RT64 a list of texture packs to load.

RT64 has a complete replacement-texture system -- a pack is a directory or a zip
holding `rt64.json` and the images, keyed by the hash of the texture they stand
in for -- and it can load one from a zip without unpacking it, which is what
makes a mod file usable as a pack directly.

What it has no way to do is be *told* which packs to load by the program
embedding it. The only path in is the Textures tab of the developer UI, behind a
file dialog. So this adds one function:

    extern "C" void RT64_SetTexturePacks(const char *const *paths, int count);

It records the list and sets a flag; `State::updateScreen` applies it on the next
frame, on the same thread the developer UI would have called from. Passing zero
paths unloads everything, which is what a mod being disabled needs.

Applying on the next frame rather than immediately matters: a mod can be enabled
while the game is running, from a different thread, and `loadReplacementDirectories`
rebuilds the texture cache.

Scripted and idempotent because it patches a submodule: a submodule update would
otherwise revert it silently. Chained into tools/patch_rt64.py.

Run from the repository root:
    python tools/patch_rt64_texturepacks.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "lib" / "RT64" / "src" / "hle" / "rt64_state.cpp"

MARKER = "RT64_SetTexturePacks"

DECL_ANCHOR = """// Set by the port to draw its own window inside RT64's inspector UI, with an
// ImGui frame already open. See tools/patch_rt64_inspector.py."""

DECL_REPLACEMENT = """// The texture packs the port wants loaded, applied on the next frame by
// State::updateScreen. See tools/patch_rt64_texturepacks.py.
namespace {
    std::mutex RT64_TexturePackMutex;
    std::vector<std::string> RT64_TexturePackPaths;
    bool RT64_TexturePacksDirty = false;
}

extern "C" void RT64_SetTexturePacks(const char *const *paths, int count) {
    const std::lock_guard<std::mutex> lock(RT64_TexturePackMutex);
    RT64_TexturePackPaths.clear();
    for (int i = 0; i < count; i++) {
        if (paths[i] != nullptr) {
            RT64_TexturePackPaths.emplace_back(paths[i]);
        }
    }
    RT64_TexturePacksDirty = true;
}

// Set by the port to draw its own window inside RT64's inspector UI, with an
// ImGui frame already open. See tools/patch_rt64_inspector.py."""

APPLY_ANCHOR = """    void State::updateScreen(const VI &newVI, bool fromEarlyPresent) {"""

APPLY_REPLACEMENT = """    void State::updateScreen(const VI &newVI, bool fromEarlyPresent) {
        // Texture packs the port asked for, applied here because this runs once
        // a frame on a thread that may touch the texture cache. An empty list
        // unloads what is there. See tools/patch_rt64_texturepacks.py.
        {
            std::vector<std::string> packPaths;
            bool packsDirty = false;
            {
                const std::lock_guard<std::mutex> lock(RT64_TexturePackMutex);
                if (RT64_TexturePacksDirty) {
                    packPaths = RT64_TexturePackPaths;
                    RT64_TexturePacksDirty = false;
                    packsDirty = true;
                }
            }

            if (packsDirty && (ext.textureCache != nullptr)) {
                std::vector<ReplacementDirectory> replacementDirectories;
                replacementDirectories.reserve(packPaths.size());
                for (const std::string &packPath : packPaths) {
                    replacementDirectories.emplace_back(ReplacementDirectory(std::filesystem::u8path(packPath)));
                }

                ext.textureCache->loadReplacementDirectories(replacementDirectories);
                fprintf(stdout, "RT64: %zu texture pack(s) loaded by the port.\\n", replacementDirectories.size());
            }
        }
"""


def main():
    if not TARGET.exists():
        sys.exit(f"missing {TARGET}\\nRun: git submodule update --init --recursive")

    text = TARGET.read_text(encoding="utf-8")

    if MARKER in text:
        print(f"  {TARGET.name}: texture pack setter already patched")
        return

    if DECL_ANCHOR not in text:
        sys.exit(f"the inspector hook was not found in {TARGET}; run "
                 f"tools/patch_rt64_inspector.py first, or upstream has changed "
                 f"and this patch needs revisiting")
    if APPLY_ANCHOR not in text:
        sys.exit(f"State::updateScreen was not found in {TARGET}; upstream has "
                 f"changed and this patch needs revisiting")

    text = text.replace(DECL_ANCHOR, DECL_REPLACEMENT, 1)
    text = text.replace(APPLY_ANCHOR, APPLY_REPLACEMENT, 1)
    TARGET.write_text(text, encoding="utf-8")
    print(f"  {TARGET.name}: texture pack setter patched")


if __name__ == "__main__":
    main()
