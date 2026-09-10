# Mods

A mod is a `.nrm`: a zip with a `mod.json` at its root and whatever content it
carries. The game scans `%LOCALAPPDATA%\WaveRace64Recomp\mods` for them, and the
**Mods** tab lists what it found, enables and reorders them, and reports the ones
it could not open.

`tools/pack_mod.py` turns a directory into one:

```
python tools/pack_mod.py examples/mods/texture-pack-template            # into dist/mods
python tools/pack_mod.py examples/mods/texture-pack-template --install  # into the game's mods folder
```

## What a mod can carry

| Content | The file that declares it | Handled by |
|---|---|---|
| **Texture pack** | `rt64.json` | this port, through RT64's replacement system |
| Recompiled code | `mod_binary.bin` + `mod_syms.bin` | librecomp, recompiled live at load |
| ROM patch | `patch.bps` | librecomp |

A mod is detected as carrying a kind of content simply by containing the file
that declares it. Nothing in `mod.json` says which it is.

## `mod.json`

Six fields are required; the rest are optional.

```json
{
    "game_id": "wr64",
    "id": "my_texture_pack",
    "display_name": "My Texture Pack",
    "short_description": "One line, shown in the list.",
    "description": "The longer text, shown when the mod is selected.",
    "version": "1.0.0",
    "authors": ["You"],
    "minimum_recomp_version": "0.6.0",
    "enabled_by_default": true
}
```

`game_id` must be `wr64`. `enabled_by_default` only applies the first time the
game sees a mod id -- after that its state lives in `mods.json`, so flipping the
field in a mod that is already installed changes nothing.

## Texture packs

RT64 reads a pack **straight out of the zip**, so the `.nrm` is the pack; there
is no second archive inside it and nothing is unpacked to disk. A pack is
`rt64.json` -- RT64's replacement database, keyed by the hash of the texture each
image stands in for -- plus the images.

Packs are applied in the order the Mods tab shows, so a mod further down wins
where two replace the same texture, and they can be switched on and off **while
the game is running**.

### Making one

1. **Get the hashes.** Press **F1**, open the **Textures** tab, and press *Start
   dumping textures*. Play through what you want to replace, then press *Stop*.
   RT64 writes a directory of the game's textures with an `rt64.json` listing
   every one of them and its hash.
2. **Draw your replacements.** Same names, your own images. Higher resolution is
   fine; RT64 scales to whatever the game asked for.
3. **Keep the dump out.** The dumped files are the game's own data. Ship *your*
   images and the `rt64.json` that names them -- never the dump, and never
   anything derived from it by upscaling or filtering. See
   [CONTRIBUTING.md](../../CONTRIBUTING.md).
4. **Zip it.** Put `mod.json` beside `rt64.json` and run `tools/pack_mod.py`.

`texture-pack-template/` is a working pack with no replacements in it: the port
detects it, hands it to RT64 and RT64 loads it, changing nothing. Copy it and
fill it in.

### Checking it worked

Run the game from a terminal and look for the chain:

```
Opening mod my_texture_pack-1.0.0
Loading mod my_texture_pack
[wr64] texture pack on: my_texture_pack (…\my_texture_pack-1.0.0.nrm)
[wr64] texture packs: 1 enabled
RT64: 1 texture pack(s) loaded by the port.
```

A mod that fails to open says so on the same stream, with the reason -- a missing
`mod.json` is the usual one.

**F4** toggles texture replacement on and off, which is the quick way to see what
a pack is actually changing.
