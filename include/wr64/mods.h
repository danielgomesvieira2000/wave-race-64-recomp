#pragma once

// Mod content this port understands, beyond the code and ROM patches librecomp
// handles on its own.
//
// A mod is a `.nrm`: a zip with a `mod.json` and whatever content it
// carries. librecomp decides what a mod *is* by looking for a file inside it, so
// a port adds a format by naming a file and saying what to do when a mod holding
// one is switched on.
//
// **Texture packs** (`rt64.json`). RT64 has a complete replacement-texture
// system -- a pack is `rt64.json` plus the images, keyed by the hash of the
// texture each stands in for -- and it reads a pack straight out of a zip
// without unpacking it, so a mod file is a pack as it stands. What RT64 has no
// way to do is be told which packs to load by the program embedding it; the
// only path in is a file dialog in its developer UI. tools/patch_rt64_texturepacks.py
// adds the one function that fixes that, and this drives it.
//
// Packs are applied in mod order, so a mod later in the list wins where two
// replace the same texture, and they can be switched on and off while the game
// is running.

namespace wr64::mods {

// Registers this port's mod content types. Must run before librecomp scans the
// mods folder -- that happens inside recomp::start -- because a mod's content is
// detected when it is opened.
void register_content_types();

}  // namespace wr64::mods
