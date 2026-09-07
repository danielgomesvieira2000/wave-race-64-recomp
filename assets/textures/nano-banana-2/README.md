# Bundled HD textures

This is the reviewed runtime pack used by this fork. It contains 1,806 images
and 1,828 RT64 version-5 mappings. HD textures are enabled by default; the
Graphics menu can switch between HD and Original during play.

Most replacements are 4× their original dimensions. Focused 8× updates include
eight helmet portraits, the rank arrow, buoy icons, ramp wood, island materials,
and secondary jet-ski side panels. World materials use uncompressed RGBA8 DDS
with complete mip chains; UI and split lettering retain PNG storage.

The pack includes both Wave Race logo variants, Dolphin Park signage, audited
font families, result labels, power flags, and 32 refined craft side-panel
textures across primary/opponent models and both color sets. Other craft
materials retain their earlier reviewed replacements. A mapping is not a claim
that every material has new generated detail: rejected candidates use
source-preserving fallbacks.

The build copies this directory into the app's assets. An explicitly supplied
`WR64_TEXTURE_PACK` overrides it; a user-installed pack at
`<settings>/textures/nano-banana-2` also takes precedence over the bundled pack.
No separate install is required for the included pack.

See [credits](CREDITS.md), the content checksums in `manifest.json`, and
[the reconstruction workflow](../../../docs/HD_TEXTURES.md).
