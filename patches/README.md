# patches

Replacement implementations of game functions, compiled with N64Recomp's
single-file output mode.

Linkers only search a static library for symbols they have not already
resolved, so listing the patch object *before* the recompiled library makes the
patched version win — without re-running the recompiler or rebuilding the
generated C. During phase 04 that is the difference between seconds and minutes
per iteration, several hundred times over.

Set this up in phase 02, before it is needed. The reference implementation is
Zelda 64: Recompiled's `patches.toml` and its patch Makefile.

Requires elf input mode. See docs/PLAN.md.
