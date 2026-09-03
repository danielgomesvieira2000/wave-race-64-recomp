# PromptFont

`promptfont.ttf` is [PromptFont](https://shinmera.com/promptfont) by Yukari
"Shinmera" Hafner: a font whose glyphs are controller buttons, keyboard keys and
mouse actions. `recompui` uses it to draw input prompts, and `recompinput`'s
`promptfont.h` is the generated mapping from those glyphs to code points.

Version 1.10, taken from the project's own release archive:

    https://github.com/Shinmera/promptfont/releases/download/v1.10/promptfont.zip

Licensed under the SIL Open Font License 1.1, copied here as `LICENSE.txt`. The
OFL permits bundling and redistribution with software; it requires that the
licence and copyright notice travel with the font, which is what that file is
for, and that the font not be sold on its own.

## Why this one is committed when the others are not

The rest of the interface's fonts -- Lato and Noto Emoji -- are copied at build
time out of the RmlUi submodule, which this repository already has. PromptFont is
not vendored anywhere in the tree, so the alternatives were to commit it or to
download it during the build. A build that reaches the network to succeed is a
build that fails on a machine without one, and phase 06's gate is that a stranger
with a dump and no context can build and play this. 140 KB is a small price for
that.

This does not weaken the project's standing rule that nothing ROM-derived is ever
committed. A freely licensed font is not derived from the cartridge, and no part
of it comes from a dump.
